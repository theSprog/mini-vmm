/* include/devices/device.h — --device 的设备类型表
 *
 * 一个 --device 参数分两个阶段落地：
 *
 *   解析阶段（vmm_config_parse_args 里）：解析参数值、按 driver 找到类型、
 *     检查属性名、调 create 读属性并分配设备对象。拼错的属性、非法的值
 *     都在这一步报出来，VM 还没创建。
 *   挂载阶段（boot_linux 里，PCI 总线建好之后）：调 attach 把设备注册到
 *     总线上。
 *
 * 分两阶段是因为属性错误应当在启动前发现，而挂载要等总线存在。设备对象
 * 由 create 分配，生命周期覆盖整个进程，不释放：它注册进总线后，vCPU
 * 线程和 I/O 线程会一直引用它，直到进程退出。
 */
#ifndef DEVICES_DEVICE_H
#define DEVICES_DEVICE_H

#include <stdio.h>

struct opts;
struct pci_bus;
struct vmm_vm;

struct vmm_device_type {
    const char *name;              /* driver= 的值 */
    const char *desc;              /* --device help 里的一行说明 */
    const char *const *props;      /* 合法属性名，NULL 结尾，含 "driver" */
    /* o 在 create 返回后释放，要保留的字符串自己拷贝 */
    int  (*create)(const struct opts *o, void **dev);
    int  (*attach)(void *dev, struct vmm_vm *vm, struct pci_bus *bus);
};

struct vmm_device {
    const struct vmm_device_type *type;
    void *dev;
};

/* 解析阶段。失败时已打印原因 */
int  vmm_device_parse(const char *arg, struct vmm_device *out);

/* 挂载阶段 */
int  vmm_device_attach(const struct vmm_device *d, struct vmm_vm *vm,
                       struct pci_bus *bus);

/* --device help 的输出 */
void vmm_device_list_types(FILE *f);

#endif /* DEVICES_DEVICE_H */
