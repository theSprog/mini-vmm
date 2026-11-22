/* src/devices/split_irqchip.c — split irqchip 的主板接线，设计见 include/devices/split_irqchip.h */
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>

#include "vmm.h"
#include "smp.h"
#include "kvm_wrappers.h"
#include "reset.h"
#include "io/event_loop.h"
#include "devices/irqchip.h"
#include "devices/split_irqchip.h"

#define ISA_IRQS 16

/* ------------------------------------------------------------------ */
/* 芯片回调（都在持 lock 时被调用）                                     */
/* ------------------------------------------------------------------ */

/* KVM_SIGNAL_MSI 的返回值含义见 irqchip.c irqchip_signal_msi */
static int ioapic_deliver(void *opaque, uint64_t addr, uint32_t data)
{
    struct split_irqchip *s = opaque;
    struct kvm_msi msi;
    int r;

    memset(&msi, 0, sizeof(msi));
    msi.address_lo = (uint32_t)addr;
    msi.address_hi = (uint32_t)(addr >> 32);
    msi.data       = data;
    r = ioctl(s->vm->vm_fd, KVM_SIGNAL_MSI, &msi);
    if (r > 0)
        return r;
    if (r == 0 || errno == EPERM) {
        vmm_dbg("split-irqchip: IOAPIC message addr=0x%llx data=0x%x "
                "accepted by no LAPIC", (unsigned long long)addr, data);
        return 0;
    }
    vmm_err("split-irqchip: ioctl(KVM_SIGNAL_MSI addr=0x%llx data=0x%x) "
            "failed: %s", (unsigned long long)addr, data, strerror(errno));
    return -1;
}

static void route_entry(const struct split_irqchip *s, int pin,
                        struct kvm_irq_routing_entry *e)
{
    uint64_t addr;
    uint32_t data;

    uint64_t rte = s->ioapic.redtbl[pin];

    ioapic_rte_msi(rte, &addr, &data);
    /* reinject 要知道 IRQ0 什么时候被确认。PIC 路径看 ISR，IOAPIC 路径只能
     * 靠 EOI 退出，而 KVM 只为 trigger 位为 level 的路由退出。所以 PIT 的
     * 引脚在路由里标成 level，投递仍走 KVM_SIGNAL_MSI 的 edge 消息，LAPIC
     * 的 TMR 跟消息走，guest 看到的还是边沿中断。KVM 内核 IOAPIC 对挂了确认
     * 通知的边沿引脚做的是同一件事（ioapic.c kvm_ioapic_scan_entry 的
     * kvm_irq_has_notifier 条件）。只对 Fixed/LowPri 标：别的投递方式没有
     * 向量可言。
     * 这依赖 guest 的 EOI 对 ioapic_handled_vectors 里的向量退出。Intel
     * APICv 按这张表装 EOI-exit bitmap，成立；AMD AVIC 对 TMR=0 的 EOI 由
     * 硬件完成、不退出（svm/avic.c is_avic_unaccelerated_access_trap），
     * KVM 内核 PIT 为此在补发时禁用 APICv（i8254.c kvm_pit_set_reinject 的
     * APICV_INHIBIT_REASON_PIT_REINJ），用户态补发时 KVM 不知道、不会禁用。
     * 所以 kvm_amd avic=1 的 host 上 IOAPIC 路径收不到确认，补发卡在第一个
     * tick 之后；avic=0（6.6 默认）的 host 上 split-irqchip 套件的补发用例
     * 能挡住这条路径的回归，avic=1 的组合要在那类 host 上另行验证 */
    if (pin == 0 && s->pit_reinject &&
        IOAPIC_RTE_DELMOD(rte) <= IOAPIC_DELMOD_LOWEST)
        data |= 1u << 15;
    memset(e, 0, sizeof(*e));
    e->gsi  = (uint32_t)pin;
    e->type = KVM_IRQ_ROUTING_MSI;
    e->u.msi.address_lo = (uint32_t)addr;
    e->u.msi.address_hi = (uint32_t)(addr >> 32);
    e->u.msi.data       = data;
}

/* 整表提交：KVM_SET_GSI_ROUTING 总是替换全部路由 */
static int routes_commit(struct split_irqchip *s)
{
    struct kvm_irq_routing *t;
    size_t sz = sizeof(*t) + sizeof(s->routes);
    int r;

    t = calloc(1, sz);
    if (!t)
        return VMM_ERR_NOMEM;
    t->nr = IOAPIC_NUM_PINS;
    memcpy(t->entries, s->routes, sizeof(s->routes));
    r = ioctl(s->vm->vm_fd, KVM_SET_GSI_ROUTING, t);
    free(t);
    if (r < 0) {
        vmm_err("split-irqchip: ioctl(KVM_SET_GSI_ROUTING) failed: %s",
                strerror(errno));
        return VMM_ERR_SYS;
    }
    s->route_updates++;
    return VMM_OK;
}

static void ioapic_rte_changed(void *opaque, int pin)
{
    struct split_irqchip *s = opaque;
    struct kvm_irq_routing_entry e;

    route_entry(s, pin, &e);
    if (!memcmp(&e, &s->routes[pin], sizeof(e)))
        return;
    s->routes[pin] = e;
    routes_commit(s);
}

static void pic_int_changed(void *opaque, int level)
{
    struct split_irqchip *s = opaque;
    struct vmm_vcpu *bsp = &s->vm->vcpus[0];

    __atomic_store_n(&s->pic_int, level, __ATOMIC_SEQ_CST);
    /* vCPU 0 可能睡在 guest 的 HLT 里，踢出来让它在下次进 KVM_RUN 之前注入。
     * vCPU 0 自己的线程拉高时不用踢：它回到 run loop 顶部就会看到 */
    if (level && !(__atomic_load_n(&bsp->thread_started, __ATOMIC_ACQUIRE) &&
                   pthread_equal(bsp->thread, pthread_self())))
        vcpu_kick(bsp);
}

static uint64_t monotonic_ns(void *opaque)
{
    struct timespec ts;

    (void)opaque;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const struct ioapic_ops ioapic_ops = {
    .deliver     = ioapic_deliver,
    .rte_changed = ioapic_rte_changed,
};
static const struct i8259_ops pic_ops = { .int_changed = pic_int_changed };
static const struct i8254_ops pit_ops = { .now_ns = monotonic_ns };

/* ------------------------------------------------------------------ */
/* 中断线和 PIT                                                         */
/* ------------------------------------------------------------------ */

static void set_line_locked(struct split_irqchip *s, int gsi, int level)
{
    if (gsi < ISA_IRQS)
        i8259_set_irq(&s->pic, gsi, level);
    ioapic_set_irq(&s->ioapic, gsi, level);
}

/* irqchip_set_irq 在 split 模式下的实现，调用方不能持有 lock */
static int split_set_irq(struct vmm_vm *vm, int gsi, int level)
{
    struct split_irqchip *s = vm->split;

    if (gsi < 0 || gsi >= IOAPIC_NUM_PINS) {
        vmm_err("split-irqchip: GSI %d is outside the IOAPIC's %d pins",
                gsi, IOAPIC_NUM_PINS);
        return VMM_OK;
    }
    pthread_mutex_lock(&s->lock);
    set_line_locked(s, gsi, level);
    pthread_mutex_unlock(&s->lock);
    return VMM_OK;
}

static void timer_arm(struct split_irqchip *s, uint64_t when)
{
    struct itimerspec its;

    if (s->timer_fd < 0 || when == s->timer_armed)
        return;
    memset(&its, 0, sizeof(its));
    if (when != I8254_NO_EVENT) {
        /* it_value 全 0 表示停止定时器，时刻 0 不会出现（单调时钟从开机计） */
        its.it_value.tv_sec  = (time_t)(when / 1000000000ull);
        its.it_value.tv_nsec = (long)(when % 1000000000ull);
    }
    if (timerfd_settime(s->timer_fd, TFD_TIMER_ABSTIME, &its, NULL) < 0) {
        vmm_err("split-irqchip: timerfd_settime(PIT) failed: %s",
                strerror(errno));
        return;
    }
    s->timer_armed = when;
}

/* IRQ0 上的一个 tick：先拉低再拉高，之后保持高电平，正是 mode 2/4 下
 * OUT 的形状（只低一个时钟）。
 *
 * 不能像 KVM 内核 PIT 那样拉高再拉低（i8254.c pit_do_work）：KVM 的 PIC
 * 在边沿模式下把 IRR 锁住，线放低也不清；8259A 的边沿模式却要求 IR 保持
 * 高电平直到第一个 INTA（datasheet 图 9，i8259.c 照它实现 IRR = line &
 * armed），刚拉高就放低，PIC 还没被 INTA 这个请求就没了，noapic 下 guest
 * 收不到一个 tick。IOAPIC 的边沿引脚只认 0→1，两种形状对它一样。
 *
 * OUT 的电平本身也不接到线上：reinject 积压期间 OUT 照样上升，线若跟着
 * OUT 走，这个上升就绕过补发直接在引脚上成了一个边沿 */
static void pit_pulse(struct split_irqchip *s)
{
    s->pit_irqs++;
    set_line_locked(s, 0, 0);
    set_line_locked(s, 0, 1);
}

/* reinject：上一个 tick 已确认且有积压时送下一个 */
static void pit_reinject_kick(struct split_irqchip *s)
{
    if (s->pit_backlog && s->pit_irq_ack) {
        s->pit_irq_ack = 0;
        s->pit_backlog--;
        pit_pulse(s);
    }
}

/* 积压清零、视为已确认。KVM 在通道 0 重新装数（create_pit_timer）、IRQ0
 * 被解除屏蔽（pit_mask_notifer）和复位时这样做：屏蔽期间送出的 tick 在
 * IOAPIC 上直接丢掉，永远等不到确认，不清的话解除屏蔽后 PIT 就哑了 */
static void pit_reinject_reset(struct split_irqchip *s)
{
    s->pit_dropped += s->pit_backlog;
    s->pit_backlog = 0;
    s->pit_irq_ack = 1;
}

static void pit_acked(struct split_irqchip *s)
{
    if (!s->pit_reinject)
        return;
    s->pit_acks++;
    s->pit_irq_ack = 1;
    pit_reinject_kick(s);
}

/* 每次可能改动 PIC 主片或 IOAPIC 状态之后调用：IRQ0 的 ISR 位被清掉是
 * PIC 路径上的确认（KVM 在 pic_clear_isr 里发确认通知，i8259.c），任一边
 * IRQ0 从屏蔽变成不屏蔽要清积压（KVM 的 mask notifier 两边各自触发） */
static void pit_watch(struct split_irqchip *s)
{
    uint8_t isr0 = s->pic.chip[0].isr & 1;
    uint8_t pic_masked = s->pic.chip[0].imr & 1;
    uint8_t io_masked = !!(s->ioapic.redtbl[0] & IOAPIC_RTE_MASK);

    if (s->pit_reinject) {
        if (s->pit_seen_isr0 && !isr0)
            pit_acked(s);
        if ((s->pit_seen_pic_masked && !pic_masked) ||
            (s->pit_seen_io_masked && !io_masked))
            pit_reinject_reset(s);
    }
    s->pit_seen_isr0 = isr0;
    s->pit_seen_pic_masked = pic_masked;
    s->pit_seen_io_masked = io_masked;
}

/* 把通道 0 从上次到现在的 OUT 上升沿交给补发策略，再按新状态重设定时器 */
static void pit_service(struct split_irqchip *s)
{
    int level;
    uint64_t edges = i8254_poll(&s->pit, 0, &level);

    s->pit_ticks += edges;
    if (s->pit_reinject) {
        s->pit_backlog += edges;
        pit_reinject_kick(s);
    } else if (edges) {
        s->pit_coalesced += edges - 1;
        pit_pulse(s);
    }
    timer_arm(s, i8254_next_rise_ns(&s->pit, 0));
}

static void pit_timer_cb(void *opaque, int fd, uint32_t events)
{
    struct split_irqchip *s = opaque;
    uint64_t expirations;

    (void)events;
    /* 水平触发的 epoll：不读掉计数就会一直报。EAGAIN 是定时器在读之前被
     * 别的线程重设过，计数已清零，照样按当前状态处理 */
    if (read(fd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
        vmm_err("split-irqchip: read(PIT timerfd) failed: %s", strerror(errno));
    pthread_mutex_lock(&s->lock);
    s->timer_armed = I8254_NO_EVENT;    /* 一次性定时器，到期后已停 */
    pit_service(s);
    pthread_mutex_unlock(&s->lock);
}

/* ------------------------------------------------------------------ */
/* PIO/MMIO 包装                                                       */
/* ------------------------------------------------------------------ */

static int port_read(void *opaque, uint64_t off, uint32_t size, void *data)
{
    struct split_port *p = opaque;
    int r;

    pthread_mutex_lock(&p->s->lock);
    r = p->read(p->chip, off, size, data);
    /* 读 PIT 也可能改变状态（读回锁存、0x61 的 OUT2 采样会先同步计数器），
     * 但不会产生新的通道 0 边沿；只有写需要重算 */
    pthread_mutex_unlock(&p->s->lock);
    return r;
}

static int port_write(void *opaque, uint64_t off, uint32_t size,
                      const void *data)
{
    struct split_port *p = opaque;
    int r;

    pthread_mutex_lock(&p->s->lock);
    /* 给通道 0 装数：先把旧计数下已经到期的 tick 按旧状态交出去，再清
     * 积压，和 KVM create_pit_timer 里的 kvm_pit_reset_reinject 对应 */
    if (p->kind == PORT_PIT && off == 0 && p->s->pit_reinject) {
        pit_service(p->s);
        pit_reinject_reset(p->s);
    }
    r = p->write(p->chip, off, size, data);
    if (p->kind == PORT_PIT || p->kind == PORT_NMISC)
        pit_service(p->s);
    else if (p->kind == PORT_PIC_MASTER || p->kind == PORT_IOAPIC)
        pit_watch(p->s);
    pthread_mutex_unlock(&p->s->lock);
    return r;
}

static int add_port(struct split_irqchip *s, int idx, const char *name,
                    bool mmio, uint64_t base, uint64_t len, void *chip,
                    int (*rd)(void *, uint64_t, uint32_t, void *),
                    int (*wr)(void *, uint64_t, uint32_t, const void *),
                    enum split_port_kind kind)
{
    struct split_port *p = &s->ports[idx];
    struct vmm_io_dev dev;

    p->s = s;
    p->chip = chip;
    p->read = rd;
    p->write = wr;
    p->kind = kind;

    memset(&dev, 0, sizeof(dev));
    dev.name   = name;
    dev.base   = base;
    dev.len    = len;
    dev.opaque = p;
    dev.read   = port_read;
    dev.write  = port_write;
    return mmio ? vmm_register_mmio(s->vm, &dev) : vmm_register_pio(s->vm, &dev);
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

/* 复位：三颗芯片回到上电状态，引脚电平清零（设备随后复位，自己记的电平
 * 也回到 0），IOAPIC 路由跟着 RTE 复位值重新提交，PIT 定时器停下 */
static void split_irqchip_reset(void *opaque)
{
    struct split_irqchip *s = opaque;
    int pin;

    pthread_mutex_lock(&s->lock);
    i8259_reset(&s->pic);
    ioapic_reset(&s->ioapic);
    i8254_reset(&s->pit);
    __atomic_store_n(&s->pic_int, 0, __ATOMIC_SEQ_CST);
    for (pin = 0; pin < IOAPIC_NUM_PINS; pin++)
        route_entry(s, pin, &s->routes[pin]);
    routes_commit(s);
    pit_reinject_reset(s);
    pit_watch(s);
    pit_service(s);
    pthread_mutex_unlock(&s->lock);
}

/* 开 KVM_CAP_SPLIT_IRQCHIP，初始化三颗芯片并提交初始路由，置 vm->split */
static int split_create(struct vmm_vm *vm)
{
    struct split_irqchip *s;
    struct kvm_enable_cap cap;
    int pin;

    if (!kvm_check_extension(vm->kvm_fd, KVM_CAP_SPLIT_IRQCHIP)) {
        vmm_err("kernel does not support KVM_CAP_SPLIT_IRQCHIP");
        return VMM_ERR_UNSUPPORTED;
    }
    if (!kvm_check_extension(vm->kvm_fd, KVM_CAP_SIGNAL_MSI) ||
        !kvm_check_extension(vm->kvm_fd, KVM_CAP_IRQ_ROUTING)) {
        vmm_err("kernel does not support KVM_CAP_SIGNAL_MSI/KVM_CAP_IRQ_ROUTING");
        return VMM_ERR_UNSUPPORTED;
    }

    memset(&cap, 0, sizeof(cap));
    cap.cap = KVM_CAP_SPLIT_IRQCHIP;
    cap.args[0] = 16;   /* ISA IRQ 0-15 */
    if (ioctl(vm->vm_fd, KVM_ENABLE_CAP, &cap) < 0) {
        vmm_err("ioctl(KVM_ENABLE_CAP KVM_CAP_SPLIT_IRQCHIP) failed: %s",
                strerror(errno));
        return VMM_ERR_SYS;
    }

    s = calloc(1, sizeof(*s));
    if (!s)
        return VMM_ERR_NOMEM;
    s->vm = vm;
    s->timer_fd = -1;
    s->timer_armed = I8254_NO_EVENT;
    s->pit_reinject = vm->cfg.pit_reinject;
    s->pit_irq_ack = 1;
    pthread_mutex_init(&s->lock, NULL);
    ioapic_init(&s->ioapic, &ioapic_ops, s);
    i8259_init(&s->pic, &pic_ops, s);
    i8254_init(&s->pit, &pit_ops, s);
    pit_watch(s);

    /* split 模式下 KVM 起始的路由表是空的（kvm_setup_empty_irq_routing），
     * 24 条要自己提交 */
    for (pin = 0; pin < IOAPIC_NUM_PINS; pin++)
        route_entry(s, pin, &s->routes[pin]);
    if (routes_commit(s) != VMM_OK) {
        pthread_mutex_destroy(&s->lock);
        free(s);
        return VMM_ERR_SYS;
    }

    vm->split = s;
    vmm_info("split irqchip: LAPIC in KVM, PIC + IOAPIC + PIT in user space "
             "(reinject %s)", s->pit_reinject ? "on" : "off");
    return VMM_OK;
}

/* 注册 PIO/MMIO、PIT 定时器（I/O 线程）和复位回调 */
static int split_attach(struct vmm_vm *vm, struct event_loop *loop)
{
    struct split_irqchip *s = vm->split;
    int r;

    r = vmm_register_reset(vm, "split-irqchip", split_irqchip_reset, s);
    if (r == VMM_OK)
        r = add_port(s, 0, "pic-master", false, I8259_MASTER_BASE, 2,
                     &s->pic.chip[0], i8259_pio_read, i8259_pio_write,
                     PORT_PIC_MASTER);
    if (r == VMM_OK)
        r = add_port(s, 1, "pic-slave", false, I8259_SLAVE_BASE, 2,
                     &s->pic.chip[1], i8259_pio_read, i8259_pio_write,
                     PORT_PLAIN);
    if (r == VMM_OK)
        r = add_port(s, 2, "elcr", false, I8259_ELCR_BASE, 2,
                     &s->pic, i8259_elcr_read, i8259_elcr_write, PORT_PLAIN);
    if (r == VMM_OK)
        r = add_port(s, 3, "pit", false, I8254_BASE, 4,
                     &s->pit, i8254_pio_read, i8254_pio_write, PORT_PIT);
    if (r == VMM_OK)
        r = add_port(s, 4, "nmisc", false, I8254_NMISC_PORT, 1,
                     &s->pit, i8254_nmisc_read, i8254_nmisc_write, PORT_NMISC);
    if (r == VMM_OK)
        r = add_port(s, 5, "ioapic", true, IOAPIC_BASE, IOAPIC_MMIO_SIZE,
                     &s->ioapic, ioapic_mmio_read, ioapic_mmio_write,
                     PORT_IOAPIC);
    if (r != VMM_OK)
        return r;

    s->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (s->timer_fd < 0) {
        vmm_err("split-irqchip: timerfd_create failed: %s", strerror(errno));
        return VMM_ERR_SYS;
    }
    return evl_add(loop, s->timer_fd, EPOLLIN, pit_timer_cb, s);
}

/* PIC 有中断时按就绪状态注入，或者请求中断窗口。PIC 的输出只接 vCPU 0
 * 的 LINT0，其他 vCPU 直接返回 */
static int split_pre_run(struct vmm_vcpu *vcpu)
{
    struct split_irqchip *s = vcpu->vm->split;
    struct kvm_run *run = vcpu->run;
    struct kvm_interrupt irq;
    uint64_t acked0;
    int pending;

    if (vcpu->id != 0)
        return VMM_OK;
    if (!__atomic_load_n(&s->pic_int, __ATOMIC_SEQ_CST)) {
        run->request_interrupt_window = 0;
        return VMM_OK;
    }
    if (!run->ready_for_interrupt_injection) {
        run->request_interrupt_window = 1;
        return VMM_OK;
    }

    pthread_mutex_lock(&s->lock);
    /* 拿锁之前 INT 可能已经被 guest 屏蔽或别的线程撤掉 */
    if (!s->pic.int_out) {
        pthread_mutex_unlock(&s->lock);
        run->request_interrupt_window = 0;
        return VMM_OK;
    }
    acked0 = s->pic.acked[0];
    irq.irq = i8259_inta(&s->pic);
    /* AEOI 下 INTA 不置 ISR，交出 IRQ0 本身就是确认；否则 ISR 位刚置上，
     * 记下来等 guest 的 EOI 清掉它 */
    if (s->pic.acked[0] != acked0 && !(s->pic.chip[0].isr & 1))
        pit_acked(s);
    pit_watch(s);
    if (ioctl(vcpu->fd, KVM_INTERRUPT, &irq) < 0) {
        /* EEXIST：槽位已满，说明就绪标志和 KVM 的状态不一致，继续下去
         * 这个向量就丢了 */
        vmm_err("vCPU %d: ioctl(KVM_INTERRUPT vector=0x%x) failed: %s",
                vcpu->id, irq.irq, strerror(errno));
        pthread_mutex_unlock(&s->lock);
        return VMM_ERR_SYS;
    }
    s->extint_injected++;
    pending = s->pic.int_out;
    pthread_mutex_unlock(&s->lock);
    /* 还有别的 IRQ 在等：这一个被 guest 接受、槽位空出来时再退出 */
    run->request_interrupt_window = pending ? 1 : 0;
    return VMM_OK;
}

static int split_eoi(struct vmm_vcpu *vcpu, uint8_t vector)
{
    struct split_irqchip *s = vcpu->vm->split;

    pthread_mutex_lock(&s->lock);
    s->eoi_exits++;
    ioapic_eoi(&s->ioapic, vector);
    /* 和 KVM 内核 IOAPIC 一样按向量认引脚，不看触发方式
     * （ioapic.c kvm_ioapic_update_eoi_one 对每个向量相符的引脚发确认） */
    if (IOAPIC_RTE_VECTOR(s->ioapic.redtbl[0]) == vector)
        pit_acked(s);
    pthread_mutex_unlock(&s->lock);
    return VMM_OK;
}

/* 补发策略、PIT/PIC/IOAPIC 计数 */
static void split_report(struct vmm_vm *vm, FILE *f)
{
    struct split_irqchip *s = vm->split;
    int irq;

    pthread_mutex_lock(&s->lock);
    fprintf(f, "split irqchip: LAPIC in KVM, PIC + IOAPIC + PIT in user space\n");
    fprintf(f, "pit: reinject %s, ticks %llu, irqs %llu, coalesced %llu, "
            "acks %llu, backlog %llu, dropped %llu\n",
            s->pit_reinject ? "on" : "off",
            (unsigned long long)s->pit_ticks,
            (unsigned long long)s->pit_irqs,
            (unsigned long long)s->pit_coalesced,
            (unsigned long long)s->pit_acks,
            (unsigned long long)s->pit_backlog,
            (unsigned long long)s->pit_dropped);
    fprintf(f, "kvm: extint injected %llu, ioapic eoi exits %llu, "
            "route updates %llu\n",
            (unsigned long long)s->extint_injected,
            (unsigned long long)s->eoi_exits,
            (unsigned long long)s->route_updates);
    fprintf(f, "pic acked:");
    for (irq = 0; irq < 16; irq++)
        fprintf(f, " %llu", (unsigned long long)s->pic.acked[irq]);
    fprintf(f, "\nioapic delivered:");
    for (irq = 0; irq < IOAPIC_NUM_PINS; irq++)
        fprintf(f, " %llu", (unsigned long long)s->ioapic.delivered[irq]);
    fprintf(f, "\n");
    pthread_mutex_unlock(&s->lock);
}

static void split_destroy(struct vmm_vm *vm)
{
    struct split_irqchip *s = vm->split;

    if (!s)
        return;
    if (s->timer_fd >= 0)
        close(s->timer_fd);
    pthread_mutex_destroy(&s->lock);
    free(s);
    vm->split = NULL;
}

const struct irqchip_ops irqchip_split_ops = {
    .kvm_owns_chips = false,
    .create         = split_create,
    .attach         = split_attach,
    .set_irq        = split_set_irq,
    .pre_run        = split_pre_run,
    .eoi            = split_eoi,
    .report         = split_report,
    .destroy        = split_destroy,
};
