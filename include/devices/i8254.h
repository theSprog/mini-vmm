/* include/devices/i8254.h — 用户态 8254 定时器和 PIIX4 的 port 0x61
 *
 * split irqchip 模式下 PIT 由这个模块模拟：counter 0-2 在端口 0x40-0x42，
 * 控制字在 0x43；PIIX4 的 NMISC（port 0x61）给 counter 2 提供 GATE 并读回
 * OUT2。PIIX4 的接线（第 8.8 节）：三个 counter 都用 OSC 14.31818 MHz 的
 * 12 分频（1.193182 MHz，周期 838 ns）作 CLK；OUT0 接 IRQ0；OUT1 产生
 * refresh request；OUT2 经 NMISC bit 1 去扬声器；GATE0、GATE1 固定为高。
 *
 * 规范：
 *   - ref/82C54_PIT_231244.pdf（Intel 231244-006）：控制字、读写顺序、
 *     latch / read-back、六种模式和 GATE 的语义；
 *   - ref/82371AB_PIIX4_290562.pdf 第 4.2.3 节（寄存器）、4.2.4.1（NMISC）、
 *     8.8（接线和时钟）。
 *
 * 时间模型：counter 的状态不随 CLK 逐拍推进，而是记下“第几个 CLK 脉冲把
 * 什么值装进了 CE”，读数、OUT 电平和下一个跳变都由经过的脉冲数算出来。
 * 1.19 MHz 逐拍模拟要每秒唤醒一百多万次，解析式只在 guest 访问端口或
 * 定时器到期时计算一次。时钟由 ops->now_ns 提供，单元测试用假时钟。
 *
 * OUT0 的中断：mode 2 的 OUT0 低电平只持续一个 CLK（838 ns），定时器
 * 不可能卡在这个窗口里采样电平，所以模块不报告电平跳变，而是累计 OUT 的
 * 上升沿（边沿触发的 IRQ0 真正关心的事件），由调用方经 i8254_poll 取走，
 * 再决定每个沿怎样送进 PIC 和 IOAPIC。定时器晚到时一次会取到多个沿，补发
 * 还是合并是调用方的策略。
 *
 * 并发：和 i8259.h 相同，入口由调用方持 split_irqchip.lock 调用。
 */
#ifndef DEVICES_I8254_H
#define DEVICES_I8254_H

#include <stdint.h>
#include <stdbool.h>

#define I8254_BASE          0x40    /* counter 0-2，0x43 控制字 */
#define I8254_NMISC_PORT    0x61

/* CLK = 14318180 Hz / 12（PIIX4 8.8） */
#define I8254_OSC_HZ        14318180ull
#define I8254_OSC_DIV       12ull

#define I8254_NO_EVENT      UINT64_MAX

/* 计数元件 CE 的一段连续计数：第 start 个 CLK 脉冲把 n 装进 CE，此后
 * 每个脉冲按模式递减。start 之前（装入还没发生）读到 pre_ce、OUT 为
 * pre_out。段内的位置 d = 已递减的脉冲数 + phase，读数和 OUT 都是 d 的
 * 函数。phase 只在 mode 3 于半周期末尾换上新计数时非 0：新值从低半周期
 * 开始走，等价于段从 d = 新值的高半周期长度处开始。 */
struct i8254_ce {
    bool     active;        /* false：CE 停在 idle_ce，OUT 为 idle_out */
    uint32_t n;             /* 装入值，二进制 1..模数（计数 0 表示模数） */
    uint64_t start;
    uint32_t phase;
    uint32_t pre_ce;
    bool     pre_out;
    bool     stopped;       /* 计数暂停：GATE 低，或 mode 0 只写了半个计数 */
    uint64_t stop_tick;     /* 暂停从哪个脉冲开始 */
    int      forced_out;    /* -1 不强制；0/1 OUT 固定为该值 */
    bool     pend;          /* mode 2/3 计数中写入的新值，在 pend_tick 生效 */
    bool     pend_low;      /* mode 3：pend_tick 是高半周期的末尾 */
    uint32_t pend_n;
    uint64_t pend_tick;
};

struct i8254_counter {
    uint8_t  cw;            /* 最后一次控制字的 bit 5:0（RW、M、BCD），status 原样回读 */
    uint8_t  mode;          /* 0-5，X10/X11 已归一 */
    uint8_t  rw;            /* 1 LSB，2 MSB，3 LSB 后 MSB */
    bool     bcd;
    uint16_t cr;            /* count register，写入的原始值 */
    bool     cr_valid;      /* 控制字之后已写完一个完整计数 */
    bool     wr_msb;        /* rw=3：下一次写的是 MSB */
    bool     rd_msb;        /* rw=3：下一次读的是 MSB */
    bool     gate;
    bool     hold;          /* mode 0 写了 LSB 还没写 MSB，计数暂停 */

    uint16_t ol;            /* output latch 锁住的计数 */
    uint8_t  ol_left;       /* 锁存的计数还剩几个字节没读，0 = 跟随 CE */
    bool     status_latched;
    uint8_t  status;

    uint64_t cr_load_tick;  /* CR 在这个脉冲装进 CE；之前 null count 为 1 */

    struct i8254_ce ce;
    uint32_t idle_ce;
    bool     idle_out;

    uint64_t last_tick;     /* 上升沿已累计到这个脉冲 */
    uint64_t rises;         /* OUT 上升沿总数 */
    uint64_t rises_polled;  /* i8254_poll 已取走的部分 */
};

struct i8254_ops {
    /* 单调时钟，纳秒。所有计时都从它换算成 CLK 脉冲号。 */
    uint64_t (*now_ns)(void *opaque);
};

struct i8254 {
    struct i8254_counter c[3];
    uint8_t nmisc;          /* port 0x61 的可写位 3:0 */

    const struct i8254_ops *ops;
    void *opaque;
};

void i8254_init(struct i8254 *pit, const struct i8254_ops *ops, void *opaque);

/* CPU RESET。82C54 上电后模式、计数和 OUT 都未定义；这里取 PIIX4 寄存器表
 * 能给出的部分（TMRSTS bit 7 即 OUT 为 0，NMISC 为 00h，所以 GATE2 为 0），
 * 其余取“未编程”：不计数、不产生上升沿，直到 guest 写控制字和计数。 */
void i8254_reset(struct i8254 *pit);

/* PIO 回调，签名与 struct vmm_io_dev 一致，opaque 是 struct i8254 *。
 * offset 0-2 = counter 0-2，3 = 控制字（只写，读回 0xFF）。宽访问按字节
 * 拆成连续端口。 */
int  i8254_pio_read(void *opaque, uint64_t offset, uint32_t size, void *data);
int  i8254_pio_write(void *opaque, uint64_t offset, uint32_t size,
                     const void *data);

/* port 0x61（NMISC）的 PIO 回调，opaque 是 struct i8254 *。 */
int  i8254_nmisc_read(void *opaque, uint64_t offset, uint32_t size, void *data);
int  i8254_nmisc_write(void *opaque, uint64_t offset, uint32_t size,
                       const void *data);

/* 取走 counter ch 自上次调用以来 OUT 的上升沿个数，*level 返回 OUT 当前
 * 电平。上升沿包括计数产生的和 guest 写端口直接造成的（例如写 mode 2 的
 * 控制字把 OUT 从低拉高）。 */
uint64_t i8254_poll(struct i8254 *pit, int ch, int *level);

/* counter ch 下一个 OUT 上升沿的时刻（ops->now_ns 的时间轴，向上取整到
 * 那个脉冲之后），没有已排定的上升沿时返回 I8254_NO_EVENT。guest 写端口
 * 可能改变它，调用方在每次写之后重新查询。 */
uint64_t i8254_next_rise_ns(struct i8254 *pit, int ch);

#endif /* DEVICES_I8254_H */
