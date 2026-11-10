/* include/io/event_loop.h — 独立于 vCPU 的 I/O 线程
 *
 * 一个 epoll 线程（线程名 "io"）负责所有后端 fd：host 终端输入，以后的
 * TAP、磁盘、socket。fd 可读/可写时它调用注册时给的回调，回调在这个线程
 * 里更新设备状态、注入中断。vCPU 线程只处理 guest 的 exit，不阻塞在后端
 * I/O 上：guest 访问设备寄存器时，数据要么已经在设备模型里，要么由回调
 * 稍后送进来，KVM_RUN 不会因为 host 侧没有数据而停住。
 *
 * 线程模型
 *   evl_add / evl_del / evl_mod 只能在 evl_start 之前或者在回调里（即 I/O 线程上）
 *   调用，从别的线程调用返回 VMM_ERR_INVAL。这样处理器表只有一个写者，
 *   不需要锁；别的线程需要 I/O 线程做事时，应当写一个由 I/O 线程监听的
 *   eventfd，而不是直接改表。
 *
 * 加锁顺序（和 vm.c 的 exit 分发路径一致）
 *   vm->io_lock  →  设备自己的锁（如 serial8250.lock）  →  KVM ioctl
 *   回调更新没有自己锁的设备时先拿 io_lock，和 vCPU 线程的 exit 分发串行；
 *   设备有自己的锁时只拿那把锁即可。任何时候持有设备锁都不能再去拿
 *   io_lock，否则和持有 io_lock 再拿设备锁的 vCPU 线程构成 ABBA 死锁。
 *   KVM_IRQ_LINE 等 ioctl 不拿 VMM 的锁，在哪一层调用都不影响这个顺序。
 *
 * 回调约定
 *   epoll 用水平触发：回调一次只需要处理它能处理的部分，剩下的数据下一轮
 *   还会再报。收到 EPOLLHUP / EPOLLERR 或读到 EOF 时，回调负责 evl_del
 *   自己的 fd，否则水平触发会让同一个事件无限重报。
 */
#ifndef IO_EVENT_LOOP_H
#define IO_EVENT_LOOP_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

/* 当前只有 host 终端一个后端；全模拟网卡、磁盘各加一两个 fd，32 个足够，
 * 用定长表是为了让处理器槽位的地址在整个生命周期里不变（见 slot 的注释） */
#define EVL_MAX_HANDLERS 32

/* events 是 epoll_wait 报上来的 EPOLLIN / EPOLLOUT / EPOLLHUP / EPOLLERR 组合 */
typedef void (*evl_handler_fn)(void *opaque, int fd, uint32_t events);

struct evl_slot {
    int             fd;
    evl_handler_fn  fn;         /* NULL 表示空闲或已删除 */
    void           *opaque;
    /* 本轮 epoll_wait 结果处理完之前被删掉的槽位暂不复用：同一批结果里可能
     * 还有它的事件，epoll_event.data 指向的是槽位，复用的话这个陈旧事件会
     * 被派发给新注册的 fd */
    bool            retired;
};

struct event_loop {
    int             epfd;
    int             stop_fd;    /* eventfd，evl_stop 写它把 I/O 线程从 epoll_wait 唤醒 */
    pthread_t       thread;
    bool            started;
    bool            dispatching;    /* I/O 线程正在派发一批事件 */
    struct evl_slot slots[EVL_MAX_HANDLERS];
};

int  evl_init(struct event_loop *l);

/* 注册 fd。fd 不支持 poll（普通文件、/dev/null）时 epoll_ctl 报 EPERM，
 * 返回 VMM_ERR_UNSUPPORTED 且不打日志，由调用方决定这算不算错误 */
int  evl_add(struct event_loop *l, int fd, uint32_t events,
             evl_handler_fn fn, void *opaque);
int  evl_del(struct event_loop *l, int fd);
/* 改变已注册 fd 关心的事件，例如暂时传 0 停止接收 EPOLLIN（流控），之后
 * 再改回来。和 evl_del + evl_add 相比不占用新槽位，也不会让同一批结果里
 * 的陈旧事件指向别的 fd。线程限制同 evl_add。 */
int  evl_mod(struct event_loop *l, int fd, uint32_t events);

int  evl_start(struct event_loop *l);
/* 唤醒并 join I/O 线程；返回后不会再有回调在运行。未启动时直接返回 */
void evl_stop(struct event_loop *l);
void evl_destroy(struct event_loop *l);

#endif /* IO_EVENT_LOOP_H */
