/* src/gdb/gdbstub.c — gdb 远程调试的 KVM 后端，结构和暂停规则见 gdb/gdbstub.h */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/eventfd.h>
#include <sys/socket.h>

#include "vmm.h"
#include "memory.h"
#include "kvm_wrappers.h"
#include "smp.h"
#include "gdb/gdbstub.h"
#include "gdb/rsp.h"
#include "gdb/x86.h"

#define PAGE_SIZE_4K  4096ull

struct gdbstub {
    struct vmm_vm *vm;
    int        listen_fd;
    int        conn_fd;         /* -1：没有 gdb 连着 */
    int        wake_fd;         /* eventfd：停止事件、VM 结束、shutdown */
    pthread_t  thread;
    bool       thread_started;
    /* 单步时额外带上的 KVM_GUESTDBG_* 位：KVM 支持时是 BLOCKIRQ，单步期间
     * 不注入中断，否则开着中断的代码每一步都可能先走进中断处理函数 */
    uint32_t   step_extra;

    /* 下面的字段由 lock 保护，cv 广播一切状态变化 */
    pthread_mutex_t lock;
    pthread_cond_t  cv;
    bool       run[VMM_MAX_VCPUS];      /* 允许这个 vCPU 运行 */
    bool       parked[VMM_MAX_VCPUS];   /* vCPU 线程正停在 park_locked() 里 */
    bool       step[VMM_MAX_VCPUS];     /* 这次是带 SINGLESTEP 放行的 */
    bool       stopping;                /* 停机已发起，事件还没报告给 gdb */
    struct rsp_stop event;              /* 本次停机要报告的事件 */
    bool       shutdown;
    struct gdb_x86_bps bps;
    /* 还给 guest 的 #BP/#DB 个数，VM 结束时打印。guest 自己的 int3 走的是
     * 一条平时看不见的路径（见 gdb_x86_classify()），计数是它确实被走过、
     * guest 也照常跑下去了的证据 */
    unsigned long reinjected_bp, reinjected_db;

    /* 只有 gdbstub 线程访问 */
    struct rsp_session rsp;
};

static void wake_thread(struct gdbstub *g)
{
    uint64_t one = 1;

    /* eventfd 计数只会在溢出 2^64-2 时写失败，不可能发生 */
    if (write(g->wake_fd, &one, sizeof(one)) != sizeof(one))
        vmm_err("gdbstub: eventfd write failed: %s", strerror(errno));
}

/* ------------------------------------------------------------------ */
/* 暂停与放行                                                          */
/* ------------------------------------------------------------------ */

static void kick_locked(struct gdbstub *g, int i)
{
    struct vmm_vcpu *v = &g->vm->vcpus[i];

    __atomic_store_n(&v->run->immediate_exit, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&v->thread_started, __ATOMIC_ACQUIRE) &&
        !pthread_equal(v->thread, pthread_self()))
        pthread_kill(v->thread, SIGUSR1);
}

/* 发起一次停机，ev 是要报告给 gdb 的事件。已有停机在进行时什么都不做，
 * 后来的事件被丢弃，理由见 gdb/gdbstub.h。 */
static void stop_all_locked(struct gdbstub *g, const struct rsp_stop *ev)
{
    int i;

    if (g->stopping)
        return;
    g->stopping = true;
    g->event = *ev;
    for (i = 0; i < g->vm->nr_vcpus; i++) {
        if (g->run[i]) {
            g->run[i] = false;
            kick_locked(g, i);
        }
    }
    wake_thread(g);
}

/* 已经在退出的 vCPU（should_stop）不会再进 guest，也算停稳 */
static bool all_stopped_locked(struct gdbstub *g)
{
    int i;

    for (i = 0; i < g->vm->nr_vcpus; i++)
        if (!g->parked[i] && !g->vm->vcpus[i].should_stop)
            return false;
    return true;
}

static void wait_all_stopped_locked(struct gdbstub *g)
{
    while (!all_stopped_locked(g))
        pthread_cond_wait(&g->cv, &g->lock);
}

static void park_locked(struct gdbstub *g, struct vmm_vcpu *v)
{
    g->parked[v->id] = true;
    pthread_cond_broadcast(&g->cv);
    while (!g->run[v->id] && !v->should_stop)
        pthread_cond_wait(&g->cv, &g->lock);
    g->parked[v->id] = false;
    /* 只有被放行时才清 immediate_exit：VM 要结束时它必须保持为 1，
     * 见 vm_request_stop() */
    if (!v->should_stop)
        __atomic_store_n(&v->run->immediate_exit, 0, __ATOMIC_SEQ_CST);
}

void gdbstub_vcpu_interrupted(struct vmm_vcpu *v)
{
    struct gdbstub *g = v->vm->gdb;

    pthread_mutex_lock(&g->lock);
    if (!g->run[v->id])
        park_locked(g, v);
    pthread_mutex_unlock(&g->lock);
}

static enum rsp_stop_kind event_to_stop(enum gdb_x86_event ev)
{
    switch (ev) {
    case GDB_X86_EV_SWBREAK: return RSP_STOP_SWBREAK;
    case GDB_X86_EV_HWBREAK: return RSP_STOP_HWBREAK;
    default:                 return RSP_STOP_TRAP;
    }
}

static const char *event_str(enum gdb_x86_event ev)
{
    switch (ev) {
    case GDB_X86_EV_SWBREAK:    return "software breakpoint";
    case GDB_X86_EV_HWBREAK:    return "hardware breakpoint";
    case GDB_X86_EV_STEP:       return "single step";
    case GDB_X86_EV_REINJECT_BP: return "guest int3";
    case GDB_X86_EV_REINJECT_DB: return "guest #DB";
    }
    return "?";
}

int gdbstub_vcpu_debug_exit(struct vmm_vcpu *v)
{
    struct gdbstub *g = v->vm->gdb;
    const struct kvm_debug_exit_arch *ex = &v->run->debug.arch;
    enum gdb_x86_event ev;
    struct kvm_guest_debug dbg;
    struct rsp_stop st;
    int r = VMM_OK;

    pthread_mutex_lock(&g->lock);
    ev = gdb_x86_classify(&g->bps, ex, g->step[v->id]);
    vmm_dbg("gdbstub: vCPU %d debug exit: exception %u pc 0x%llx dr6 0x%llx "
            "-> %s", v->id, ex->exception, (unsigned long long)ex->pc,
            (unsigned long long)ex->dr6, event_str(ev));

    if (ev == GDB_X86_EV_REINJECT_BP || ev == GDB_X86_EV_REINJECT_DB) {
        /* 不是我们的事件：带上 INJECT_BP/INJECT_DB 重设一次 guest debug，
         * KVM 把异常排进这个 vCPU，下次进 guest 时投递。#BP 是陷阱类，
         * KVM 投递时把 rip 推进到 int3 之后，guest 的处理函数看到的现场
         * 和没有调试器时一样。 */
        gdb_x86_guest_debug(&g->bps, g->step[v->id], g->step_extra, &dbg);
        dbg.control |= KVM_GUESTDBG_ENABLE |
                       (ev == GDB_X86_EV_REINJECT_BP ? KVM_GUESTDBG_INJECT_BP
                                                     : KVM_GUESTDBG_INJECT_DB);
        r = kvm_set_guest_debug(v->fd, &dbg);
        if (ev == GDB_X86_EV_REINJECT_BP)
            g->reinjected_bp++;
        else
            g->reinjected_db++;
        pthread_mutex_unlock(&g->lock);
        return r;
    }

    st.kind = event_to_stop(ev);
    st.cpu = v->id;
    st.exit_code = 0;
    stop_all_locked(g, &st);
    g->run[v->id] = false;
    park_locked(g, v);
    pthread_mutex_unlock(&g->lock);
    return r;
}

void gdbstub_vm_stopping(struct gdbstub *g)
{
    pthread_mutex_lock(&g->lock);
    pthread_cond_broadcast(&g->cv);
    pthread_mutex_unlock(&g->lock);
    wake_thread(g);
}

/* 按断点表和单步标记给每个暂停中的 vCPU 设 guest debug，再放行 act 里
 * 不是 NONE 的 vCPU。调用时所有 vCPU 都已停稳。 */
static void resume_locked(struct gdbstub *g, const enum rsp_action *act)
{
    struct kvm_guest_debug dbg;
    int i;

    for (i = 0; i < g->vm->nr_vcpus; i++) {
        if (!g->parked[i])
            continue;               /* 已经在退出 */
        g->step[i] = act[i] == RSP_ACT_STEP;
        gdb_x86_guest_debug(&g->bps, g->step[i], g->step_extra, &dbg);
        if (kvm_set_guest_debug(g->vm->vcpus[i].fd, &dbg) != VMM_OK)
            vmm_err("gdbstub: vCPU %d: KVM_SET_GUEST_DEBUG control 0x%x "
                    "failed", i, dbg.control);
    }
    for (i = 0; i < g->vm->nr_vcpus; i++)
        if (g->parked[i] && act[i] != RSP_ACT_NONE)
            g->run[i] = true;
    pthread_cond_broadcast(&g->cv);
}

/* ------------------------------------------------------------------ */
/* target 回调（gdbstub 线程，所有 vCPU 已停稳）                        */
/* ------------------------------------------------------------------ */

static int t_get_regs(void *t, int cpu, struct rsp_regs *r)
{
    struct gdbstub *g = t;
    int fd = g->vm->vcpus[cpu].fd;
    struct kvm_regs kr;
    struct kvm_sregs ks;
    struct kvm_fpu kf;

    if (kvm_get_regs(fd, &kr) != VMM_OK || kvm_get_sregs(fd, &ks) != VMM_OK ||
        kvm_get_fpu(fd, &kf) != VMM_OK)
        return -1;
    gdb_x86_regs_from_kvm(r, &kr, &ks, &kf);
    return 0;
}

static int t_set_regs(void *t, int cpu, const struct rsp_regs *r)
{
    struct gdbstub *g = t;
    int fd = g->vm->vcpus[cpu].fd;
    struct kvm_regs kr;

    if (kvm_get_regs(fd, &kr) != VMM_OK)
        return -1;
    gdb_x86_regs_to_kvm(r, &kr);
    return kvm_set_regs(fd, &kr) == VMM_OK ? 0 : -1;
}

/*
 * 把 [gva, gva+len) 里从 gva 开始、落在同一个 4K 页内的一段翻译成 HVA。
 * 每页单独翻译：相邻的虚拟页可以映射到不相邻的物理页。只接受 RAM：
 * gpa_to_hva 对 MMIO 区间返回失败，gdb 读一个设备寄存器地址不会触发设备
 * 的读副作用。
 */
static size_t map_chunk(struct gdbstub *g, int cpu, uint64_t gva, size_t len,
                        uint64_t *gpa, uint8_t **hva)
{
    size_t n = PAGE_SIZE_4K - (gva & (PAGE_SIZE_4K - 1));
    void *p;

    if (n > len)
        n = len;
    if (kvm_translate(g->vm->vcpus[cpu].fd, gva, gpa) != VMM_OK)
        return 0;
    if (gpa_to_hva(g->vm, *gpa, n, &p) != GPA_OK)
        return 0;
    *hva = p;
    return n;
}

static size_t t_read_mem(void *t, int cpu, uint64_t addr, void *buf, size_t len)
{
    struct gdbstub *g = t;
    uint8_t *out = buf, *hva;
    uint64_t gpa;
    size_t done = 0, n;

    while (done < len) {
        n = map_chunk(g, cpu, addr + done, len - done, &gpa, &hva);
        if (!n)
            break;
        memcpy(out + done, hva, n);
        pthread_mutex_lock(&g->lock);
        gdb_x86_sw_hide(&g->bps, gpa, out + done, n);
        pthread_mutex_unlock(&g->lock);
        done += n;
    }
    return done;
}

static size_t t_write_mem(void *t, int cpu, uint64_t addr, const void *buf,
                          size_t len)
{
    struct gdbstub *g = t;
    const uint8_t *in = buf;
    uint8_t tmp[PAGE_SIZE_4K], *hva;
    uint64_t gpa;
    size_t done = 0, n;

    while (done < len) {
        n = map_chunk(g, cpu, addr + done, len - done, &gpa, &hva);
        if (!n)
            break;
        memcpy(tmp, in + done, n);
        pthread_mutex_lock(&g->lock);
        gdb_x86_sw_shadow_write(&g->bps, gpa, tmp, n);
        pthread_mutex_unlock(&g->lock);
        memcpy(hva, tmp, n);
        done += n;
    }
    return done;
}

/*
 * 软件断点在插入时就写进 guest 内存，guest debug 的 USE_SW_BP 在放行时
 * 才打开。地址翻译不了（例如启动早期内核还没建好 0xffffffff8... 的映射，
 * 只有 VMM 建的恒等映射）时返回错误，gdb 报 "Cannot insert breakpoint"，
 * 这时用 hbreak：硬件断点比较的是线性地址，不需要翻译。
 */
static int t_insert_bp(void *t, int cpu, enum rsp_bp_type type, uint64_t addr)
{
    struct gdbstub *g = t;
    uint8_t *hva;
    uint64_t gpa;
    int r;

    pthread_mutex_lock(&g->lock);
    if (type == RSP_BP_HW) {
        r = gdb_x86_hw_add(&g->bps, addr);
    } else if (gdb_x86_sw_find(&g->bps, addr) >= 0) {
        r = 0;
    } else if (!map_chunk(g, cpu, addr, 1, &gpa, &hva)) {
        r = -EFAULT;
    } else {
        r = gdb_x86_sw_add(&g->bps, addr, gpa, *hva);
        if (r == 0)
            *hva = GDB_X86_INT3;
    }
    pthread_mutex_unlock(&g->lock);
    vmm_dbg("gdbstub: insert %s breakpoint at 0x%llx: %s",
            type == RSP_BP_HW ? "hardware" : "software",
            (unsigned long long)addr, r < 0 ? strerror(-r) : "ok");
    return r < 0 ? r : 0;
}

static int t_remove_bp(void *t, int cpu, enum rsp_bp_type type, uint64_t addr)
{
    struct gdbstub *g = t;
    struct gdb_x86_sw_bp bp;
    void *hva;
    int r;

    (void)cpu;
    pthread_mutex_lock(&g->lock);
    if (type == RSP_BP_HW) {
        r = gdb_x86_hw_del(&g->bps, addr);
    } else {
        r = gdb_x86_sw_del(&g->bps, addr, &bp);
        if (r == 0 && gpa_to_hva(g->vm, bp.gpa, 1, &hva) == GPA_OK)
            *(uint8_t *)hva = bp.orig;
    }
    pthread_mutex_unlock(&g->lock);
    return r;
}

static void t_resume(void *t, const enum rsp_action *act)
{
    struct gdbstub *g = t;

    pthread_mutex_lock(&g->lock);
    resume_locked(g, act);
    pthread_mutex_unlock(&g->lock);
}

static void t_interrupt(void *t)
{
    struct gdbstub *g = t;
    struct rsp_stop st = { RSP_STOP_INTERRUPT, g->rsp.g_cpu, 0 };

    pthread_mutex_lock(&g->lock);
    stop_all_locked(g, &st);
    pthread_mutex_unlock(&g->lock);
}

/* 撤掉所有断点、关掉 guest debug、放行所有 vCPU。gdb 退出（D）和连接
 * 意外断开时都走这里：调试器走了，guest 不能停在那里或者留着 int3。 */
static void t_detach(void *t)
{
    struct gdbstub *g = t;
    enum rsp_action act[RSP_MAX_CPUS];
    struct gdb_x86_sw_bp bp;
    void *hva;
    int i;

    pthread_mutex_lock(&g->lock);
    while (g->bps.nr_sw) {
        if (gdb_x86_sw_del(&g->bps, g->bps.sw[0].gva, &bp) == 0 &&
            gpa_to_hva(g->vm, bp.gpa, 1, &hva) == GPA_OK)
            *(uint8_t *)hva = bp.orig;
    }
    memset(g->bps.hw_used, 0, sizeof(g->bps.hw_used));
    for (i = 0; i < RSP_MAX_CPUS; i++)
        act[i] = RSP_ACT_CONT;
    resume_locked(g, act);
    pthread_mutex_unlock(&g->lock);
    vmm_info("gdbstub: gdb detached, guest resumed");
}

static void t_kill(void *t)
{
    struct gdbstub *g = t;

    vmm_info("gdbstub: gdb killed the guest");
    vm_request_stop(g->vm);
}

static const struct rsp_target_ops target_ops = {
    .get_regs  = t_get_regs,
    .set_regs  = t_set_regs,
    .read_mem  = t_read_mem,
    .write_mem = t_write_mem,
    .insert_bp = t_insert_bp,
    .remove_bp = t_remove_bp,
    .resume    = t_resume,
    .interrupt = t_interrupt,
    .detach    = t_detach,
    .kill      = t_kill,
};

/* ------------------------------------------------------------------ */
/* 连接                                                                */
/* ------------------------------------------------------------------ */

static int conn_write(void *io, const void *buf, size_t len)
{
    struct gdbstub *g = io;
    const char *p = buf;

    while (len) {
        /* MSG_NOSIGNAL：gdb 断开后再写不能让整个 VMM 被 SIGPIPE 杀掉 */
        ssize_t n = send(g->conn_fd, p, len, MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            vmm_warn("gdbstub: send failed: %s", strerror(errno));
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void close_conn(struct gdbstub *g)
{
    close(g->conn_fd);
    g->conn_fd = -1;
    vmm_info("gdbstub: connection closed");
}

/* 让所有 vCPU 停稳。stop_all_locked 记下的事件不报告：调用方处在一个
 * 新会话的开头或连接的末尾，那里不需要停止应答。 */
static void stop_quietly(struct gdbstub *g)
{
    struct rsp_stop st = { RSP_STOP_INTERRUPT, 0, 0 };

    pthread_mutex_lock(&g->lock);
    stop_all_locked(g, &st);
    wait_all_stopped_locked(g);
    g->stopping = false;
    pthread_mutex_unlock(&g->lock);
}

/* 连接意外断开（EOF、读写出错）：等同于 D */
static void drop_conn(struct gdbstub *g)
{
    stop_quietly(g);
    t_detach(g);
    close_conn(g);
}

/* 新连接：附加到一个正在跑的 VM 时先把它停下，gdb 的会话总是从“目标机
 * 已停止”开始 */
static void accept_conn(struct gdbstub *g)
{
    int fd, one = 1;

    fd = accept4(g->listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) {
        if (errno != EINTR && errno != EAGAIN)
            vmm_warn("gdbstub: accept failed: %s", strerror(errno));
        return;
    }
    /* 协议是一问一答的小包，Nagle 会让每个应答多等一个 ACK */
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0)
        vmm_warn("gdbstub: TCP_NODELAY: %s", strerror(errno));
    g->conn_fd = fd;
    stop_quietly(g);
    rsp_session_init(&g->rsp, &target_ops, g, g->vm->nr_vcpus, conn_write, g);
    vmm_info("gdbstub: gdb connected, %d vCPU(s) stopped", g->vm->nr_vcpus);
}

static void conn_input(struct gdbstub *g)
{
    char buf[4096];
    ssize_t n = read(g->conn_fd, buf, sizeof(buf));

    if (n < 0 && errno == EINTR)
        return;
    if (n <= 0) {
        if (n < 0)
            vmm_warn("gdbstub: read failed: %s", strerror(errno));
        drop_conn(g);
        return;
    }
    switch (rsp_feed(&g->rsp, buf, (size_t)n)) {
    case RSP_CONTINUE: break;
    case RSP_CLOSED:   close_conn(g); break;
    case RSP_IO_ERROR: drop_conn(g); break;
    }
}

/* eventfd 被写：有停止事件要报告，VM 在结束，或者要退出线程。
 * 返回 true 表示线程该退出了。 */
static bool handle_wake(struct gdbstub *g)
{
    struct rsp_stop st;
    bool report, ending;

    pthread_mutex_lock(&g->lock);
    if (g->shutdown) {
        pthread_mutex_unlock(&g->lock);
        return true;
    }
    ending = !g->vm->running;
    report = g->stopping && !ending;
    if (report) {
        wait_all_stopped_locked(g);
        st = g->event;
        g->stopping = false;
    }
    pthread_mutex_unlock(&g->lock);

    if (g->conn_fd < 0)
        return false;
    if (ending) {
        /* gdb 在等停止应答时报告 W，它会打印 inferior 已退出；已经停着的
         * 会话直接断开 */
        if (g->rsp.running) {
            st.kind = RSP_STOP_EXITED;
            st.cpu = 0;
            st.exit_code = g->vm->exit_code;
            rsp_report_stop(&g->rsp, &st);
        }
        close_conn(g);
        return false;
    }
    if (report && g->rsp.running) {
        vmm_dbg("gdbstub: all vCPUs stopped, reporting stop on vCPU %d",
                st.cpu);
        if (rsp_report_stop(&g->rsp, &st) == RSP_IO_ERROR)
            drop_conn(g);
    }
    return false;
}

static void *gdbstub_thread(void *arg)
{
    struct gdbstub *g = arg;
    uint64_t cnt;

    pthread_setname_np(pthread_self(), "gdbstub");
    for (;;) {
        struct pollfd pfd[2] = {
            { g->conn_fd >= 0 ? g->conn_fd : g->listen_fd, POLLIN, 0 },
            { g->wake_fd, POLLIN, 0 },
        };

        if (poll(pfd, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            vmm_err("gdbstub: poll failed: %s", strerror(errno));
            break;
        }
        if (pfd[1].revents & POLLIN) {
            if (read(g->wake_fd, &cnt, sizeof(cnt)) < 0 && errno != EAGAIN)
                vmm_warn("gdbstub: eventfd read: %s", strerror(errno));
            if (handle_wake(g))
                break;
        }
        if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (g->conn_fd >= 0)
                conn_input(g);
            else
                accept_conn(g);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

static int open_listener(uint16_t port, uint16_t *bound)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    int fd, one = 1;

    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        vmm_err("gdbstub: socket: %s", strerror(errno));
        return -1;
    }
    /* 上一次运行留下的 TIME_WAIT 连接不能挡住同一个端口 */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
        vmm_warn("gdbstub: SO_REUSEADDR: %s", strerror(errno));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        vmm_err("gdbstub: bind 127.0.0.1:%u: %s", port, strerror(errno));
        goto fail;
    }
    /* 同一时刻只服务一个 gdb，后来的连接在 backlog 里等前一个断开 */
    if (listen(fd, 1) < 0) {
        vmm_err("gdbstub: listen: %s", strerror(errno));
        goto fail;
    }
    if (getsockname(fd, (struct sockaddr *)&sa, &sl) < 0) {
        vmm_err("gdbstub: getsockname: %s", strerror(errno));
        goto fail;
    }
    *bound = ntohs(sa.sin_port);
    return fd;
fail:
    close(fd);
    return -1;
}

int gdbstub_init(struct vmm_vm *vm, uint16_t port)
{
    struct gdbstub *g;
    uint16_t bound;
    int caps, i;

    if (!kvm_check_extension(vm->kvm_fd, KVM_CAP_SET_GUEST_DEBUG)) {
        vmm_err("gdbstub: kernel does not support KVM_CAP_SET_GUEST_DEBUG");
        return VMM_ERR_UNSUPPORTED;
    }

    g = calloc(1, sizeof(*g));
    if (!g)
        return VMM_ERR_NOMEM;
    g->vm = vm;
    g->conn_fd = -1;
    g->wake_fd = -1;
    pthread_mutex_init(&g->lock, NULL);
    pthread_cond_init(&g->cv, NULL);

    /* KVM_CAP_SET_GUEST_DEBUG2 返回 KVM 接受的 KVM_GUESTDBG_* 全集；
     * 老内核没有这个 capability，返回 0 */
    caps = kvm_check_extension(vm->kvm_fd, KVM_CAP_SET_GUEST_DEBUG2);
    if (caps > 0 && (caps & KVM_GUESTDBG_BLOCKIRQ))
        g->step_extra = KVM_GUESTDBG_BLOCKIRQ;
    else
        vmm_warn("gdbstub: KVM lacks KVM_GUESTDBG_BLOCKIRQ; single-stepping "
                 "with interrupts enabled may step into interrupt handlers");

    g->listen_fd = open_listener(port, &bound);
    if (g->listen_fd < 0)
        goto fail;
    g->wake_fd = eventfd(0, EFD_CLOEXEC);
    if (g->wake_fd < 0) {
        vmm_err("gdbstub: eventfd: %s", strerror(errno));
        goto fail;
    }

    /* 所有 vCPU 第一次进 KVM_RUN 就因 immediate_exit 返回并暂停，等 gdb */
    for (i = 0; i < vm->nr_vcpus; i++) {
        g->run[i] = false;
        __atomic_store_n(&vm->vcpus[i].run->immediate_exit, 1,
                         __ATOMIC_SEQ_CST);
    }

    vm->gdb = g;
    if (pthread_create(&g->thread, NULL, gdbstub_thread, g) != 0) {
        vmm_err("gdbstub: pthread_create failed");
        vm->gdb = NULL;
        goto fail;
    }
    g->thread_started = true;
    vmm_info("gdbstub: listening on 127.0.0.1:%u, guest paused until gdb "
             "connects", bound);
    return VMM_OK;

fail:
    if (g->listen_fd >= 0)
        close(g->listen_fd);
    if (g->wake_fd >= 0)
        close(g->wake_fd);
    pthread_cond_destroy(&g->cv);
    pthread_mutex_destroy(&g->lock);
    free(g);
    return VMM_ERR_SYS;
}

void gdbstub_destroy(struct vmm_vm *vm)
{
    struct gdbstub *g = vm->gdb;

    if (!g)
        return;
    pthread_mutex_lock(&g->lock);
    g->shutdown = true;
    pthread_mutex_unlock(&g->lock);
    wake_thread(g);
    if (g->thread_started)
        pthread_join(g->thread, NULL);
    vmm_info("gdbstub: passed %lu guest #BP and %lu guest #DB back to the "
             "guest", g->reinjected_bp, g->reinjected_db);
    if (g->conn_fd >= 0)
        close(g->conn_fd);
    close(g->listen_fd);
    close(g->wake_fd);
    pthread_cond_destroy(&g->cv);
    pthread_mutex_destroy(&g->lock);
    vm->gdb = NULL;
    free(g);
}
