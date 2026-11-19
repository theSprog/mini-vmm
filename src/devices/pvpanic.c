/* src/devices/pvpanic.c — guest panic 通知设备，约定见 devices/pvpanic.h  [pvpanic] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmm.h"
#include "devices/device.h"
#include "devices/pvpanic.h"

/* 日志里把事件位写成名字，"events 0x03: PANICKED|CRASH_LOADED" */
static const char *event_names(unsigned ev, char *buf, size_t len)
{
    static const char *const names[] = { "PANICKED", "CRASH_LOADED", "SHUTDOWN" };
    size_t i, n = 0;

    buf[0] = '\0';
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (ev & (1u << i))
            n += (size_t)snprintf(buf + n, n < len ? len - n : 0, "%s%s",
                                  n ? "|" : "", names[i]);
    return buf;
}

static int pvpanic_read(void *opaque, uint64_t off, uint32_t size, void *data)
{
    (void)opaque; (void)off;
    memset(data, 0, size);
    *(uint8_t *)data = PVPANIC_FEATURES;
    return VMM_OK;
}

static int pvpanic_write(void *opaque, uint64_t off, uint32_t size,
                         const void *data)
{
    struct vmm_vm *vm = opaque;
    uint8_t v = *(const uint8_t *)data;
    unsigned ev = v & PVPANIC_FEATURES;
    char names[48];

    (void)off; (void)size;
    if (!ev) {
        vmm_info("pvpanic: ignored write 0x%02x (no recognized event bits)", v);
        return VMM_OK;
    }
    event_names(ev, names, sizeof(names));
    if (ev & PVPANIC_PANICKED) {
        vmm_err("pvpanic: guest panicked (events 0x%02x: %s), exiting with code %d",
                ev, names, VMM_EXIT_GUEST_PANIC);
        vm->exit_code = VMM_EXIT_GUEST_PANIC;
        return VMM_ERR_EXIT;
    }
    if (ev & PVPANIC_SHUTDOWN) {
        vmm_info("pvpanic: guest reported shutdown (events 0x%02x: %s), powering off",
                 ev, names);
        vm->exit_code = 0;
        return VMM_ERR_EXIT;
    }
    vmm_warn("pvpanic: guest panicked and handles it itself (events 0x%02x: %s), "
             "guest keeps running", ev, names);
    return VMM_OK;
}

static int pvpanic_create(const struct opts *o, void **dev)
{
    (void)o;
    *dev = NULL;
    return VMM_OK;
}

static int pvpanic_attach(const struct vmm_device *d, struct vmm_vm *vm,
                          struct pci_bus *bus, struct event_loop *loop)
{
    (void)loop;
    struct vmm_io_dev dev;
    int r;

    (void)d; (void)bus;
    if (vm->pvpanic) {
        vmm_err("--device pvpanic: only one pvpanic device (port 0x%x) is supported",
                PVPANIC_PORT);
        return VMM_ERR_INVAL;
    }
    memset(&dev, 0, sizeof(dev));
    dev.name   = "pvpanic";
    dev.base   = PVPANIC_PORT;
    dev.len    = 1;
    dev.opaque = vm;
    dev.read   = pvpanic_read;
    dev.write  = pvpanic_write;
    r = vmm_register_pio(vm, &dev);
    if (r == VMM_OK)
        vm->pvpanic = true;
    return r;
}

static const char *const pvpanic_props[] = { "driver", NULL };

const struct vmm_device_type pvpanic_device_type = {
    .name   = "pvpanic",
    .desc   = "ISA panic notifier at port 0x505 (ACPI QEMU0001); a guest panic "
              "ends the VMM with exit code 3",
    .props  = pvpanic_props,
    .create = pvpanic_create,
    .attach = pvpanic_attach,
};
