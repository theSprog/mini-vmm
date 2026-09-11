/* include/boot/mptable.h — Intel MultiProcessor Specification 1.4 表
 *
 * 为什么需要：没有 ACPI MADT 也没有 MP table 时，Linux 只能进
 * "virtual wire mode with no configuration"，不知道 IOAPIC 在哪、
 * ISA 中断怎么路由，结果 PIT 中断送不到 LAPIC，定时器校准死等。
 * MP table 是描述这些信息最简单的格式（比 ACPI 简单一个数量级），
 * kvmtool / Firecracker 都用它。Step 6 做 SMP 时每个 vCPU 一个
 * processor entry，也是在这里加。
 *
 * 结构体按规范自己定义：内核的 asm/mpspec_def.h 不是 uapi，用户态拿不到。
 */
#ifndef BOOT_MPTABLE_H
#define BOOT_MPTABLE_H

#include <stdint.h>

struct vmm_vm;

/* 放在低端内存最后 1KiB（EBDA 区）。Linux 的 smp_scan_config 会扫
 * [639K, 640K) 这一段找 "_MP_"，而 e820 已经把它报成保留，不会被当 RAM 用。 */
#define MPTABLE_GPA        0x9fc00ULL
#define MPTABLE_MAX_SIZE   0x400

#define MP_LAPIC_BASE      0xfee00000U
#define MP_IOAPIC_BASE     0xfec00000U

int mptable_setup(struct vmm_vm *vm, int nr_cpus);

#endif /* BOOT_MPTABLE_H */
