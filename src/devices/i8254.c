/* src/devices/i8254.c — 用户态 8254 和 PIIX4 port 0x61，接口见
 * include/devices/i8254.h。“第 N 页”指 ref/82C54_PIT_231244.pdf，
 * “PIIX4 x.y”指 ref/82371AB_PIIX4_290562.pdf 的章节。
 *
 * 时间以 CLK 脉冲号计：tick = floor(ns * 14318180 / 12e9)。第 k 个脉冲在
 * 时刻 k / 1.193182 MHz 发生；在 tick T（脉冲 T 已经发生、T+1 还没有）
 * 写入的计数，按数据手册“loaded on the next CLK pulse”在脉冲 T+1 装入。
 *
 * 各模式在段内位置 d（装入后已递减的脉冲数）上的读数和 OUT（第 9-13 页）：
 *
 *   mode 0  CE = n - d（回绕）   OUT = d >= n          写计数时 OUT 拉低
 *   mode 1  CE = n - d（回绕）   OUT = d >= n          触发后装入
 *   mode 2  CE = n - d mod n     OUT 低 iff d mod n == n-1
 *   mode 3  每脉冲减 2           OUT 高 iff d mod n < (n+1)/2
 *   mode 4  CE = n - d（回绕）   OUT 低 iff d == n     写计数时装入
 *   mode 5  CE = n - d（回绕）   OUT 低 iff d == n     触发后装入
 *
 * 由此 mode 0 的 OUT 在写计数后第 n+1 个脉冲变高，mode 2 在第 n 个脉冲变低，
 * mode 4 在第 n+1 个脉冲选通，和第 9、11、12 页的原文一致。
 *
 * 所有改状态的入口都先 sync() 到当前脉冲：把旧状态下 (last_tick, now] 里
 * 的上升沿计入 rises、应用到期的 mode 2/3 换值，再改状态；改完比较前后
 * OUT，写端口直接造成的上升沿也计入。这样状态在两次 sync 之间恒定，上升沿
 * 计数只需要对一段恒定的状态做区间计数。
 */
#include <string.h>

#include "vmm.h"
#include "devices/i8254.h"

#define NS_PER_SEC  1000000000ull

/* ------------------------------------------------------------------ */
/* 时间换算                                                            */
/* ------------------------------------------------------------------ */

static uint64_t ns_to_tick(uint64_t ns)
{
    return (uint64_t)((unsigned __int128)ns * I8254_OSC_HZ /
                      (I8254_OSC_DIV * NS_PER_SEC));
}

/* 脉冲 t 发生的时刻，向上取整，保证 ns_to_tick(结果) >= t */
static uint64_t tick_to_ns(uint64_t t)
{
    unsigned __int128 num = (unsigned __int128)t * I8254_OSC_DIV * NS_PER_SEC;

    return (uint64_t)((num + I8254_OSC_HZ - 1) / I8254_OSC_HZ);
}

static uint64_t now_tick(const struct i8254 *pit)
{
    return ns_to_tick(pit->ops->now_ns(pit->opaque));
}

/* ------------------------------------------------------------------ */
/* 计数值                                                              */
/* ------------------------------------------------------------------ */

/* 计数 0 等价于 2^16（二进制）或 10^4（BCD），第 14 页 */
static uint32_t modulus(const struct i8254_counter *c)
{
    return c->bcd ? 10000 : 65536;
}

static uint32_t from_bcd(uint16_t v)
{
    return (v & 0xF) + ((v >> 4) & 0xF) * 10 + ((v >> 8) & 0xF) * 100 +
           ((v >> 12) & 0xF) * 1000;
}

static uint16_t to_bcd(uint32_t v)
{
    return (uint16_t)((v % 10) | (v / 10 % 10) << 4 | (v / 100 % 10) << 8 |
                      (v / 1000 % 10) << 12);
}

/* CR 对应的装入值，二进制 1..模数 */
static uint32_t cr_n(const struct i8254_counter *c)
{
    uint32_t m = modulus(c);
    uint32_t n = (c->bcd ? from_bcd(c->cr) : c->cr) % m;

    return n ? n : m;
}

/* mode 3 一个周期里 OUT 为高的脉冲数：偶数 n/2，奇数 (n+1)/2（第 11-12 页） */
static uint32_t high_len(uint32_t n)
{
    return (n + 1) / 2;
}

/* ------------------------------------------------------------------ */
/* 计数段：读数、OUT 和上升沿                                          */
/* ------------------------------------------------------------------ */

/* t >= start 时段内的位置 d */
static uint64_t seg_d(const struct i8254_ce *ce, uint64_t t)
{
    uint64_t e = ce->stopped && ce->stop_tick < t ? ce->stop_tick : t;

    return (e > ce->start ? e - ce->start : 0) + ce->phase;
}

static uint32_t seg_count(const struct i8254_counter *c, uint64_t d)
{
    uint32_t n = c->ce.n, m = modulus(c);
    uint32_t p, h;

    switch (c->mode) {
    case 2:
        return (n - (uint32_t)(d % n)) % m;
    case 3:
        /* 偶数：装入 n，每脉冲减 2，减到头翻转 OUT 并重装。奇数：装入
         * n-1，高半周期多走一个脉冲（读到 0 的那一拍），低半周期少一个。 */
        p = (uint32_t)(d % n);
        h = high_len(n);
        if (p >= h)
            p -= h;
        return ((n & 1 ? n - 1 : n) - 2 * p) % m;
    default:
        return (uint32_t)((n + m - d % m) % m);
    }
}

static bool seg_out(const struct i8254_counter *c, uint64_t d)
{
    uint32_t n = c->ce.n;

    switch (c->mode) {
    case 0:
    case 1:
        return d >= n;
    case 2:
        return d % n != n - 1;
    case 3:
        return d % n < high_len(n);
    default:
        return d != n;
    }
}

static bool out_at(const struct i8254_counter *c, uint64_t t)
{
    const struct i8254_ce *ce = &c->ce;

    if (!ce->active)
        return c->idle_out;
    if (ce->forced_out >= 0)
        return ce->forced_out;
    if (t < ce->start)
        return ce->pre_out;
    return seg_out(c, seg_d(ce, t));
}

/* CE 的二进制值 0..模数-1 */
static uint32_t ce_at(const struct i8254_counter *c, uint64_t t)
{
    const struct i8254_ce *ce = &c->ce;

    if (!ce->active)
        return c->idle_ce;
    if (t < ce->start)
        return ce->pre_ce;
    return seg_count(c, seg_d(ce, t));
}

/* 装入那一拍本身的上升沿：装入前 OUT 低、装入后高（例如 mode 4 在选通
 * 那一拍被重写）。 */
static bool load_rises(const struct i8254_counter *c)
{
    return !c->ce.pre_out && seg_out(c, c->ce.phase);
}

/* (a, b] 里的上升沿个数。前提：段在这段时间内恒定（见文件头）。 */
static uint64_t seg_rises(const struct i8254_counter *c, uint64_t a, uint64_t b)
{
    const struct i8254_ce *ce = &c->ce;
    uint64_t s = ce->start, cnt = 0, r, da, db;
    uint32_t n = ce->n;

    if (!ce->active || ce->forced_out >= 0 || b <= a)
        return 0;
    if (a < s && s <= b && load_rises(c))
        cnt++;
    if (ce->stopped || b < s)
        return cnt;

    switch (c->mode) {
    case 0:
    case 1:
        r = s + n;
        return cnt + (a < r && r <= b);
    case 4:
    case 5:
        r = s + n + 1;
        return cnt + (a < r && r <= b);
    default:
        /* 周期模式：d 每到 n 的正整数倍就是一个上升沿（低半周期或单拍
         * 低电平结束）。n < 2 是非法计数（第 11 页 mode 2 “COUNT of 1 is
         * illegal”，图 22 mode 3 最小 2），OUT 恒定，没有沿。 */
        if (n < 2)
            return cnt;
        da = a < s ? ce->phase : a - s + ce->phase;
        db = b - s + ce->phase;
        return cnt + db / n - da / n;
    }
}

/* 第一个 > after 的上升沿，不考虑 pend */
static uint64_t seg_next_rise(const struct i8254_counter *c, uint64_t after)
{
    const struct i8254_ce *ce = &c->ce;
    uint64_t s = ce->start, r, d;
    uint32_t n = ce->n;

    if (!ce->active || ce->forced_out >= 0)
        return I8254_NO_EVENT;
    if (after < s && load_rises(c))
        return s;
    if (ce->stopped)
        return I8254_NO_EVENT;

    switch (c->mode) {
    case 0:
    case 1:
        r = s + n;
        break;
    case 4:
    case 5:
        r = s + n + 1;
        break;
    default:
        if (n < 2)
            return I8254_NO_EVENT;
        d = after < s ? ce->phase : after - s + ce->phase;
        return s + (d / n + 1) * n - ce->phase;
    }
    return r > after ? r : I8254_NO_EVENT;
}

/* mode 2/3 计数中写入的新值在 pend_tick 生效：mode 2 在周期末，mode 3 在
 * 当前半周期末（第 11 页）。在高半周期末换值时，新值从它的低半周期开始。 */
static void apply_pending(struct i8254_counter *c)
{
    struct i8254_ce *ce = &c->ce;

    ce->start = ce->pend_tick;
    ce->n = ce->pend_n;
    ce->phase = ce->pend_low ? high_len(ce->pend_n) : 0;
    ce->pre_out = true;
    ce->pend = false;
}

static void sync(struct i8254_counter *c, uint64_t t)
{
    struct i8254_ce *ce = &c->ce;
    uint64_t a = c->last_tick;

    if (t <= a)
        return;
    if (ce->active && ce->pend && ce->pend_tick <= t) {
        c->rises += seg_rises(c, a, ce->pend_tick);
        a = ce->pend_tick > a ? ce->pend_tick : a;
        apply_pending(c);
    }
    c->rises += seg_rises(c, a, t);
    c->last_tick = t;
}

/* 写端口直接造成的上升沿 */
static void note_edge(struct i8254_counter *c, bool before, uint64_t t)
{
    if (!before && out_at(c, t))
        c->rises++;
}

/* ------------------------------------------------------------------ */
/* 状态变化                                                            */
/* ------------------------------------------------------------------ */

/* 新的一段：下一个脉冲装入 n */
static void new_segment(struct i8254_counter *c, uint64_t t, uint32_t n,
                        bool pre_out)
{
    struct i8254_ce *ce = &c->ce;
    uint32_t pre_ce = ce_at(c, t);

    memset(ce, 0, sizeof(*ce));
    ce->active = true;
    ce->n = n;
    ce->start = t + 1;
    ce->pre_ce = pre_ce;
    ce->pre_out = pre_out;
    ce->forced_out = -1;
}

/* mode 0/4 的暂停与恢复。GATE 为低时不计数（第 9、12 页），mode 0 写了
 * 两字节计数的 LSB 也暂停（见 write_first_byte）。恢复时把 start 往后推，
 * 暂停前已递减的脉冲数保持不变；暂停发生在装入之前（GATE 为低时写计数，
 * 第 10 页说计数照样在下一个脉冲装入），恢复后从装入值开始数。 */
static void update_stop(struct i8254_counter *c, uint64_t t)
{
    struct i8254_ce *ce = &c->ce;
    bool want = !c->gate || c->hold;
    uint64_t done;

    if (!ce->active || want == ce->stopped)
        return;
    if (want) {
        ce->stopped = true;
        ce->stop_tick = t;
        return;
    }
    done = ce->stop_tick > ce->start ? ce->stop_tick - ce->start : 0;
    if (t - done > ce->start)
        ce->start = t - done;
    ce->stopped = false;
}

/* modes 1/2/3/5 的 trigger（GATE 上升沿）：下一个脉冲装入 CR（图 21） */
static void trigger(struct i8254_counter *c, uint64_t t)
{
    new_segment(c, t, cr_n(c), out_at(c, t));
    if (c->cr_load_tick > t + 1)
        c->cr_load_tick = t + 1;
}

static void set_gate(struct i8254_counter *c, bool level, uint64_t t)
{
    bool before, rising = level && !c->gate;

    sync(c, t);
    before = out_at(c, t);
    c->gate = level;

    switch (c->mode) {
    case 0:
    case 4:
        update_stop(c, t);
        break;
    case 1:
    case 5:
        if (rising && c->cr_valid)
            trigger(c, t);
        break;
    default:
        /* GATE 为低：停止计数并立刻把 OUT 拉高；上升沿重新装入（图 21）。
         * 停住期间到期的换值作废，CR 里的新值等上升沿再装。 */
        if (!level && c->ce.active && !c->ce.stopped) {
            c->ce.stopped = true;
            c->ce.stop_tick = t;
            c->ce.forced_out = 1;
            if (c->ce.pend) {
                c->ce.pend = false;
                c->cr_load_tick = I8254_NO_EVENT;
            }
        } else if (rising && c->cr_valid) {
            trigger(c, t);
        }
        break;
    }
    note_edge(c, before, t);
}

/* 一个完整计数写进了 CR */
static void load_count(struct i8254_counter *c, uint64_t t)
{
    struct i8254_ce *ce = &c->ce;
    uint32_t n = cr_n(c);
    bool before = out_at(c, t);
    uint64_t d;
    uint32_t p;

    c->cr_valid = true;
    c->hold = false;

    switch (c->mode) {
    case 0:
    case 4:
        /* 新计数在下一个脉冲装入，计数从新值继续（第 9、12 页）；mode 0
         * 写计数把 OUT 拉低，mode 4 的 OUT 不受影响。 */
        new_segment(c, t, n, c->mode == 0 ? false : before);
        c->cr_load_tick = t + 1;
        update_stop(c, t);
        break;
    case 1:
    case 5:
        /* 写计数只是武装，等 trigger 才装入；进行中的单稳不受影响 */
        c->cr_load_tick = I8254_NO_EVENT;
        break;
    default:
        if (!ce->active || t < ce->start) {
            new_segment(c, t, n, true);
            c->cr_load_tick = t + 1;
            if (!c->gate) {
                ce->stopped = true;
                ce->stop_tick = t;
                ce->forced_out = 1;
            }
        } else if (ce->stopped) {
            c->cr_load_tick = I8254_NO_EVENT;
        } else {
            /* 计数中写新值不影响当前周期（mode 3：当前半周期），到点再换 */
            d = seg_d(ce, t);
            p = (uint32_t)(d % ce->n);
            ce->pend = true;
            ce->pend_n = n;
            if (c->mode == 3 && p < high_len(ce->n)) {
                ce->pend_tick = t + (high_len(ce->n) - p);
                ce->pend_low = true;
            } else {
                ce->pend_tick = t + (ce->n - p);
                ce->pend_low = false;
            }
            c->cr_load_tick = ce->pend_tick;
        }
        break;
    }
    note_edge(c, before, t);
}

/* 两字节计数的 LSB。mode 0：第 9 页 1) 说 OUT 立刻变低；3) 说计数还没
 * 减到 0 再回绕到 FFFFh 之前写 LSB 会停住计数，回绕之后写则不停。1) 里
 * “does not disable counting”和 3) 冲突，按更具体的 3)。mode 4：第 12 页
 * 说 LSB 不影响计数。mode 1/2/3/5 计数中写新值本来就不影响当前序列。 */
static void write_first_byte(struct i8254_counter *c, uint64_t t)
{
    struct i8254_ce *ce = &c->ce;

    if (c->mode != 0 || !ce->active)
        return;
    c->hold = !(t >= ce->start && seg_d(ce, t) > ce->n);
    ce->forced_out = 0;
    update_stop(c, t);
}

static uint16_t read_value(const struct i8254_counter *c, uint64_t t)
{
    uint32_t v = ce_at(c, t);

    return c->bcd ? to_bcd(v) : (uint16_t)v;
}

static void latch_count(struct i8254_counter *c, uint64_t t)
{
    /* 已锁存未读时再锁存被忽略（第 7 页） */
    if (c->ol_left)
        return;
    c->ol = read_value(c, t);
    c->ol_left = c->rw == 3 ? 2 : 1;
}

static void latch_status(struct i8254_counter *c, uint64_t t)
{
    if (c->status_latched)
        return;
    c->status = (uint8_t)(out_at(c, t) << 7 | (t < c->cr_load_tick) << 6 |
                          c->cw);
    c->status_latched = true;
}

static void write_control(struct i8254 *pit, uint8_t v, uint64_t t)
{
    int sel = v >> 6, rw = (v >> 4) & 3;
    struct i8254_counter *c;
    uint8_t m;
    bool before;

    if (sel == 3) {
        /* read-back（图 10）：D5=0 锁计数，D4=0 锁状态，D3..D1 选 counter */
        for (int i = 0; i < 3; i++) {
            if (!(v & (2u << i)))
                continue;
            c = &pit->c[i];
            sync(c, t);
            if (!(v & 0x20))
                latch_count(c, t);
            if (!(v & 0x10))
                latch_status(c, t);
        }
        return;
    }

    c = &pit->c[sel];
    sync(c, t);
    if (rw == 0) {
        latch_count(c, t);
        return;
    }

    /* 写控制字：控制逻辑立刻复位，OUT 进入该模式的初始电平（第 14 页）；
     * CRM/CRL 清零（第 4 页）；锁住的计数作废（第 7 页 “or until the
     * Counter is reprogrammed”）。CE 没有清零的说法，停在当前值。 */
    before = out_at(c, t);
    c->idle_ce = ce_at(c, t);
    m = (v >> 1) & 7;
    if (m & 2)
        m &= 3;             /* X10 = mode 2，X11 = mode 3 */
    c->cw = v & 0x3F;
    c->mode = m;
    c->rw = (uint8_t)rw;
    c->bcd = v & 1;
    c->idle_ce %= modulus(c);
    c->cr = 0;
    c->cr_valid = false;
    c->wr_msb = false;
    c->rd_msb = false;
    c->hold = false;
    c->ol_left = 0;
    c->cr_load_tick = I8254_NO_EVENT;
    memset(&c->ce, 0, sizeof(c->ce));
    c->ce.forced_out = -1;
    c->idle_out = m != 0;
    note_edge(c, before, t);
}

static void write_counter(struct i8254_counter *c, uint8_t v, uint64_t t)
{
    sync(c, t);
    switch (c->rw) {
    case 1:
        c->cr = (uint16_t)((c->cr & 0xFF00) | v);
        load_count(c, t);
        break;
    case 2:
        c->cr = (uint16_t)((c->cr & 0x00FF) | v << 8);
        load_count(c, t);
        break;
    default:
        if (!c->wr_msb) {
            c->cr = (uint16_t)((c->cr & 0xFF00) | v);
            c->wr_msb = true;
            write_first_byte(c, t);
        } else {
            c->cr = (uint16_t)((c->cr & 0x00FF) | v << 8);
            c->wr_msb = false;
            load_count(c, t);
        }
        break;
    }
}

/* 读 counter：锁住的状态优先，然后是锁住的计数，最后跟随 CE（第 8 页） */
static uint8_t read_counter(struct i8254_counter *c, uint64_t t)
{
    uint16_t v;
    bool msb;

    sync(c, t);
    if (c->status_latched) {
        c->status_latched = false;
        return c->status;
    }
    if (c->ol_left) {
        v = c->ol;
        c->ol_left--;
    } else {
        v = read_value(c, t);
    }
    if (c->rw == 3) {
        msb = c->rd_msb;
        c->rd_msb = !c->rd_msb;
    } else {
        msb = c->rw == 2;
    }
    return msb ? v >> 8 : v & 0xFF;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

void i8254_init(struct i8254 *pit, const struct i8254_ops *ops, void *opaque)
{
    memset(pit, 0, sizeof(*pit));
    pit->ops = ops;
    pit->opaque = opaque;
    i8254_reset(pit);
}

void i8254_reset(struct i8254 *pit)
{
    uint64_t t = now_tick(pit);

    for (int i = 0; i < 3; i++) {
        struct i8254_counter *c = &pit->c[i];

        memset(c, 0, sizeof(*c));
        c->rw = 3;
        c->cw = 0x30;
        c->gate = i != 2;       /* GATE0/1 固定为高，GATE2 = NMISC bit 0 */
        c->cr_load_tick = I8254_NO_EVENT;
        c->ce.forced_out = -1;
        c->last_tick = t;
    }
    pit->nmisc = 0;
}

int i8254_pio_read(void *opaque, uint64_t offset, uint32_t size, void *data)
{
    struct i8254 *pit = opaque;
    uint8_t *d = data;
    uint64_t t = now_tick(pit);

    for (uint32_t i = 0; i < size; i++)
        d[i] = offset + i < 3 ? read_counter(&pit->c[offset + i], t) : 0xFF;
    return VMM_OK;
}

int i8254_pio_write(void *opaque, uint64_t offset, uint32_t size,
                    const void *data)
{
    struct i8254 *pit = opaque;
    const uint8_t *d = data;
    uint64_t t = now_tick(pit);

    for (uint32_t i = 0; i < size; i++) {
        if (offset + i < 3)
            write_counter(&pit->c[offset + i], d[i], t);
        else if (offset + i == 3)
            write_control(pit, d[i], t);
    }
    return VMM_OK;
}

/* NMISC（PIIX4 4.2.4.1）：bit 7/6 是 SERR#/IOCHK# NMI 状态，平台没有这两个
 * NMI 源，恒为 0；bit 5 = OUT2；bit 4 每个 refresh 周期翻转一次，refresh
 * request 由 counter 1 产生（PIIX4 8.8），所以取 OUT1 上升沿计数的奇偶；
 * bit 3:0 可写，bit 0 是 GATE2。 */
int i8254_nmisc_read(void *opaque, uint64_t offset, uint32_t size, void *data)
{
    struct i8254 *pit = opaque;
    uint8_t *d = data;
    uint64_t t = now_tick(pit);

    for (uint32_t i = 0; i < size; i++) {
        if (offset + i != 0) {
            d[i] = 0xFF;
            continue;
        }
        sync(&pit->c[1], t);
        sync(&pit->c[2], t);
        d[i] = (uint8_t)(out_at(&pit->c[2], t) << 5 |
                         (pit->c[1].rises & 1) << 4 | pit->nmisc);
    }
    return VMM_OK;
}

int i8254_nmisc_write(void *opaque, uint64_t offset, uint32_t size,
                      const void *data)
{
    struct i8254 *pit = opaque;
    const uint8_t *d = data;

    if (offset != 0 || size == 0)
        return VMM_OK;
    pit->nmisc = d[0] & 0x0F;
    set_gate(&pit->c[2], d[0] & 1, now_tick(pit));
    return VMM_OK;
}

uint64_t i8254_poll(struct i8254 *pit, int ch, int *level)
{
    struct i8254_counter *c = &pit->c[ch];
    uint64_t t = now_tick(pit), n;

    sync(c, t);
    if (level)
        *level = out_at(c, t);
    n = c->rises - c->rises_polled;
    c->rises_polled = c->rises;
    return n;
}

uint64_t i8254_next_rise_ns(struct i8254 *pit, int ch)
{
    struct i8254_counter *c = &pit->c[ch];
    uint64_t t = now_tick(pit), r;

    sync(c, t);
    r = seg_next_rise(c, t);
    if (c->ce.active && c->ce.pend && (r == I8254_NO_EVENT ||
                                       r > c->ce.pend_tick)) {
        struct i8254_counter after = *c;

        apply_pending(&after);
        r = seg_next_rise(&after, c->ce.pend_tick);
    }
    return r == I8254_NO_EVENT ? r : tick_to_ns(r);
}
