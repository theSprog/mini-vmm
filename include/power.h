/* include/power.h — 整机电源事件：PM1 寄存器接到主板上                 [acpi-events]
 *
 * devices/acpi_pm.h 只管寄存器语义，这里负责接线：
 *   - PM1a 事件块、控制块和扩展睡眠寄存器注册在 [ACPI_PM1A_EVT_PORT,
 *     +ACPI_PM_IO_LEN)，地址与 FADT 里写的一致（boot/acpi.h）
 *   - SCI 接到 GSI ACPI_SCI_IRQ（9）。ISA IRQ 与 IOAPIC 引脚恒等映射，MADT
 *     不需要 Interrupt Source Override：ACPI 6.5 5.2.12.5 只要求非恒等映射或
 *     极性不合标准时给 ISO，而 SCI 的标准属性（FADT SCI_INT：shareable、
 *     level、active low）正是 Linux 没有 ISO 时采用的默认值。线上的电平按
 *     “1 = 有效”送给 irqchip：KVM 和用户态 IOAPIC 都不看 RTE 的极性位，
 *     PCI INTx 也是同样的约定
 *   - guest 经 PM1_CNT 或 SLEEP_CONTROL 请求 S5 时 VMM 以退出码 0 退出
 *   - 整机复位时寄存器回到 0、SCI 落下
 *   - monitor 命令 system_powerdown：按一下电源按钮
 *
 * 全部入口在 vm->io_lock 下运行：PIO 回调由 exit 分发路径持锁调用，monitor
 * 命令在 I/O 线程上自己拿锁，复位回调由 vm_reset 持锁调用。
 */
#ifndef POWER_H
#define POWER_H

struct vmm_vm;
struct monitor;

int power_attach(struct vmm_vm *vm);
int power_monitor_register(struct monitor *m);

#endif /* POWER_H */
