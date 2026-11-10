/* src/io/event_loop.c — epoll I/O 线程，接口和加锁顺序见 include/io/event_loop.h */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

#include "vmm.h"
#include "io/event_loop.h"

/* 一次 epoll_wait 取回的事件数上限，只影响批量大小，不影响正确性 */
#define EVL_BATCH 16

int evl_init(struct event_loop *l)
{
    struct epoll_event ev;
    int i;

    memset(l, 0, sizeof(*l));
    l->epfd = l->stop_fd = -1;
    for (i = 0; i < EVL_MAX_HANDLERS; i++)
        l->slots[i].fd = -1;

    l->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (l->epfd < 0) {
        vmm_err("epoll_create1 failed: %s", strerror(errno));
        return VMM_ERR_SYS;
    }
    l->stop_fd = eventfd(0, EFD_CLOEXEC);
    if (l->stop_fd < 0) {
        vmm_err("eventfd failed: %s", strerror(errno));
        evl_destroy(l);
        return VMM_ERR_SYS;
    }
    /* data.ptr = NULL 标记停机事件，处理器槽位的指针不可能是 NULL */
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = NULL;
    if (epoll_ctl(l->epfd, EPOLL_CTL_ADD, l->stop_fd, &ev) < 0) {
        vmm_err("epoll_ctl(ADD stop eventfd) failed: %s", strerror(errno));
        evl_destroy(l);
        return VMM_ERR_SYS;
    }
    return VMM_OK;
}

/* 处理器表只允许一个写者，见头文件“线程模型” */
static bool on_owner_thread(struct event_loop *l, const char *op, int fd)
{
    if (!__atomic_load_n(&l->started, __ATOMIC_ACQUIRE) ||
        pthread_equal(l->thread, pthread_self()))
        return true;
    vmm_err("%s(fd=%d) called from a thread other than the io thread", op, fd);
    return false;
}

int evl_add(struct event_loop *l, int fd, uint32_t events,
            evl_handler_fn fn, void *opaque)
{
    struct epoll_event ev;
    struct evl_slot *s = NULL;
    int i;

    if (fd < 0 || !fn || !on_owner_thread(l, "evl_add", fd))
        return VMM_ERR_INVAL;

    for (i = 0; i < EVL_MAX_HANDLERS; i++) {
        if (!l->slots[i].fn && !l->slots[i].retired) {
            s = &l->slots[i];
            break;
        }
    }
    if (!s) {
        vmm_err("evl_add(fd=%d): all %d handler slots in use",
                fd, EVL_MAX_HANDLERS);
        return VMM_ERR_NOMEM;
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.ptr = s;
    if (epoll_ctl(l->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        if (errno == EPERM)
            return VMM_ERR_UNSUPPORTED;
        vmm_err("epoll_ctl(ADD fd=%d events=0x%x) failed: %s",
                fd, events, strerror(errno));
        return VMM_ERR_SYS;
    }
    s->fd = fd;
    s->fn = fn;
    s->opaque = opaque;
    return VMM_OK;
}

int evl_del(struct event_loop *l, int fd)
{
    int i;

    if (!on_owner_thread(l, "evl_del", fd))
        return VMM_ERR_INVAL;

    for (i = 0; i < EVL_MAX_HANDLERS; i++) {
        struct evl_slot *s = &l->slots[i];

        if (!s->fn || s->fd != fd)
            continue;
        if (epoll_ctl(l->epfd, EPOLL_CTL_DEL, fd, NULL) < 0)
            vmm_warn("epoll_ctl(DEL fd=%d) failed: %s", fd, strerror(errno));
        s->fn = NULL;
        s->opaque = NULL;
        s->fd = -1;
        s->retired = l->dispatching;
        return VMM_OK;
    }
    vmm_err("evl_del(fd=%d): fd is not registered", fd);
    return VMM_ERR_INVAL;
}

int evl_mod(struct event_loop *l, int fd, uint32_t events)
{
    struct epoll_event ev;
    int i;

    if (!on_owner_thread(l, "evl_mod", fd))
        return VMM_ERR_INVAL;

    for (i = 0; i < EVL_MAX_HANDLERS; i++) {
        struct evl_slot *s = &l->slots[i];

        if (!s->fn || s->fd != fd)
            continue;
        memset(&ev, 0, sizeof(ev));
        ev.events = events;
        ev.data.ptr = s;
        if (epoll_ctl(l->epfd, EPOLL_CTL_MOD, fd, &ev) < 0) {
            vmm_err("epoll_ctl(MOD fd=%d events=0x%x) failed: %s",
                    fd, events, strerror(errno));
            return VMM_ERR_SYS;
        }
        return VMM_OK;
    }
    vmm_err("evl_mod(fd=%d): fd is not registered", fd);
    return VMM_ERR_INVAL;
}

static void *io_thread(void *arg)
{
    struct event_loop *l = arg;
    struct epoll_event evs[EVL_BATCH];
    bool stop = false;
    int n, i;

    pthread_setname_np(pthread_self(), "io");
    vmm_dbg("io thread: started");

    while (!stop) {
        n = epoll_wait(l->epfd, evs, EVL_BATCH, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            vmm_err("epoll_wait failed: %s, io thread exits", strerror(errno));
            break;
        }

        l->dispatching = true;
        for (i = 0; i < n; i++) {
            struct evl_slot *s = evs[i].data.ptr;

            if (!s) {
                stop = true;
                continue;
            }
            /* 同一批里排在前面的回调可能已经把它删掉了 */
            if (s->fn)
                s->fn(s->opaque, s->fd, evs[i].events);
        }
        l->dispatching = false;
        for (i = 0; i < EVL_MAX_HANDLERS; i++)
            l->slots[i].retired = false;
    }
    vmm_dbg("io thread: stopped");
    return NULL;
}

int evl_start(struct event_loop *l)
{
    int r;

    r = pthread_create(&l->thread, NULL, io_thread, l);
    if (r != 0) {
        vmm_err("pthread_create io thread failed: %s", strerror(r));
        return VMM_ERR_SYS;
    }
    __atomic_store_n(&l->started, true, __ATOMIC_RELEASE);
    return VMM_OK;
}

void evl_stop(struct event_loop *l)
{
    uint64_t one = 1;

    if (!__atomic_load_n(&l->started, __ATOMIC_ACQUIRE))
        return;
    /* eventfd 只有计数器将要溢出时才会写失败，这里每次只加 1。万一失败，
     * I/O 线程收不到停机事件，join 会永远等下去，所以宁可不 join：进程
     * 随后退出时线程一并回收 */
    if (write(l->stop_fd, &one, sizeof(one)) != sizeof(one)) {
        vmm_err("write(stop eventfd) failed: %s, io thread left running",
                strerror(errno));
        return;
    }
    pthread_join(l->thread, NULL);
    __atomic_store_n(&l->started, false, __ATOMIC_RELEASE);
}

void evl_destroy(struct event_loop *l)
{
    evl_stop(l);
    if (l->stop_fd >= 0)
        close(l->stop_fd);
    if (l->epfd >= 0)
        close(l->epfd);
    l->stop_fd = l->epfd = -1;
}
