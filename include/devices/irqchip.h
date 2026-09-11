/* include/devices/irqchip.h — 内核态中断控制器（PIC/IOAPIC/LAPIC + PIT）
 *
 * Linux 启动离不开两样东西：一个会产生周期中断的时钟源（PIT），
 * 一条能把设备中断送进 vCPU 的通路（PIC/IOAPIC/LAPIC）。
 * 这两样都交给 KVM 在内核里模拟，用户态只做两件事：
 *   1) KVM_CREATE_IRQCHIP / KVM_CREATE_PIT2 —— 必须在创建 vCPU 之前
 *   2) KVM_IRQ_LINE 拉高/拉低某根 GSI —— 设备（如串口）要中断时调用
 *
 * 副作用：有了内核态 LAPIC 之后，guest 的 HLT 由 KVM 就地处理
 * （阻塞到有中断为止），不再以 KVM_EXIT_HLT 返回用户态。
 */
#ifndef DEVICES_IRQCHIP_H
#define DEVICES_IRQCHIP_H

struct vmm_vm;

int  irqchip_create(struct vmm_vm *vm);

/* 设置 GSI 电平；level=1 拉高，0 拉低 */
int  irqchip_set_irq(struct vmm_vm *vm, int gsi, int level);

/* 给串口等设备用的回调形式，opaque 是 struct vmm_vm * */
void irqchip_irq_cb(void *opaque, int gsi, int level);

#endif /* DEVICES_IRQCHIP_H */
