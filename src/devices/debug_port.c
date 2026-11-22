/* src/devices/debug_port.c — 裸 payload 用的两个调试端口，语义见
 * include/devices/debug_port.h */
#include <stdio.h>
#include <string.h>

#include "vmm.h"
#include "devices/debug_port.h"

static int dbgport_write(void *opaque, uint64_t off, uint32_t size,
                         const void *data)
{
    const uint8_t *b = data;

    (void)opaque; (void)off;
    printf(">>> guest wrote port 0x10: 0x%02x (%u bytes)", b[0], size);
    if (b[0] >= 0x20 && b[0] < 0x7F)
        printf(" = '%c'", b[0]);
    printf("\n");
    fflush(stdout);
    return VMM_OK;
}

static int dbgport_read(void *opaque, uint64_t off, uint32_t size, void *data)
{
    (void)opaque; (void)off;
    memset(data, 0x5A, size);   /* 固定图案，方便 guest 侧验证读通路 */
    return VMM_OK;
}

/* 返回 VMM_ERR_EXIT 让 vCPU 线程停下，exit_code 成为进程退出码 */
static int exitport_write(void *opaque, uint64_t off, uint32_t size,
                          const void *data)
{
    struct vmm_vm *vm = opaque;
    const uint8_t *b = data;

    (void)off; (void)size;
    vm->exit_code = b[0];
    vmm_info("guest requested exit, code=%d", b[0]);
    return VMM_ERR_EXIT;
}

int debug_ports_attach(struct vmm_vm *vm)
{
    struct vmm_io_dev dev;
    int r;

    memset(&dev, 0, sizeof(dev));
    dev.name  = "dbg-port";
    dev.base  = DEBUG_PORT_PRINT;
    dev.len   = 1;
    dev.read  = dbgport_read;
    dev.write = dbgport_write;
    r = vmm_register_pio(vm, &dev);
    if (r != VMM_OK)
        return r;

    memset(&dev, 0, sizeof(dev));
    dev.name   = "exit-port";
    dev.base   = DEBUG_PORT_EXIT;
    dev.len    = 1;
    dev.opaque = vm;
    dev.write  = exitport_write;
    return vmm_register_pio(vm, &dev);
}
