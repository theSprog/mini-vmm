/* include/devices/split_irqchip.h — split irqchip：LAPIC 在 KVM，PIC/IOAPIC/PIT 在用户态
 *
 * --kernel-irqchip split 时，KVM 只模拟每个 vCPU 的 LAPIC，主板上的 8259A
 * 一对、82093AA IOAPIC 和 8254 PIT（含 PIIX4 的端口 0x61）由本模块组装在
 * 用户态。芯片模型本身在 ioapic.c、i8259.c、i8254.c，它们不认识 KVM；这里
 * 是“主板”：接线、锁、和 KVM 之间的四条通道。
 *
 *   设备 ──irqchip_set_irq(gsi)──▶ ISA IRQ 0-15 同时接 PIC 和 IOAPIC 同号引脚，
 *                                  GSI 16-23 只接 IOAPIC（与 MADT/MP table
 *                                  的恒等映射一致，表里没有 ISO）
 *   IOAPIC ──KVM_SIGNAL_MSI──▶ LAPIC         RTE 按 SDM 13.11 换算成 MSI 消息
 *   LAPIC ──KVM_EXIT_IOAPIC_EOI──▶ IOAPIC    level 中断的 EOI 回到用户态
 *   PIC INT ──KVM_INTERRUPT──▶ vCPU 0 LINT0  ExtINT，交出向量即 INTA
 *
 * KVM 侧的约定（Documentation/virt/kvm/api.rst 的 KVM_CAP_SPLIT_IRQCHIP、
 * KVM_INTERRUPT、KVM_EXIT_IOAPIC_EOI；host 6.6 arch/x86/kvm）：
 *   - KVM_ENABLE_CAP(KVM_CAP_SPLIT_IRQCHIP) 必须在创建 vCPU 之前，args[0] 是
 *     保留给 IOAPIC 的 GSI 路由条数，这里是 24。
 *   - KVM 只对“前 args[0] 条路由里、类型为 MSI 且 trigger 位为 level、目标
 *     包含本 vCPU”的向量在 EOI 时退出（irq_comm.c kvm_scan_ioapic_routes）。
 *     所以 24 条 RTE 必须镜像进 KVM_SET_GSI_ROUTING，否则 level 中断的 EOI
 *     不出来，remote IRR 永远不清，这根线投递一次之后就再也没有中断。
 *     镜像不看 mask 位：guest 可能在中断送达之后、EOI 之前屏蔽 RTE（Linux 的
 *     mask + EOI 序列），这时 EOI 仍必须回来清 remote IRR；被屏蔽的 level
 *     引脚多出的 EOI 退出无害。
 *   - KVM_INTERRUPT 只在 kvm_run->ready_for_interrupt_injection 为 1 时调用：
 *     KVM 只有一个 pending_external_vector 槽位，满了返回 EEXIST；而且就绪还
 *     意味着 guest 可以立刻接受中断（IF=1、LINT0 收 ExtINT、没有待重注的
 *     事件，x86.c kvm_vcpu_ready_for_interrupt_injection），INTA 在这里提前
 *     做不会和 guest 之后对 PIC 的改动错开。不就绪时置
 *     request_interrupt_window，KVM 在就绪时以 KVM_EXIT_IRQ_WINDOW_OPEN 返回。
 *
 * ExtINT 只送 vCPU 0：Linux 只在 BSP 上把 LVT0 设成未屏蔽的 ExtINT
 * （arch/x86/kernel/apic/apic.c setup_local_APIC 的 `!cpu` 条件），AP 的
 * LINT0 一直屏蔽，送过去 KVM 也不会收（kvm_apic_accept_pic_intr）。
 *
 * PIT 通道 0 的 OUT 接 IRQ0。下一次 OUT 上升沿的时刻由 i8254_next_rise_ns
 * 给出，用 CLOCK_MONOTONIC 的 timerfd 在 I/O 线程上等它；每次 guest 写 PIT
 * 之后重新计算并重设。每个上升沿是一个 tick，IRQ0 上以一个脉冲送出。
 *
 * tick 来得比 guest 处理得快（host 过载、vCPU 没被调度）时有两种补发策略，
 * 由 --pit-reinject 选，和 KVM 内核 PIT 的 KVM_REINJECT_CONTROL 对应：
 *   on   送出一个 tick 后等 guest 确认，确认之前到期的 tick 记进 backlog，
 *        每次确认送一个，guest 最终收到的 tick 数等于到期的 tick 数。确认
 *        是 PIC 清掉 IRQ0 的 ISR 位（EOI 或 AEOI 下的 INTA），或 IOAPIC 上
 *        pin 0 的向量收到 EOI。后者要 KVM 退出，所以 pin 0 的路由标成
 *        level（见 route_entry）。通道 0 重新装数、IRQ0 解除屏蔽、复位时
 *        积压清零（KVM 在 create_pit_timer、pit_mask_notifer、kvm_pit_reset
 *        里调 kvm_pit_reset_reinject；它另在运行中打开补发时清零，这里
 *        策略启动时就定了，没有这个时机）。
 *   off  一次唤醒里攒下多个上升沿时只送一个脉冲，多出的计入 pit_coalesced；
 *        前一个还没被处理时 PIC/IOAPIC 的 IRR 本来也会把它们合并。guest
 *        看到的是丢 tick，不是一串背靠背的中断。
 * 两种策略的取舍和测得的漂移见 blog 的 split irqchip 系列。
 *
 * 加锁：三颗芯片共用 lock，它是最内层的锁，顺序是
 *   vm->io_lock → 设备自己的锁（serial8250.lock）→ split_irqchip.lock → KVM ioctl
 * 串口在 I/O 线程上只持自己的锁就拉中断线，所以芯片不能挂在 io_lock 下。
 * 持这把锁时调用的 KVM ioctl（KVM_SIGNAL_MSI、KVM_SET_GSI_ROUTING、
 * KVM_INTERRUPT）都不回头拿 VMM 的锁。
 */
#ifndef DEVICES_SPLIT_IRQCHIP_H
#define DEVICES_SPLIT_IRQCHIP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <linux/kvm.h>

#include "devices/ioapic.h"
#include "devices/i8259.h"
#include "devices/i8254.h"

struct vmm_vm;
struct vmm_vcpu;
struct event_loop;

/* 写完之后包装层要做的事 */
enum split_port_kind {
    PORT_PLAIN,
    PORT_PIT,           /* 重算通道 0 的边沿和定时器；写通道 0 先清 backlog */
    PORT_NMISC,         /* 重算通道 0（0x61 与通道 0 无关，重算是无害的） */
    PORT_PIC_MASTER,    /* 看 IRQ0 的 ISR/IMR 有没有变：reinject 的确认和解除屏蔽 */
    PORT_IOAPIC,        /* 看 pin 0 的 mask 有没有解除 */
};

/* PIO/MMIO 表项的包装：先拿 split_irqchip.lock 再调芯片的回调 */
struct split_port {
    struct split_irqchip *s;
    void *chip;                 /* 芯片回调的 opaque */
    int (*read)(void *opaque, uint64_t off, uint32_t size, void *data);
    int (*write)(void *opaque, uint64_t off, uint32_t size, const void *data);
    enum split_port_kind kind;
};

#define SPLIT_NR_PORTS 6        /* PIC 主、从，ELCR，PIT，0x61，IOAPIC */

struct split_irqchip {
    struct vmm_vm   *vm;
    pthread_mutex_t  lock;
    struct ioapic    ioapic;
    struct i8259     pic;
    struct i8254     pit;

    /* PIC 主片 INT 输出。持 lock 写，vCPU 0 进 KVM_RUN 前不拿锁先看一眼
     * （原子读），为 1 时再拿锁做 INTA */
    int              pic_int;

    int              timer_fd;      /* PIT 通道 0 的 timerfd，-1 表示未创建 */
    uint64_t         timer_armed;   /* 已设的绝对时刻，I8254_NO_EVENT 表示停 */

    /* 已提交给 KVM 的 24 条 IOAPIC 路由，RTE 改动只在换算结果变了时才重新
     * 提交（只改 mask 位不影响路由，KVM_SET_GSI_ROUTING 要等 SRCU 宽限期） */
    struct kvm_irq_routing_entry routes[IOAPIC_NUM_PINS];

    struct split_port ports[SPLIT_NR_PORTS];

    /* 观测计数，持 lock 更新 */
    uint64_t extint_injected;       /* KVM_INTERRUPT 成功次数 */
    uint64_t eoi_exits;             /* KVM_EXIT_IOAPIC_EOI 次数 */
    uint64_t route_updates;         /* KVM_SET_GSI_ROUTING 次数 */
    uint64_t pit_ticks;             /* 通道 0 OUT 的上升沿总数 */
    uint64_t pit_irqs;              /* 送出的 IRQ0 脉冲 */
    uint64_t pit_coalesced;         /* off：合并掉的上升沿 */
    uint64_t pit_acks;              /* on：观察到的确认 */
    uint64_t pit_dropped;           /* on：装数、解除屏蔽、复位时清掉的积压 */

    /* 补发策略的状态，持 lock 访问 */
    bool     pit_reinject;          /* --pit-reinject */
    uint64_t pit_backlog;           /* on：到期了还没送出的 tick */
    int      pit_irq_ack;           /* on：上一个送出的 tick 已被确认 */
    /* pit_watch 上次看到的 IRQ0 状态：PIC ISR 位、PIC IMR 位、RTE mask 位 */
    uint8_t  pit_seen_isr0, pit_seen_pic_masked, pit_seen_io_masked;
};

/* vm_create 里、创建 vCPU 之前调用：开 KVM_CAP_SPLIT_IRQCHIP，初始化三颗
 * 芯片并提交初始路由，置 vm->split 和 vm->has_irqchip */
int  split_irqchip_create(struct vmm_vm *vm);

/* guest 启动前调用：注册 PIO/MMIO、PIT 定时器（I/O 线程）和复位回调。
 * 复位回调要先于其他设备登记，见 reset.h 对中断线电平的要求 */
int  split_irqchip_attach(struct vmm_vm *vm, struct event_loop *loop);

/* irqchip_set_irq 在 split 模式下的实现，调用方不能持有 lock */
void split_irqchip_set_irq(struct split_irqchip *s, int gsi, int level);

/* vCPU 0 每次进 KVM_RUN 之前调用：PIC 有中断时按就绪状态注入或请求中断窗口 */
int  split_irqchip_pre_run(struct vmm_vcpu *vcpu);

/* KVM_EXIT_IOAPIC_EOI 的处理 */
int  split_irqchip_eoi(struct vmm_vcpu *vcpu, uint8_t vector);

/* monitor 的 info irqchip：补发策略、PIT/PIC/IOAPIC 计数 */
void split_irqchip_report(struct split_irqchip *s, FILE *f);

void split_irqchip_destroy(struct vmm_vm *vm);

#endif /* DEVICES_SPLIT_IRQCHIP_H */
