/* include/boot/acpi.h — 最小 ACPI 表：RSDP / XSDT / FADT / DSDT / MADT
 *
 * 和 MP table 是同一份信息的两种表达。两者都提供：
 *   - 内核开了 CONFIG_X86_MPPARSE 的，能用 MP table
 *   - 开了 CONFIG_ACPI 的（现代发行版内核都开），用 MADT
 * Linux 同时看到两者时优先 ACPI。
 *
 * FADT 故意不设 HW_REDUCED_ACPI：Linux 看到硬件精简模式会把 PIT 和
 * 8259 当作不存在（x86_init.acpi.reduced_hw_early_init），而没有 kvmclock
 * 的内核要靠 PIT 校准 TSC。代价是得提供 PM1a 事件/控制寄存器块，见 acpi_pm。
 * 编译期只支持硬件精简 ACPI 的内核不看这个标志也不碰 PM1，它们用的
 * 扩展睡眠寄存器和 RESET_REG 同样在 FADT 里提供。
 */
#ifndef BOOT_ACPI_H
#define BOOT_ACPI_H

#include <stdint.h>

struct vmm_vm;

/* 0xE0000 起是 BIOS 区（e820 已报保留），RSDP 按规范应在
 * [0xE0000, 0xFFFFF] 的 16 字节边界上，内核会扫这段。
 * 同时通过 boot_params.acpi_rsdp_addr 直接告诉内核，不必扫。 */
#define ACPI_TABLES_GPA   0xE0000ULL
#define ACPI_TABLES_MAX   0x10000ULL

/* PM1a 寄存器块端口，FADT 里写这两个地址，acpi_pm 设备在这里响应 */
#define ACPI_PM1A_EVT_PORT 0x600
#define ACPI_PM1A_CNT_PORT 0x604
#define ACPI_PM1_EVT_LEN   4
#define ACPI_PM1_CNT_LEN   2
#define ACPI_SCI_IRQ       9

/* ACPI 5.0 扩展睡眠寄存器（FADT SLEEP_CONTROL_REG / SLEEP_STATUS_REG），
 * 各 1 字节，紧跟在 PM1a 控制块之后，由同一个 acpi_pm 设备响应。
 *
 * 为什么 PM1 之外还要它：内核开了 CONFIG_ACPI_REDUCED_HARDWARE_ONLY 时，
 * ACPICA 在编译期删掉了全部 PM1 访问代码，关机只走这两个寄存器
 * （acpi_hw_extended_sleep）；它们缺失时 acpi_power_off() 静默返回，
 * 内核接着 panic。全功能内核仍走 PM1，两条路并存互不影响。 */
#define ACPI_SLEEP_CTL_PORT 0x606
#define ACPI_SLEEP_STS_PORT 0x607
#define ACPI_PM_IO_LEN      8     /* [0x600, 0x608)：PM1a EVT + CNT + 扩展睡眠 */

/* 构建全部表，返回 RSDP 的 GPA */
int acpi_setup(struct vmm_vm *vm, int nr_cpus, uint64_t *rsdp_gpa);

/* PM1a + 扩展睡眠寄存器设备：guest 经任一路径请求 S5（poweroff）时让 VMM 退出 */
int acpi_pm_attach(struct vmm_vm *vm);
/* 整机复位回调（reset.h）：PM1 寄存器回到 0 */
void acpi_pm_reset(void *opaque);

#endif /* BOOT_ACPI_H */
