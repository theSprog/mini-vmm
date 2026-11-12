/* src/devices/device.c — --device 的设备类型表，两阶段的约定见 devices/device.h */
#include <stdlib.h>
#include <string.h>

#include "vmm.h"
#include "opts.h"
#include "devices/device.h"
#include "devices/ahci.h"
#include "devices/e1000.h"
#include "devices/pci_test.h"

static const struct vmm_device_type *const device_types[] = {
    &ahci_device_type,
    &e1000_device_type,
    &pci_test_device_type,
};

#define NR_DEVICE_TYPES  (sizeof(device_types) / sizeof(device_types[0]))

void vmm_device_list_types(FILE *f)
{
    size_t i;

    fprintf(f, "Available --device drivers:\n");
    for (i = 0; i < NR_DEVICE_TYPES; i++)
        fprintf(f, "  %-12s %s\n", device_types[i]->name, device_types[i]->desc);
}

int vmm_device_parse(const char *arg, struct vmm_device *out)
{
    const struct vmm_device_type *t = NULL;
    struct opts o;
    const char *driver;
    size_t i;
    int r;

    r = opts_parse(&o, "--device", arg, "driver");
    if (r != VMM_OK)
        return r;
    driver = opts_get(&o, "driver");
    if (!driver) {
        /* 第一个元素带了 '='，又没有显式写 driver= */
        vmm_err("--device %s: missing driver (see --device help)", arg);
        r = VMM_ERR_INVAL;
        goto out;
    }
    for (i = 0; i < NR_DEVICE_TYPES; i++)
        if (!strcmp(device_types[i]->name, driver))
            t = device_types[i];
    if (!t) {
        vmm_err("--device %s: unknown driver '%s' (see --device help)",
                arg, driver);
        r = VMM_ERR_INVAL;
        goto out;
    }
    r = opts_check_keys(&o, t->props);
    if (r != VMM_OK)
        goto out;

    out->type = t;
    out->arg = arg;
    out->net = NULL;
    out->netdev_id = NULL;
    out->blk = NULL;
    out->drive_id = NULL;
    if (opts_get(&o, "netdev")) {
        out->netdev_id = strdup(opts_get(&o, "netdev"));
        if (!out->netdev_id) {
            r = VMM_ERR_NOMEM;
            goto out;
        }
    }
    if (opts_get(&o, "drive")) {
        out->drive_id = strdup(opts_get(&o, "drive"));
        if (!out->drive_id) {
            r = VMM_ERR_NOMEM;
            goto out;
        }
    }
    r = t->create(&o, &out->dev);
out:
    opts_free(&o);
    return r;
}

int vmm_device_attach(const struct vmm_device *d, struct vmm_vm *vm,
                      struct pci_bus *bus)
{
    return d->type->attach(d, vm, bus);
}
