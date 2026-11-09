/* tests/unit/event_loop_test.c — src/io/event_loop.c 的线程与派发契约
 *
 * 判据来自 include/io/event_loop.h 写明的契约，逐条对应：
 *   - 回调在 I/O 线程上运行，不在注册它的线程上；
 *   - evl_stop 能把阻塞在 epoll_wait 里的空闲 I/O 线程唤醒并 join；
 *   - 回调里删掉同一批结果里还没派发的 fd，它的回调不再被调用；
 *   - 同一批里删掉一个 fd 再注册新 fd，陈旧事件不能派发给新 fd
 *     （新 fd 是空管道，回调被调用就说明派发错了）；
 *   - I/O 线程运行期间，别的线程调用 evl_add / evl_del 被拒绝；
 *   - fd 不支持 poll 时返回 VMM_ERR_UNSUPPORTED；
 *   - 写端关闭后回调能收到 EOF/HUP 并删掉自己，循环不会空转。
 *
 * 等待一律等事件本身：回调完成后 post 一个信号量，主线程 sem_wait。
 * alarm() 只兜底“契约被破坏导致永远等不到”的情况（例如 evl_stop 唤不醒
 * I/O 线程），SIGALRM 默认终止进程，unit suite 以非零退出码报失败。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>

#include "vmm.h"
#include "io/event_loop.h"
#include "harness.h"

/* 兜底超时：契约成立时每一步都是毫秒级，30 秒只在 I/O 线程卡死时触发 */
#define HANG_GUARD_SEC 30

int vmm_log_level = -1;

void vmm_log(int level, const char *file, int line, const char *fmt, ...)
{
    (void)level; (void)file; (void)line; (void)fmt;
}

static void make_pipe(int p[2])
{
    if (pipe2(p, O_CLOEXEC) < 0) {
        perror("pipe2");
        _exit(2);
    }
}

static void put_byte(int fd)
{
    char c = 'x';

    if (write(fd, &c, 1) != 1) {
        perror("write");
        _exit(2);
    }
}

/* ------------------------------------------------------------------ */
/* 回调在 I/O 线程上运行                                               */
/* ------------------------------------------------------------------ */

struct thread_probe {
    sem_t     done;
    pthread_t ran_on;
    int       calls;
};

static void on_probe(void *opaque, int fd, uint32_t events)
{
    struct thread_probe *p = opaque;
    char c;

    if (read(fd, &c, 1) != 1)
        return;
    p->ran_on = pthread_self();
    p->calls++;
    sem_post(&p->done);
}

static void case_runs_on_io_thread(void)
{
    struct event_loop l;
    struct thread_probe p = { .calls = 0 };
    int fds[2];

    T_CASE("handler runs on the io thread, not the registering thread");
    sem_init(&p.done, 0, 0);
    make_pipe(fds);
    CHECK_U64(evl_init(&l), VMM_OK, "evl_init");
    CHECK_U64(evl_add(&l, fds[0], EPOLLIN, on_probe, &p), VMM_OK, "evl_add");
    CHECK_U64(evl_start(&l), VMM_OK, "evl_start");

    put_byte(fds[1]);
    sem_wait(&p.done);
    CHECK(!pthread_equal(p.ran_on, pthread_self()),
          "handler ran on the main thread");
    CHECK(pthread_equal(p.ran_on, l.thread),
          "handler did not run on the io thread");

    evl_destroy(&l);
    CHECK_U64(p.calls, 1, "handler calls for one byte");
    close(fds[0]);
    close(fds[1]);
    sem_destroy(&p.done);
}

/* ------------------------------------------------------------------ */
/* evl_stop 唤醒空闲线程                                                */
/* ------------------------------------------------------------------ */

static void case_stop_wakes_idle_loop(void)
{
    struct event_loop l;

    T_CASE("evl_stop wakes an io thread blocked in epoll_wait");
    CHECK_U64(evl_init(&l), VMM_OK, "evl_init");
    CHECK_U64(evl_start(&l), VMM_OK, "evl_start");
    /* 没有任何 fd，I/O 线程只可能阻塞在 epoll_wait 里；evl_stop 返回本身
     * 就是判据，唤不醒时 alarm 终止进程 */
    evl_stop(&l);
    CHECK(!l.started, "loop still marked started after evl_stop");
    /* 第二次 stop 是空操作，不能再 join 一个已经 join 过的线程 */
    evl_stop(&l);
    evl_destroy(&l);
}

/* ------------------------------------------------------------------ */
/* 同一批结果里的删除与重注册                                           */
/* ------------------------------------------------------------------ */

struct batch_ctx {
    struct event_loop *l;
    int    a_rd, b_rd;
    int    c_rd;              /* 空管道：从来没有可读数据 */
    int    a_calls, b_calls, c_calls;
    bool   readd;             /* 删掉对方后再注册 c */
    sem_t  done;
};

static void on_c(void *opaque, int fd, uint32_t events)
{
    struct batch_ctx *x = opaque;

    (void)fd; (void)events;
    x->c_calls++;
}

/* a 和 b 共用这个回调：谁先被派发，谁就删掉对方，然后吃掉自己的字节 */
static void on_ab(void *opaque, int fd, uint32_t events)
{
    struct batch_ctx *x = opaque;
    int other = (fd == x->a_rd) ? x->b_rd : x->a_rd;
    char c;

    (void)events;
    if (fd == x->a_rd)
        x->a_calls++;
    else
        x->b_calls++;
    if (x->a_calls + x->b_calls == 1) {
        evl_del(x->l, other);
        if (x->readd)
            evl_add(x->l, x->c_rd, EPOLLIN, on_c, x);
    }
    if (read(fd, &c, 1) == 1)
        sem_post(&x->done);
}

static void run_batch_case(bool readd)
{
    struct event_loop l;
    struct batch_ctx x;
    int a[2], b[2], c[2];

    memset(&x, 0, sizeof(x));
    sem_init(&x.done, 0, 0);
    make_pipe(a);
    make_pipe(b);
    make_pipe(c);
    x.l = &l;
    x.a_rd = a[0];
    x.b_rd = b[0];
    x.c_rd = c[0];
    x.readd = readd;

    CHECK_U64(evl_init(&l), VMM_OK, "evl_init");
    CHECK_U64(evl_add(&l, a[0], EPOLLIN, on_ab, &x), VMM_OK, "evl_add a");
    CHECK_U64(evl_add(&l, b[0], EPOLLIN, on_ab, &x), VMM_OK, "evl_add b");
    /* 启动前两边都可读，第一次 epoll_wait 就在同一批里拿到两个事件 */
    put_byte(a[1]);
    put_byte(b[1]);
    CHECK_U64(evl_start(&l), VMM_OK, "evl_start");

    sem_wait(&x.done);
    /* 再走一轮：往 a/b 里没被删的那一个写字节并等它的回调。这次回调发生
     * 在下一次 epoll_wait 之后，等到它就说明第一批已经派发完 */
    put_byte(x.a_calls ? a[1] : b[1]);
    sem_wait(&x.done);
    evl_stop(&l);

    CHECK_U64(x.a_calls + x.b_calls, 2,
              "the deleted fd's handler ran (a=%d b=%d)", x.a_calls, x.b_calls);
    CHECK(x.a_calls == 0 || x.b_calls == 0,
          "both a and b were dispatched (a=%d b=%d)", x.a_calls, x.b_calls);
    CHECK_U64(x.c_calls, 0, "stale event dispatched to the empty pipe c");

    evl_destroy(&l);
    close(a[0]); close(a[1]);
    close(b[0]); close(b[1]);
    close(c[0]); close(c[1]);
    sem_destroy(&x.done);
}

static void case_delete_in_batch(void)
{
    T_CASE("fd deleted earlier in the same batch is not dispatched");
    run_batch_case(false);
}

static void case_reuse_in_batch(void)
{
    T_CASE("stale event in the batch is not dispatched to a newly added fd");
    run_batch_case(true);
}

/* ------------------------------------------------------------------ */
/* 线程约束、不可 poll 的 fd、EOF                                       */
/* ------------------------------------------------------------------ */

static void on_never(void *opaque, int fd, uint32_t events)
{
    (void)opaque; (void)fd; (void)events;
}

static void case_foreign_thread_rejected(void)
{
    struct event_loop l;
    int fds[2];

    T_CASE("evl_add/evl_del from another thread are rejected while running");
    make_pipe(fds);
    CHECK_U64(evl_init(&l), VMM_OK, "evl_init");
    CHECK_U64(evl_add(&l, fds[0], EPOLLIN, on_never, NULL), VMM_OK,
              "evl_add before start");
    CHECK_U64(evl_start(&l), VMM_OK, "evl_start");
    CHECK_U64(evl_add(&l, fds[1], EPOLLOUT, on_never, NULL),
              (uint64_t)VMM_ERR_INVAL, "evl_add from main while running");
    CHECK_U64(evl_del(&l, fds[0]), (uint64_t)VMM_ERR_INVAL,
              "evl_del from main while running");
    evl_stop(&l);
    /* 停下之后主线程又是唯一的写者 */
    CHECK_U64(evl_del(&l, fds[0]), VMM_OK, "evl_del after stop");
    evl_destroy(&l);
    close(fds[0]);
    close(fds[1]);
}

static void case_unpollable_fd(void)
{
    struct event_loop l;
    int fd;

    T_CASE("an fd epoll cannot watch reports VMM_ERR_UNSUPPORTED");
    fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0, "open /dev/null: %s", strerror(errno));
    CHECK_U64(evl_init(&l), VMM_OK, "evl_init");
    CHECK_U64(evl_add(&l, fd, EPOLLIN, on_never, NULL),
              (uint64_t)VMM_ERR_UNSUPPORTED, "evl_add(/dev/null)");
    /* 失败的注册不能占住槽位：同一个槽位还能给别的 fd 用 */
    CHECK(l.slots[0].fn == NULL, "failed evl_add left a slot occupied");
    evl_destroy(&l);
    close(fd);
}

struct eof_ctx {
    struct event_loop *l;
    int   calls;
    uint32_t events;
    sem_t done;
};

static void on_eof(void *opaque, int fd, uint32_t events)
{
    struct eof_ctx *x = opaque;
    char c;

    x->calls++;
    x->events = events;
    if (read(fd, &c, 1) == 0) {
        evl_del(x->l, fd);
        sem_post(&x->done);
    }
}

static void case_eof_self_delete(void)
{
    struct event_loop l;
    struct eof_ctx x = { .calls = 0 };
    int fds[2];

    T_CASE("closing the write end delivers HUP and the handler can remove itself");
    sem_init(&x.done, 0, 0);
    make_pipe(fds);
    x.l = &l;
    CHECK_U64(evl_init(&l), VMM_OK, "evl_init");
    CHECK_U64(evl_add(&l, fds[0], EPOLLIN, on_eof, &x), VMM_OK, "evl_add");
    CHECK_U64(evl_start(&l), VMM_OK, "evl_start");
    close(fds[1]);
    sem_wait(&x.done);
    evl_stop(&l);
    CHECK(x.events & EPOLLHUP, "events 0x%x lack EPOLLHUP", x.events);
    CHECK_U64(x.calls, 1, "handler calls after EOF (level-triggered spin?)");
    CHECK(l.slots[0].fn == NULL, "slot still registered after evl_del");
    evl_destroy(&l);
    close(fds[0]);
    sem_destroy(&x.done);
}

int main(int argc, char **argv)
{
    (void)argc;
    t_init(argv[0]);
    alarm(HANG_GUARD_SEC);

    case_runs_on_io_thread();
    case_stop_wakes_idle_loop();
    case_delete_in_batch();
    case_reuse_in_batch();
    case_foreign_thread_rejected();
    case_unpollable_fd();
    case_eof_self_delete();
    return T_SUMMARY();
}
