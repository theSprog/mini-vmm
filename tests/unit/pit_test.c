/* tests/unit/pit_test.c — 用户态 8254 和 port 0x61 的计数、OUT 与上升沿
 *
 * 判据（oracle）：
 *   - ref/82C54_PIT_231244.pdf：每个期望的 OUT 跳变时刻都来自模式定义里
 *     “OUT goes low N CLK pulses after the initial count is written”这类
 *     原文（第 9-13 页），用例注释里引出处；读数顺序、latch 和 read-back
 *     来自第 6-8 页，read-back 的组合直接回放图 13；
 *   - ref/82371AB_PIIX4_290562.pdf：CLK = 14.31818 MHz / 12（8.8），
 *     TMRSTS 和 NMISC 的复位值与位定义（4.2.3.2、4.2.4.1）；
 *   - Linux 6.6 drivers/clocksource/i8253.c（periodic / oneshot / shutdown）、
 *     arch/x86/kernel/tsc.c（pit_calibrate_tsc、quick_pit_calibrate）、
 *     drivers/input/misc/pcspkr.c 的端口序列按原样回放。
 *
 * 时间用假时钟：at(k) 把时钟拨到第 BASE+k 个 CLK 脉冲刚发生之后。用例里
 * 说“在 tick k 写”就是脉冲 k 已经发生、k+1 还没有时写，数据手册的“next
 * CLK pulse”就是 k+1。BASE 让 tick 号远离 0，避免掩盖减法回绕的错误。
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "vmm.h"
#include "devices/i8254.h"
#include "harness.h"

int vmm_log_level = -1;

void vmm_log(int level, const char *file, int line, const char *fmt, ...)
{
    (void)level; (void)file; (void)line; (void)fmt;
}

#define BASE    1000000ull

static uint64_t fake_ns;

static uint64_t fake_clock(void *opaque)
{
    (void)opaque;
    return fake_ns;
}

static const struct i8254_ops ops = { .now_ns = fake_clock };
static struct i8254 pit;

/* 第 k 个脉冲的时刻：k * 12 / 14318180 秒，向上取整到纳秒 */
static uint64_t tick_ns(uint64_t k)
{
    unsigned __int128 num = (unsigned __int128)k * 12 * 1000000000ull;

    return (uint64_t)((num + 14318180 - 1) / 14318180);
}

static void at(uint64_t k)
{
    fake_ns = tick_ns(BASE + k);
}

static uint64_t unclaimed[3];

static void reset_at(uint64_t k)
{
    at(k);
    i8254_init(&pit, &ops, NULL);
    memset(unclaimed, 0, sizeof(unclaimed));
}

/* ------------------------------------------------------------------ */
/* 端口访问                                                            */
/* ------------------------------------------------------------------ */

static void outb(uint8_t v, uint16_t port)
{
    if (port == I8254_NMISC_PORT)
        i8254_nmisc_write(&pit, 0, 1, &v);
    else
        i8254_pio_write(&pit, port - I8254_BASE, 1, &v);
}

static uint8_t inb(uint16_t port)
{
    uint8_t v = 0;

    if (port == I8254_NMISC_PORT)
        i8254_nmisc_read(&pit, 0, 1, &v);
    else
        i8254_pio_read(&pit, port - I8254_BASE, 1, &v);
    return v;
}

static void write16(int ch, uint16_t v)
{
    outb(v & 0xFF, 0x40 + ch);
    outb(v >> 8, 0x40 + ch);
}

/* 按 LSB、MSB 的顺序读两字节（计数按 LSB/MSB 编程） */
static uint16_t read16(int ch)
{
    uint16_t lo = inb(0x40 + ch);
    uint16_t hi = inb(0x40 + ch);

    return (uint16_t)(lo | hi << 8);
}

/* Counter Latch Command 后读两字节 */
static uint16_t latch16(int ch)
{
    outb((uint8_t)(ch << 6), 0x43);
    return read16(ch);
}

/* read-back 只锁状态（图 10：D5=1 不锁计数，D4=0 锁状态） */
static uint8_t status(int ch)
{
    outb((uint8_t)(0xE0 | 2 << ch), 0x43);
    return inb(0x40 + ch);
}

/* i8254_poll 取走上升沿的同时给出电平。只想看电平时把取走的沿存进
 * unclaimed，留给下一次 rises()。 */

static int out_level(int ch)
{
    int level = -1;

    unclaimed[ch] += i8254_poll(&pit, ch, &level);
    return level;
}

static uint64_t rises(int ch)
{
    uint64_t n = unclaimed[ch] + i8254_poll(&pit, ch, NULL);

    unclaimed[ch] = 0;
    return n;
}

static int out2(void)
{
    return !!(inb(I8254_NMISC_PORT) & 0x20);
}

/* ------------------------------------------------------------------ */
/* 用例                                                                */
/* ------------------------------------------------------------------ */

static void test_reset(void)
{
    T_CASE("reset: OUT low, NMISC 00h, no events");
    reset_at(0);
    /* PIIX4 4.2.3.2 TMRSTS default bit 7 = 0；4.2.4.1 NMISC default 00h */
    CHECK_U64(status(0) & 0x80, 0, "OUT0 after reset");
    CHECK_U64(inb(I8254_NMISC_PORT), 0x00, "NMISC after reset");
    at(100000);
    CHECK_U64(rises(0), 0, "no OUT0 edge before programming");
    CHECK_U64(i8254_next_rise_ns(&pit, 0), I8254_NO_EVENT, "nothing scheduled");
}

static void test_mode2_periodic(void)
{
    const uint16_t N = 1193;    /* Linux PIT_LATCH for HZ=1000 */
    int lvl;

    T_CASE("mode 2: Linux pit_set_periodic, period N, low on pulse N");
    reset_at(0);
    outb(0x34, 0x43);           /* counter 0, LSB/MSB, mode 2, binary */
    /* 第 11 页：mode 2 “OUT will initially be high”。复位后 OUT 为低，
     * 写控制字把它拉高，IRQ0 上就是一个上升沿。 */
    CHECK_U64(i8254_poll(&pit, 0, &lvl), 1, "control word raises OUT0");
    CHECK_U64(lvl, 1, "OUT0 high after mode 2 control word");
    write16(0, N);
    CHECK(status(0) & 0x40, "null count set until the count is loaded");
    CHECK_U64(status(0) & 0x3F, 0x34, "status echoes the control word");

    /* 第 11 页：counter loaded on the next CLK pulse；“OUT goes low N CLK
     * Pulses after the initial count is written” */
    at(1);
    CHECK_U64(status(0) & 0x40, 0, "null count clears on the load pulse");
    CHECK_U64(latch16(0), N, "CE holds N on the load pulse");
    at(6);
    CHECK_U64(latch16(0), N - 5, "decrements once per pulse");
    at(N - 1);
    CHECK_U64(out_level(0), 1, "still high one pulse before");
    CHECK_U64(rises(0), 0, "no edge yet");
    at(N);
    CHECK_U64(out_level(0), 0, "low N pulses after the write");
    CHECK_U64(latch16(0), 1, "count 1 while low");
    at(N + 1);
    CHECK_U64(out_level(0), 1, "high again after one pulse");
    CHECK_U64(rises(0), 1, "one rising edge per period");
    CHECK_U64(latch16(0), N, "reloads N");

    /* 下一个上升沿在一个周期之后，换算到纳秒后正好落在那个脉冲上 */
    uint64_t ns = i8254_next_rise_ns(&pit, 0);
    CHECK_U64(ns, tick_ns(BASE + 2 * N + 1), "next rise one period later");

    at(N + 1 + 10 * (uint64_t)N);
    CHECK_U64(rises(0), 10, "a late poll sees every missed period");
}

static void test_mode2_rewrite(void)
{
    T_CASE("mode 2: new count takes effect at the end of the period");
    reset_at(0);
    outb(0x34, 0x43);
    write16(0, 100);            /* 脉冲 1 装入，脉冲 100 为低，101 重装 */
    rises(0);
    at(50);
    write16(0, 40);
    CHECK(status(0) & 0x40, "null count while the new count waits");
    /* 第 11 页：“Writing a new count while counting does not affect the
     * current counting sequence ... the new count will be loaded at the end
     * of the current counting cycle” */
    CHECK_U64(i8254_next_rise_ns(&pit, 0), tick_ns(BASE + 101),
              "current period still ends on the old count");
    at(100);
    CHECK_U64(out_level(0), 0, "old period: low on pulse 100");
    at(101);
    CHECK_U64(status(0) & 0x40, 0, "new count loaded at the period end");
    CHECK_U64(latch16(0), 40, "CE holds the new count");
    CHECK_U64(i8254_next_rise_ns(&pit, 0), tick_ns(BASE + 141),
              "next period uses the new count");
    at(140);
    CHECK_U64(out_level(0), 0, "low after 40 pulses");
    at(141);
    CHECK_U64(rises(0), 2, "edges at 101 and 141");
}

static void test_mode4_oneshot(void)
{
    T_CASE("mode 4: Linux pit_set_oneshot / pit_next_event");
    reset_at(0);
    outb(0x38, 0x43);           /* counter 0, LSB/MSB, mode 4 */
    CHECK_U64(rises(0), 1, "mode 4 control word raises OUT0");
    at(10);
    write16(0, 500);
    CHECK_U64(i8254_next_rise_ns(&pit, 0), tick_ns(BASE + 512),
              "strobe ends N+2 pulses after the write");
    /* 第 12 页：“OUT does not strobe low until N+1 CLK pulses after the
     * initial count is written”，低一个脉冲后回到高 */
    at(10 + 500);
    CHECK_U64(out_level(0), 1, "high before the strobe");
    at(10 + 501);
    CHECK_U64(out_level(0), 0, "strobe low N+1 pulses after the write");
    at(10 + 502);
    CHECK_U64(out_level(0), 1, "strobe lasts one pulse");
    CHECK_U64(rises(0), 1, "one edge");
    at(10 + 502 + 60000);
    CHECK_U64(rises(0), 0, "one-shot: no further edge");

    /* 第 12 页：计数中写新值在下一个脉冲装入，“OUT strobes low N+1 CLK
     * pulses after the new count of N is written” */
    at(70000);
    write16(0, 100);
    at(70000 + 50);
    write16(0, 100);            /* 重写：重新开始 */
    at(70000 + 101);
    CHECK_U64(out_level(0), 1, "first count was superseded");
    at(70000 + 50 + 101);
    CHECK_U64(out_level(0), 0, "strobe from the second write");
    at(70000 + 50 + 102);
    CHECK_U64(rises(0), 1, "exactly one edge for two writes");
}

static void test_mode0_tsc_calibrate(void)
{
    const uint16_t N = 59659;   /* tsc.c CAL_LATCH: 50 ms */

    T_CASE("mode 0 via port 0x61: Linux pit_calibrate_tsc");
    reset_at(0);
    /* outb((inb(0x61) & ~0x02) | 0x01, 0x61)：GATE2 高，扬声器关 */
    outb((inb(0x61) & ~0x02) | 0x01, 0x61);
    CHECK_U64(inb(0x61) & 0x03, 0x01, "gate on, speaker off read back");
    outb(0xb0, 0x43);           /* counter 2, LSB/MSB, mode 0 */
    at(5);
    write16(2, N);
    /* 第 9 页：mode 0 “OUT does not go high until N+1 CLK pulses after the
     * initial count is written” */
    at(5 + N);
    CHECK_U64(out2(), 0, "OUT2 low N pulses after the write");
    at(5 + N + 1);
    CHECK_U64(out2(), 1, "OUT2 high N+1 pulses after the write");
    at(5 + N + 1 + 100000);
    CHECK_U64(out2(), 1, "OUT2 stays high after terminal count");
}

static void test_quick_calibrate_msb(void)
{
    T_CASE("mode 0 unlatched reads: Linux quick_pit_calibrate");
    reset_at(0);
    outb((inb(0x61) & ~0x02) | 0x01, 0x61);
    outb(0xb0, 0x43);
    outb(0xff, 0x42);
    outb(0xff, 0x42);
    /* pit_verify_msb：读 LSB 丢掉，再读 MSB。LSB/MSB 格式下两次读交替取
     * 低、高字节（第 7 页）。脉冲 1 装入 0xFFFF，此后每脉冲减 1。 */
    at(1);
    CHECK_U64(inb(0x42), 0xFF, "LSB on the load pulse");
    CHECK_U64(inb(0x42), 0xFF, "MSB on the load pulse");
    at(1 + 255);
    inb(0x42);
    CHECK_U64(inb(0x42), 0xFF, "MSB still 0xff after 255 pulses");
    at(1 + 256);
    inb(0x42);
    CHECK_U64(inb(0x42), 0xFE, "MSB 0xfe after 256 pulses");
    at(1 + 256 * 10);
    inb(0x42);
    CHECK_U64(inb(0x42), 0xF5, "MSB 0xf5 after 2560 pulses");
}

static void test_latch(void)
{
    T_CASE("counter latch: holds until read, second latch ignored");
    reset_at(0);
    outb(0x34, 0x43);
    write16(0, 1000);
    at(11);
    outb(0x00, 0x43);           /* latch counter 0：CE = 1000 - 10 */
    at(21);
    outb(0x00, 0x43);           /* 第 7 页：未读之前再次 latch 被忽略 */
    at(31);
    CHECK_U64(read16(0), 990, "count from the first latch");
    at(41);
    CHECK_U64(read16(0), 960, "unlatched after reading");

    /* 第 7 页：同一 counter 的读写可以交错，读和写各有自己的字节顺序 */
    at(100);
    outb(0x00, 0x43);           /* 锁住 1000 - 99 = 901 */
    uint8_t lo = inb(0x40);
    outb(0x34, 0x40);           /* 新计数 LSB */
    uint8_t hi = inb(0x40);
    outb(0x01, 0x40);           /* MSB：0x0134 = 308 */
    CHECK_U64(lo | hi << 8, 901, "latched read survives an interleaved write");
    at(1100);                   /* 新值在当前周期结束（脉冲 1001）后生效 */
    CHECK_U64(latch16(0), 308 - 99, "interleaved write produced 0x0134");
}

static void test_readback_fig13(void)
{
    uint8_t st;

    T_CASE("read-back: Figure 13 sequence");
    reset_at(0);
    outb(0x34, 0x43); write16(0, 10000);    /* counter 0 mode 2 */
    outb(0x74, 0x43); write16(1, 20000);    /* counter 1 mode 2 */
    outb(0x01, 0x61);
    outb(0xB4, 0x43); write16(2, 30000);    /* counter 2 mode 2 */

    at(101);  outb(0xC2, 0x43);     /* count+status counter 0 */
    at(201);  outb(0xE4, 0x43);     /* status counter 1 */
    at(301);  outb(0xEC, 0x43);     /* status counters 2,1：1 已锁，只锁 2 */
    at(401);  outb(0xD8, 0x43);     /* count counter 2 */
    at(501);  outb(0xC4, 0x43);     /* count+status counter 1：只锁 count */
    /* 图 13 把 11100010 写成 “status of Counter 1”，按图 10 的位定义 D1
     * 选的是 counter 0；两种读法下状态都已锁存，命令都被忽略。 */
    at(601);  outb(0xE2, 0x43);
    at(701);

    /* 第 8 页：count 和 status 都锁存时，第一次读返回 status */
    st = inb(0x40);
    CHECK_U64(st, 0x80 | 0x34, "counter 0 status: OUT high, mode word");
    CHECK_U64(read16(0), 10000 - 100, "counter 0 count at 101");
    st = inb(0x41);
    CHECK_U64(st, 0x80 | 0x34, "counter 1 status latched at 201");
    CHECK_U64(read16(1), 20000 - 500, "counter 1 count at 501");
    st = inb(0x42);
    CHECK_U64(st, 0x80 | 0x34, "counter 2 status latched at 301");
    CHECK_U64(read16(2), 30000 - 400, "counter 2 count at 401");
    /* 读完后回到跟随 CE */
    CHECK_U64(read16(0), 10000 - 700, "counter 0 follows CE");
}

static void test_mode3_square(void)
{
    T_CASE("mode 3: square wave, even and odd counts");
    reset_at(0);
    outb(0x36, 0x43);           /* counter 0, LSB/MSB, mode 3 */
    write16(0, 10);
    rises(0);
    /* 第 11 页偶数：装入 N，每脉冲减 2，减到头 OUT 翻转并重装。脉冲 1-5
     * 高（10,8,6,4,2），6-10 低，11 回到高。 */
    static const uint16_t even_ce[] = { 10, 8, 6, 4, 2, 10, 8, 6, 4, 2 };
    for (int k = 1; k <= 10; k++) {
        at(k);
        CHECK_U64(out_level(0), k <= 5, "even N=10: OUT at pulse %d", k);
        CHECK_U64(latch16(0), even_ce[k - 1], "even N=10: CE at pulse %d", k);
    }
    at(11);
    CHECK_U64(rises(0), 1, "rising edge at pulse 11");

    /* 奇数：装入 N-1 每脉冲减 2，减到头后再过一个脉冲 OUT 变低并重装 N-1，
     * 低半周期减到头 OUT 变高并重装：高 (N+1)/2 个脉冲，低 (N-1)/2 个 */
    reset_at(0);
    outb(0x36, 0x43);
    write16(0, 5);
    rises(0);
    static const uint16_t odd_ce[] = { 4, 2, 0, 4, 2 };
    for (int k = 1; k <= 5; k++) {
        at(k);
        CHECK_U64(out_level(0), k <= 3, "odd N=5: OUT at pulse %d", k);
        CHECK_U64(latch16(0), odd_ce[k - 1], "odd N=5: CE at pulse %d", k);
    }
    at(6);
    CHECK_U64(rises(0), 1, "rising edge at pulse 6");
    at(6 + 5 * 100);
    CHECK_U64(rises(0), 100, "period N");
}

static void test_mode3_rewrite(void)
{
    T_CASE("mode 3: new count at the end of the half cycle");
    reset_at(0);
    outb(0x36, 0x43);
    write16(0, 10);             /* 高 1-5，低从 6 开始 */
    rises(0);
    at(3);
    write16(0, 6);
    /* 第 11 页：“the new count will be loaded at the end of the current
     * half-cycle”。高半周期在脉冲 5 结束，脉冲 6 起用新值走低半周期
     * （3 个脉冲），脉冲 9 变高。旧值的下一个上升沿本该在脉冲 11，定时器
     * 必须按新值排在 9。 */
    CHECK_U64(i8254_next_rise_ns(&pit, 0), tick_ns(BASE + 9),
              "next rise follows the pending count");
    at(5);
    CHECK_U64(out_level(0), 1, "old half cycle unaffected");
    at(6);
    CHECK_U64(out_level(0), 0, "low half starts on schedule");
    CHECK_U64(latch16(0), 6, "new count loaded");
    at(8);
    CHECK_U64(out_level(0), 0, "low for 3 pulses");
    at(9);
    CHECK_U64(out_level(0), 1, "high after the new low half");
    CHECK_U64(rises(0), 1, "edge at 9");
    CHECK_U64(i8254_next_rise_ns(&pit, 0), tick_ns(BASE + 15),
              "then period 6");
}

static void test_mode2_gate(void)
{
    T_CASE("mode 2 on counter 2: GATE low forces OUT high, rise reloads");
    reset_at(0);
    outb(0x01, 0x61);
    outb(0xB4, 0x43);           /* counter 2, LSB/MSB, mode 2 */
    write16(2, 100);
    at(50);
    outb(0x00, 0x61);           /* GATE2 低：停止计数 */
    CHECK_U64(latch16(2), 51, "count at the gate fall");
    at(80);
    CHECK_U64(latch16(2), 51, "count frozen while GATE low");
    CHECK_U64(out2(), 1, "OUT2 high while GATE low");
    outb(0x01, 0x61);           /* trigger */
    /* 图 21 / 第 11 页：trigger 在下一个脉冲重装初值，“OUT goes low N CLK
     * pulses after the trigger” */
    at(81);
    CHECK_U64(latch16(2), 100, "reloaded on the pulse after the trigger");
    at(179);
    CHECK_U64(out2(), 1, "high before N pulses");
    at(180);
    CHECK_U64(out2(), 0, "low N pulses after the trigger");
    rises(2);
    outb(0x00, 0x61);           /* 第 11 页：低电平脉冲中 GATE 落下，OUT 立刻变高 */
    CHECK_U64(out2(), 1, "GATE fall during the low pulse sets OUT high");
    CHECK_U64(rises(2), 1, "that is a rising edge");
}

static void test_mode0_gate(void)
{
    T_CASE("mode 0 on counter 2: GATE low pauses counting");
    reset_at(0);
    outb(0xB0, 0x43);           /* GATE2 低 */
    write16(2, 100);
    at(10);
    /* 第 10 页：GATE 为低时写的计数照样在下一个脉冲装入；“When GATE goes
     * high, OUT will go high N CLK pulses later” */
    CHECK_U64(status(2) & 0x40, 0, "loaded although GATE is low");
    CHECK_U64(latch16(2), 100, "not counting while GATE low");
    at(50);
    outb(0x01, 0x61);
    at(149);
    CHECK_U64(out2(), 0, "low before N pulses");
    at(150);
    CHECK_U64(out2(), 1, "high N pulses after GATE rises");

    /* 计数中途停住：已递减的脉冲数保持 */
    reset_at(0);
    outb(0x01, 0x61);
    outb(0xB0, 0x43);
    write16(2, 100);            /* 脉冲 101 OUT 变高 */
    at(51);
    outb(0x00, 0x61);           /* 已递减 50 */
    at(91);
    CHECK_U64(latch16(2), 50, "paused at 50");
    outb(0x01, 0x61);
    at(140);
    CHECK_U64(out2(), 0, "remaining 50 pulses not done");
    at(141);
    CHECK_U64(out2(), 1, "terminal count after the remaining 50");
}

static void test_mode1_oneshot(void)
{
    T_CASE("mode 1 on counter 2: retriggerable one-shot");
    reset_at(0);
    outb(0xB2, 0x43);           /* counter 2, LSB/MSB, mode 1，GATE2 低 */
    write16(2, 20);
    at(100);
    /* 第 10 页：OUT initially high，等 trigger */
    CHECK_U64(out2(), 1, "armed, OUT high without a trigger");
    outb(0x01, 0x61);
    CHECK_U64(out2(), 1, "OUT changes only on the next pulse");
    at(101);
    CHECK_U64(out2(), 0, "low on the pulse after the trigger");
    at(120);
    CHECK_U64(out2(), 0, "pulse lasts N pulses");
    at(121);
    CHECK_U64(out2(), 1, "high after N pulses");

    /* 再触发：OUT 在任一 trigger 之后保持低 N 个脉冲 */
    at(200);
    outb(0x00, 0x61);
    outb(0x01, 0x61);           /* 脉冲 201 起低 */
    at(210);
    outb(0x00, 0x61);
    outb(0x01, 0x61);           /* 再触发：脉冲 211 重装 */
    at(221);
    CHECK_U64(out2(), 0, "retrigger extends the pulse");
    at(231);
    CHECK_U64(out2(), 1, "high N pulses after the last trigger");
}

static void test_mode5_strobe(void)
{
    T_CASE("mode 5 on counter 2: hardware triggered strobe");
    reset_at(0);
    outb(0xBA, 0x43);           /* counter 2, LSB/MSB, mode 5 */
    write16(2, 20);
    at(100);
    outb(0x01, 0x61);
    /* 第 13 页：“OUT does not strobe low until N+1 CLK pulses after a
     * trigger” */
    at(120);
    CHECK_U64(out2(), 1, "high N pulses after the trigger");
    at(121);
    CHECK_U64(out2(), 0, "strobe N+1 pulses after the trigger");
    at(122);
    CHECK_U64(out2(), 1, "strobe lasts one pulse");
}

static void test_mode0_two_byte(void)
{
    T_CASE("mode 0: LSB of a two-byte count stops the counter");
    reset_at(0);
    outb(0x30, 0x43);           /* Linux pit_shutdown 的控制字 */
    write16(0, 1000);
    at(101);                    /* CE = 1000 - 100 */
    outb(0x10, 0x40);
    at(201);
    CHECK_U64(latch16(0), 900, "stopped after the LSB (page 9, 3)");
    outb(0x00, 0x40);           /* MSB：N = 0x0010 */
    at(201 + 16);
    CHECK_U64(out_level(0), 0, "low N pulses after the MSB");
    at(201 + 17);
    CHECK_U64(out_level(0), 1, "high N+1 pulses after the MSB");

    /* 回绕到 FFFFh 之后写 LSB：OUT 立刻变低，计数不停 */
    reset_at(0);
    outb(0x30, 0x43);
    write16(0, 10);
    at(12);
    CHECK_U64(out_level(0), 1, "terminal count reached");
    CHECK_U64(latch16(0), 0xFFFF, "rolled over");
    at(50);
    outb(0x10, 0x40);
    CHECK_U64(out_level(0), 0, "LSB sets OUT low immediately (page 9, 1)");
    at(60);
    CHECK_U64(latch16(0), (uint16_t)(10 - 59), "still counting after rollover");
}

static void test_bcd(void)
{
    T_CASE("BCD counting");
    reset_at(0);
    outb(0x35, 0x43);           /* counter 0, LSB/MSB, mode 2, BCD */
    write16(0, 0x1000);         /* 1000 */
    at(2);
    CHECK_U64(latch16(0), 0x0999, "reads back in BCD");
    at(1000);
    CHECK_U64(out_level(0), 0, "period is 1000 decimal");

    reset_at(0);
    outb(0x31, 0x43);           /* mode 0, BCD，计数 0 = 10^4（第 14 页） */
    write16(0, 0);
    at(2);
    CHECK_U64(latch16(0), 0x9999, "0 means 10000 in BCD");
    at(10000);
    CHECK_U64(out_level(0), 0, "not yet");
    at(10001);
    CHECK_U64(out_level(0), 1, "terminal count after 10^4 pulses");
}

static void test_nmisc(void)
{
    uint8_t a, b;

    T_CASE("port 0x61: writable bits, OUT2, refresh toggle");
    reset_at(0);
    outb(0xFF, 0x61);
    /* 4.2.4.1：bit 7-4 只读（写时必须为 0），bit 3-0 可写 */
    CHECK_U64(inb(0x61) & 0xCF, 0x0F, "only bits 3:0 are writable");
    outb(0x0C, 0x61);
    CHECK_U64(inb(0x61), 0x0C, "bits 3:0 read back, OUT2 low");

    /* pcspkr.c：0xB6 + 计数，然后 0x61 |= 3 打开 GATE2 和扬声器 */
    outb(0xB6, 0x43);
    write16(2, 1000);
    outb(inb(0x61) | 3, 0x61);
    CHECK_U64(inb(0x61) & 0x23, 0x23, "speaker on, OUT2 high");

    /* refresh toggle 每个 refresh 周期翻转一次，refresh request 是
     * counter 1 的输出（PIIX4 8.8）。BIOS 惯用 mode 2、计数 18。 */
    reset_at(0);
    outb(0x54, 0x43);           /* counter 1, LSB only, mode 2 */
    outb(18, 0x41);
    at(18);
    a = inb(0x61) & 0x10;
    at(19);
    b = inb(0x61) & 0x10;
    CHECK(a != b, "toggles at the end of a refresh period");
    at(36);
    CHECK_U64(inb(0x61) & 0x10, b, "steady within a period");
    at(37);
    CHECK_U64(inb(0x61) & 0x10, a, "toggles again one period later");
}

static void test_wide_access(void)
{
    uint8_t buf[2] = { 0x34, 0x12 };
    uint8_t rd[2];

    T_CASE("wide access splits into consecutive ports");
    reset_at(0);
    outb(0x70, 0x43);           /* counter 1, LSB/MSB, mode 0 */
    outb(0x30, 0x43);           /* counter 0, LSB/MSB, mode 0 */
    i8254_pio_write(&pit, 0, 2, buf);   /* 0x34 -> 0x40，0x12 -> 0x41 */
    outb(0x00, 0x40);
    outb(0x00, 0x41);
    at(2);
    CHECK_U64(latch16(0), 0x0034 - 1, "byte 0 went to counter 0");
    CHECK_U64(latch16(1), 0x0012 - 1, "byte 1 went to counter 1");
    outb(0x80, 0x43);           /* latch counter 2 */
    i8254_pio_read(&pit, 2, 2, rd);
    CHECK_U64(rd[1], 0xFF, "control word port reads 0xff (Figure 14)");
}

int main(int argc, char **argv)
{
    T_MAIN_BEGIN();
    test_reset();
    test_mode2_periodic();
    test_mode2_rewrite();
    test_mode4_oneshot();
    test_mode0_tsc_calibrate();
    test_quick_calibrate_msb();
    test_latch();
    test_readback_fig13();
    test_mode3_square();
    test_mode3_rewrite();
    test_mode2_gate();
    test_mode0_gate();
    test_mode1_oneshot();
    test_mode5_strobe();
    test_mode0_two_byte();
    test_bcd();
    test_nmisc();
    test_wide_access();
    return T_SUMMARY();
}
