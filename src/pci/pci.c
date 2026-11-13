/* src/pci/pci.c — PCI 总线：conf1 端口、配置空间读写、BAR decode    [pci-bus]
 *
 * 设计见 include/pci/pci.h。
 */
#include <stdio.h>
#include <string.h>
#include <linux/pci_regs.h>

#include "vmm.h"
#include "memory.h"
#include "devices/irqchip.h"
#include "pci/pci.h"

/* CONFIG_ADDRESS 的字段（PCI 3.0 第 3.2.2.3.2 节；Linux drivers/pci/pci.h
 * 的 PCI_CONF1_* 是同一套定义） */
#define CONF1_ENABLE        0x80000000u
#define CONF1_RESERVED      0x7F000000u
#define CONF1_BUS(a)        (((a) >> 16) & 0xFF)
#define CONF1_DEVFN(a)      (((a) >> 8) & 0xFF)
#define CONF1_REG(a)        ((a) & 0xFC)
/* bit 1:0 只读为 0 */
#define CONF1_WRITABLE      (~0x3u)

#define PCI_SLOT_OF(devfn)  (((devfn) >> 3) & 0x1F)
#define PCI_FUNC_OF(devfn)  ((devfn) & 0x07)

#define BAR_REG(i)          (PCI_BASE_ADDRESS_0 + 4 * (i))

/* class code 0x06 / 0x00：bridge / host bridge（PCI Code and ID Assignment
 * Spec；Linux 的 PCI_CLASS_BRIDGE_HOST 在 pci_ids.h 里，不是 UAPI） */
#define CLASS_HOST_BRIDGE   0x060000u

static void cfg_set8(struct pci_device *d, unsigned reg, uint8_t v)
{
    d->cfg[reg] = v;
}

static void cfg_set16(struct pci_device *d, unsigned reg, uint16_t v)
{
    d->cfg[reg]     = (uint8_t)v;
    d->cfg[reg + 1] = (uint8_t)(v >> 8);
}

static void cfg_set32(struct pci_device *d, unsigned reg, uint32_t v)
{
    cfg_set16(d, reg, (uint16_t)v);
    cfg_set16(d, reg + 2, (uint16_t)(v >> 16));
}

static void wmask_set16(struct pci_device *d, unsigned reg, uint16_t v)
{
    d->wmask[reg]     = (uint8_t)v;
    d->wmask[reg + 1] = (uint8_t)(v >> 8);
}

static void wmask_set32(struct pci_device *d, unsigned reg, uint32_t v)
{
    wmask_set16(d, reg, (uint16_t)v);
    wmask_set16(d, reg + 2, (uint16_t)(v >> 16));
}

static uint32_t cfg_get(const struct pci_device *d, unsigned reg, unsigned size)
{
    uint32_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++)
        v |= (uint32_t)d->cfg[reg + i] << (8 * i);
    return v;
}

static const char *bar_type_str(enum pci_bar_type t)
{
    return t == PCI_BAR_IO ? "io" : "mem";
}

/* ------------------------------------------------------------------ */
/* BAR decode                                                          */
/* ------------------------------------------------------------------ */

/* 根据 command 和 BAR 的当前值，算出这个 BAR 现在应当响应的地址。
 * 规范里 decode 只由 command 的 IO/MEM 位控制，BAR 的值是 0 也照样响应，
 * 这里不额外把 0 当作“未分配”，guest 编出什么就是什么。 */
static void bar_update(struct pci_device *d, int i)
{
    struct pci_bar *b = &d->bars[i];
    uint16_t cmd = (uint16_t)cfg_get(d, PCI_COMMAND, 2);
    uint32_t v = cfg_get(d, BAR_REG(i), 4);
    enum vmm_io_space space;
    uint64_t base;
    bool on;

    if (b->type == PCI_BAR_IO) {
        space = VMM_IO_PIO;
        on    = cmd & PCI_COMMAND_IO;
        base  = v & PCI_BASE_ADDRESS_IO_MASK;
    } else {
        space = VMM_IO_MMIO;
        on    = cmd & PCI_COMMAND_MEMORY;
        base  = v & PCI_BASE_ADDRESS_MEM_MASK;
    }
    if (on == b->mapped && (!on || base == b->base))
        return;

    vmm_io_move(d->bus->vm, space, b->io_slot, base, on ? b->size : 0);
    b->mapped = on;
    b->base   = base;
}

static void bars_update(struct pci_device *d)
{
    int i;

    for (i = 0; i < PCI_NUM_BARS; i++)
        if (d->bars[i].type != PCI_BAR_UNUSED)
            bar_update(d, i);
}

/* ------------------------------------------------------------------ */
/* INTx                                                                */
/* ------------------------------------------------------------------ */

/* 设备实际驱动到 INTx 线上的电平（PCI 3.0 第 6.2.2 节 Interrupt Disable、
 * 第 6.2.3 节 Interrupt Status；MSI 打开时不得使用 INTx，第 6.8.3.3 节） */
static bool intx_output(const struct pci_device *d)
{
    return d->intx_pin && d->intx_level &&
           !(cfg_get(d, PCI_COMMAND, 2) & PCI_COMMAND_INTX_DISABLE) &&
           !pci_msi_enabled(d);
}

/* 重新计算 d 所在那根线的电平，变了才通知 KVM */
static void intx_line_update(struct pci_bus *bus, const struct pci_device *d)
{
    int gsi = pci_intx_gsi(d->slot, d->intx_pin);
    int line = gsi - PCI_INTX_GSI_BASE;
    bool level = false;
    int s;

    for (s = 0; s < PCI_MAX_SLOTS && !level; s++) {
        const struct pci_device *o = bus->devs[s];

        if (o && o->intx_pin && pci_intx_gsi(s, o->intx_pin) == gsi)
            level = intx_output(o);
    }
    if (level == bus->intx_line[line])
        return;
    bus->intx_line[line] = level;
    irqchip_set_irq(bus->vm, gsi, level);
}

void pci_set_irq(struct pci_device *d, bool level)
{
    uint16_t st;

    if (!d->intx_pin) {
        vmm_err("pci 00:%02x.0 %s: pci_set_irq on a device without INTx",
                d->slot, d->name);
        return;
    }
    if (level == d->intx_level)
        return;
    d->intx_level = level;
    st = (uint16_t)cfg_get(d, PCI_STATUS, 2);
    st = level ? st | PCI_STATUS_INTERRUPT : st & ~PCI_STATUS_INTERRUPT;
    cfg_set16(d, PCI_STATUS, st);
    intx_line_update(d->bus, d);
}

/* ------------------------------------------------------------------ */
/* MSI                                                                 */
/* ------------------------------------------------------------------ */

#define MSI_CTRL    (PCI_MSI_CAP_OFFSET + PCI_MSI_FLAGS)
#define MSI_ADDR    (PCI_MSI_CAP_OFFSET + PCI_MSI_ADDRESS_LO)
#define MSI_ADDR_HI (PCI_MSI_CAP_OFFSET + PCI_MSI_ADDRESS_HI)
#define MSI_DATA    (PCI_MSI_CAP_OFFSET + PCI_MSI_DATA_64)

bool pci_msi_enabled(const struct pci_device *d)
{
    return d->msi && (cfg_get(d, MSI_CTRL, 2) & PCI_MSI_FLAGS_ENABLE);
}

static uint64_t msi_addr(const struct pci_device *d)
{
    return (uint64_t)cfg_get(d, MSI_ADDR_HI, 4) << 32 | cfg_get(d, MSI_ADDR, 4);
}

/* MSI Enable 的切换记一行日志：之后每条中断走哪条路，看这一行就知道 */
static void msi_note_enable(struct pci_device *d)
{
    bool on = pci_msi_enabled(d);

    if (on == d->msi_was_enabled)
        return;
    d->msi_was_enabled = on;
    if (on)
        vmm_info("pci 00:%02x.0 %s: MSI enabled, address 0x%llx data 0x%04x",
                 d->slot, d->name, (unsigned long long)msi_addr(d),
                 cfg_get(d, MSI_DATA, 2));
    else
        vmm_info("pci 00:%02x.0 %s: MSI disabled, interrupts go to INT%c",
                 d->slot, d->name, d->intx_pin ? 'A' + d->intx_pin - 1 : '-');
}

void pci_msi_notify(struct pci_device *d)
{
    uint64_t addr;
    uint32_t data;
    const char *why;

    if (!pci_msi_enabled(d)) {
        vmm_err("pci 00:%02x.0 %s: pci_msi_notify with MSI disabled",
                d->slot, d->name);
        return;
    }
    addr = msi_addr(d);
    /* 数据寄存器只有 16 位，消息的高 16 位恒为 0（第 6.8.3.4 节）；
     * MME 为 0，设备不改数据的低位（第 6.8.1.6 节） */
    data = cfg_get(d, MSI_DATA, 2);

    /* MSI 是设备发起的内存写，Bus Master 为 0 时不能发（第 6.2.2 节） */
    if (!(cfg_get(d, PCI_COMMAND, 2) & PCI_COMMAND_MASTER))
        why = "bus master disabled";
    else if (!(why = irqchip_msi_invalid(addr, data)) &&
             irqchip_signal_msi(d->bus->vm, addr, data, &why) == VMM_OK) {
        d->msi_sent++;
        d->msi_drop_run = 0;
        return;
    }

    d->msi_dropped++;
    if (d->msi_drop_run == 0 || addr != d->msi_drop_addr || data != d->msi_drop_data) {
        d->msi_drop_addr = addr;
        d->msi_drop_data = data;
        d->msi_drop_run  = 0;
    }
    d->msi_drop_run++;
    if ((d->msi_drop_run & (d->msi_drop_run - 1)) == 0)
        vmm_warn("pci 00:%02x.0 %s: MSI address 0x%llx data 0x%04x not "
                 "delivered: %s (%llu with this address/data, %llu in total)",
                 d->slot, d->name, (unsigned long long)addr, data, why,
                 (unsigned long long)d->msi_drop_run,
                 (unsigned long long)d->msi_dropped);
}

/* ------------------------------------------------------------------ */
/* DMA                                                                 */
/* ------------------------------------------------------------------ */

static bool dma_allowed(struct pci_device *d, uint64_t gpa, size_t len,
                        const char *dir)
{
    if (cfg_get(d, PCI_COMMAND, 2) & PCI_COMMAND_MASTER)
        return true;
    if (!d->dma_blocked_logged) {
        d->dma_blocked_logged = true;
        vmm_warn("pci 00:%02x.0 %s: DMA %s gpa 0x%llx len %zu with bus "
                 "master disabled, dropped (logged once)", d->slot, d->name,
                 dir, (unsigned long long)gpa, len);
    }
    return false;
}

int pci_dma_read(struct pci_device *d, uint64_t gpa, void *dst, size_t len)
{
    if (!dma_allowed(d, gpa, len, "read"))
        return VMM_ERR_GUEST;
    return mem_read(d->bus->vm, gpa, dst, len);
}

int pci_dma_write(struct pci_device *d, uint64_t gpa, const void *src,
                  size_t len)
{
    if (!dma_allowed(d, gpa, len, "write"))
        return VMM_ERR_GUEST;
    return mem_write(d->bus->vm, gpa, src, len);
}

int pci_dma_map(struct pci_device *d, uint64_t gpa, size_t len, void **hva)
{
    enum gpa_status st;
    void *p;

    if (!dma_allowed(d, gpa, len, "map"))
        return VMM_ERR_GUEST;
    st = gpa_to_hva(d->bus->vm, gpa, len, &p);
    if (st != GPA_OK) {
        vmm_err("pci 00:%02x.0 %s: DMA map GPA [0x%llx,+0x%zx): %s", d->slot,
                d->name, (unsigned long long)gpa, len, gpa_status_str(st));
        return VMM_ERR_GUEST;
    }
    *hva = p;
    return VMM_OK;
}

/* ------------------------------------------------------------------ */
/* 配置空间                                                            */
/* ------------------------------------------------------------------ */

static struct pci_device *find_dev(struct pci_bus *bus, uint8_t busno,
                                   uint8_t devfn)
{
    /* 只有 bus 0，每个 slot 只有 function 0。其余地址上没有设备应答，
     * 真实总线上表现为 master abort，读回全 1。 */
    if (busno != 0 || PCI_FUNC_OF(devfn) != 0)
        return NULL;
    return bus->devs[PCI_SLOT_OF(devfn)];
}

static bool access_ok(uint16_t reg, uint32_t size)
{
    return (size == 1 || size == 2 || size == 4) &&
           (reg & 3) + size <= 4 && reg + size <= PCI_CFG_SPACE_SIZE;
}

uint32_t pci_cfg_read(struct pci_bus *bus, uint8_t busno, uint8_t devfn,
                      uint16_t reg, uint32_t size)
{
    struct pci_device *d = find_dev(bus, busno, devfn);
    uint32_t ones = size >= 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1;

    if (!d || !access_ok(reg, size))
        return ones;
    return cfg_get(d, reg, size);
}

void pci_cfg_write(struct pci_bus *bus, uint8_t busno, uint8_t devfn,
                   uint16_t reg, uint32_t size, uint32_t val)
{
    struct pci_device *d = find_dev(bus, busno, devfn);
    unsigned i, end;

    if (!d || !access_ok(reg, size))
        return;
    for (i = 0; i < size; i++) {
        uint8_t m = d->wmask[reg + i];
        uint8_t b = (uint8_t)(val >> (8 * i));

        d->cfg[reg + i] = (uint8_t)((d->cfg[reg + i] & ~m) | (b & m));
    }

    /* command 或任一 BAR 被写到，decode 可能变了；command 还可能改了
     * INTx Disable，设备输出到线上的电平跟着变 */
    end = reg + size;
    if ((reg < PCI_COMMAND + 2 && end > PCI_COMMAND) ||
        (reg < PCI_BASE_ADDRESS_5 + 4 && end > PCI_BASE_ADDRESS_0))
        bars_update(d);
    if (d->intx_pin && reg < PCI_COMMAND + 2 && end > PCI_COMMAND)
        intx_line_update(bus, d);
    /* MSI Enable 变了，INTx 的输出跟着变：打开时撤下线上的请求，关掉时
     * 还挂着的请求回到线上 */
    if (d->msi && reg < MSI_CTRL + 2 && end > MSI_CTRL) {
        msi_note_enable(d);
        if (d->intx_pin)
            intx_line_update(bus, d);
    }
}

/* ------------------------------------------------------------------ */
/* conf1 端口                                                          */
/* ------------------------------------------------------------------ */

/* CONFIG_ADDRESS 只认 0xCF8 上的 32 位访问。规范规定其他宽度、其他偏移
 * 的访问按普通 I/O 处理，不碰 CONFIG_ADDRESS。Linux 的 pci_check_type1()
 * 就是这么判断 conf1 存在的：先 outb 到 0xCFB，再 outl 0x80000000 到
 * 0xCF8，读回同一个值才算数。这里那条 outb 没有设备接收：写丢弃，读全 1。 */
static int conf1_addr_read(void *opaque, uint64_t off, uint32_t size,
                           void *data)
{
    struct pci_bus *bus = opaque;

    if (off == 0 && size == 4)
        memcpy(data, &bus->conf1_addr, 4);
    else
        memset(data, 0xFF, size);
    return VMM_OK;
}

static int conf1_addr_write(void *opaque, uint64_t off, uint32_t size,
                            const void *data)
{
    struct pci_bus *bus = opaque;
    uint32_t v;

    if (off == 0 && size == 4) {
        memcpy(&v, data, 4);
        bus->conf1_addr = v & CONF1_WRITABLE;
    }
    return VMM_OK;
}

/* CONFIG_ADDRESS 选中的那个 dword 是否可访问。
 *
 * bit 30:24 规范定为保留。AMD 把 bit 27:24 用作扩展寄存器号（ECS，访问
 * 256 字节以上的配置空间），Linux 在 family >= 0x10 的 AMD/海光 CPU 上
 * 默认认为 ECS 已打开（arch/x86/pci/amd_bus.c pci_io_ecs_init），guest 跑
 * 在这类 host 上时可能发出带这些位的地址。这里的设备只有 256 字节配置
 * 空间，所以保留位非 0 一律按“没有这个寄存器”处理：读全 1、写丢弃。
 * 如果忽略这些位，扩展寄存器的写会落到低 256 字节的同偏移处，改坏
 * command 或 BAR。 */
static bool conf1_selected(const struct pci_bus *bus)
{
    return (bus->conf1_addr & CONF1_ENABLE) &&
           !(bus->conf1_addr & CONF1_RESERVED);
}

/* CONFIG_DATA 是 0xCFC-0xCFF 一个 dword 窗口，访问 0xCFC + n 对应选中
 * dword 的第 n 字节。跨出这 4 个端口的访问（比如 inl 0xCFE）不是合法的
 * 配置访问，按普通 I/O 处理。 */
static int conf1_data_read(void *opaque, uint64_t off, uint32_t size,
                           void *data)
{
    struct pci_bus *bus = opaque;
    uint32_t a = bus->conf1_addr, v;

    if (!conf1_selected(bus) || off + size > 4) {
        memset(data, 0xFF, size);
        return VMM_OK;
    }
    v = pci_cfg_read(bus, CONF1_BUS(a), CONF1_DEVFN(a),
                     (uint16_t)(CONF1_REG(a) + off), size);
    memcpy(data, &v, size);
    return VMM_OK;
}

static int conf1_data_write(void *opaque, uint64_t off, uint32_t size,
                            const void *data)
{
    struct pci_bus *bus = opaque;
    uint32_t a = bus->conf1_addr, v = 0;

    if (!conf1_selected(bus) || off + size > 4)
        return VMM_OK;
    memcpy(&v, data, size);
    pci_cfg_write(bus, CONF1_BUS(a), CONF1_DEVFN(a),
                  (uint16_t)(CONF1_REG(a) + off), size, v);
    return VMM_OK;
}

/* ------------------------------------------------------------------ */
/* 设备注册                                                            */
/* ------------------------------------------------------------------ */

static uint64_t align_up(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(a - 1);
}

/* 固件的那一步：给 BAR 分配地址。MMIO 和 IO 各一个游标，按 BAR 大小
 * 自然对齐（规范要求 BAR 地址按自身大小对齐，wmask 也保证了低位写不进去）。 */
static int bar_assign(struct pci_bus *bus, struct pci_device *d, int i)
{
    struct pci_bar *b = &d->bars[i];
    uint64_t base;

    if (b->type == PCI_BAR_IO) {
        base = align_up(bus->io_next, b->size);
        if (base + b->size > PCI_IO_WINDOW_END) {
            vmm_err("pci: %s BAR%d: io window exhausted (needs 0x%x bytes)",
                    d->name, i, b->size);
            return VMM_ERR_NOMEM;
        }
        bus->io_next = (uint32_t)(base + b->size);
        cfg_set32(d, BAR_REG(i), (uint32_t)base | PCI_BASE_ADDRESS_SPACE_IO);
    } else {
        base = align_up(bus->mmio_next, b->size);
        if (base + b->size > PCI_MMIO_WINDOW_END) {
            vmm_err("pci: %s BAR%d: mmio window exhausted (needs 0x%x bytes)",
                    d->name, i, b->size);
            return VMM_ERR_NOMEM;
        }
        bus->mmio_next = base + b->size;
        cfg_set32(d, BAR_REG(i), (uint32_t)base);
    }
    vmm_info("pci 00:%02x.0 %s BAR%d [%s 0x%llx-0x%llx]", d->slot, d->name, i,
             bar_type_str(b->type), (unsigned long long)base,
             (unsigned long long)(base + b->size - 1));
    return VMM_OK;
}

static int bar_setup(struct pci_bus *bus, struct pci_device *d, int i,
                     uint16_t *cmd_mask)
{
    struct pci_bar *b = &d->bars[i];
    struct vmm_io_dev io;
    uint32_t min = b->type == PCI_BAR_IO ? 4 : 16;
    int r;

    if (b->size < min || (b->size & (b->size - 1)) ||
        (b->type == PCI_BAR_IO && b->size > 256)) {
        /* IO BAR 上限 256 字节是规范的要求（PCI 3.0 第 6.2.5.1 节） */
        vmm_err("pci: %s BAR%d: invalid %s size 0x%x", d->name, i,
                bar_type_str(b->type), b->size);
        return VMM_ERR_INVAL;
    }

    snprintf(b->name, sizeof(b->name), "%s.bar%d", d->name, i);
    memset(&io, 0, sizeof(io));
    io.name   = b->name;
    io.opaque = b->opaque;
    io.read   = b->read;
    io.write  = b->write;
    r = vmm_io_reserve(bus->vm, b->type == PCI_BAR_IO ? VMM_IO_PIO : VMM_IO_MMIO,
                       &io, &b->io_slot);
    if (r != VMM_OK)
        return r;
    b->mapped = false;
    b->base   = 0;

    /* 地址位里低于 size 的部分不可写，类型位只读：guest 写全 1 读回的
     * 就是 ~(size - 1) | 类型位，这正是大小探测要的结果。
     * IO BAR 的高 16 位也只读为 0：x86 的端口空间只有 64K。 */
    if (b->type == PCI_BAR_IO) {
        wmask_set32(d, BAR_REG(i), ~(b->size - 1) & 0xFFFFu & (uint32_t)PCI_BASE_ADDRESS_IO_MASK);
        *cmd_mask |= PCI_COMMAND_IO;
    } else {
        wmask_set32(d, BAR_REG(i), ~(b->size - 1) & (uint32_t)PCI_BASE_ADDRESS_MEM_MASK);
        *cmd_mask |= PCI_COMMAND_MEMORY;
    }
    return bar_assign(bus, d, i);
}

/* MSI capability（第 6.7、6.8.1 节），形状见 pci.h 文件头。复位值：MSI
 * 关闭、MME 为 0、地址和数据为 0。地址的 bit 1:0 只读为 0（DWORD 对齐，
 * 第 6.8.1.4 节）。MME 按规范是可写的；软件写进大于 MMC 的值时设备照样
 * 只用一个向量，数据也不改。 */
static void msi_setup(struct pci_device *d)
{
    cfg_set16(d, PCI_STATUS, (uint16_t)(cfg_get(d, PCI_STATUS, 2) | PCI_STATUS_CAP_LIST));
    cfg_set8(d, PCI_CAPABILITY_LIST, PCI_MSI_CAP_OFFSET);
    cfg_set8(d, PCI_MSI_CAP_OFFSET + PCI_CAP_LIST_ID, PCI_CAP_ID_MSI);
    cfg_set8(d, PCI_MSI_CAP_OFFSET + PCI_CAP_LIST_NEXT, 0);
    cfg_set16(d, MSI_CTRL, PCI_MSI_FLAGS_64BIT);
    wmask_set16(d, MSI_CTRL, PCI_MSI_FLAGS_ENABLE | PCI_MSI_FLAGS_QSIZE);
    wmask_set32(d, MSI_ADDR, ~0x3u);
    wmask_set32(d, MSI_ADDR_HI, ~0u);
    wmask_set16(d, MSI_DATA, 0xFFFF);
}

/* 配置空间头（type 0）。只读字段直接写进 cfg，可写字段在 wmask 里置位。 */
static int dev_setup(struct pci_bus *bus, struct pci_device *d)
{
    uint16_t cmd_mask = 0;
    int i, r;

    memset(d->cfg, 0, sizeof(d->cfg));
    memset(d->wmask, 0, sizeof(d->wmask));

    cfg_set16(d, PCI_VENDOR_ID, d->vendor_id);
    cfg_set16(d, PCI_DEVICE_ID, d->device_id);
    cfg_set32(d, PCI_CLASS_REVISION, d->class_code << 8 | d->revision);
    cfg_set8(d, PCI_HEADER_TYPE, PCI_HEADER_TYPE_NORMAL);
    cfg_set16(d, PCI_SUBSYSTEM_VENDOR_ID, d->subsys_vendor_id);
    cfg_set16(d, PCI_SUBSYSTEM_ID, d->subsys_id);
    /* cache line size 由软件写、硬件只是记住（规范第 6.2.4 节） */
    d->wmask[PCI_CACHE_LINE_SIZE] = 0xFF;

    for (i = 0; i < PCI_NUM_BARS; i++) {
        if (d->bars[i].type == PCI_BAR_UNUSED)
            continue;
        r = bar_setup(bus, d, i, &cmd_mask);
        if (r != VMM_OK)
            return r;
    }

    if (d->intx_pin) {
        if (d->intx_pin > 4) {
            vmm_err("pci: %s: invalid interrupt pin %u", d->name, d->intx_pin);
            return VMM_ERR_INVAL;
        }
        cfg_set8(d, PCI_INTERRUPT_PIN, d->intx_pin);
        /* interrupt line 由固件写入，x86 上 0xFF 表示“未知/不接 8259”
         * （规范第 6.2.4 节）。这里的 INTx 只接到 IOAPIC 16 号以上的脚，
         * 不经过 8259，填 0xFF 是如实描述；guest 拿到的 IRQ 只能来自
         * _PRT 或 MP table，验收时才能证明这两张表真的起了作用。 */
        cfg_set8(d, PCI_INTERRUPT_LINE, 0xFF);
        d->wmask[PCI_INTERRUPT_LINE] = 0xFF;
        cmd_mask |= PCI_COMMAND_INTX_DISABLE;
    }

    if (d->msi)
        msi_setup(d);

    /* command 里只有设备真正具备的功能位可写，其余只读为 0
     * （规范第 6.2.2 节）。没有 DMA 能力的设备 bus master 位也只读；
     * 发 MSI 也是做 master，同样需要这一位。 */
    if (d->bus_master || d->msi)
        cmd_mask |= PCI_COMMAND_MASTER;
    wmask_set16(d, PCI_COMMAND, cmd_mask);
    d->intx_level         = false;
    d->dma_blocked_logged = false;
    d->msi_was_enabled    = false;
    d->msi_sent           = 0;
    d->msi_dropped        = 0;
    d->msi_drop_run       = 0;

    /* 固件分配完 BAR 后打开 decode：Linux 沿用固件的分配，前提是 BAR
     * 已经在响应。Bus Master 保持关闭，由驱动 pci_set_master 打开：
     * 驱动接管之前设备不该能写 guest 内存。 */
    cfg_set16(d, PCI_COMMAND, cmd_mask & (PCI_COMMAND_IO | PCI_COMMAND_MEMORY));
    bars_update(d);
    return VMM_OK;
}

int pci_register_device(struct pci_bus *bus, struct pci_device *dev)
{
    int slot, r;

    for (slot = 0; slot < PCI_MAX_SLOTS && bus->devs[slot]; slot++)
        ;
    if (slot == PCI_MAX_SLOTS) {
        vmm_err("pci: no free slot for %s", dev->name);
        return VMM_ERR_NOMEM;
    }
    dev->bus  = bus;
    dev->slot = slot;
    r = dev_setup(bus, dev);
    if (r != VMM_OK)
        return r;
    bus->devs[slot] = dev;
    vmm_info("pci 00:%02x.0 %s [%04x:%04x] class %06x%s%s", slot, dev->name,
             dev->vendor_id, dev->device_id, dev->class_code,
             dev->intx_pin ? "" : ", no INTx", dev->msi ? ", MSI capable" : "");
    if (dev->intx_pin)
        vmm_info("pci 00:%02x.0 %s INT%c -> GSI %d", slot, dev->name,
                 'A' + dev->intx_pin - 1, pci_intx_gsi(slot, dev->intx_pin));
    return VMM_OK;
}

int pci_bus_init(struct pci_bus *bus, struct vmm_vm *vm)
{
    struct pci_device *hb = &bus->host_bridge;
    struct vmm_io_dev io;
    int r;

    memset(bus, 0, sizeof(*bus));
    bus->vm        = vm;
    bus->mmio_next = PCI_MMIO_WINDOW_START;
    bus->io_next   = PCI_IO_ALLOC_START;

    memset(&io, 0, sizeof(io));
    io.name   = "pci-conf1-addr";
    io.base   = PCI_CONF1_ADDR_PORT;
    io.len    = 4;
    io.opaque = bus;
    io.read   = conf1_addr_read;
    io.write  = conf1_addr_write;
    r = vmm_register_pio(vm, &io);
    if (r != VMM_OK)
        return r;

    io.name   = "pci-conf1-data";
    io.base   = PCI_CONF1_DATA_PORT;
    io.read   = conf1_data_read;
    io.write  = conf1_data_write;
    r = vmm_register_pio(vm, &io);
    if (r != VMM_OK)
        return r;

    /* slot 0 的 host bridge 不止是装饰：没有 DMI 信息时（直启没有 SMBIOS），
     * Linux 的 pci_sanity_check() 要在 bus 0 上找到一个 class 为 host bridge
     * 或 VGA 的设备，才承认 conf1 可用；找不到就报
     * "PCI: Fatal: No config space access function found"。 */
    memset(hb, 0, sizeof(*hb));
    hb->name       = "host-bridge";
    hb->vendor_id  = PCI_VENDOR_ID_MINIVMM;
    hb->device_id  = PCI_DEVICE_ID_HOST_BRIDGE;
    hb->class_code = CLASS_HOST_BRIDGE;
    return pci_register_device(bus, hb);
}
