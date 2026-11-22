/* tests/unit/ioapic_test.c — 用户态 IOAPIC 的寄存器与中断语义
 *
 * 判据（oracle）：
 *   - 寄存器复位值、只读位、位域：ref/82093AA_IOAPIC_290566.pdf 第 3 章；
 *   - level 触发的 Remote IRR 握手、屏蔽期间的边沿丢弃：同上第 3.2.4 节
 *     bit 14、bit 16 的文字；
 *   - “masked + edge 再写回”清 Remote IRR：Linux 6.6
 *     arch/x86/kernel/apic/io_apic.c __eoi_ioapic_pin 的写序列，按原样回放；
 *   - 中断消息格式：Intel SDM Vol.3 第 13.11.1、13.11.2 节，期望值在用例里
 *     手工按位拼出，不调用被测的 ioapic_rte_msi。
 *
 * 所有访问都经过 MMIO 回调（IOREGSEL 写下标、IOWIN 读写数据），和 guest
 * 驱动走同一条路。投递回调是一个记录器，可以设定“有几个 LAPIC 收下”。
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "vmm.h"
#include "devices/ioapic.h"
#include "harness.h"

int vmm_log_level = -1;

void vmm_log(int level, const char *file, int line, const char *fmt, ...)
{
    (void)level; (void)file; (void)line; (void)fmt;
}

/* ------------------------------------------------------------------ */
/* 记录器                                                              */
/* ------------------------------------------------------------------ */

#define MAX_MSGS 16

static struct {
    int      n;
    uint64_t addr[MAX_MSGS];
    uint32_t data[MAX_MSGS];
    int      accept;            /* deliver 的返回值 */
    int      changed;           /* rte_changed 被调用的次数 */
    int      changed_pin;
} rec;

static int fake_deliver(void *opaque, uint64_t addr, uint32_t data)
{
    (void)opaque;
    if (rec.n < MAX_MSGS) {
        rec.addr[rec.n] = addr;
        rec.data[rec.n] = data;
    }
    rec.n++;
    return rec.accept;
}

static void fake_changed(void *opaque, int pin)
{
    (void)opaque;
    rec.changed++;
    rec.changed_pin = pin;
}

static const struct ioapic_ops ops = {
    .deliver = fake_deliver,
    .rte_changed = fake_changed,
};

static struct ioapic io;

static void fresh(void)
{
    memset(&rec, 0, sizeof(rec));
    rec.accept = 1;
    ioapic_init(&io, &ops, NULL);
}

/* ------------------------------------------------------------------ */
/* guest 视角的寄存器访问                                              */
/* ------------------------------------------------------------------ */

static uint32_t reg_read(uint8_t sel)
{
    uint32_t s = sel, v = 0xDEADBEEF;

    ioapic_mmio_write(&io, 0x00, 4, &s);
    ioapic_mmio_read(&io, 0x10, 4, &v);
    return v;
}

static void reg_write(uint8_t sel, uint32_t v)
{
    uint32_t s = sel;

    ioapic_mmio_write(&io, 0x00, 4, &s);
    ioapic_mmio_write(&io, 0x10, 4, &v);
}

static uint32_t rte_lo(int pin) { return reg_read(0x10 + 2 * pin); }
static uint32_t rte_hi(int pin) { return reg_read(0x11 + 2 * pin); }

/* Linux ioapic_write_entry 的顺序：先写高 32 位再写低 32 位 */
static void rte_write(int pin, uint32_t hi, uint32_t lo)
{
    reg_write(0x11 + 2 * pin, hi);
    reg_write(0x10 + 2 * pin, lo);
}

#define LO_LEVEL   (1u << 15)
#define LO_MASK    (1u << 16)
#define LO_RIRR    (1u << 14)
#define LO_LOGICAL (1u << 11)
#define DM(x)      ((uint32_t)(x) << 8)

/* ------------------------------------------------------------------ */

static void test_reset_values(void)
{
    T_CASE("reset: VER 00170011h, ID/ARB 0, every RTE masked");
    fresh();
    CHECK_U64(reg_read(0x01), 0x00170011, "IOAPICVER");
    CHECK_U64(reg_read(0x00), 0, "IOAPICID");
    CHECK_U64(reg_read(0x02), 0, "IOAPICARB");
    for (int pin = 0; pin < IOAPIC_NUM_PINS; pin++) {
        CHECK_U64(rte_lo(pin), LO_MASK, "RTE %d low", pin);
        CHECK_U64(rte_hi(pin), 0, "RTE %d high", pin);
    }
    CHECK_U64(reg_read(0x03), 0, "undefined index 03h");
    CHECK_U64(reg_read(0x40), 0, "index past RTE 23");
}

static void test_id_and_readonly(void)
{
    T_CASE("IOAPICID keeps 27:24 only, ARB follows it, VER is read-only");
    fresh();
    reg_write(0x00, 0xFFFFFFFF);
    CHECK_U64(reg_read(0x00), 0x0F000000, "IOAPICID after writing all ones");
    CHECK_U64(reg_read(0x02), 0x0F000000, "IOAPICARB loaded with the ID");
    reg_write(0x01, 0);
    CHECK_U64(reg_read(0x01), 0x00170011, "IOAPICVER after a write");
    reg_write(0x02, 0);
    CHECK_U64(reg_read(0x02), 0x0F000000, "IOAPICARB after a write");
}

static void test_rte_bits(void)
{
    T_CASE("RTE: delivery status, remote IRR and reserved bits are not writable");
    fresh();
    rte_write(3, 0xFFFFFFFF, 0xFFFFFFFF);
    /* 0-11, 13, 15, 16 可写；12、14 只读且此时为 0；17-31 保留 */
    CHECK_U64(rte_lo(3), 0x0001AFFF, "low dword after writing all ones");
    CHECK_U64(rte_hi(3), 0xFF000000, "high dword after writing all ones");
    CHECK_U64(rte_lo(4), LO_MASK, "neighbouring RTE untouched");
}

static void test_msi_encoding(void)
{
    T_CASE("message: physical fixed edge, dest 0x1f vector 0x31");
    fresh();
    rte_write(1, 0x1F000000, 0x31);
    ioapic_set_irq(&io, 1, 1);
    CHECK_U64(rec.n, 1, "messages");
    CHECK_U64(rec.addr[0], 0xFEE1F000, "address: dest in 19:12, DM=0");
    CHECK_U64(rec.data[0], 0x4031, "data: vector, fixed, edge, assert");

    T_CASE("message: logical lowest-priority level, dest 0x03 vector 0x41");
    fresh();
    rte_write(2, 0x03000000, 0x41 | DM(1) | LO_LOGICAL | LO_LEVEL);
    ioapic_set_irq(&io, 2, 1);
    CHECK_U64(rec.n, 1, "messages");
    CHECK_U64(rec.addr[0], 0xFEE03004, "address: dest 0x03, DM=1 in bit 2");
    CHECK_U64(rec.data[0], 0xC141, "data: vector, lowest priority, level, assert");

    T_CASE("message: NMI programmed level is still sent edge, no remote IRR");
    fresh();
    rte_write(5, 0, DM(4) | LO_LEVEL);
    ioapic_set_irq(&io, 5, 1);
    CHECK_U64(rec.n, 1, "messages");
    CHECK_U64(rec.data[0], 0x4400, "data: NMI, edge");
    CHECK(!(rte_lo(5) & LO_RIRR), "remote IRR set on an NMI entry");
    ioapic_set_irq(&io, 5, 1);
    CHECK_U64(rec.n, 1, "held line must not resend an edge-treated entry");
}

static void test_edge(void)
{
    T_CASE("edge: one message per rising edge");
    fresh();
    rte_write(4, 0, 0x30);
    ioapic_set_irq(&io, 4, 1);
    ioapic_set_irq(&io, 4, 1);
    CHECK_U64(rec.n, 1, "line held high");
    ioapic_set_irq(&io, 4, 0);
    CHECK_U64(rec.n, 1, "falling edge");
    ioapic_set_irq(&io, 4, 1);
    CHECK_U64(rec.n, 2, "second rising edge");
    CHECK_U64(io.delivered[4], 2, "delivered count");
    CHECK(!(rte_lo(4) & LO_RIRR), "remote IRR on an edge entry");

    T_CASE("edge: masked edges are dropped, not held for unmask");
    fresh();
    rte_write(4, 0, LO_MASK | 0x30);
    ioapic_set_irq(&io, 4, 1);
    CHECK_U64(rec.n, 0, "edge on a masked pin");
    rte_write(4, 0, 0x30);
    CHECK_U64(rec.n, 0, "unmask with the line still high");
    ioapic_set_irq(&io, 4, 0);
    ioapic_set_irq(&io, 4, 1);
    CHECK_U64(rec.n, 1, "next rising edge after unmask");

    T_CASE("edge: nobody accepted counts as lost");
    fresh();
    rec.accept = 0;
    rte_write(4, 0, 0x30);
    ioapic_set_irq(&io, 4, 1);
    CHECK_U64(io.lost[4], 1, "lost count");
    CHECK_U64(io.delivered[4], 0, "delivered count");
}

static void test_level(void)
{
    T_CASE("level: remote IRR blocks resend until a matching EOI");
    fresh();
    rte_write(10, 0, 0x50 | LO_LEVEL);
    ioapic_set_irq(&io, 10, 1);
    CHECK_U64(rec.n, 1, "first assert");
    CHECK(rte_lo(10) & LO_RIRR, "remote IRR after the LAPIC accepted");
    ioapic_set_irq(&io, 10, 1);
    CHECK_U64(rec.n, 1, "assert again while remote IRR is set");
    ioapic_eoi(&io, 0x51);
    CHECK_U64(rec.n, 1, "EOI for another vector");
    CHECK(rte_lo(10) & LO_RIRR, "remote IRR after another vector's EOI");
    ioapic_eoi(&io, 0x50);
    CHECK_U64(rec.n, 2, "EOI with the line still asserted resends");
    CHECK(rte_lo(10) & LO_RIRR, "remote IRR set again by the resend");
    ioapic_set_irq(&io, 10, 0);
    ioapic_eoi(&io, 0x50);
    CHECK_U64(rec.n, 2, "EOI after the device deasserted");
    CHECK(!(rte_lo(10) & LO_RIRR), "remote IRR after the last EOI");

    T_CASE("level: masked assert sends on unmask, EOI clears a masked entry");
    fresh();
    rte_write(11, 0, LO_MASK | 0x52 | LO_LEVEL);
    ioapic_set_irq(&io, 11, 1);
    CHECK_U64(rec.n, 0, "assert on a masked pin");
    rte_write(11, 0, 0x52 | LO_LEVEL);
    CHECK_U64(rec.n, 1, "unmask with the line asserted");
    reg_write(0x10 + 2 * 11, LO_MASK | 0x52 | LO_LEVEL);
    ioapic_eoi(&io, 0x52);
    CHECK(!(rte_lo(11) & LO_RIRR), "EOI must clear remote IRR on a masked entry");
    CHECK_U64(rec.n, 1, "no send while masked");
    reg_write(0x10 + 2 * 11, 0x52 | LO_LEVEL);
    CHECK_U64(rec.n, 2, "unmask again, line still asserted");

    T_CASE("level: not accepted leaves remote IRR clear, so EOI-less retry works");
    fresh();
    rec.accept = 0;
    rte_write(12, 0, 0x53 | LO_LEVEL);
    ioapic_set_irq(&io, 12, 1);
    CHECK_U64(io.lost[12], 1, "lost count");
    CHECK(!(rte_lo(12) & LO_RIRR), "remote IRR without an accepting LAPIC");
    rec.accept = 1;
    rte_write(12, 0x01000000, 0x53 | LO_LEVEL);
    CHECK_U64(io.delivered[12], 1, "retargeting the entry resends");
}

/* Linux __eoi_ioapic_pin，版本 < 20h 的分支：读出表项，写一份 masked +
 * edge 的副本，再把原表项写回。 */
static void linux_eoi_ioapic_pin(int pin)
{
    uint32_t hi = rte_hi(pin), lo = rte_lo(pin);
    uint32_t lo1 = (lo | LO_MASK) & ~LO_LEVEL;

    rte_write(pin, hi, lo1);
    rte_write(pin, hi, lo);
}

static void test_edge_switch(void)
{
    T_CASE("Linux mask+edge rewrite clears a stuck remote IRR");
    fresh();
    rte_write(9, 0, 0x60 | LO_LEVEL);
    ioapic_set_irq(&io, 9, 1);
    ioapic_set_irq(&io, 9, 0);
    CHECK(rte_lo(9) & LO_RIRR, "precondition: remote IRR set, no EOI coming");
    linux_eoi_ioapic_pin(9);
    CHECK(!(rte_lo(9) & LO_RIRR), "remote IRR after the edge round trip");
    CHECK_U64(rec.n, 1, "line is deasserted, nothing to resend");

    T_CASE("Linux mask+edge rewrite with the line still asserted resends");
    fresh();
    rte_write(9, 0, 0x60 | LO_LEVEL);
    ioapic_set_irq(&io, 9, 1);
    linux_eoi_ioapic_pin(9);
    CHECK_U64(rec.n, 2, "resend after restoring the level entry");
    CHECK(rte_lo(9) & LO_RIRR, "remote IRR set by the resend");
}

static void test_rte_changed(void)
{
    T_CASE("rte_changed fires on a real change only");
    fresh();
    rte_write(7, 0, 0x70);
    CHECK_U64(rec.changed, 1, "low dword changed (high write was a no-op)");
    CHECK_U64(rec.changed_pin, 7, "pin");
    rte_write(7, 0, 0x70);
    CHECK_U64(rec.changed, 1, "identical rewrite");
    reg_write(0x11 + 2 * 7, 0x02000000);
    CHECK_U64(rec.changed, 2, "destination changed");
    rte_write(7, 0x02000000, 0x70 | (1u << 12) | LO_RIRR);
    CHECK_U64(rec.changed, 2, "writing only read-only bits");
}

static void test_access_width(void)
{
    T_CASE("IOREGSEL takes a byte write, IOWIN ignores non-dword access");
    fresh();
    uint8_t b = 0x01;
    uint32_t v = 0;
    ioapic_mmio_write(&io, 0x00, 1, &b);
    ioapic_mmio_read(&io, 0x10, 4, &v);
    CHECK_U64(v, 0x00170011, "IOWIN after a byte IOREGSEL write");
    uint16_t h = 0xBEEF;
    ioapic_mmio_read(&io, 0x10, 2, &h);
    CHECK_U64(h, 0, "16-bit IOWIN read");
    b = 0x10;
    ioapic_mmio_write(&io, 0x00, 1, &b);
    h = 0;
    ioapic_mmio_write(&io, 0x10, 2, &h);
    CHECK_U64(rte_lo(0), LO_MASK, "16-bit IOWIN write must not unmask RTE 0");
    v = 0xFFFFFF01;
    ioapic_mmio_write(&io, 0x00, 4, &v);
    ioapic_mmio_read(&io, 0x00, 4, &v);
    CHECK_U64(v, 0x01, "IOREGSEL bits 31:8 are reserved");
}

static void test_reset_after_use(void)
{
    T_CASE("reset: entries masked again, line state forgotten");
    fresh();
    reg_write(0x00, 0x05000000);
    rte_write(10, 0x01000000, 0x50 | LO_LEVEL);
    ioapic_set_irq(&io, 10, 1);
    ioapic_reset(&io);
    CHECK_U64(reg_read(0x00), 0, "IOAPICID");
    CHECK_U64(rte_lo(10), LO_MASK, "RTE 10 low");
    CHECK_U64(rte_hi(10), 0, "RTE 10 high");
    rte_write(10, 0, 0x50 | LO_LEVEL);
    CHECK_U64(rec.n, 1, "unmask after reset must not see the old line level");
}

int main(int argc, char **argv)
{
    T_MAIN_BEGIN();
    test_reset_values();
    test_id_and_readonly();
    test_rte_bits();
    test_msi_encoding();
    test_edge();
    test_level();
    test_edge_switch();
    test_rte_changed();
    test_access_width();
    test_reset_after_use();
    return T_SUMMARY();
}
