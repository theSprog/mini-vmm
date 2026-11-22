/* tests/unit/pic_test.c — 用户态 8259A 对的寄存器与中断语义
 *
 * 判据（oracle）：
 *   - 芯片语义：ref/8259A_PIC_231468.pdf。边沿检测、IRR 透明锁存、ICW1
 *     的副作用来自图 9 和第 9-10 页 a-f；OCW2/OCW3 编码来自图 8；
 *   - ELCR 和 PIIX4 的固定连线：ref/82371AB_PIIX4_290562.pdf 第 4.2.2 节；
 *   - Linux 6.6 arch/x86/kernel/i8259.c 的 init_8259A、mask_and_ack_8259A、
 *     probe_8259A、i8259A_irq_real，按原样回放端口序列；
 *   - 期望向量按 8086 模式的格式（ICW2 bit 7:3 加 IR 号）手工写出。
 *
 * 所有访问都经过 PIO 回调，INTA 经 i8259_inta，INT 输出经回调记录。
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "vmm.h"
#include "devices/i8259.h"
#include "harness.h"

int vmm_log_level = -1;

void vmm_log(int level, const char *file, int line, const char *fmt, ...)
{
    (void)level; (void)file; (void)line; (void)fmt;
}

static struct {
    int level;          /* INT 输出当前电平 */
    int rises;          /* 上升沿次数 */
} rec;

static void fake_int(void *opaque, int level)
{
    (void)opaque;
    if (level && !rec.level)
        rec.rises++;
    rec.level = level;
}

static const struct i8259_ops ops = { .int_changed = fake_int };
static struct i8259 pic;

/* ------------------------------------------------------------------ */
/* 端口访问（Linux 的 outb/inb）                                       */
/* ------------------------------------------------------------------ */

#define PIC_MASTER_CMD  0x20
#define PIC_MASTER_IMR  0x21
#define PIC_SLAVE_CMD   0xA0
#define PIC_SLAVE_IMR   0xA1

static void outb(uint8_t v, uint16_t port)
{
    if (port >= 0x4D0)
        i8259_elcr_write(&pic, port - 0x4D0, 1, &v);
    else
        i8259_pio_write(&pic.chip[port >= 0xA0], port & 1, 1, &v);
}

static uint8_t inb(uint16_t port)
{
    uint8_t v = 0;

    if (port >= 0x4D0)
        i8259_elcr_read(&pic, port - 0x4D0, 1, &v);
    else
        i8259_pio_read(&pic.chip[port >= 0xA0], port & 1, 1, &v);
    return v;
}

/* Linux 6.6：ISA_IRQ_VECTOR(irq) = ((FIRST_EXTERNAL_VECTOR + 16) & ~15) + irq，
 * FIRST_EXTERNAL_VECTOR = 0x20，所以主片 0x30、从片 0x38 */
#define ISA_IRQ_VECTOR(i)   (0x30 + (i))
#define MASTER_ICW4_DEFAULT 0x01
#define SLAVE_ICW4_DEFAULT  0x01
#define PIC_ICW4_AEOI       0x02

/* init_8259A 的写序列，掩码最后恢复成 cached mask */
static void linux_init_8259A(int auto_eoi, uint8_t mmask, uint8_t smask)
{
    outb(0xff, PIC_MASTER_IMR);
    outb(0x11, PIC_MASTER_CMD);
    outb(ISA_IRQ_VECTOR(0), PIC_MASTER_IMR);
    outb(1u << I8259_CASCADE_IR, PIC_MASTER_IMR);
    outb(auto_eoi ? MASTER_ICW4_DEFAULT | PIC_ICW4_AEOI : MASTER_ICW4_DEFAULT,
         PIC_MASTER_IMR);
    outb(0x11, PIC_SLAVE_CMD);
    outb(ISA_IRQ_VECTOR(8), PIC_SLAVE_IMR);
    outb(I8259_CASCADE_IR, PIC_SLAVE_IMR);
    outb(SLAVE_ICW4_DEFAULT, PIC_SLAVE_IMR);
    outb(mmask, PIC_MASTER_IMR);
    outb(smask, PIC_SLAVE_IMR);
}

/* mask_and_ack_8259A 的 EOI 部分（handle_real_irq） */
static void linux_ack(int irq)
{
    if (irq & 8) {
        outb(0x60 + (irq & 7), PIC_SLAVE_CMD);
        outb(0x60 + I8259_CASCADE_IR, PIC_MASTER_CMD);
    } else {
        outb(0x60 + irq, PIC_MASTER_CMD);
    }
}

/* i8259A_irq_real：读 ISR 再切回 IRR */
static int linux_irq_real(int irq)
{
    int v;

    if (irq < 8) {
        outb(0x0B, PIC_MASTER_CMD);
        v = inb(PIC_MASTER_CMD) & (1 << irq);
        outb(0x0A, PIC_MASTER_CMD);
        return v;
    }
    outb(0x0B, PIC_SLAVE_CMD);
    v = inb(PIC_SLAVE_CMD) & ((1 << irq) >> 8);
    outb(0x0A, PIC_SLAVE_CMD);
    return v;
}

static uint8_t read_isr(uint16_t cmd)
{
    uint8_t v;

    outb(0x0B, cmd);
    v = inb(cmd);
    outb(0x0A, cmd);
    return v;
}

static uint8_t read_irr(uint16_t cmd)
{
    outb(0x0A, cmd);
    return inb(cmd);
}

static void fresh(void)
{
    memset(&rec, 0, sizeof(rec));
    i8259_init(&pic, &ops, NULL);
}

/* 只放开 cascade 和给定 IRQ 的掩码 */
static void fresh_linux(int auto_eoi, uint16_t unmasked)
{
    uint16_t mask = ~(unmasked | (1u << I8259_CASCADE_IR));

    fresh();
    linux_init_8259A(auto_eoi, mask & 0xFF, mask >> 8);
}

/* ------------------------------------------------------------------ */

static void test_reset_and_probe(void)
{
    T_CASE("reset: IMR/IRR/ISR/ELCR 0, INT low; Linux probe reads its mask back");
    fresh();
    CHECK_U64(inb(PIC_MASTER_IMR), 0, "master IMR");
    CHECK_U64(inb(PIC_SLAVE_IMR), 0, "slave IMR");
    CHECK_U64(inb(PIC_MASTER_CMD), 0, "master IRR (default read)");
    CHECK_U64(inb(0x4D0), 0, "ELCR1");
    CHECK_U64(inb(0x4D1), 0, "ELCR2");
    CHECK(!rec.level, "INT high after reset");
    outb(0xff, PIC_SLAVE_IMR);
    outb(0xFB, PIC_MASTER_IMR);
    CHECK_U64(inb(PIC_MASTER_IMR), 0xFB, "probe_8259A read-back");
}

static void test_master_irq(void)
{
    T_CASE("master IRQ4: INT, vector 0x34, held edge does not repeat, specific EOI");
    fresh_linux(0, 1u << 4);
    i8259_set_irq(&pic, 4, 1);
    CHECK(rec.level, "INT after IRQ4 asserted");
    CHECK_U64(i8259_inta(&pic), 0x34, "vector");
    CHECK(!rec.level, "INT after INTA");
    CHECK_U64(read_isr(PIC_MASTER_CMD), 0x10, "master ISR");
    CHECK_U64(read_irr(PIC_MASTER_CMD), 0, "edge IRR cleared by INTA");
    i8259_set_irq(&pic, 4, 1);
    linux_ack(4);
    CHECK_U64(read_isr(PIC_MASTER_CMD), 0, "master ISR after specific EOI");
    CHECK(!rec.level, "held line after EOI (no new edge)");
    i8259_set_irq(&pic, 4, 0);
    i8259_set_irq(&pic, 4, 1);
    CHECK(rec.level, "next rising edge");
    CHECK_U64(pic.acked[4], 1, "acked count before the second INTA");
}

static void test_cascade(void)
{
    T_CASE("slave IRQ12 through master IR2: vector 0x3c, both ISRs, Linux ack");
    fresh_linux(0, 1u << 12);
    i8259_set_irq(&pic, 12, 1);
    CHECK(rec.level, "INT after IRQ12 asserted");
    CHECK_U64(i8259_inta(&pic), 0x3C, "vector from the slave");
    CHECK_U64(read_isr(PIC_MASTER_CMD), 0x04, "master ISR (cascade IR2)");
    CHECK_U64(read_isr(PIC_SLAVE_CMD), 0x10, "slave ISR");
    CHECK(linux_irq_real(12), "i8259A_irq_real(12)");
    linux_ack(12);
    CHECK_U64(read_isr(PIC_MASTER_CMD), 0, "master ISR after ack");
    CHECK_U64(read_isr(PIC_SLAVE_CMD), 0, "slave ISR after ack");
    CHECK_U64(pic.acked[12], 1, "acked count");

    T_CASE("master ICW3 without the cascade bit: master answers IR2 itself");
    fresh();
    outb(0x11, PIC_MASTER_CMD);
    outb(0x30, PIC_MASTER_IMR);
    outb(0x00, PIC_MASTER_IMR);
    outb(0x01, PIC_MASTER_IMR);
    outb(0x11, PIC_SLAVE_CMD);
    outb(0x38, PIC_SLAVE_IMR);
    outb(0x02, PIC_SLAVE_IMR);
    outb(0x01, PIC_SLAVE_IMR);
    i8259_set_irq(&pic, 9, 1);
    CHECK_U64(i8259_inta(&pic), 0x32, "vector base + 2 from the master");
    CHECK_U64(read_isr(PIC_SLAVE_CMD), 0, "slave saw no INTA");

    T_CASE("external drivers cannot touch IRQ2, it belongs to the slave");
    fresh_linux(0, 0);
    i8259_set_irq(&pic, 2, 1);
    CHECK(!rec.level, "INT after driving IRQ2");
    CHECK_U64(read_irr(PIC_MASTER_CMD), 0, "master IRR");
}

static void test_edge_rules(void)
{
    T_CASE("edge: request dropped before INTA gives a spurious IR7, ISR untouched");
    fresh_linux(0, 1u << 3);
    i8259_set_irq(&pic, 3, 1);
    i8259_set_irq(&pic, 3, 0);
    CHECK(!rec.level, "INT after the request vanished");
    CHECK_U64(i8259_inta(&pic), 0x37, "vector of IR7");
    CHECK(!linux_irq_real(7), "IR7 must not be in service (Linux spurious test)");
    CHECK_U64(pic.spurious[0], 1, "master spurious count");

    T_CASE("edge: masking the slave request drops master IR2 and the INT output");
    fresh_linux(0, 1u << 10);
    i8259_set_irq(&pic, 10, 1);
    CHECK(rec.level, "precondition: INT high for IRQ10");
    /* 从片 INT 落下 → 主片 IR2 落下 → 主片边沿请求随之消失（图 9） */
    outb(0xFF, PIC_SLAVE_IMR);
    CHECK(!rec.level, "master INT follows the slave INT down");
    CHECK_U64(read_irr(PIC_MASTER_CMD) & 0x04, 0, "master IRR2");

    T_CASE("ICW1: a line already high must fall and rise again");
    fresh();
    i8259_set_irq(&pic, 5, 1);
    linux_init_8259A(0, (uint8_t)~(1u << 5 | 1u << 2), 0xFF);
    CHECK(!rec.level, "INT with IRQ5 high across ICW1");
    CHECK_U64(read_irr(PIC_MASTER_CMD) & 0x20, 0, "IRR5 after ICW1");
    i8259_set_irq(&pic, 5, 0);
    i8259_set_irq(&pic, 5, 1);
    CHECK(rec.level, "INT after a fresh rising edge");

    T_CASE("ICW1 clears IMR but keeps ISR");
    fresh_linux(0, 1u << 4);
    i8259_set_irq(&pic, 4, 1);
    i8259_inta(&pic);
    outb(0x11, PIC_MASTER_CMD);
    CHECK_U64(inb(PIC_MASTER_IMR), 0, "IMR right after ICW1");
    outb(0x30, PIC_MASTER_IMR);
    outb(0x04, PIC_MASTER_IMR);
    outb(0x01, PIC_MASTER_IMR);
    CHECK_U64(read_isr(PIC_MASTER_CMD), 0x10, "ISR survives ICW1");
}

static void test_level_elcr(void)
{
    T_CASE("ELCR: only the PIIX4-programmable bits stick");
    fresh();
    outb(0xFF, 0x4D0);
    outb(0xFF, 0x4D1);
    CHECK_U64(inb(0x4D0), 0xF8, "ELCR1 (IRQ0-2 always edge)");
    CHECK_U64(inb(0x4D1), 0xDE, "ELCR2 (IRQ8, IRQ13 always edge)");

    T_CASE("level IRQ11: EOI with the line still high interrupts again");
    fresh_linux(0, 1u << 11);
    outb(0x08, 0x4D1);
    i8259_set_irq(&pic, 11, 1);
    CHECK_U64(i8259_inta(&pic), 0x3B, "vector");
    CHECK(!rec.level, "INT while IRQ11 is in service");
    CHECK_U64(read_irr(PIC_SLAVE_CMD), 0x08, "level IRR follows the line after INTA");
    linux_ack(11);
    CHECK(rec.level, "INT again after EOI with the line high");
    CHECK_U64(i8259_inta(&pic), 0x3B, "second vector");
    i8259_set_irq(&pic, 11, 0);
    CHECK_U64(read_irr(PIC_SLAVE_CMD), 0, "IRR after the device deasserted");
    linux_ack(11);
    CHECK(!rec.level, "no INT once the line is low");

    T_CASE("level: the same line in edge mode interrupts only once");
    fresh_linux(0, 1u << 11);
    i8259_set_irq(&pic, 11, 1);
    i8259_inta(&pic);
    linux_ack(11);
    CHECK(!rec.level, "edge IRQ11 held high after EOI");
}

static void test_priority(void)
{
    T_CASE("fully nested: lower waits, higher nests, non-specific EOI clears the highest");
    fresh_linux(0, 0x1A);               /* IRQ1、3、4 */
    i8259_set_irq(&pic, 3, 1);
    CHECK_U64(i8259_inta(&pic), 0x33, "IRQ3");
    i8259_set_irq(&pic, 4, 1);
    CHECK(!rec.level, "IRQ4 blocked by IRQ3 in service");
    i8259_set_irq(&pic, 1, 1);
    CHECK(rec.level, "IRQ1 nests over IRQ3");
    CHECK_U64(i8259_inta(&pic), 0x31, "IRQ1");
    outb(0x20, PIC_MASTER_CMD);
    CHECK_U64(read_isr(PIC_MASTER_CMD), 0x08, "non-specific EOI cleared IRQ1");
    CHECK(!rec.level, "IRQ4 still blocked by IRQ3");
    outb(0x20, PIC_MASTER_CMD);
    CHECK(rec.level, "IRQ4 after IRQ3's EOI");
    CHECK_U64(i8259_inta(&pic), 0x34, "IRQ4");

    T_CASE("set priority: bottom IR4 makes IR5 the highest");
    fresh_linux(0, 0x48);               /* IRQ3、6 */
    outb(0xC4, PIC_MASTER_CMD);
    i8259_set_irq(&pic, 3, 1);
    i8259_set_irq(&pic, 6, 1);
    CHECK_U64(i8259_inta(&pic), 0x36, "IRQ6 outranks IRQ3 after rotation");

    T_CASE("rotate on non-specific EOI: the serviced level becomes the lowest");
    fresh_linux(0, 0x48);
    i8259_set_irq(&pic, 3, 1);
    CHECK_U64(i8259_inta(&pic), 0x33, "IRQ3 first (IR0 highest)");
    outb(0xA0, PIC_MASTER_CMD);
    i8259_set_irq(&pic, 3, 0);
    i8259_set_irq(&pic, 3, 1);
    i8259_set_irq(&pic, 6, 1);
    CHECK_U64(i8259_inta(&pic), 0x36, "IRQ6 wins once IR3 is the lowest");

    T_CASE("rotate on specific EOI and the no-op code");
    fresh_linux(0, 0x48);
    i8259_set_irq(&pic, 3, 1);
    i8259_inta(&pic);
    outb(0x40, PIC_MASTER_CMD);         /* 010：无操作 */
    CHECK_U64(read_isr(PIC_MASTER_CMD), 0x08, "ISR after the no-op");
    outb(0xE3, PIC_MASTER_CMD);
    CHECK_U64(read_isr(PIC_MASTER_CMD), 0, "ISR after rotate on specific EOI");
    i8259_set_irq(&pic, 3, 0);
    i8259_set_irq(&pic, 3, 1);
    i8259_set_irq(&pic, 6, 1);
    CHECK_U64(i8259_inta(&pic), 0x36, "IR3 is the lowest");
}

static void test_aeoi(void)
{
    T_CASE("AEOI: ISR never set, held edge does not repeat");
    fresh_linux(1, 1u << 4);
    i8259_set_irq(&pic, 4, 1);
    CHECK_U64(i8259_inta(&pic), 0x34, "vector");
    CHECK_U64(read_isr(PIC_MASTER_CMD), 0, "ISR in AEOI mode");
    CHECK(!rec.level, "INT after INTA");

    T_CASE("rotate in AEOI mode, then cleared by R=0 SL=0 EOI=0");
    fresh_linux(1, 0x48);
    outb(0x80, PIC_MASTER_CMD);
    i8259_set_irq(&pic, 3, 1);
    CHECK_U64(i8259_inta(&pic), 0x33, "IRQ3");
    i8259_set_irq(&pic, 3, 0);
    i8259_set_irq(&pic, 3, 1);
    i8259_set_irq(&pic, 6, 1);
    CHECK_U64(i8259_inta(&pic), 0x36, "IRQ6 first after AEOI rotation");
    outb(0x00, PIC_MASTER_CMD);
    CHECK_U64(i8259_inta(&pic), 0x33, "IRQ3 still pending");
    i8259_set_irq(&pic, 3, 0);
    i8259_set_irq(&pic, 3, 1);
    i8259_set_irq(&pic, 6, 0);
    i8259_set_irq(&pic, 6, 1);
    /* 最后一次轮转发生在交出 IRQ6 时，IR6 最低。轮转已关，交出 IRQ3 不再
     * 改优先级，IRQ3 继续赢；若还在轮转，IR3 会变成最低、IRQ6 赢 */
    CHECK_U64(i8259_inta(&pic), 0x33, "IRQ3: rotation stopped, IR6 still the lowest");
}

static void test_special_mask(void)
{
    T_CASE("special mask mode: masking the in-service level lets lower levels in");
    fresh_linux(0, 0x28);               /* IRQ3、5 */
    i8259_set_irq(&pic, 3, 1);
    i8259_inta(&pic);
    i8259_set_irq(&pic, 5, 1);
    CHECK(!rec.level, "IRQ5 blocked in normal mask mode");
    outb(0x68, PIC_MASTER_CMD);         /* ESMM=1 SMM=1 */
    outb((uint8_t)~(0x20 | 0x04), PIC_MASTER_IMR);
    CHECK(rec.level, "IRQ5 allowed with IRQ3 masked in SMM");
    outb(0x20, PIC_MASTER_CMD);
    CHECK_U64(read_isr(PIC_MASTER_CMD), 0x08,
              "non-specific EOI must skip the masked in-service level");
    outb(0x0A, PIC_MASTER_CMD);         /* 读 IRR 命令，ESMM=0：SMM 位不起作用 */
    CHECK(rec.level, "an OCW3 with ESMM=0 must leave SMM on");
    outb(0x48, PIC_MASTER_CMD);         /* ESMM=1 SMM=0 */
    CHECK(!rec.level, "back to normal mask mode, IRQ5 blocked again");
}

static void test_poll_and_irr(void)
{
    T_CASE("poll: next command-port read acknowledges and returns I + level");
    fresh_linux(0, 1u << 6);
    outb(0x0C, PIC_MASTER_CMD);
    CHECK_U64(inb(PIC_MASTER_CMD), 0x00, "poll with nothing pending");
    i8259_set_irq(&pic, 6, 1);
    outb(0x0C, PIC_MASTER_CMD);
    CHECK_U64(inb(PIC_MASTER_CMD), 0x86, "poll word");
    CHECK_U64(read_isr(PIC_MASTER_CMD), 0x40, "ISR set by the poll");
    CHECK(!rec.level, "INT after the poll acknowledged");
    CHECK_U64(inb(PIC_MASTER_CMD), 0, "poll is one-shot, back to IRR");

    T_CASE("IRR is not affected by IMR (Linux startup_ioapic_irq)");
    fresh_linux(0, 0);
    i8259_set_irq(&pic, 4, 1);
    CHECK(!rec.level, "masked IRQ4 raises no INT");
    CHECK_U64(read_irr(PIC_MASTER_CMD), 0x10, "IRR shows the masked request");
    outb((uint8_t)~(0x10 | 0x04), PIC_MASTER_IMR);
    CHECK(rec.level, "unmask raises INT for the latched edge");
}

static void test_sfnm(void)
{
    T_CASE("special fully nested: a higher slave request nests over a slave in service");
    for (int sfnm = 0; sfnm <= 1; sfnm++) {
        fresh();
        linux_init_8259A(0, 0xFB, (uint8_t)~(0x02 | 0x10));
        if (sfnm) {
            outb(0x11, PIC_MASTER_CMD);
            outb(0x30, PIC_MASTER_IMR);
            outb(0x04, PIC_MASTER_IMR);
            outb(0x11, PIC_MASTER_IMR);     /* SFNM | μPM */
            outb(0xFB, PIC_MASTER_IMR);
        }
        i8259_set_irq(&pic, 12, 1);
        CHECK_U64(i8259_inta(&pic), 0x3C, "IRQ12 (sfnm=%d)", sfnm);
        i8259_set_irq(&pic, 9, 1);
        if (sfnm)
            CHECK(rec.level, "IRQ9 must reach the CPU in SFNM");
        else
            CHECK(!rec.level, "IRQ9 must wait in fully nested mode");
    }
    CHECK_U64(i8259_inta(&pic), 0x39, "IRQ9 vector in SFNM");
}

static void test_wide_access_and_reset(void)
{
    T_CASE("16-bit write is split into byte cycles on the two ports");
    fresh();
    uint8_t w[2] = { 0x11, 0x50 };
    i8259_pio_write(&pic.chip[0], 0, 2, w);
    outb(0x01, PIC_MASTER_IMR);         /* SNGL=0：ICW3 */
    outb(0x01, PIC_MASTER_IMR);         /* ICW4 */
    outb(0xEF, PIC_MASTER_IMR);
    i8259_set_irq(&pic, 4, 1);
    CHECK_U64(i8259_inta(&pic), 0x54, "vector from ICW2 written as the high byte");

    T_CASE("reset: everything cleared, INT driven low");
    fresh_linux(0, 1u << 4);
    i8259_set_irq(&pic, 4, 1);
    CHECK(rec.level, "precondition: INT high");
    i8259_reset(&pic);
    CHECK(!rec.level, "INT after reset");
    CHECK_U64(inb(PIC_MASTER_IMR), 0, "IMR after reset");
    CHECK_U64(read_isr(PIC_MASTER_CMD), 0, "ISR after reset");
}

int main(int argc, char **argv)
{
    T_MAIN_BEGIN();
    test_reset_and_probe();
    test_master_irq();
    test_cascade();
    test_edge_rules();
    test_level_elcr();
    test_priority();
    test_aeoi();
    test_special_mask();
    test_poll_and_irr();
    test_sfnm();
    test_wide_access_and_reset();
    return T_SUMMARY();
}
