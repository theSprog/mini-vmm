/* src/devices/ioapic.c — 用户态 IOAPIC（Intel 82093AA），接口见
 * include/devices/ioapic.h。节号均指 ref/82093AA_IOAPIC_290566.pdf。 */
#include <string.h>

#include "vmm.h"
#include "devices/ioapic.h"

/* RTE 里 guest 可写的位（第 3.2.4 节）：低 32 位的 vector、投递模式、
 * 目标模式、极性、触发模式、mask，高 32 位的目标。Delivery Status 和
 * Remote IRR 只读，写入不影响它们；55:17 是保留位，读回 0。 */
#define RTE_RW_LO  (0xFFFull | IOAPIC_RTE_INTPOL | IOAPIC_RTE_LEVEL | \
                    IOAPIC_RTE_MASK)
#define RTE_RW_HI  (0xFFull << 56)

/* 第 3.2.4 节 Default Value “xxx1 xxxx xxxx xxxxh”：只有 mask 位有定义，
 * 其余未定义的位取 0。 */
#define RTE_RESET  IOAPIC_RTE_MASK

void ioapic_init(struct ioapic *io, const struct ioapic_ops *ops, void *opaque)
{
    memset(io, 0, sizeof(*io));
    io->ops = ops;
    io->opaque = opaque;
    ioapic_reset(io);
}

/* ID 复位成 0（第 3.2.1 节 Default Value 00h），不是 MADT 里登记的值。
 * 64 位 Linux 只用 MADT 里的 ID，只有多个 IOAPIC 的 MADT ID 冲突时才读
 * 这个寄存器（io_apic.c io_apic_unique_id），而这里只有一个 IOAPIC。 */
void ioapic_reset(struct ioapic *io)
{
    io->ioregsel = 0;
    io->id = 0;
    io->line = 0;
    for (int i = 0; i < IOAPIC_NUM_PINS; i++)
        io->redtbl[i] = RTE_RESET;
}

static void send(struct ioapic *io, int pin)
{
    uint64_t rte = io->redtbl[pin];
    uint64_t addr;
    uint32_t data;
    int n;

    ioapic_rte_msi(rte, &addr, &data);
    n = io->ops->deliver(io->opaque, addr, data);
    if (n <= 0) {
        io->lost[pin]++;
        return;
    }
    io->delivered[pin]++;
    /* Remote IRR 在 LAPIC 收下 level 中断时置位（bit 14）。没人收的时候
     * 不置位：置了也不会有 EOI 来清它，这根引脚就永远发不出中断了。 */
    if (ioapic_rte_is_level(rte))
        io->redtbl[pin] |= IOAPIC_RTE_REMOTE_IRR;
}

/* level 表项的发送条件：未屏蔽、引脚被断言、上一次投递已经 EOI。
 * level 是状态而不是事件，任何可能让条件变成真的时刻（引脚断言、
 * 解除屏蔽、EOI）都要重新评估一次。边沿表项只在 set_irq 看到上升沿
 * 时发送，这里不处理。 */
static void service_level(struct ioapic *io, int pin)
{
    uint64_t rte = io->redtbl[pin];

    if (!ioapic_rte_is_level(rte) || (rte & IOAPIC_RTE_MASK) ||
        (rte & IOAPIC_RTE_REMOTE_IRR) || !(io->line & (1u << pin)))
        return;
    send(io, pin);
}

static uint32_t read_indirect(struct ioapic *io)
{
    uint8_t sel = io->ioregsel;

    switch (sel) {
    case IOAPIC_REG_ID:
    /* 仲裁 ID 在写 IOAPICID 时装入同一个值（第 3.2.3 节）；APIC 总线
     * 仲裁时的轮转在没有 APIC 总线的虚拟平台上不存在，所以两者始终相等。 */
    case IOAPIC_REG_ARB:
        return (uint32_t)io->id << 24;
    case IOAPIC_REG_VER:
        return IOAPIC_VERSION;
    }
    if (sel >= IOAPIC_REG_REDTBL &&
        sel < IOAPIC_REG_REDTBL + 2 * IOAPIC_NUM_PINS) {
        uint64_t rte = io->redtbl[(sel - IOAPIC_REG_REDTBL) / 2];
        return (sel & 1) ? (uint32_t)(rte >> 32) : (uint32_t)rte;
    }
    /* 03h-0Fh 和 40h 以上数据手册没有定义寄存器 */
    return 0;
}

static void write_rte(struct ioapic *io, int pin, bool hi, uint32_t val)
{
    uint64_t old = io->redtbl[pin];
    uint64_t rte;

    if (hi)
        rte = (old & ~RTE_RW_HI) | (((uint64_t)val << 32) & RTE_RW_HI);
    else
        rte = (old & ~RTE_RW_LO) | (val & RTE_RW_LO);

    /* 切成边沿触发时清 Remote IRR。数据手册只说它对边沿表项无定义；
     * 版本 11h 没有 EOI 寄存器，Linux 清一根卡住的 level 引脚的办法是
     * 先写成 masked + edge 再写回原值（io_apic.c __eoi_ioapic_pin，
     * clear_IO_APIC_pin 在启动时对每根引脚都可能走这条路），依赖的就是
     * 真实芯片这个行为。不清的话 Linux 会打印
     * "Unable to reset IRR for apic"，那根引脚再也收不到中断。 */
    if (!(rte & IOAPIC_RTE_LEVEL))
        rte &= ~IOAPIC_RTE_REMOTE_IRR;

    io->redtbl[pin] = rte;
    if (rte != old && io->ops->rte_changed)
        io->ops->rte_changed(io->opaque, pin);
    service_level(io, pin);
}

static void write_indirect(struct ioapic *io, uint32_t val)
{
    uint8_t sel = io->ioregsel;

    if (sel == IOAPIC_REG_ID) {
        io->id = (val >> 24) & 0xF;
        return;
    }
    if (sel >= IOAPIC_REG_REDTBL &&
        sel < IOAPIC_REG_REDTBL + 2 * IOAPIC_NUM_PINS) {
        write_rte(io, (sel - IOAPIC_REG_REDTBL) / 2, sel & 1, val);
        return;
    }
    /* IOAPICVER、IOAPICARB 只读，其余地址没有寄存器 */
}

/* 访问宽度：IOREGSEL 允许字节写（第 3.0 节“a byte memory write ... loads
 * the IOREGSEL Register”），这里对任何宽度都只取 bit 7:0；IOWIN 必须按
 * DWORD 访问（同一节“The IOWIN Register must be accessed as a Dword
 * quantity”），其余宽度数据手册没有定义结果，读回 0、写入丢弃并报出
 * offset 和宽度，因为合规的驱动不会这样访问。 */
int ioapic_mmio_read(void *opaque, uint64_t offset, uint32_t size, void *data)
{
    struct ioapic *io = opaque;
    uint32_t v = 0;

    if (offset == 0x00) {
        v = io->ioregsel;
    } else if (offset == 0x10 && size == 4) {
        v = read_indirect(io);
    } else if (offset >= 0x10 && offset < 0x14) {
        vmm_warn("ioapic: %u-byte read at IOWIN offset 0x%llx ignored (IOWIN is dword-only)",
                 size, (unsigned long long)offset);
    }
    memset(data, 0, size);
    memcpy(data, &v, size < 4 ? size : 4);
    return VMM_OK;
}

int ioapic_mmio_write(void *opaque, uint64_t offset, uint32_t size,
                      const void *data)
{
    struct ioapic *io = opaque;
    uint32_t v = 0;

    memcpy(&v, data, size < 4 ? size : 4);
    if (offset == 0x00) {
        io->ioregsel = (uint8_t)v;
    } else if (offset == 0x10 && size == 4) {
        write_indirect(io, v);
    } else if (offset >= 0x10 && offset < 0x14) {
        vmm_warn("ioapic: %u-byte write 0x%x at IOWIN offset 0x%llx ignored (IOWIN is dword-only)",
                 size, v, (unsigned long long)offset);
    }
    return VMM_OK;
}

void ioapic_set_irq(struct ioapic *io, int pin, int level)
{
    uint32_t bit;
    bool was;
    uint64_t rte;

    if (pin < 0 || pin >= IOAPIC_NUM_PINS)
        return;
    bit = 1u << pin;
    was = io->line & bit;
    if (level)
        io->line |= bit;
    else
        io->line &= ~bit;

    rte = io->redtbl[pin];
    if (ioapic_rte_is_level(rte)) {
        service_level(io, pin);
        return;
    }
    /* 边沿：屏蔽期间的边沿直接丢弃，不挂起（第 3.2.4 节 bit 16），所以
     * 解除屏蔽时不补发。电平记录照常更新，下一个上升沿才认得出来。 */
    if (level && !was && !(rte & IOAPIC_RTE_MASK))
        send(io, pin);
}

void ioapic_eoi(struct ioapic *io, uint8_t vector)
{
    for (int pin = 0; pin < IOAPIC_NUM_PINS; pin++) {
        uint64_t rte = io->redtbl[pin];

        if (IOAPIC_RTE_VECTOR(rte) != vector || !ioapic_rte_is_level(rte) ||
            !(rte & IOAPIC_RTE_REMOTE_IRR))
            continue;
        io->redtbl[pin] = rte & ~IOAPIC_RTE_REMOTE_IRR;
        service_level(io, pin);
    }
}
