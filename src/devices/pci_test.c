/* src/devices/pci_test.c — 验证 PCI 总线用的最小设备              [pci-bus]
 *
 * 寄存器布局见 include/devices/pci_test.h。
 */
#include <stdlib.h>
#include <string.h>

#include "vmm.h"
#include "devices/pci_test.h"

/* 按访问时的状态拼出完整的寄存器文件，再截取 [off, off + size) */
static void regs_snapshot(const struct pci_test_bar *b, uint8_t regs[PCI_TEST_REGS_LEN])
{
    uint32_t v;

    v = PCI_TEST_MAGIC;
    memcpy(regs + PCI_TEST_REG_MAGIC, &v, 4);
    memcpy(regs + PCI_TEST_REG_SCRATCH, &b->dev->scratch, 4);
    v = b->index;
    memcpy(regs + PCI_TEST_REG_BAR, &v, 4);
}

static int pci_test_read(void *opaque, uint64_t off, uint32_t size, void *data)
{
    const struct pci_test_bar *b = opaque;
    uint8_t regs[PCI_TEST_REGS_LEN], *out = data;
    uint32_t i;

    regs_snapshot(b, regs);
    for (i = 0; i < size; i++)
        out[i] = off + i < PCI_TEST_REGS_LEN ? regs[off + i] : 0;
    return VMM_OK;
}

static int pci_test_write(void *opaque, uint64_t off, uint32_t size,
                          const void *data)
{
    const struct pci_test_bar *b = opaque;
    uint8_t *scratch = (uint8_t *)&b->dev->scratch;
    const uint8_t *in = data;
    uint32_t i;

    for (i = 0; i < size; i++) {
        uint64_t o = off + i;

        if (o >= PCI_TEST_REG_SCRATCH && o < PCI_TEST_REG_SCRATCH + 4)
            scratch[o - PCI_TEST_REG_SCRATCH] = in[i];
    }
    return VMM_OK;
}

int pci_test_attach(struct pci_test *t, struct pci_bus *bus)
{
    struct pci_device *d = &t->pci;
    int i;

    memset(t, 0, sizeof(*t));
    d->name       = "pci-test";
    d->vendor_id  = PCI_VENDOR_ID_MINIVMM;
    d->device_id  = PCI_DEVICE_ID_TEST;
    d->class_code = 0xFF0000;
    d->intx_pin   = 1;

    for (i = 0; i < 2; i++) {
        t->bar_ctx[i].dev   = t;
        t->bar_ctx[i].index = (uint32_t)i;
        d->bars[i].opaque   = &t->bar_ctx[i];
        d->bars[i].read     = pci_test_read;
        d->bars[i].write    = pci_test_write;
    }
    d->bars[0].type = PCI_BAR_MEM32;
    d->bars[0].size = PCI_TEST_MMIO_SIZE;
    d->bars[1].type = PCI_BAR_IO;
    d->bars[1].size = PCI_TEST_IO_SIZE;

    return pci_register_device(bus, d);
}

static int pci_test_create(const struct opts *o, void **dev)
{
    (void)o;
    *dev = calloc(1, sizeof(struct pci_test));
    return *dev ? VMM_OK : VMM_ERR_NOMEM;
}

static int pci_test_dev_attach(const struct vmm_device *d, struct vmm_vm *vm,
                               struct pci_bus *bus)
{
    (void)vm;
    return pci_test_attach(d->dev, bus);
}

static void pci_test_reset(void *dev)
{
    struct pci_test *t = dev;

    t->scratch = 0;
}

static const char *const pci_test_props[] = { "driver", NULL };

const struct vmm_device_type pci_test_device_type = {
    .name   = "pci-test",
    .desc   = "PCI bus test device: MMIO and IO BAR over a magic/scratch register file",
    .props  = pci_test_props,
    .create = pci_test_create,
    .attach = pci_test_dev_attach,
    .reset  = pci_test_reset,
};
