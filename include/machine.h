/* include/machine.h — 板级装配：VM 由哪些设备组成、guest 怎么被送到入口
 *
 * --mode 同时决定两件事：
 *   平台    real16/long64 是"裸机"：只有两个调试端口和 COM1。linux 是 PC
 *           平台：在裸机之上加中断控制器、i8042、RTC、电源、PCI 总线和
 *           --device 设备。
 *   装载器  裸机装 payload（boot/payload.h），PC 平台装 Linux 内核
 *           （boot/linux_boot.h）。
 * 现有的三种模式里平台和装载器一一对应，所以都由 cfg.mode 决定。
 *
 * 生命周期（都在主线程上）：
 *   machine_init    vm_create、evl_init 之后：挂所有模式共有的设备
 *   machine_boot    挂平台设备，做第一次装载。返回后 VM 处于上电状态，
 *                   可以 evl_start 和 vm_run
 *   machine_reload  整机复位时作为 vm_reset 的 reload 回调，重新装载镜像
 *   machine_stop    vCPU 线程和 I/O 线程都停下之后：恢复终端
 *
 * 设备对象嵌在 struct machine 里，生命周期覆盖整个进程：注册进 I/O 表
 * 之后，vCPU 线程和 I/O 线程会一直引用它们。
 */
#ifndef MACHINE_H
#define MACHINE_H

#include <stdbool.h>

#include "boot/acpi.h"
#include "devices/serial8250.h"
#include "pci/pci.h"

struct vmm_vm;
struct event_loop;

struct machine {
    struct vmm_vm     *vm;
    struct event_loop *loop;       /* 还没启动的事件循环，后端在这里登记 fd */
    struct serial8250  com1;
    struct pci_bus     pci;        /* 只有 PC 平台初始化 */
    struct acpi_devices acpi;      /* 设备 attach 时登记，生成 DSDT 用 */
    bool               console;    /* host stdin 已接到 COM1，退出时要恢复终端 */
};

int  machine_init(struct machine *m, struct vmm_vm *vm,
                  struct event_loop *loop);
int  machine_boot(struct machine *m);
int  machine_reload(void *opaque);   /* opaque 是 struct machine * */
void machine_stop(struct machine *m);

#endif
