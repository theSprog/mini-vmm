/* src/net/netdev.c — --netdev 的解析、前后端连接、帧收发与 RX 流控
 *
 * 接口约定和生命周期见 include/net/netdev.h，TAP 的打开见 tap.c。
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

#include "vmm.h"
#include "opts.h"
#include "io/event_loop.h"
#include "net/netdev.h"
#include "net/tap.h"

static const char *const tap_props[] = { "type", "id", "ifname", NULL };

void netdev_list_types(FILE *f)
{
    fprintf(f, "Available --netdev types:\n");
    fprintf(f, "  %-12s %s\n", "tap",
            "host TAP interface; properties: id, ifname");
}

int netdev_parse(const char *arg, struct netdev **out)
{
    struct netdev *nd = NULL;
    const char *type, *id, *ifname;
    struct opts o;
    int r;

    r = opts_parse(&o, "--netdev", arg, "type");
    if (r != VMM_OK)
        return r;

    r = VMM_ERR_INVAL;
    type = opts_get(&o, "type");
    if (!type) {
        vmm_err("--netdev %s: missing type (see --netdev help)", arg);
        goto out;
    }
    if (strcmp(type, "tap")) {
        vmm_err("--netdev %s: unknown type '%s' (see --netdev help)", arg, type);
        goto out;
    }
    if (opts_check_keys(&o, tap_props) != VMM_OK)
        goto out;

    id = opts_require(&o, "id");
    ifname = opts_require(&o, "ifname");
    if (!id || !ifname)
        goto out;
    if (!opts_id_wellformed(id) || strlen(id) >= NETDEV_ID_MAX) {
        vmm_err("--netdev %s: invalid id '%s' (a letter followed by letters, "
                "digits, '-', '.' or '_', at most %d characters)",
                arg, id, NETDEV_ID_MAX - 1);
        goto out;
    }
    /* 其余字符规则（不能含 '/'、':'、空白等）由内核在 TUNSETIFF 时检查，
     * 这里只管长度，因为它决定能不能放进 ifreq.ifr_name */
    if (!ifname[0] || strlen(ifname) >= IFNAMSIZ) {
        vmm_err("--netdev %s: ifname must be 1 to %d characters",
                arg, IFNAMSIZ - 1);
        goto out;
    }

    nd = calloc(1, sizeof(*nd));
    if (!nd) {
        r = VMM_ERR_NOMEM;
        goto out;
    }
    memcpy(nd->id, id, strlen(id) + 1);
    memcpy(nd->ifname, ifname, strlen(ifname) + 1);
    nd->arg = arg;
    nd->fd = -1;
    nd->wake_fd = -1;
    *out = nd;
    r = VMM_OK;
out:
    opts_free(&o);
    return r;
}

int netdev_link(struct vmm_device *devs, int nr_devs,
                struct netdev *const *nets, int nr_nets)
{
    int r = VMM_OK;
    int i, j;

    for (i = 0; i < nr_nets; i++)
        for (j = 0; j < i; j++)
            if (!strcmp(nets[i]->id, nets[j]->id)) {
                vmm_err("--netdev %s: duplicate id '%s' (also used by --netdev %s)",
                        nets[i]->arg, nets[i]->id, nets[j]->arg);
                r = VMM_ERR_INVAL;
            }
    if (r != VMM_OK)
        return r;

    for (i = 0; i < nr_devs; i++) {
        struct vmm_device *d = &devs[i];
        struct netdev *nd = NULL;

        if (!d->netdev_id)
            continue;
        for (j = 0; j < nr_nets; j++)
            if (!strcmp(nets[j]->id, d->netdev_id))
                nd = nets[j];
        if (!nd) {
            vmm_err("--device %s: no --netdev with id '%s'", d->arg, d->netdev_id);
            r = VMM_ERR_INVAL;
            continue;
        }
        /* 一个后端只接一个前端：两张网卡共用一个 TAP 时，收到的帧该给谁
         * 没有定义 */
        if (nd->user) {
            vmm_err("--device %s: netdev '%s' is already used by --device %s",
                    d->arg, nd->id, nd->user);
            r = VMM_ERR_INVAL;
            continue;
        }
        nd->user = d->arg;
        d->net = nd;
    }

    /* 没有前端的后端是写漏了 --device：TAP 照样会被打开，看起来一切正常，
     * guest 里却没有网卡 */
    for (i = 0; i < nr_nets; i++)
        if (!nets[i]->user) {
            vmm_err("--netdev %s: not used by any --device (a NIC needs netdev=%s)",
                    nets[i]->arg, nets[i]->id);
            r = VMM_ERR_INVAL;
        }
    return r;
}

void netdev_set_frontend(struct netdev *nd, netdev_receive_fn fn, void *opaque)
{
    nd->receive = fn;
    nd->opaque = opaque;
}

/* 把 rxbuf 里挂着的那一帧交给前端。前端没空间时返回 false，帧继续挂着 */
static bool rx_deliver(struct netdev *nd)
{
    if (nd->receive(nd->opaque, nd->rxbuf, nd->pending_len) == NETDEV_RX_FULL)
        return false;
    nd->stats.rx_frames++;
    nd->stats.rx_bytes += nd->pending_len;
    nd->pending_len = 0;
    return true;
}

static void rx_set_polling(struct netdev *nd, bool on)
{
    /* 失败只可能是 fd 已经不在 epoll 里（读出错后已删除），没有别的补救 */
    evl_mod(nd->loop, nd->fd, on ? EPOLLIN : 0);
}

/* 前端缓冲用尽：停止读 TAP，等 netdev_rx_resume 唤醒。
 *
 * 必须停：epoll 是水平触发，TAP 里还有帧时 fd 一直可读，不停的话 I/O
 * 线程会反复回调、反复得到 NETDEV_RX_FULL，空转占满一个 CPU。
 *
 * 先置 rx_blocked 再重试一次，是为了不丢唤醒。前端第一次返回 FULL 之后、
 * 这里置位之前，guest 可能已经补充了缓冲，那次 netdev_rx_resume 看到
 * rx_blocked 还是 false，不会唤醒；这次重试负责接住它。置位之后补充的，
 * resume 一定看到 true 并写 eventfd。两边都用 seq_cst，前端的 receive
 * 和 resume 又都在同一把设备锁下面，两种先后顺序都被覆盖。 */
static void rx_pause(struct netdev *nd)
{
    rx_set_polling(nd, false);
    __atomic_store_n(&nd->rx_blocked, true, __ATOMIC_SEQ_CST);
    if (rx_deliver(nd)) {
        /* resume 可能已经抢先清零并写了 eventfd，那次唤醒到来时无事可做 */
        __atomic_store_n(&nd->rx_blocked, false, __ATOMIC_SEQ_CST);
        rx_set_polling(nd, true);
        return;
    }
    nd->stats.rx_pauses++;
    vmm_dbg("netdev %s: frontend has no RX buffer, pausing", nd->id);
}

static void netdev_on_rx(void *opaque, int fd, uint32_t events)
{
    struct netdev *nd = opaque;
    int i;

    (void)events;
    for (i = 0; i < NETDEV_RX_BATCH; i++) {
        if (!nd->pending_len) {
            ssize_t n = read(fd, nd->rxbuf, sizeof(nd->rxbuf));

            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR)
                    return;
                /* 典型情况是接口被 "ip link del" 删掉了（EBADFD，poll
                 * 报 EPOLLERR）。不删 fd 的话水平触发会无限重报 */
                vmm_err("netdev %s: read from TAP %s failed: %s, receive path stopped",
                        nd->id, nd->ifname, strerror(errno));
                nd->stats.rx_errors++;
                evl_del(nd->loop, fd);
                return;
            }
            if (n == 0)
                return;
            if ((size_t)n > sizeof(nd->rxbuf)) {
                vmm_warn("netdev %s: dropped a %zd-byte frame, longer than the "
                         "%zu-byte receive buffer", nd->id, n, sizeof(nd->rxbuf));
                nd->stats.rx_errors++;
                continue;
            }
            nd->pending_len = (size_t)n;
        }
        if (!rx_deliver(nd)) {
            rx_pause(nd);
            return;
        }
    }
}

/* netdev_rx_resume 写了 eventfd：重试挂着的帧，成功就恢复读 TAP */
static void netdev_on_wake(void *opaque, int fd, uint32_t events)
{
    struct netdev *nd = opaque;
    uint64_t v;

    (void)events;
    /* EAGAIN 表示计数已经被读空，eventfd 不会有别的读失败 */
    if (read(fd, &v, sizeof(v)) < 0 && errno != EAGAIN)
        vmm_warn("netdev %s: read(wake eventfd): %s", nd->id, strerror(errno));

    if (nd->pending_len && !rx_deliver(nd)) {
        /* 补充的缓冲又被别的帧用完了，或者这是一次多余的唤醒 */
        rx_pause(nd);
        return;
    }
    vmm_dbg("netdev %s: frontend refilled RX buffers, resuming", nd->id);
    rx_set_polling(nd, true);
}

int netdev_open(struct netdev *nd, struct event_loop *loop)
{
    int r;

    if (!nd->receive) {
        vmm_err("netdev %s: opened without a frontend", nd->id);
        return VMM_ERR_INVAL;
    }
    r = tap_open(nd->id, nd->ifname, &nd->fd);
    if (r != VMM_OK)
        return r;
    nd->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (nd->wake_fd < 0) {
        vmm_err("netdev %s: eventfd: %s", nd->id, strerror(errno));
        r = VMM_ERR_SYS;
        goto fail;
    }
    nd->loop = loop;
    r = evl_add(loop, nd->fd, EPOLLIN, netdev_on_rx, nd);
    if (r != VMM_OK)
        goto fail;
    r = evl_add(loop, nd->wake_fd, EPOLLIN, netdev_on_wake, nd);
    if (r != VMM_OK) {
        evl_del(loop, nd->fd);
        goto fail;
    }
    return VMM_OK;
fail:
    netdev_close(nd);
    return r;
}

int netdev_send(struct netdev *nd, const struct iovec *iov, int iovcnt)
{
    size_t want = 0;
    ssize_t n;
    int i;

    for (i = 0; i < iovcnt; i++)
        want += iov[i].iov_len;

    /* TAP 的一次 write 就是一帧（tun_get_user），不存在“写了一半再补” */
    n = writev(nd->fd, iov, iovcnt);
    if (n >= 0 && (size_t)n == want) {
        nd->stats.tx_frames++;
        nd->stats.tx_bytes += want;
        return VMM_OK;
    }
    /* 第一次失败给出原因（最常见的是接口没 up：EIO），之后只计数，否则
     * guest 每发一帧就打一行 */
    if (nd->stats.tx_errors++ == 0) {
        if (n < 0)
            vmm_warn("netdev %s: write to TAP %s failed: %s (further failures "
                     "are only counted)", nd->id, nd->ifname, strerror(errno));
        else
            vmm_warn("netdev %s: short write to TAP %s: %zd of %zu bytes",
                     nd->id, nd->ifname, n, want);
    }
    return VMM_ERR_SYS;
}

void netdev_rx_resume(struct netdev *nd)
{
    uint64_t one = 1;

    if (!__atomic_exchange_n(&nd->rx_blocked, false, __ATOMIC_SEQ_CST))
        return;
    /* eventfd 只在计数将要溢出时写失败，这里每次只加 1 */
    if (write(nd->wake_fd, &one, sizeof(one)) != sizeof(one))
        vmm_err("netdev %s: write(wake eventfd): %s, receive path stays paused",
                nd->id, strerror(errno));
}

void netdev_close(struct netdev *nd)
{
    /* 暂停次数说明前端的环满过几次（rx_pause()）。后端流控有没有真的被
     * 用到只有这里看得到：帧在 TAP 队列里等着，guest 侧没有对应的计数 */
    if (nd->fd >= 0)
        vmm_info("netdev %s: rx %llu frames, %llu pauses, %llu errors; tx %llu frames, %llu errors",
                 nd->id, (unsigned long long)nd->stats.rx_frames,
                 (unsigned long long)nd->stats.rx_pauses,
                 (unsigned long long)nd->stats.rx_errors,
                 (unsigned long long)nd->stats.tx_frames,
                 (unsigned long long)nd->stats.tx_errors);
    if (nd->fd >= 0)
        close(nd->fd);
    if (nd->wake_fd >= 0)
        close(nd->wake_fd);
    nd->fd = nd->wake_fd = -1;
}
