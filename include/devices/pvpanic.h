/* include/devices/pvpanic.h — guest panic 通知设备（ISA 端口版）          [pvpanic]
 *
 * guest 内核 panic 时往一个 I/O 端口写一个字节，VMM 据此知道“guest 崩了”，
 * 而不必从串口输出里猜。寄存器约定出自 QEMU docs/specs/pvpanic.rst，位定义
 * 出自 Linux include/uapi/misc/pvpanic.h：
 *
 *   端口 0x505，1 字节
 *   读：返回设备认识的位，guest 驱动拿它和自己认识的位取交集作为 capability
 *   写：设备不认识的位忽略
 *     bit 0 PANICKED      guest panic 了，交给 host 处理
 *     bit 1 CRASH_LOADED  guest panic 了，但 guest 自己会处理（装了 kdump
 *                         内核）；host 只记录，不能影响 guest 继续执行
 *     bit 2 SHUTDOWN      guest 正常关机，交给 host 处理
 *
 * VMM 对三种事件的处理：
 *   PANICKED     记录后以退出码 VMM_EXIT_GUEST_PANIC 退出，和正常关机/
 *                --no-reboot 下的复位（退出码 0）区分开
 *   CRASH_LOADED 只记录。guest 接着跳进 kdump 内核，VMM 一停就丢了 dump
 *   SHUTDOWN     记录后按关机处理，退出码 0。Linux 把这个事件挂在优先级
 *                最低的 sys_off handler 上（SYS_OFF_PRIO_LOW），ACPI S5 能用
 *                时轮不到它；轮到它说明别的关机手段都没生效，写完端口内核就
 *                进 halt 死等，VMM 不结束 guest 就永远挂着
 *   PANICKED 和别的位同时出现时按 PANICKED 处理：它要求 host 介入，其余两个
 *   都不阻止 host 介入
 *
 * 设备没有状态：读永远返回同一个值，写只触发一次动作，所以没有复位回调。
 *
 * guest 驱动经 ACPI 发现它：DSDT 里的 Device 带 _HID "QEMU0001" 和一个 IO
 * 资源，Linux 的 pvpanic-mmio 按 _HID 绑定，从 _CRS 拿端口。这是一个可选
 * 设备（--device pvpanic），挂上时置 vm->pvpanic，boot/acpi.c 据此在 DSDT 里
 * 描述它。可选的理由：挂上后 panic 不再走 panic=-1 的复位路径，而复位路径
 * 本身也要能测（system-reset 等 suite 用 VFS panic 触发复位）。
 */
#ifndef DEVICES_PVPANIC_H
#define DEVICES_PVPANIC_H

#define PVPANIC_PORT          0x505
#define PVPANIC_PANICKED      (1u << 0)
#define PVPANIC_CRASH_LOADED  (1u << 1)
#define PVPANIC_SHUTDOWN      (1u << 2)
#define PVPANIC_FEATURES      (PVPANIC_PANICKED | PVPANIC_CRASH_LOADED | \
                               PVPANIC_SHUTDOWN)

/* ACPI ID（ACPI 6.5 6.1.5 的 "NNNN####" 形式），Linux pvpanic-mmio 的
 * acpi_device_id 表里就是这个字符串 */
#define PVPANIC_ACPI_HID      "QEMU0001"

struct vmm_device_type;
extern const struct vmm_device_type pvpanic_device_type;

#endif /* DEVICES_PVPANIC_H */
