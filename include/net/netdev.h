/* include/net/netdev.h — 网络后端（--netdev）与网卡前端之间的接口
 *
 * 分层：网卡模型（e1000，以后的 virtio-net）是前端，只和 guest 的寄存器、
 * 描述符环打交道；后端负责把以太网帧送进、送出 host（目前只有 TAP）。
 * 前端不碰后端 fd，后端不认识任何寄存器，两者之间只有三个动作：
 *
 *   TX  前端在 vCPU 线程上调用 netdev_send()，一次一整帧，可以是多段 iovec
 *   RX  后端在 I/O 线程上调用前端注册的 receive()，一次一整帧
 *   流控 前端没有空闲 RX 缓冲时 receive() 返回 NETDEV_RX_FULL；后端留住这
 *       一帧、停止读 fd，直到前端调用 netdev_rx_resume()（guest 补充了
 *       缓冲，例如 e1000 的 RDT 写）
 *
 * 生命周期分两个阶段，和 --device 一致（见 devices/device.h）：
 *   解析阶段  netdev_parse 检查参数，只分配对象，不打开任何 fd；所有
 *            --device、--netdev 都解析完之后，netdev_link 把每个前端的
 *            netdev= 引用解析成对象，并检查一一对应
 *   启动阶段  前端 attach 时 netdev_set_frontend 登记 receive；然后
 *            netdev_open 打开 TAP、注册到 I/O 线程（必须在 evl_start 前）
 * 对象不释放，理由同 --device：注册进 I/O 线程之后一直被引用到进程退出。
 *
 * 帧格式：不带 FCS、不带 TAP 的 packet information 头（IFF_NO_PI），
 * 也不用 IFF_VNET_HDR。校验和与 TSO 分段由前端在设备模型里完成，后端收发
 * 的都是线上的普通以太网帧；这样同一个后端可以服务没有 offload 元数据的
 * 全模拟网卡，offload 的语义也留在实现它的设备模型里。
 */
#ifndef NET_NETDEV_H
#define NET_NETDEV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <net/if.h>
#include <sys/uio.h>

struct event_loop;
struct vmm_device;

/* id 的长度上限。QEMU 对 id 没有长度限制，这里只是为了定长存储 */
#define NETDEV_ID_MAX     32

/* 一次 read() 的缓冲大小，按 TAP 能交出的最长帧取。没有 IFF_VNET_HDR 时
 * 内核在交给 TAP 之前已经把 GSO 包切成不超过 MTU 的帧；TAP 的 MTU 上限是
 * 65535 减以太网头（drivers/net/tun.c tun_net_init 的 max_mtu），帧再加
 * 以太网头和可能插回的 4 字节 VLAN tag（tun_put_user），最长 65539。
 * tun_put_user 在缓冲不够时截断拷贝、但仍返回整帧长度，所以 read() 返回
 * 值大于缓冲就是截断，按错误丢弃。 */
#define NETDEV_RX_BUF     (65535 + 4)

/* 一次回调里最多读多少帧。到达上限就返回事件循环，让同一批里别的 fd
 * 也有机会被处理；水平触发保证剩下的帧下一轮还会报上来。 */
#define NETDEV_RX_BATCH   64

enum netdev_rx_result {
    NETDEV_RX_DONE = 0,   /* 帧已处理（交给 guest，或者前端按自己的规则丢弃） */
    NETDEV_RX_FULL = 1,   /* 前端没有空闲缓冲，帧原样保留，稍后重试 */
};

/* I/O 线程上调用。frame 只在调用期间有效 */
typedef enum netdev_rx_result (*netdev_receive_fn)(void *opaque,
                                                   const uint8_t *frame,
                                                   size_t len);

/* RX 计数只由 I/O 线程更新；TX 计数在 netdev_send 里更新，前端在自己的
 * 设备锁下发送，多个 vCPU 不会同时写。读者只在 VM 停下后读，所以不需要原子 */
struct netdev_stats {
    uint64_t rx_frames, rx_bytes;   /* 交给前端并被接受（NETDEV_RX_DONE） */
    uint64_t rx_errors;             /* read() 出错或帧被截断 */
    uint64_t rx_pauses;             /* 因前端缓冲用尽而暂停读 fd 的次数 */
    uint64_t tx_frames, tx_bytes;
    uint64_t tx_errors;             /* writev() 失败或写不全，帧丢失 */
};

struct netdev {
    char        id[NETDEV_ID_MAX];
    char        ifname[IFNAMSIZ];
    const char *arg;                /* 原始参数，报错用；指向 argv，进程内有效 */
    const char *user;               /* 引用它的 --device 参数，netdev_link 填 */

    int         fd;                 /* TAP，O_NONBLOCK；未打开时为 -1 */
    int         wake_fd;            /* eventfd，netdev_rx_resume 用它唤醒 I/O 线程 */
    struct event_loop *loop;

    netdev_receive_fn receive;
    void       *opaque;

    /* RX 流控，见 netdev.c 里 rx_pause 的注释 */
    bool        rx_blocked;         /* 原子访问：I/O 线程置位，任意线程清零 */
    size_t      pending_len;        /* >0 表示 rxbuf 里有一帧还没被前端接受；
                                       只有 I/O 线程访问 */

    struct netdev_stats stats;
    uint8_t     rxbuf[NETDEV_RX_BUF];
};

/* --netdev 的参数值，如 "tap,id=n0,ifname=tap0"。失败时已打印原因 */
int  netdev_parse(const char *arg, struct netdev **out);

/* --netdev help 的输出 */
void netdev_list_types(FILE *f);

/* 所有参数解析完之后调用一次：把每个设备的 netdev= 解析成对象，要求
 * 引用的 id 存在、每个 netdev 恰好被一个设备引用。nets 里 id 重复也在
 * 这里报。失败时已打印原因。 */
int  netdev_link(struct vmm_device *devs, int nr_devs,
                 struct netdev *const *nets, int nr_nets);

/* 前端 attach 时调用，netdev_open 之前 */
void netdev_set_frontend(struct netdev *nd, netdev_receive_fn fn, void *opaque);

/* 打开 TAP 并把它和唤醒用的 eventfd 注册到 loop。必须在 evl_start 之前、
 * 前端登记之后调用。失败时已打印原因，包括怎样预先建好 TAP 的提示。 */
int  netdev_open(struct netdev *nd, struct event_loop *loop);

/* 发送一帧，任意线程。帧丢失时返回 VMM_ERR_SYS 并计入 tx_errors；
 * 以太网本来就允许丢帧，调用方通常只需要继续处理下一帧。 */
int  netdev_send(struct netdev *nd, const struct iovec *iov, int iovcnt);

/* 前端补充了 RX 缓冲之后调用，任意线程。后端没有暂停时几乎没有开销
 * （一次原子交换），所以前端每次补充缓冲都可以调用。 */
void netdev_rx_resume(struct netdev *nd);

/* 关闭 fd。调用前 I/O 线程必须已经停止（evl_stop） */
void netdev_close(struct netdev *nd);

#endif /* NET_NETDEV_H */
