/* include/devices/i8259.h — 用户态 8259A 中断控制器对（PIIX4 平台）
 *
 * split irqchip 模式下 PIC 由这个模块模拟：主片端口 0x20/0x21，从片
 * 0xA0/0xA1，从片 INT 输出接主片 IR2；边沿 / 电平由 PIIX4 的 ELCR
 * （0x4D0/0x4D1）逐根 IRQ 选择。主片 INT 输出经 ops->int_changed 交给
 * 调用方，CPU 侧的 INTA 周期由 i8259_inta 完成，所以单元测试不需要 KVM。
 *
 * 规范：
 *   - ref/8259A_PIC_231468.pdf（Intel 231468-003）：芯片语义。图 9 的
 *     priority cell 决定了 IRR、边沿检测和 ICW1 的精确行为，见 i8259.c；
 *   - ref/82371AB_PIIX4_290562.pdf 第 4.2.2 节：PIIX4 对 ICW 取值的约束和
 *     ELCR 寄存器；第 8.6.5 节：ELCR 取代 ICW1 的 LTIM 位。
 *
 * 两份文档冲突时按 8259A：PIIX4 的 OCW1 说被屏蔽的 IRQ 不置 IRR，8259A 说
 * IRR 不受 IMR 影响，而 Linux 恰好依赖后者（io_apic.c startup_ioapic_irq
 * 先屏蔽 legacy IRQ 再读 IRR 看它是否挂起）；PIIX4 的 OCW3 文字把 bit 6/5
 * 写成 SMM/ESMM，和 8259A 图 8 的 ESMM/SMM 相反，这里按 8259A。
 *
 * 并发：和 ioapic.h 相同，入口由调用方持 split_irqchip.lock 调用，回调
 * 也在这把锁下被调用。
 */
#ifndef DEVICES_I8259_H
#define DEVICES_I8259_H

#include <stdint.h>
#include <stdbool.h>

#define I8259_MASTER_BASE   0x20
#define I8259_SLAVE_BASE    0xA0
#define I8259_ELCR_BASE     0x4D0
#define I8259_CASCADE_IR    2       /* 从片 INT 接主片 IR2（PIIX4 4.2.2.3） */

struct i8259;

/* 一片 8259A。irr 不单独存：图 9 里 request latch 是透明锁存器，
 * IRR = 引脚电平 & (电平模式 | 边沿已武装)，读的时候现算。 */
struct pic8259 {
    uint8_t line;           /* IR0-7 当前电平 */
    uint8_t armed;          /* 图 9 的 edge sense latch */
    uint8_t isr;
    uint8_t imr;
    uint8_t elcr;           /* 1 = 电平触发 */
    uint8_t elcr_mask;      /* ELCR 里可编程的位（PIIX4 4.2.2.9/10） */
    uint8_t priority_add;   /* 当前最高优先级的 IR 号；复位后 IR0 最高 */
    uint8_t vector_base;    /* ICW2 bit 7:3 */
    uint8_t icw3;           /* 主片：哪些 IR 接了从片 */
    uint8_t init_state;     /* 0 = 正常工作，1/2/3 = 等 ICW2/ICW3/ICW4 */
    bool    single;         /* ICW1 SNGL */
    bool    need_icw4;      /* ICW1 IC4 */
    bool    read_isr;       /* OCW3 RR/RIS 选中的是 ISR */
    bool    poll;           /* OCW3 P：下一次读命令端口当 INTA */
    bool    special_mask;
    bool    auto_eoi;
    bool    rotate_on_aeoi;
    bool    sfnm;           /* 特殊全嵌套，只对主片有意义 */

    struct i8259 *pair;
    bool    is_slave;
};

struct i8259_ops {
    /* 主片 INT 输出电平变化时调用。调用方据此安排向 vCPU 注入 ExtINT，
     * 注入的那一刻调 i8259_inta 取向量。 */
    void (*int_changed)(void *opaque, int level);
};

struct i8259 {
    struct pic8259 chip[2];     /* [0] 主片，[1] 从片 */
    bool  int_out;              /* 主片 INT 输出的当前电平 */

    /* INTA 交出的每个 IRQ 的次数，以及两片各自的伪中断（交出 IR7 却没有
     * 置 ISR）次数。中断丢失或风暴时先看这些数。 */
    uint64_t acked[16];
    uint64_t spurious[2];

    const struct i8259_ops *ops;
    void *opaque;
};

void i8259_init(struct i8259 *p, const struct i8259_ops *ops, void *opaque);

/* CPU RESET：8259A 自身没有 RESET 引脚，复位后的状态取 PIIX4 寄存器表的
 * Default Value（IMR 00h、ELCR 00h、ICW4 01h、读 IRR），未定义的部分取 0。
 * 引脚电平属于外部设备，一并清零，由设备重新驱动。 */
void i8259_reset(struct i8259 *p);

/* PIO 回调，签名与 struct vmm_io_dev 一致。opaque 是 &p->chip[0] 或
 * &p->chip[1]，offset 0 = 命令端口（A0=0），1 = 数据端口（A0=1）。 */
int  i8259_pio_read(void *opaque, uint64_t offset, uint32_t size, void *data);
int  i8259_pio_write(void *opaque, uint64_t offset, uint32_t size,
                     const void *data);

/* ELCR 的 PIO 回调，opaque 是 struct i8259 *，offset 0 = ELCR1，1 = ELCR2 */
int  i8259_elcr_read(void *opaque, uint64_t offset, uint32_t size, void *data);
int  i8259_elcr_write(void *opaque, uint64_t offset, uint32_t size,
                      const void *data);

/* ISA IRQ 0-15 的电平，level 为 1 表示断言。IRQ 2 是级联输入，由从片
 * 驱动，外部设备驱动它会被忽略。 */
void i8259_set_irq(struct i8259 *p, int irq, int level);

/* CPU 的 INTA 周期（8086 模式的两个 INTA 脉冲合成一步），返回 CPU 拿到的
 * 向量。没有请求时按数据手册交出 IR7 的向量且不置 ISR（伪中断）。 */
uint8_t i8259_inta(struct i8259 *p);

#endif /* DEVICES_I8259_H */
