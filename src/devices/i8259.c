/* src/devices/i8259.c — 用户态 8259A 对（PIIX4 平台），接口见
 * include/devices/i8259.h。“图 N”指 ref/8259A_PIC_231468.pdf 的插图，
 * “PIIX4 x.y”指 ref/82371AB_PIIX4_290562.pdf 的章节。
 *
 * 图 9（priority cell）是下面三条规则的出处：
 *   - edge sense latch 在 IR 为低时置位（武装），在 ISR 置位或 ICW1 的
 *     master clear 时清零；
 *   - request latch 是透明锁存器，D = IR & (LTIM | 已武装)，所以边沿模式下
 *     IR 在 INTA 之前落下，请求随之消失（正文“IR inputs must remain high
 *     until after the falling edge of the first INTA”说的就是这个）；
 *   - master clear 只接 mask latch 和 edge sense latch，不接 in-service
 *     latch：ICW1 清 IMR、重置边沿检测，不清 ISR。
 */
#include <string.h>

#include "vmm.h"
#include "devices/i8259.h"

/* PIIX4 4.2.2.9/10：IRQ0、1、2、8、13 固定边沿触发，ELCR 里对应位保留 */
#define ELCR1_MASK  0xF8
#define ELCR2_MASK  0xDE

static uint8_t irr(const struct pic8259 *c)
{
    return c->line & (c->elcr | c->armed);
}

/* 优先级 0 最高。priority_add 是当前最高优先级的 IR，轮转只改它。 */
static int prio(const struct pic8259 *c, int irq)
{
    return (irq - c->priority_add) & 7;
}

static int highest(const struct pic8259 *c, uint8_t bits)
{
    int best = -1;

    for (int irq = 0; irq < 8; irq++)
        if ((bits & (1u << irq)) && (best < 0 || prio(c, irq) < prio(c, best)))
            best = irq;
    return best;
}

/* 这片芯片此刻要不要拉高 INT，要的话是哪个 IR。
 *
 * 全嵌套模式下，正在服务的级别挡住同级和更低级的请求。特殊屏蔽模式下，
 * 被 IMR 屏蔽的在服务级别不再挡别人：数据手册说 SMM 下置位的屏蔽位“inhibits
 * further interrupts at that level and enables interrupts from all other
 * levels (lower as well as higher)”，典型用法就是中断处理程序屏蔽自己的
 * 级别来放行低级别。特殊全嵌套模式下主片不让接从片的 IR 被自己的 ISR
 * 挡住，从片里更高级的请求能嵌套进来（第 18 页 Special Fully Nest Mode a）。 */
static int get_irq(const struct pic8259 *c)
{
    uint8_t pending = irr(c) & ~c->imr;
    uint8_t in_service = c->isr;
    int req, svc;

    if (!pending)
        return -1;
    req = highest(c, pending);
    if (c->special_mask)
        in_service &= ~c->imr;
    if (!c->is_slave && c->sfnm)
        in_service &= ~c->icw3;
    svc = highest(c, in_service);
    if (svc >= 0 && prio(c, req) >= prio(c, svc))
        return -1;
    return req;
}

static void set_line(struct pic8259 *c, int irq, int level)
{
    uint8_t bit = 1u << irq;

    if (level) {
        c->line |= bit;
    } else {
        c->line &= ~bit;
        c->armed |= bit;
    }
}

/* 从片 INT 驱动主片 IR2，主片 INT 是整对芯片的输出 */
static void update(struct i8259 *p)
{
    struct pic8259 *m = &p->chip[0], *s = &p->chip[1];
    bool out;

    set_line(m, I8259_CASCADE_IR, get_irq(s) >= 0);
    out = get_irq(m) >= 0;
    if (out != p->int_out) {
        p->int_out = out;
        if (p->ops && p->ops->int_changed)
            p->ops->int_changed(p->opaque, out);
    }
}

/* INTA 第一个脉冲：置 ISR、清 edge sense latch（于是边沿模式的 IRR 随之
 * 清零，电平模式的 IRR 继续跟随引脚）。AEOI 在最后一个脉冲结尾做一次
 * 非特定 EOI，清掉的就是刚置上的这一位，因为它刚刚赢得了优先级判决。 */
static void ack(struct pic8259 *c, int irq)
{
    uint8_t bit = 1u << irq;

    c->isr |= bit;
    c->armed &= ~bit;
    if (c->auto_eoi) {
        if (c->rotate_on_aeoi)
            c->priority_add = (irq + 1) & 7;
        c->isr &= ~bit;
    }
}

uint8_t i8259_inta(struct i8259 *p)
{
    struct pic8259 *m = &p->chip[0], *s = &p->chip[1];
    int irq = get_irq(m), irq2;
    uint8_t vec;

    if (irq < 0) {
        /* 第 7 页：INTA 时没有请求，交出 IR7 的向量，ISR 不变 */
        p->spurious[0]++;
        vec = m->vector_base | 7;
    } else {
        ack(m, irq);
        if (!m->single && (m->icw3 & (1u << irq))) {
            /* 级联：主片只置自己的 ISR，向量由从片在第二个 INTA 交出 */
            irq2 = get_irq(s);
            if (irq2 < 0) {
                p->spurious[1]++;
                vec = s->vector_base | 7;
            } else {
                ack(s, irq2);
                p->acked[8 + irq2]++;
                vec = s->vector_base | irq2;
            }
        } else {
            p->acked[irq]++;
            vec = m->vector_base | irq;
        }
    }
    update(p);
    return vec;
}

/* 非特定 EOI 清掉优先级最高的在服务位。特殊屏蔽模式下被屏蔽的在服务位
 * 不会被它清掉（第 15 页 End of Interrupt 最后一段）。 */
static int eoi_nonspecific(struct pic8259 *c)
{
    int irq = highest(c, c->special_mask ? (c->isr & ~c->imr) : c->isr);

    if (irq >= 0)
        c->isr &= ~(1u << irq);
    return irq;
}

/* OCW2 bit 7:5 = R SL EOI，编码表见图 8（PIIX4 4.2.2.7 有同一张表） */
static void write_ocw2(struct pic8259 *c, uint8_t val)
{
    int level = val & 3, irq;

    switch (val >> 5) {
    case 1:                                 /* 非特定 EOI */
        eoi_nonspecific(c);
        break;
    case 5:                                 /* 非特定 EOI 并轮转 */
        irq = eoi_nonspecific(c);
        if (irq >= 0)
            c->priority_add = (irq + 1) & 7;
        break;
    case 3:                                 /* 特定 EOI */
        c->isr &= ~(1u << level);
        break;
    case 7:                                 /* 特定 EOI 并轮转 */
        c->isr &= ~(1u << level);
        c->priority_add = (level + 1) & 7;
        break;
    case 6:                                 /* 设置最低优先级 */
        c->priority_add = (level + 1) & 7;
        break;
    case 4:
        c->rotate_on_aeoi = true;
        break;
    case 0:
        c->rotate_on_aeoi = false;
        break;
    case 2:                                 /* 无操作 */
        break;
    }
}

/* OCW3（图 8）：D6 ESMM，D5 SMM，D2 P，D1 RR，D0 RIS。RR=0 时 RIS 不改变
 * 选择，ESMM=0 时 SMM 不起作用。 */
static void write_ocw3(struct pic8259 *c, uint8_t val)
{
    if (val & 0x04)
        c->poll = true;
    if (val & 0x02)
        c->read_isr = val & 0x01;
    if (val & 0x40)
        c->special_mask = val & 0x20;
}

/* ICW1 的副作用按第 9-10 页 a-f 逐条：边沿检测重置（此刻为高的 IR 要先
 * 落下再升起才算请求，所以 armed 只留下当前为低的引脚）、清 IMR、IR7
 * 最低优先级、清特殊屏蔽并选中读 IRR、IC4=0 时 ICW4 的功能全部清零。
 * PIIX4 4.2.2.7 另说明初始化后轮转模式关闭。ISR 不清，理由见文件开头。
 * LTIM（bit 3）被 PIIX4 禁用、由 ELCR 取代（PIIX4 4.2.2.1），ADI 只用于
 * MCS-80 模式的 CALL 地址，都不看。 */
static void write_icw1(struct pic8259 *c, uint8_t val)
{
    c->imr = 0;
    c->armed = ~c->line;
    c->priority_add = 0;
    c->special_mask = false;
    c->read_isr = false;
    c->poll = false;
    c->rotate_on_aeoi = false;
    c->single = val & 0x02;
    c->need_icw4 = val & 0x01;
    if (!c->need_icw4) {
        c->auto_eoi = false;
        c->sfnm = false;
    }
    c->init_state = 1;
}

/* ICW2-4 和 OCW1 共用数据端口，靠初始化序列的进度区分（图 6）。
 * ICW4 只取 AEOI 和 SFNM：μPM 选 8086 还是 MCS-80 的 INTA 格式，而 x86
 * CPU 只发 8086 格式的两个 INTA（PIIX4 4.2.2.5 要求它为 1）；BUF、M/S
 * 控制的是 SP/EN 引脚和缓冲器，虚拟平台上没有这两样东西。 */
static void write_data(struct pic8259 *c, uint8_t val)
{
    switch (c->init_state) {
    case 1:
        c->vector_base = val & 0xF8;
        c->init_state = !c->single ? 2 : c->need_icw4 ? 3 : 0;
        break;
    case 2:
        c->icw3 = val;
        c->init_state = c->need_icw4 ? 3 : 0;
        break;
    case 3:
        c->auto_eoi = val & 0x02;
        c->sfnm = val & 0x10;
        c->init_state = 0;
        break;
    default:
        c->imr = val;
        break;
    }
}

static void write_byte(struct pic8259 *c, unsigned port, uint8_t val)
{
    if (port == 1)
        write_data(c, val);
    else if (val & 0x10)
        write_icw1(c, val);
    else if (val & 0x08)
        write_ocw3(c, val);
    else
        write_ocw2(c, val);
    update(c->pair);
}

/* 轮询命令（第 16 页）：下一次读命令端口被当作 INTA，置上 ISR，返回
 * I 位（bit 7）加上最高优先级请求的级别。轮询只作用于被读的那一片：
 * 软件轮询级联系统时自己再去读从片。 */
static uint8_t read_byte(struct pic8259 *c, unsigned port)
{
    int irq;

    if (port == 1)
        return c->imr;
    if (c->poll) {
        c->poll = false;
        irq = get_irq(c);
        if (irq < 0)
            return 0;
        ack(c, irq);
        update(c->pair);
        return 0x80 | irq;
    }
    return c->read_isr ? c->isr : irr(c);
}

/* 8 位设备上的宽访问按 ISA 总线的做法拆成逐字节的周期，落在两个端口之外
 * 的字节读回 0xFF、写入丢弃（ISA 上没人应答时数据线被上拉）。 */
int i8259_pio_read(void *opaque, uint64_t offset, uint32_t size, void *data)
{
    uint8_t *d = data;

    for (uint32_t i = 0; i < size; i++)
        d[i] = offset + i < 2 ? read_byte(opaque, offset + i) : 0xFF;
    return VMM_OK;
}

int i8259_pio_write(void *opaque, uint64_t offset, uint32_t size,
                    const void *data)
{
    const uint8_t *d = data;

    for (uint32_t i = 0; i < size; i++)
        if (offset + i < 2)
            write_byte(opaque, offset + i, d[i]);
    return VMM_OK;
}

int i8259_elcr_read(void *opaque, uint64_t offset, uint32_t size, void *data)
{
    struct i8259 *p = opaque;
    uint8_t *d = data;

    for (uint32_t i = 0; i < size; i++)
        d[i] = offset + i < 2 ? p->chip[offset + i].elcr : 0xFF;
    return VMM_OK;
}

int i8259_elcr_write(void *opaque, uint64_t offset, uint32_t size,
                     const void *data)
{
    struct i8259 *p = opaque;
    const uint8_t *d = data;

    for (uint32_t i = 0; i < size; i++) {
        struct pic8259 *c;

        if (offset + i >= 2)
            continue;
        c = &p->chip[offset + i];
        c->elcr = d[i] & c->elcr_mask;
    }
    update(p);
    return VMM_OK;
}

void i8259_set_irq(struct i8259 *p, int irq, int level)
{
    if (irq < 0 || irq > 15 || irq == I8259_CASCADE_IR)
        return;
    set_line(&p->chip[irq >> 3], irq & 7, level);
    update(p);
}

void i8259_reset(struct i8259 *p)
{
    for (int i = 0; i < 2; i++) {
        struct pic8259 *c = &p->chip[i];

        memset(c, 0, sizeof(*c));
        c->pair = p;
        c->is_slave = i == 1;
        c->elcr_mask = i == 0 ? ELCR1_MASK : ELCR2_MASK;
        c->armed = 0xFF;
    }
    update(p);
}

void i8259_init(struct i8259 *p, const struct i8259_ops *ops, void *opaque)
{
    memset(p, 0, sizeof(*p));
    p->ops = ops;
    p->opaque = opaque;
    i8259_reset(p);
}
