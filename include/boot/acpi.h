/* include/boot/acpi.h — 最小 ACPI 表：RSDP / XSDT / FADT / DSDT / MADT
 *
 * 和 MP table 是同一份信息的两种表达。两者都提供：
 *   - 内核开了 CONFIG_X86_MPPARSE 的，能用 MP table
 *   - 开了 CONFIG_ACPI 的（现代发行版内核都开），用 MADT
 * Linux 同时看到两者时优先 ACPI。
 *
 * FADT 故意不设 HW_REDUCED_ACPI：Linux 看到硬件精简模式会把 PIT 和
 * 8259 当作不存在（x86_init.acpi.reduced_hw_early_init），而我们正
 * 靠 PIT 校准 TSC。代价是得提供 PM1a 事件/控制寄存器块，见 acpi_pm。
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

/* 构建全部表，返回 RSDP 的 GPA */
int acpi_setup(struct vmm_vm *vm, int nr_cpus, uint64_t *rsdp_gpa);

/* PM1a 设备：guest 写 SLP_TYP=5|SLP_EN 时（poweroff）让 VMM 退出 */
int acpi_pm_attach(struct vmm_vm *vm);

#endif /* BOOT_ACPI_H */
