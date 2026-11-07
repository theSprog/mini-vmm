/* include/devices/i8042.h — i8042 键盘控制器 */
#ifndef DEVICES_I8042_H
#define DEVICES_I8042_H

#define I8042_DATA_PORT 0x60
#define I8042_CMD_PORT  0x64
/* 控制器命令 0xFE：脉冲拉低输出端口 bit0（CPU reset 线）。
 * FADT 的 RESET_REG 也指向这里，见 boot/acpi.c。 */
#define I8042_CMD_PULSE_RESET 0xfe

struct vmm_vm;

int i8042_attach(struct vmm_vm *vm);

#endif /* DEVICES_I8042_H */
