/* src/devices/edu.c — edu 教学 PCI 设备，规范见 devices/edu_regs.h，模型结构见 devices/edu.h */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

#include "vmm.h"
#include "opts.h"
#include "io/event_loop.h"
#include "devices/edu.h"

/* guest 能反复触发的异常访问只记第一次，免得 guest 用它刷爆日志 */
enum {
    W_REG_WIDTH  = 1u << 0,
    W_BUSY_WRITE = 1u << 1,
};
static unsigned g_warned;

#define warn_once(bit, ...)                     \
    do {                                        \
        if (!(g_warned & (bit))) {              \
            g_warned |= (bit);                  \
            vmm_warn(__VA_ARGS__);              \
        }                                       \
    } while (0)

/* 中断原因从 old 变成 e->int_status 之后同步输出：INTx 电平跟着
 * INT_STATUS != 0 走（MSI 打开时 PCI 核心不把它放上线）；MSI 打开时，
 * 每多出一个原来为 0 的位发一条消息。 */
static void edu_update_irq(struct edu *e, uint32_t old)
{
    pci_set_irq(&e->pci, e->int_status != 0);
    if ((e->int_status & ~old) && pci_msi_enabled(&e->pci))
        pci_msi_notify(&e->pci);
}

static void edu_set_int(struct edu *e, uint32_t bits)
{
    uint32_t old = e->int_status;

    e->int_status |= bits;
    edu_update_irq(e, old);
}

static uint32_t edu_reg_read(struct edu *e, uint32_t off)
{
    switch (off) {
    case EDU_REG_ID:             return EDU_ID_VALUE;
    case EDU_REG_SCRATCH:        return e->scratch;
    case EDU_REG_INT_STATUS:     return e->int_status;
    case EDU_REG_DMA_ADDR_LO:    return e->dma_addr_lo;
    case EDU_REG_DMA_ADDR_HI:    return e->dma_addr_hi;
    case EDU_REG_DMA_OFF:        return e->dma_off;
    case EDU_REG_DMA_LEN:        return e->dma_len;
    case EDU_REG_DMA_CMD:        return e->dma_cmd;
    case EDU_REG_DMA_RESULT:     return e->dma_result;
    case EDU_REG_DMA_DONE_COUNT: return e->dma_done_count;
    default:                     return 0;
    }
}

static void edu_start_dma(struct edu *e, uint32_t cmd)
{
    static const uint64_t one = 1;

    e->dma_cmd  = cmd;
    e->run_addr = (uint64_t)e->dma_addr_hi << 32 | e->dma_addr_lo;
    e->run_off  = e->dma_off;
    e->run_len  = e->dma_len;
    e->run_cmd  = cmd;
    /* eventfd 的计数不会溢出：每条命令最多写一次，执行中再写 START 被丢弃 */
    if (write(e->wake_fd, &one, sizeof(one)) != sizeof(one))
        vmm_err("edu: write(eventfd) failed: %s", strerror(errno));
}

static void edu_reg_write(struct edu *e, uint32_t off, uint32_t v)
{
    bool busy = e->dma_cmd & EDU_DMA_CMD_START;
    uint32_t old;

    switch (off) {
    case EDU_REG_SCRATCH:
        e->scratch = v;
        return;
    case EDU_REG_INT_RAISE:
        edu_set_int(e, v & EDU_INT_SOFT_MASK);
        return;
    case EDU_REG_INT_ACK:
        old = e->int_status;
        e->int_status &= ~v;
        edu_update_irq(e, old);
        return;
    case EDU_REG_DMA_ADDR_LO:
    case EDU_REG_DMA_ADDR_HI:
    case EDU_REG_DMA_OFF:
    case EDU_REG_DMA_LEN:
    case EDU_REG_DMA_CMD:
        if (busy) {
            warn_once(W_BUSY_WRITE,
                      "edu: write 0x%x to DMA register 0x%03x ignored while a command is running",
                      v, off);
            return;
        }
        break;
    default:
        return;
    }

    switch (off) {
    case EDU_REG_DMA_ADDR_LO: e->dma_addr_lo = v; break;
    case EDU_REG_DMA_ADDR_HI: e->dma_addr_hi = v; break;
    case EDU_REG_DMA_OFF:     e->dma_off = v; break;
    case EDU_REG_DMA_LEN:     e->dma_len = v; break;
    case EDU_REG_DMA_CMD:
        v &= EDU_DMA_CMD_START | EDU_DMA_CMD_TO_RAM | EDU_DMA_CMD_IRQ;
        if (v & EDU_DMA_CMD_START)
            edu_start_dma(e, v);
        else
            e->dma_cmd = v;
        break;
    }
}

static int edu_mmio_read(void *opaque, uint64_t off, uint32_t size, void *data)
{
    struct edu *e = opaque;
    uint32_t v;

    if (off >= EDU_BUF_OFFSET) {
        uint64_t o = off - EDU_BUF_OFFSET;

        /* BAR 大小 8 KiB，exit 分发保证 off + size 不超出 BAR */
        memcpy(data, e->buf + o, size);
        return VMM_OK;
    }
    if (size != 4 || (off & 3)) {
        warn_once(W_REG_WIDTH,
                  "edu: %u-byte read at 0x%03llx returns all ones (registers take aligned 32-bit access)",
                  size, (unsigned long long)off);
        memset(data, 0xFF, size);
        return VMM_OK;
    }
    v = edu_reg_read(e, (uint32_t)off);
    memcpy(data, &v, 4);
    return VMM_OK;
}

static int edu_mmio_write(void *opaque, uint64_t off, uint32_t size,
                          const void *data)
{
    struct edu *e = opaque;
    uint32_t v;

    if (off >= EDU_BUF_OFFSET) {
        memcpy(e->buf + (off - EDU_BUF_OFFSET), data, size);
        return VMM_OK;
    }
    if (size != 4 || (off & 3)) {
        warn_once(W_REG_WIDTH,
                  "edu: %u-byte write at 0x%03llx ignored (registers take aligned 32-bit access)",
                  size, (unsigned long long)off);
        return VMM_OK;
    }
    memcpy(&v, data, 4);
    edu_reg_write(e, (uint32_t)off, v);
    return VMM_OK;
}

/* I/O 线程：执行 edu_start_dma 取下的命令 */
static void edu_run_dma(struct edu *e)
{
    uint32_t res;
    int r;

    if (e->run_len == 0 || e->run_off > EDU_BUF_SIZE ||
        e->run_len > EDU_BUF_SIZE - e->run_off) {
        res = EDU_DMA_RES_BAD_RANGE;
    } else {
        if (e->run_cmd & EDU_DMA_CMD_TO_RAM)
            r = pci_dma_write(&e->pci, e->run_addr, e->buf + e->run_off, e->run_len);
        else
            r = pci_dma_read(&e->pci, e->run_addr, e->buf + e->run_off, e->run_len);
        res = r == VMM_OK ? EDU_DMA_RES_OK : EDU_DMA_RES_BUS_ERROR;
    }
    if (res != EDU_DMA_RES_OK)
        vmm_dbg("edu: DMA %s addr 0x%llx off 0x%x len 0x%x failed: result %u",
                e->run_cmd & EDU_DMA_CMD_TO_RAM ? "to RAM" : "from RAM",
                (unsigned long long)e->run_addr, e->run_off, e->run_len, res);

    e->dma_result = res;
    e->dma_done_count++;
    e->dma_cmd &= ~EDU_DMA_CMD_START;
    if (e->run_cmd & EDU_DMA_CMD_IRQ)
        edu_set_int(e, EDU_INT_DMA);
}

static void edu_on_wake(void *opaque, int fd, uint32_t events)
{
    struct edu *e = opaque;
    uint64_t n;

    (void)events;
    if (read(fd, &n, sizeof(n)) != sizeof(n)) {
        /* 非阻塞 eventfd 只在计数为 0 时失败（EAGAIN），水平触发不会这样报 */
        if (errno != EAGAIN)
            vmm_err("edu: read(eventfd) failed: %s", strerror(errno));
        return;
    }
    pthread_mutex_lock(&e->vm->io_lock);
    /* 复位可能已经丢弃了这条命令 */
    if (e->dma_cmd & EDU_DMA_CMD_START)
        edu_run_dma(e);
    pthread_mutex_unlock(&e->vm->io_lock);
}

void edu_reset(void *opaque)
{
    struct edu *e = opaque;

    e->scratch = 0;
    e->int_status = 0;
    e->dma_addr_lo = e->dma_addr_hi = 0;
    e->dma_off = e->dma_len = 0;
    e->dma_cmd = 0;
    e->dma_result = 0;
    e->dma_done_count = 0;
    memset(e->buf, 0, sizeof(e->buf));
    /* PCI 核心已经撤销了总线上的 INTx；设备记着的电平也回到 0 */
    pci_set_irq(&e->pci, false);
}

int edu_attach(struct edu *e, struct vmm_vm *vm, struct pci_bus *bus,
               struct event_loop *loop)
{
    struct pci_device *d = &e->pci;
    int r;

    e->vm = vm;
    d->name       = "edu";
    d->vendor_id  = EDU_VENDOR_ID;
    d->device_id  = EDU_DEVICE_ID;
    d->class_code = EDU_CLASS_CODE;
    d->intx_pin   = 1;
    d->bus_master = true;
    d->msi        = true;
    d->bars[0].type   = PCI_BAR_MEM32;
    d->bars[0].size   = EDU_BAR0_SIZE;
    d->bars[0].opaque = e;
    d->bars[0].read   = edu_mmio_read;
    d->bars[0].write  = edu_mmio_write;

    e->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (e->wake_fd < 0) {
        vmm_err("edu: eventfd failed: %s", strerror(errno));
        return VMM_ERR_SYS;
    }
    r = evl_add(loop, e->wake_fd, EPOLLIN, edu_on_wake, e);
    if (r != VMM_OK) {
        vmm_err("edu: cannot register eventfd with the event loop: %d", r);
        return r;
    }
    return pci_register_device(bus, d);
}

static int edu_create(const struct opts *o, void **dev)
{
    struct edu *e;

    (void)o;
    e = calloc(1, sizeof(*e));
    if (!e)
        return VMM_ERR_NOMEM;
    e->wake_fd = -1;
    *dev = e;
    return VMM_OK;
}

static int edu_dev_attach(const struct vmm_device *d, struct vmm_vm *vm,
                          struct pci_bus *bus, struct event_loop *loop)
{
    return edu_attach(d->dev, vm, bus, loop);
}

static const char *const edu_props[] = { "driver", NULL };

const struct vmm_device_type edu_device_type = {
    .name   = "edu",
    .desc   = "teaching PCI device: registers, a DMA engine, INTx and MSI",
    .props  = edu_props,
    .create = edu_create,
    .attach = edu_dev_attach,
    .reset  = edu_reset,
};
