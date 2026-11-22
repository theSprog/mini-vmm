/* tests/unit/netdev_test.c — 网络后端（src/net/netdev.c、src/net/tap.c）
 *
 * 判据来源：
 *   - --netdev 的语法和 id 规则：include/opts.h、QEMU util/id.c 的
 *     id_wellformed；引用规则（id 存在、一一对应、没有前端是错误）：
 *     include/net/netdev.h 的 netdev_link 契约。
 *   - TAP 的行为：linux drivers/net/tun.c。写进 TAP 的一帧原样出现在接口
 *     上（tun_get_user），接口发出的帧原样从 fd 读出（tun_put_user）；
 *     接口没 up 时写返回 EIO；同一个单队列 TAP 第二次 TUNSETIFF 得到
 *     EBUSY，同名接口不是 TAP 得到 EINVAL（tun_set_iff / tun_attach）。
 *     接口另一侧用 AF_PACKET socket 收发，它和被测代码互相独立，所以
 *     比对的是内核真正看到的帧，而不是被测代码自己的说法。
 *   - RX 流控：include/net/netdev.h。前端返回 NETDEV_RX_FULL 后后端停止
 *     读 fd、帧不丢、顺序不变，netdev_rx_resume 之后按原顺序交付。
 *
 * TAP 用例需要建接口，所以进程先 unshare 出自己的 user + net namespace，
 * 在里面有 CAP_NET_ADMIN，不需要 root，也不碰 host 的网络。内核禁止
 * 非特权 user namespace 时（例如部分发行版的 AppArmor 限制）这部分打印
 * SKIP，解析和引用规则的用例照常运行。
 *
 * 等待一律等事件本身（前端回调 post 信号量、对端 socket 收到帧）。每次
 * 等待有 WAIT_SEC 的上限，只用来兜底“契约被破坏、事件永远不来”的情况
 * （例如帧丢了、暂停后再也没恢复），超时时报出正在等什么然后退出，
 * 而不是挂住。只有“暂停之后前端不再被调用”这一条是在证明一件事没有
 * 发生，没有信号可等，用 200 ms 的观察窗；不暂停的实现在水平触发下会在
 * 微秒级反复回调，窗口长短不影响判定。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>

#include "vmm.h"
#include "io/event_loop.h"
#include "net/netdev.h"
#include "harness.h"

#define HANG_GUARD_SEC  60
/* 单次等待的上限：正常情况下都是毫秒级 */
#define WAIT_SEC        5
#define TAP_NAME        "mvt0"
/* IEEE 802 本地实验用 EtherType，内核自己不会发这种帧，用来从接口上的
 * 其它流量（IPv6 邻居发现等）里认出测试帧 */
#define TEST_ETHERTYPE  0x88B5

/* vmm_log 桩：所有日志都留下来，I/O 线程也会打日志，所以加锁 */
int vmm_log_level = -1;
static char g_log[8192];
static size_t g_log_len;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

void vmm_log(int level, const char *file, int line, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    int n;

    (void)level; (void)file; (void)line;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_log_lock);
    n = snprintf(g_log + g_log_len, sizeof(g_log) - g_log_len, "%s\n", msg);
    if (n > 0 && g_log_len + (size_t)n < sizeof(g_log))
        g_log_len += (size_t)n;
    pthread_mutex_unlock(&g_log_lock);
    if (vmm_log_level >= 0)
        fprintf(stderr, "       [log] %s\n", msg);
}

static void log_clear(void)
{
    pthread_mutex_lock(&g_log_lock);
    g_log_len = 0;
    g_log[0] = '\0';
    pthread_mutex_unlock(&g_log_lock);
}

static bool log_has(const char *s)
{
    bool r;

    pthread_mutex_lock(&g_log_lock);
    r = strstr(g_log, s) != NULL;
    pthread_mutex_unlock(&g_log_lock);
    return r;
}

static int log_count(const char *s)
{
    const char *p;
    int n = 0;

    pthread_mutex_lock(&g_log_lock);
    for (p = g_log; (p = strstr(p, s)) != NULL; p += strlen(s))
        n++;
    pthread_mutex_unlock(&g_log_lock);
    return n;
}

/* ------------------------------------------------------------------ */
/* --netdev 解析                                                       */
/* ------------------------------------------------------------------ */

static void expect_parse_fail(const char *arg, const char *msg)
{
    struct netdev *nd = NULL;

    log_clear();
    CHECK_U64(netdev_parse(arg, &nd), (uint64_t)VMM_ERR_INVAL, "parse \"%s\"", arg);
    CHECK(log_has(msg), "\"%s\": log lacks \"%s\": %s", arg, msg, g_log);
    CHECK(log_has(arg), "\"%s\": error does not quote the argument: %s", arg, g_log);
    free(nd);
}

static void case_parse(void)
{
    struct netdev *nd = NULL;

    T_CASE("--netdev parses tap,id=,ifname= and rejects malformed values");
    CHECK_U64(netdev_parse("tap,id=n0,ifname=tap0", &nd), VMM_OK, "valid");
    if (nd) {
        CHECK(!strcmp(nd->id, "n0"), "id %s", nd->id);
        CHECK(!strcmp(nd->ifname, "tap0"), "ifname %s", nd->ifname);
        CHECK(nd->fd == -1 && nd->wake_fd == -1, "fds open at parse time");
        free(nd);
    }
    nd = NULL;
    CHECK_U64(netdev_parse("type=tap,ifname=t-1,id=Net_0.a", &nd), VMM_OK,
              "explicit type=, any key order, id with -._");
    free(nd);
    /* 最长的合法值正好填满定长字段 */
    nd = NULL;
    CHECK_U64(netdev_parse("tap,id=a234567890123456789012345678901,"
                           "ifname=abcdefghijklmno", &nd), VMM_OK,
              "id of 31 and ifname of 15 characters");
    free(nd);

    expect_parse_fail("user,id=n0", "unknown type 'user'");
    expect_parse_fail("id=n0,ifname=t0", "missing type");
    expect_parse_fail("tap,ifname=t0", "missing property 'id'");
    expect_parse_fail("tap,id=n0", "missing property 'ifname'");
    expect_parse_fail("tap,id=n0,ifname=t0,mac=x", "unknown property 'mac'");
    expect_parse_fail("tap,id=0n,ifname=t0", "invalid id '0n'");
    expect_parse_fail("tap,id=n/0,ifname=t0", "invalid id 'n/0'");
    expect_parse_fail("tap,id=a2345678901234567890123456789012,ifname=t0",
                      "invalid id");
    expect_parse_fail("tap,id=n0,ifname=", "ifname must be");
    expect_parse_fail("tap,id=n0,ifname=abcdefghijklmnop", "ifname must be");
}

/* ------------------------------------------------------------------ */
/* netdev= 引用                                                        */
/* ------------------------------------------------------------------ */

static struct netdev *mk_netdev(const char *arg)
{
    struct netdev *nd = NULL;

    if (netdev_parse(arg, &nd) != VMM_OK) {
        printf("cannot parse %s\n", arg);
        exit(2);
    }
    return nd;
}

static void case_link(void)
{
    struct netdev *n0 = mk_netdev("tap,id=n0,ifname=t0");
    struct netdev *n1 = mk_netdev("tap,id=n1,ifname=t1");
    struct netdev *dup = mk_netdev("tap,id=n0,ifname=t2");
    struct vmm_device devs[3];
    struct netdev *nets[2];

    T_CASE("netdev= references resolve one-to-one");
    memset(devs, 0, sizeof(devs));
    /* --netdev 在 --device 之后也能解析：引用只按 id 查 */
    devs[0].arg = "nic,netdev=n1";  devs[0].netdev_id = "n1";
    devs[1].arg = "plain";          /* 没有 netdev= 的设备不参与 */
    devs[2].arg = "nic,netdev=n0";  devs[2].netdev_id = "n0";
    nets[0] = n0; nets[1] = n1;
    CHECK_U64(netdev_link(devs, 3, nets, 2), VMM_OK, "two NICs, two netdevs");
    CHECK_PTR(devs[0].net, n1, "dev0 -> n1");
    CHECK_PTR(devs[1].net, NULL, "dev1 has no netdev");
    CHECK_PTR(devs[2].net, n0, "dev2 -> n0");
    CHECK(n0->user && !strcmp(n0->user, "nic,netdev=n0"), "n0 user %s", n0->user);

    T_CASE("netdev= to an unknown id is rejected");
    n0->user = n1->user = NULL;
    memset(devs, 0, sizeof(devs));
    devs[0].arg = "nic,netdev=n9"; devs[0].netdev_id = "n9";
    nets[0] = n0;
    log_clear();
    CHECK_U64(netdev_link(devs, 1, nets, 1), (uint64_t)VMM_ERR_INVAL, "unknown");
    CHECK(log_has("--device nic,netdev=n9: no --netdev with id 'n9'"), "%s", g_log);

    T_CASE("two NICs on one netdev are rejected");
    n0->user = NULL;
    memset(devs, 0, sizeof(devs));
    devs[0].arg = "a,netdev=n0"; devs[0].netdev_id = "n0";
    devs[1].arg = "b,netdev=n0"; devs[1].netdev_id = "n0";
    log_clear();
    CHECK_U64(netdev_link(devs, 2, nets, 1), (uint64_t)VMM_ERR_INVAL, "shared");
    CHECK(log_has("--device b,netdev=n0: netdev 'n0' is already used by --device a,netdev=n0"),
          "%s", g_log);

    T_CASE("a netdev no NIC uses is rejected");
    n0->user = NULL;
    memset(devs, 0, sizeof(devs));
    devs[0].arg = "a,netdev=n0"; devs[0].netdev_id = "n0";
    nets[0] = n0; nets[1] = n1;
    log_clear();
    CHECK_U64(netdev_link(devs, 1, nets, 2), (uint64_t)VMM_ERR_INVAL, "unused n1");
    CHECK(log_has("--netdev tap,id=n1,ifname=t1: not used by any --device"),
          "%s", g_log);

    T_CASE("duplicate netdev ids are rejected");
    n0->user = n1->user = NULL;
    nets[0] = n0; nets[1] = dup;
    log_clear();
    CHECK_U64(netdev_link(devs, 1, nets, 2), (uint64_t)VMM_ERR_INVAL, "dup id");
    CHECK(log_has("duplicate id 'n0'"), "%s", g_log);

    free(n0); free(n1); free(dup);
}

/* ------------------------------------------------------------------ */
/* TAP：在自己的 user + net namespace 里                               */
/* ------------------------------------------------------------------ */

static bool write_file(const char *path, const char *s)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    bool ok;

    if (fd < 0)
        return false;
    ok = write(fd, s, strlen(s)) == (ssize_t)strlen(s);
    close(fd);
    return ok;
}

static void deadline(struct timespec *t, long sec, long nsec)
{
    clock_gettime(CLOCK_REALTIME, t);
    t->tv_sec += sec;
    t->tv_nsec += nsec;
    if (t->tv_nsec >= 1000000000L) {
        t->tv_sec++;
        t->tv_nsec -= 1000000000L;
    }
}

/* 等不到就没法继续后面的步骤：记一次失败、打印摘要后退出 */
static void wait_for(sem_t *sem, const char *what)
{
    struct timespec t;

    deadline(&t, WAIT_SEC, 0);
    while (sem_timedwait(sem, &t) < 0) {
        if (errno == EINTR)
            continue;
        T_FAILED_("timed out after %d s waiting for %s", WAIT_SEC, what);
        exit(T_SUMMARY());
    }
}

/* 必须在任何线程创建之前调用：多线程进程 unshare(CLONE_NEWUSER) 得到 EINVAL */
static bool enter_netns(char *why, size_t len)
{
    char map[64];
    uid_t uid = getuid();
    gid_t gid = getgid();

    if (unshare(CLONE_NEWUSER | CLONE_NEWNET) < 0) {
        snprintf(why, len, "unshare(CLONE_NEWUSER|CLONE_NEWNET): %s", strerror(errno));
        return false;
    }
    /* 写 gid_map 之前必须先禁用 setgroups（user_namespaces(7)） */
    snprintf(map, sizeof(map), "0 %u 1", (unsigned)uid);
    if (!write_file("/proc/self/setgroups", "deny") ||
        !write_file("/proc/self/uid_map", map)) {
        snprintf(why, len, "writing uid_map: %s", strerror(errno));
        return false;
    }
    snprintf(map, sizeof(map), "0 %u 1", (unsigned)gid);
    if (!write_file("/proc/self/gid_map", map)) {
        snprintf(why, len, "writing gid_map: %s", strerror(errno));
        return false;
    }
    return true;
}

static void set_up(const char *ifname, bool up)
{
    struct ifreq ifr;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_name, ifname);
    if (s < 0 || ioctl(s, SIOCGIFFLAGS, &ifr) < 0) {
        printf("SIOCGIFFLAGS %s: %s\n", ifname, strerror(errno));
        exit(2);
    }
    ifr.ifr_flags = up ? (ifr.ifr_flags | IFF_UP) : (ifr.ifr_flags & ~IFF_UP);
    if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) {
        printf("SIOCSIFFLAGS %s: %s\n", ifname, strerror(errno));
        exit(2);
    }
    close(s);
}

/* 接口另一侧：绑在 TAP 上、只收发测试 EtherType 的 packet socket */
static int open_peer(const char *ifname, int *ifindex)
{
    struct timeval tv = { .tv_sec = WAIT_SEC };
    struct sockaddr_ll sll;
    int s = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(TEST_ETHERTYPE));

    if (s < 0) {
        printf("socket(AF_PACKET): %s\n", strerror(errno));
        exit(2);
    }
    /* 帧没出现在接口上时 recv 不至于永远阻塞，见文件头 */
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(TEST_ETHERTYPE);
    sll.sll_ifindex = (int)if_nametoindex(ifname);
    if (bind(s, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        printf("bind(AF_PACKET %s): %s\n", ifname, strerror(errno));
        exit(2);
    }
    *ifindex = sll.sll_ifindex;
    return s;
}

/* 广播目的地址、本地管理的源地址、测试 EtherType，再加一个标签 */
static size_t make_frame(uint8_t *f, const char *tag)
{
    static const uint8_t hdr[14] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
        TEST_ETHERTYPE >> 8, TEST_ETHERTYPE & 0xff,
    };
    size_t n = strlen(tag);

    memcpy(f, hdr, sizeof(hdr));
    memcpy(f + sizeof(hdr), tag, n);
    /* 补到以太网最短帧 60 字节（不含 FCS），对端 socket 收到的就是这么长 */
    memset(f + sizeof(hdr) + n, 0, 60 - sizeof(hdr) - n);
    return 60;
}

static void peer_send(int s, int ifindex, const char *tag)
{
    struct sockaddr_ll sll;
    uint8_t f[60];
    size_t n = make_frame(f, tag);

    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = ifindex;
    sll.sll_halen = 6;
    memset(sll.sll_addr, 0xff, 6);
    if (sendto(s, f, n, 0, (struct sockaddr *)&sll, sizeof(sll)) != (ssize_t)n) {
        printf("sendto: %s\n", strerror(errno));
        exit(2);
    }
}

/* 假前端：记下交付的测试帧，full 时一律回 NETDEV_RX_FULL */
struct fake_nic {
    pthread_mutex_t lock;
    bool  full;
    int   full_calls;           /* full 期间被调用的次数（任何帧） */
    int   n;                    /* 收下的测试帧 */
    char  tags[8][16];
    sem_t got;                  /* 每收下一个测试帧 post 一次 */
    sem_t refused;              /* 每回一次 FULL post 一次 */
};

static enum netdev_rx_result fake_receive(void *opaque, const uint8_t *f, size_t len)
{
    struct fake_nic *nic = opaque;
    enum netdev_rx_result r = NETDEV_RX_DONE;

    pthread_mutex_lock(&nic->lock);
    if (nic->full) {
        nic->full_calls++;
        r = NETDEV_RX_FULL;
        sem_post(&nic->refused);
    } else if (len >= 20 && f[12] == (TEST_ETHERTYPE >> 8) &&
               f[13] == (TEST_ETHERTYPE & 0xff) && nic->n < 8) {
        snprintf(nic->tags[nic->n++], sizeof(nic->tags[0]), "%.6s",
                 (const char *)f + 14);
        sem_post(&nic->got);
    }
    pthread_mutex_unlock(&nic->lock);
    return r;
}

static void case_tap(void)
{
    struct fake_nic nic;
    struct event_loop loop;
    struct netdev *nd, *busy, *lo;
    struct timespec until;
    uint8_t f[60], got[128];
    struct iovec iov[2];
    int peer, ifindex, i;
    ssize_t n;

    memset(&nic, 0, sizeof(nic));
    pthread_mutex_init(&nic.lock, NULL);
    sem_init(&nic.got, 0, 0);
    sem_init(&nic.refused, 0, 0);

    T_CASE("netdev_open creates the TAP and warns while it is down");
    nd = mk_netdev("tap,id=n0,ifname=" TAP_NAME);
    netdev_set_frontend(nd, fake_receive, &nic);
    CHECK_U64(evl_init(&loop), VMM_OK, "evl_init");
    log_clear();
    CHECK_U64(netdev_open(nd, &loop), VMM_OK, "netdev_open");
    CHECK(if_nametoindex(TAP_NAME) != 0, "interface %s does not exist", TAP_NAME);
    CHECK(log_has("TAP " TAP_NAME " is down"), "no down warning: %s", g_log);
    /* 不关 IPv6 的话 up 之后内核会往接口上发邻居发现，测试帧靠 EtherType
     * 区分，这里关掉只是少些干扰；内核没编 IPv6 时文件不存在，无妨 */
    write_file("/proc/sys/net/ipv6/conf/" TAP_NAME "/disable_ipv6", "1");
    set_up(TAP_NAME, true);
    peer = open_peer(TAP_NAME, &ifindex);

    T_CASE("netdev_send: an iovec frame appears on the interface unchanged");
    make_frame(f, "TX-one");
    iov[0].iov_base = f;      iov[0].iov_len = 14;
    iov[1].iov_base = f + 14; iov[1].iov_len = sizeof(f) - 14;
    CHECK_U64(netdev_send(nd, iov, 2), VMM_OK, "netdev_send");
    n = recv(peer, got, sizeof(got), 0);
    CHECK(n >= 0, "no test frame on the interface within %d s: %s",
          WAIT_SEC, strerror(errno));
    CHECK_U64(n, sizeof(f), "frame length on the interface");
    CHECK(n == (ssize_t)sizeof(f) && !memcmp(got, f, sizeof(f)), "frame bytes differ");
    CHECK_U64(nd->stats.tx_frames, 1, "tx_frames");
    CHECK_U64(nd->stats.tx_bytes, sizeof(f), "tx_bytes");

    T_CASE("frames sent to the interface reach the frontend on the io thread");
    CHECK_U64(evl_start(&loop), VMM_OK, "evl_start");
    peer_send(peer, ifindex, "RX-one");
    wait_for(&nic.got, "RX-one at the frontend");
    CHECK(!strcmp(nic.tags[0], "RX-one"), "got %s", nic.tags[0]);

    T_CASE("a full frontend pauses reading; resume delivers in order");
    pthread_mutex_lock(&nic.lock);
    nic.full = true;
    pthread_mutex_unlock(&nic.lock);
    peer_send(peer, ifindex, "RX-A");
    peer_send(peer, ifindex, "RX-B");
    peer_send(peer, ifindex, "RX-C");
    /* 第一次 FULL，加上 rx_pause 置位后的那次重试 */
    wait_for(&nic.refused, "the first NETDEV_RX_FULL");
    wait_for(&nic.refused, "the retry after rx_blocked is set");
    deadline(&until, 0, 200 * 1000 * 1000);
    CHECK(sem_timedwait(&nic.refused, &until) < 0 && errno == ETIMEDOUT,
          "frontend called again while paused (io thread keeps polling)");
    pthread_mutex_lock(&nic.lock);
    CHECK_U64(nic.full_calls, 2, "calls while full");
    nic.full = false;
    pthread_mutex_unlock(&nic.lock);
    CHECK(__atomic_load_n(&nd->rx_blocked, __ATOMIC_SEQ_CST), "rx_blocked not set");
    netdev_rx_resume(nd);       /* 主线程调用：任意线程都可以 */
    for (i = 0; i < 3; i++)
        wait_for(&nic.got, "RX-A, RX-B, RX-C after netdev_rx_resume");
    CHECK(!strcmp(nic.tags[1], "RX-A") && !strcmp(nic.tags[2], "RX-B") &&
          !strcmp(nic.tags[3], "RX-C"),
          "order after resume: %s %s %s", nic.tags[1], nic.tags[2], nic.tags[3]);
    /* 恢复之后读 fd 的状态也恢复了：新帧照常到达 */
    peer_send(peer, ifindex, "RX-D");
    wait_for(&nic.got, "RX-D after the TAP fd is polled again");
    CHECK(!strcmp(nic.tags[4], "RX-D"), "after resume got %s", nic.tags[4]);

    T_CASE("a second open of the same TAP reports EBUSY");
    busy = mk_netdev("tap,id=n1,ifname=" TAP_NAME);
    netdev_set_frontend(busy, fake_receive, &nic);
    log_clear();
    CHECK_U64(netdev_open(busy, &loop), (uint64_t)VMM_ERR_SYS, "second open");
    CHECK(log_has("another process already has it open"), "%s", g_log);
    CHECK(busy->fd == -1 && busy->wake_fd == -1, "fds left open after failure");

    T_CASE("an existing non-TAP interface reports EINVAL");
    lo = mk_netdev("tap,id=n2,ifname=lo");
    netdev_set_frontend(lo, fake_receive, &nic);
    log_clear();
    CHECK_U64(netdev_open(lo, &loop), (uint64_t)VMM_ERR_SYS, "open lo");
    CHECK(log_has("exists but is not a single-queue TAP"), "%s", g_log);

    evl_stop(&loop);
    CHECK_U64(nd->stats.rx_pauses, 1, "rx_pauses");
    CHECK(nd->stats.rx_frames >= 5, "rx_frames %llu",
          (unsigned long long)nd->stats.rx_frames);

    T_CASE("sending while the interface is down fails with EIO, warned once");
    set_up(TAP_NAME, false);
    log_clear();
    CHECK_U64(netdev_send(nd, iov, 2), (uint64_t)VMM_ERR_SYS, "send while down");
    CHECK_U64(netdev_send(nd, iov, 2), (uint64_t)VMM_ERR_SYS, "send while down");
    CHECK_U64(nd->stats.tx_errors, 2, "tx_errors");
    CHECK(log_has("Input/output error"), "%s", g_log);
    CHECK_U64(log_count("write to TAP"), 1, "failure warnings: %s", g_log);

    netdev_close(nd);
    evl_destroy(&loop);
    close(peer);
    free(nd); free(busy); free(lo);
    sem_destroy(&nic.got);
    sem_destroy(&nic.refused);
}

int main(int argc, char **argv)
{
    char why[256];
    bool netns;

    T_MAIN_BEGIN();
    alarm(HANG_GUARD_SEC);
    if (getenv("NETDEV_TEST_VERBOSE"))
        vmm_log_level = 3;

    /* 先于任何线程（evl_start 会建 I/O 线程） */
    netns = enter_netns(why, sizeof(why));

    case_parse();
    case_link();
    if (netns)
        case_tap();
    else
        printf("  SKIP TAP cases: %s\n", why);
    return T_SUMMARY();
}
