/* include/boot/linux_boot.h — Linux 装载器（--mode linux）
 *
 * 把内核（vmlinux 或 bzImage）、initrd、boot_params、MP table、ACPI 表和
 * 最初的恒等映射页表装进 guest 内存，再按 64 位启动协议设好 BSP 的入口
 * 寄存器。
 *
 * 第一次启动和每次整机复位都要调用：guest 运行时会改写这些内存，复位后
 * 必须重新装载。调用时 PCI 设备必须已经全部挂上，因为 MP table 按总线上
 * 已有的设备列中断项。
 */
#ifndef BOOT_LINUX_BOOT_H
#define BOOT_LINUX_BOOT_H

struct vmm_vm;
struct pci_bus;
struct acpi_devices;

int linux_boot_load(struct vmm_vm *vm, const struct pci_bus *pci,
                    const struct acpi_devices *acpi);

#endif
