/* include/devices/i8042.h — 键盘控制器占位，只处理 reset 命令 */
#ifndef DEVICES_I8042_H
#define DEVICES_I8042_H

struct vmm_vm;

int i8042_attach(struct vmm_vm *vm);

#endif /* DEVICES_I8042_H */
