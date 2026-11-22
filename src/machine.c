/* src/machine.c — 板级装配，平台与装载器的对应关系见 include/machine.h */
#include <string.h>
#include <unistd.h>

#include "vmm.h"
#include "machine.h"
#include "console.h"
#include "power.h"
#include "boot/linux_boot.h"
#include "boot/payload.h"
#include "devices/debug_port.h"
#include "devices/device.h"
#include "devices/i8042.h"
#include "devices/irqchip.h"
#include "devices/rtc.h"
#include "devices/serial8250.h"
#include "block/blockdev.h"
#include "net/netdev.h"

int machine_init(struct machine *m, struct vmm_vm *vm,
                 struct event_loop *loop)
{
    int r;

    memset(m, 0, sizeof(*m));
    m->vm   = vm;
    m->loop = loop;

    r = debug_ports_attach(vm);
    if (r != VMM_OK)
        return r;

    /* COM1：所有模式都挂上，串口载荷和 Linux guest 的主输出通道 */
    serial8250_init(&m->com1, STDOUT_FILENO);
    r = serial8250_attach(vm, &m->com1, SERIAL_COM1_BASE, SERIAL_COM1_IRQ);
    if (r == VMM_OK)
        r = vmm_register_reset(vm, "serial8250", serial8250_reset, &m->com1);
    return r;
}

/* PC 平台的设备。复位回调按挂载顺序登记，顺序要求见 reset.h：中断控制器
 * 最先，PCI 总线先于它上面的设备。 */
static int pc_attach(struct machine *m)
{
    struct vmm_vm *vm = m->vm;
    const struct vmm_config *cfg = &vm->cfg;
    struct vmm_attach_ctx ctx = {
        .vm = vm, .pci = &m->pci, .loop = m->loop, .acpi = &m->acpi,
    };
    int i, r;

    m->com1.irq_set    = irqchip_irq_cb;
    m->com1.irq_opaque = vm;

    /* 中断控制器在用户态时最先挂：它的复位回调要排在所有会拉
     * 中断线的设备之前 */
    if (vm->irqchip->attach) {
        r = vm->irqchip->attach(vm, m->loop);
        if (r != VMM_OK)
            return r;
    }

    r = i8042_attach(vm);
    if (r == VMM_OK)
        r = vmm_register_reset(vm, "i8042", i8042_reset, NULL);
    if (r != VMM_OK)
        return r;
    /* RTC 不登记复位回调：CMOS RAM 由电池供电，复位不改变它的内容；这个
     * 模型不产生中断，也就没有中断允许位之类要在复位时清掉的状态 */
    r = rtc_attach(vm);
    if (r != VMM_OK)
        return r;
    r = power_attach(vm);
    if (r != VMM_OK)
        return r;

    /* PCI 设备要在生成 MP table 之前注册完：MP table 按已插设备列中断项 */
    r = pci_bus_init(&m->pci, vm);
    if (r == VMM_OK)
        r = vmm_register_reset(vm, "pci", pci_bus_reset, &m->pci);
    if (r != VMM_OK)
        return r;
    /* 磁盘控制器 attach 时要按镜像容量生成设备信息，所以镜像先打开 */
    for (i = 0; i < cfg->nr_drives; i++) {
        r = blockdev_open(cfg->drives[i]);
        if (r != VMM_OK)
            return r;
    }
    for (i = 0; i < cfg->nr_devices; i++) {
        r = vmm_device_attach(&cfg->devices[i], &ctx);
        if (r != VMM_OK)
            return r;
    }
    /* 网卡 attach 时登记了收帧回调，之后才能打开后端开始收帧 */
    for (i = 0; i < cfg->nr_netdevs; i++) {
        r = netdev_open(cfg->netdevs[i], m->loop);
        if (r != VMM_OK)
            return r;
    }
    return VMM_OK;
}

int machine_boot(struct machine *m)
{
    struct vmm_vm *vm = m->vm;
    int r;

    if (vm->cfg.mode != VMM_BOOT_LINUX)
        return payload_boot(vm, &vm->cfg);

    r = pc_attach(m);
    if (r != VMM_OK)
        return r;
    r = linux_boot_load(vm, &m->pci, &m->acpi);
    if (r != VMM_OK)
        return r;
    r = console_start(vm, &m->com1, m->loop);
    if (r != VMM_OK)
        return r;
    m->console = true;
    /* 只有 PC 平台有复位来源（i8042）。要在第一次 KVM_RUN 之前存，存下的
     * 才是上电状态 */
    return vm_power_on_save(vm);
}

int machine_reload(void *opaque)
{
    struct machine *m = opaque;

    return linux_boot_load(m->vm, &m->pci, &m->acpi);
}

void machine_stop(struct machine *m)
{
    if (m->console)
        console_stop();
}
