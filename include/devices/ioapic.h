/* include/devices/ioapic.h — 用户态 IOAPIC（Intel 82093AA）
 *
 * split irqchip 模式下 LAPIC 还在 KVM 里，IOAPIC 由这个模块模拟。它只
 * 负责芯片本身的语义：IOREGSEL/IOWIN 间接寻址、24 条重定向表项（RTE）、
 * 引脚电平到中断消息的转换、level 触发的 Remote IRR 握手。怎么把消息
 * 交给 KVM、RTE 改了之后怎么同步 KVM 的 GSI 路由，都经 ioapic_ops 回调
 * 交给调用方，所以单元测试不需要 KVM。
 *
 * 规范：ref/82093AA_IOAPIC_290566.pdf（Intel 290566-001）第 3 章。
 *
 * 并发：结构体本身不带锁。所有入口（MMIO、set_irq、eoi）由调用方持同一
 * 把锁调用，split 模式下是 split_irqchip.lock（为什么不是 vm->io_lock 见
 * split_irqchip.h 的加锁一节）；回调也在这把锁下被调用，回调里不能再取它。
 */
#ifndef DEVICES_IOAPIC_H
#define DEVICES_IOAPIC_H

#include <stdint.h>
#include <stdbool.h>

#define IOAPIC_BASE          0xFEC00000u
/* 数据手册表 1：IOREGSEL 在 00h，IOWIN 在 10h，两者之外没有直接可寻址的
 * 寄存器，所以窗口只占 0x20 字节；页内其余地址按未注册 MMIO 处理。 */
#define IOAPIC_MMIO_SIZE     0x20u
#define IOAPIC_NUM_PINS      24

/* 间接寄存器（数据手册表 2） */
#define IOAPIC_REG_ID        0x00
#define IOAPIC_REG_VER       0x01
#define IOAPIC_REG_ARB       0x02
#define IOAPIC_REG_REDTBL    0x10   /* 0x10 + 2*pin 低 32 位，+1 高 32 位 */

/* IOAPICVER 的复位值 00170011h（第 3.2.2 节）：最大表项号 17h（24 条），
 * 版本 11h。版本 11h 意味着没有 EOI 寄存器（Linux 只在版本 >= 20h 时
 * 写它，arch/x86/kernel/apic/io_apic.c __eoi_ioapic_pin），Remote IRR
 * 只能由 LAPIC 广播的 EOI 或下面的“切边沿”清掉。 */
#define IOAPIC_VERSION       0x00170011u

/* RTE 位域（第 3.2.4 节） */
#define IOAPIC_RTE_VECTOR(e)     ((uint32_t)(e) & 0xFFu)
#define IOAPIC_RTE_DELMOD(e)     (((uint32_t)(e) >> 8) & 0x7u)
#define IOAPIC_RTE_DESTMOD       (1ull << 11)
#define IOAPIC_RTE_DELIVS        (1ull << 12)
#define IOAPIC_RTE_INTPOL        (1ull << 13)
#define IOAPIC_RTE_REMOTE_IRR    (1ull << 14)
#define IOAPIC_RTE_LEVEL         (1ull << 15)
#define IOAPIC_RTE_MASK          (1ull << 16)
#define IOAPIC_RTE_DEST(e)       ((uint32_t)((e) >> 56) & 0xFFu)

#define IOAPIC_DELMOD_FIXED      0
#define IOAPIC_DELMOD_LOWEST     1

struct ioapic_ops {
    /* 发出一条中断消息（x86 MSI 格式，见 ioapic_rte_msi）。返回收下它的
     * LAPIC 个数，0 表示没人收；负值表示投递本身失败。原因由回调记日志，
     * 它知道失败来自哪个 ioctl。 */
    int  (*deliver)(void *opaque, uint64_t addr, uint32_t data);
    /* guest 改写了 pin 的 RTE（值确实变了）之后调用；可以为 NULL。
     * split 模式靠它把 RTE 镜像进 KVM 的 GSI 路由表：KVM 只对路由表里
     * 登记为 level 触发的向量产生 KVM_EXIT_IOAPIC_EOI。 */
    void (*rte_changed)(void *opaque, int pin);
};

struct ioapic {
    uint8_t  ioregsel;
    uint8_t  id;                        /* IOAPICID[27:24]，4 位 */
    uint64_t redtbl[IOAPIC_NUM_PINS];
    uint32_t line;                      /* bit n = 引脚 n 当前是否被断言 */

    /* 每根引脚发出且被收下的消息数、发出但没人收或投递失败的次数。
     * 中断丢失时先看这两组数，判断是没发出来还是发出来没人收。 */
    uint64_t delivered[IOAPIC_NUM_PINS];
    uint64_t lost[IOAPIC_NUM_PINS];

    const struct ioapic_ops *ops;
    void    *opaque;
};

void ioapic_init(struct ioapic *io, const struct ioapic_ops *ops, void *opaque);

/* RESET 引脚：所有寄存器回到数据手册的复位值，引脚电平记录清零。
 * 引脚电平属于外部设备，复位后由设备重新驱动。 */
void ioapic_reset(struct ioapic *io);

/* MMIO 回调，签名与 struct vmm_io_dev 一致；offset 相对 IOAPIC_BASE。 */
int  ioapic_mmio_read(void *opaque, uint64_t offset, uint32_t size, void *data);
int  ioapic_mmio_write(void *opaque, uint64_t offset, uint32_t size,
                       const void *data);

/* 设备驱动引脚。level 是“断言 / 撤销”，不是电压：
 * INTPOL 只记录不参与判断。guest 按固件表（MADT ISO、_PRT）描述的极性
 * 编程 INTPOL，而这里的连线本来就传递断言与否，等于极性已经在连线两端
 * 抵消；如果再按 INTPOL 取反，PCI INTx（Linux 按 _PRT 默认编成低有效）
 * 就会在设备拉高时被当成撤销。 */
void ioapic_set_irq(struct ioapic *io, int pin, int level);

/* LAPIC 广播的 EOI：清掉向量匹配的 level 表项的 Remote IRR，引脚仍被
 * 断言时立即重新投递（第 3.2.4 节 bit 14）。 */
void ioapic_eoi(struct ioapic *io, uint8_t vector);

/* RTE 是否按 level 语义工作。数据手册规定 SMI、NMI、INIT、ExtINT 一律
 * 按边沿处理，不论 bit 15 怎么编（第 3.2.4 节 bit 10:8 的表），所以只有
 * Fixed / Lowest Priority 才看 bit 15。 */
static inline bool ioapic_rte_is_level(uint64_t rte)
{
    uint32_t dm = IOAPIC_RTE_DELMOD(rte);

    return (rte & IOAPIC_RTE_LEVEL) &&
           (dm == IOAPIC_DELMOD_FIXED || dm == IOAPIC_DELMOD_LOWEST);
}

/* RTE → 中断消息。82093AA 用 APIC 总线发消息，KVM 的 split 模式只收 MSI
 * 格式（KVM_SIGNAL_MSI、MSI 类型的 GSI 路由），所以逐字段翻译成 Intel SDM
 * Vol.3 325384-093US 第 13.11.1、13.11.2 节的布局：
 *   地址 bit 19:12 = RTE[63:56] 目标，bit 2 = RTE[11] 目标模式；
 *   数据 bit 7:0 = 向量，bit 10:8 = 投递模式，bit 15 = 触发模式，
 *   bit 14 = 1（断言；level 消息只在断言时发出）。
 * 目标取满 8 位：数据手册的物理模式只定义了 59:56（APIC ID 0-15），而本
 * VMM 支持 32 个 vCPU，Linux 把完整的 8 位 APIC ID 写进 63:56
 * （io_apic.c 里 entry.destid_0_7）。接收端是 KVM 的 kvm_set_msi_irq
 * （arch/x86/kvm/irq_comm.c），它按上面的位置取各字段，目标模式只看
 * bit 2。
 * split 模式的 GSI 路由镜像也用这个函数，保证路由里登记的向量和触发
 * 模式就是实际投递的那条消息。 */
static inline void ioapic_rte_msi(uint64_t rte, uint64_t *addr, uint32_t *data)
{
    *addr = 0xFEE00000ull |
            ((uint64_t)IOAPIC_RTE_DEST(rte) << 12) |
            ((rte & IOAPIC_RTE_DESTMOD) ? (1ull << 2) : 0);
    *data = IOAPIC_RTE_VECTOR(rte) |
            (IOAPIC_RTE_DELMOD(rte) << 8) |
            (ioapic_rte_is_level(rte) ? (1u << 15) : 0) |
            (1u << 14);
}

#endif /* DEVICES_IOAPIC_H */
