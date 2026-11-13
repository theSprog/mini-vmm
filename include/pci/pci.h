/* include/pci/pci.h — PCI 总线：Type 1 配置机制、type 0 配置空间、BAR、INTx 路由
 *
 * 一条总线（bus 0），最多 32 个 slot，每个 slot 只有 function 0。slot 0 固定
 * 是 host bridge，其余 slot 按 pci_register_device 的调用顺序分配。
 *
 * 配置空间按字节建模：每个设备有 256 字节的 cfg[] 和同样大小的 wmask[]，
 * guest 写入时只有 wmask 里为 1 的位会改变（只读位、未实现的寄存器自然
 * 写不进去）。BAR 的大小探测完全由 wmask 表达：一个 size 字节的 BAR，
 * 地址位里低于 size 的那些位不可写，guest 写全 1 再读回，得到的就是
 * ~(size - 1) 加上只读的类型位，不需要为“探测”写专门的代码。
 *
 * BAR 的 decode 由 command 寄存器的 IO/MEM 位和 BAR 当前的值共同决定。
 * 任何触及 command 或 BAR 的配置写之后重新计算每个 BAR 应当响应的地址，
 * 有变化就调 vmm_io_move 把对应的 I/O 表项挪过去或撤掉。
 *
 * BAR 地址由 VMM 在注册设备时分配，并打开 decode，这是固件的职责：
 * 真实机器上 BIOS/UEFI 在启动 OS 之前就给每个 BAR 分好了地址，Linux 默认
 * 沿用固件的分配（pcibios_resource_survey），只给没分配的 BAR 补分配。
 * 直启 Linux 时没有固件，VMM 替它做这一步，guest 看到的就是“固件已经
 * 分配好”的状态。
 *
 * INTx 是电平信号。设备用 pci_set_irq 报告自己“有没有待处理的中断”，
 * 核心据此维护三样东西：
 *   - status 的 Interrupt Status 位：如实反映设备的电平，不受屏蔽影响；
 *   - 设备实际驱动到线上的电平：电平为 1 且 command 的 INTx Disable 为 0；
 *   - 每根 INTx 线的电平：线上所有设备输出的“或”（INTx# 是开漏、低有效，
 *     任何一个设备拉低整根线就有效），只在变化时调 KVM_IRQ_LINE。
 * 线电平必须按“或”算，而不是让每个设备直接拉 GSI：同一根线上的两个设备，
 * 一个撤销中断时会把另一个仍在请求的中断一起拉低，后者的中断就丢了。
 * 撤销同样要真的拉低：IOAPIC 的 PCI 引脚是电平触发，线一直高着，guest 每次
 * EOI 后 IOAPIC 都会再投递一次，表现为中断风暴。
 *
 * MSI（PCI 3.0 第 6.8 节）：设备声明 msi 后，核心在配置空间里生成 MSI
 * capability，guest 写什么地址和数据、什么时候打开，全部由 wmask 和配置写
 * 处理，设备不用碰配置空间。打开 MSI Enable 之后设备不得再用 INTx 引脚
 * （第 6.8.3.3 节），所以 INTx 的输出电平里多了一个条件；设备照样用
 * pci_set_irq 报告电平，guest 关掉 MSI 的那一刻，还挂着的请求立刻回到
 * INTx 线上，不会丢。
 *
 * 什么时候发一条 MSI 由设备决定，核心只提供 pci_msi_notify“发一条”：电平
 * 和消息之间没有通用的换算，AHCI 就规定 IS 寄存器的某些变化各发一条
 * （AHCI 1.3.1 第 10.7.2.1 节），不是简单地在电平上升沿发。
 *
 * 发不出去的消息（Bus Master 关着、地址或数据不合法、KVM 没有 vCPU 收下）
 * 都记日志，带 BDF、地址、数据和原因。换成一条新的地址/数据组合时立刻记，
 * 同一个组合连续失败按第 1、2、4、8……次记：配置写坏时日志不会刷屏，丢了
 * 多少条也看得出来，guest 接连写出几种坏配置时每一种都能看到。
 *
 * capability 的形状固定为：单个向量（MMC = 0）、64 位地址、没有 per-vector
 * masking，放在 0x40，是链表里唯一的一项。64 位地址是规范的要求：设备作为
 * master 支持 64 位寻址时必须实现 Message Upper Address（第 6.8.1.5 节脚注），
 * 而做 DMA 的设备都是 64 位的。多向量和屏蔽位只有出现真正需要的设备时再加，
 * 那时 capability 的偏移也要改成由核心分配。
 *
 * DMA 经 pci_dma_read/pci_dma_write，它们先检查 command 的 Bus Master
 * Enable：规范规定这一位为 0 时设备不能发起总线访问（第 6.2.2 节），驱动
 * 在 probe 里调 pci_set_master 打开它。没打开就 DMA 说明设备模型或驱动
 * 次序有问题，拒绝并记一次日志，而不是悄悄读写 guest 内存。
 *
 * 并发：配置空间、BAR、INTx 状态只在 vm->io_lock 下访问。conf1 端口的
 * 访问本来就经 exit 分发进来、持着这把锁；I/O 线程上的设备回调先拿 io_lock
 * 再调这里的函数（加锁顺序见 io/event_loop.h）；注册发生在 guest 启动之前。
 *
 * 参考：PCI Local Bus Spec 3.0（ref/PCI_Local_Bus_3.0.pdf）第 3.2.2.3.2 节
 * （Type 1 配置机制）、第 6.1 节（配置空间头）、第 6.2.5 节（BAR）、
 * 第 6.7 节（capability 链表）、第 6.8 节（MSI）；linux/pci_regs.h；
 * Linux arch/x86/pci/direct.c（pci_check_type1）、drivers/pci/probe.c
 * （__pci_read_base 的探测顺序）。
 */
#ifndef PCI_PCI_H
#define PCI_PCI_H

#include <stdbool.h>
#include <stdint.h>

#include "vmm.h"
#include "memory.h"

struct vmm_vm;

#define PCI_MAX_SLOTS        32
#define PCI_CFG_SPACE_SIZE   256
#define PCI_NUM_BARS         6

/* Type 1 配置机制的两个端口：CONFIG_ADDRESS（只认 32 位访问）和
 * CONFIG_DATA（0xCFC-0xCFF，1/2/4 字节访问选中 dword 里的对应字节） */
#define PCI_CONF1_ADDR_PORT  0xCF8
#define PCI_CONF1_DATA_PORT  0xCFC

/* host bridge 向下转发的地址窗口，DSDT 的 _CRS 和 BAR 分配共用。
 *
 * MMIO 窗口就是 guest-memory 留出的 3G-4G 空洞去掉顶上的平台设备：
 * 0xFEC00000 起依次是 IOAPIC、HPET、LAPIC 和固件区，这些地址不经过 PCI。
 *
 * IO 窗口覆盖整个 64K 端口空间，只挖掉 conf1 自己的 0xCF8-0xCFF：传统 ISA
 * 设备（串口、i8042、ACPI PM）在真实机器上也挂在 host bridge 下面，由
 * PCI-ISA 桥做减法译码，所以它们的端口本来就落在根总线窗口里。 */
#define PCI_MMIO_WINDOW_START  VMM_MEM_HOLE_START
#define PCI_MMIO_WINDOW_END    0xFEC00000ULL          /* 不含 */
#define PCI_IO_WINDOW_END      0x10000U               /* 不含 */

/* VMM 分配 IO BAR 时从这里开始：0x1000 以下是 ISA 时代的固定端口，
 * Linux 自己分配时也从这里起（arch/x86 的 PCIBIOS_MIN_IO）。 */
#define PCI_IO_ALLOC_START     0x1000U

/* INTx 接线：slot s 的 INTA..INTD（pin 1..4）接 IOAPIC 引脚
 * 16 + ((s + pin - 1) & 3)。
 *
 * IOAPIC 的 0-15 脚留给 ISA IRQ（KVM 默认 GSI 路由是恒等的），PCI 从 16
 * 开始。相邻 slot 的同一个 pin 错开一根线（swizzle），这样都只用 INTA 的
 * 单功能设备会均匀分到 4 根线上，而不是全挤在一根上共享。
 *
 * DSDT 的 _PRT 和 MP table 的 PCI 中断项都由这个函数生成，两种固件表
 * 描述的是同一套接线。 */
#define PCI_INTX_GSI_BASE      16
#define PCI_INTX_LINES         4

static inline int pci_intx_gsi(int slot, int pin)
{
    return PCI_INTX_GSI_BASE + ((slot + pin - 1) & (PCI_INTX_LINES - 1));
}

/* 本项目自己的设备用的 vendor ID。0x1234 不在 PCI-SIG 的分配表里
 * （pci.ids 没有这个 vendor），QEMU 也拿它标识自己的模拟设备。Linux 里
 * 唯一按 0x1234 匹配 vendor 的驱动是 bochs（device 0x1111），本项目的
 * device ID 都避开了它，所以按 vendor/device 匹配的驱动不会绑上来。按类代码
 * 匹配的驱动不看 vendor ID：ahci 设备（类代码 01/06/01）靠 Linux ahci 驱动的
 * 通用类匹配表项绑定，见 include/devices/ahci.h。 */
#define PCI_VENDOR_ID_MINIVMM      0x1234
#define PCI_DEVICE_ID_HOST_BRIDGE  0x0001

enum pci_bar_type {
    PCI_BAR_UNUSED = 0,
    PCI_BAR_IO,          /* 16 位 IO 地址，size 至少 4 */
    PCI_BAR_MEM32,       /* 32 位、不可预取，size 至少 16 */
};

struct pci_bar {
    /* 由设备填写 */
    enum pci_bar_type type;
    uint32_t size;                 /* 2 的幂 */
    void    *opaque;
    int (*read)(void *opaque, uint64_t offset, uint32_t size, void *data);
    int (*write)(void *opaque, uint64_t offset, uint32_t size,
                 const void *data);

    /* 以下由 PCI 核心维护 */
    char     name[32];             /* "<设备名>.bar<i>"，exit 统计里的设备名 */
    int      io_slot;              /* 在 vm 的 PIO/MMIO 表里的下标 */
    uint64_t base;                 /* 当前 decode 的起始地址 */
    bool     mapped;               /* 当前是否在响应 */
};

struct pci_bus;

struct pci_device {
    /* 由设备填写 */
    const char *name;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t class_code;           /* 24 位：base class | sub class | prog-if */
    uint8_t  revision;
    /* 0 表示不填（保持 0）。真实板卡由 EEPROM 或固件写入，驱动有时按它区分
     * 同一芯片的不同板型 */
    uint16_t subsys_vendor_id;
    uint16_t subsys_id;
    uint8_t  intx_pin;             /* 0 = 不用 INTx，1..4 = INTA..INTD */
    bool     bus_master;           /* 会做 DMA：command 的 Bus Master 位可写 */
    bool     msi;                  /* 有 MSI capability，Bus Master 位也随之可写 */
    struct pci_bar bars[PCI_NUM_BARS];

    /* 以下由 PCI 核心维护 */
    struct pci_bus *bus;
    int      slot;
    uint8_t  cfg[PCI_CFG_SPACE_SIZE];
    uint8_t  wmask[PCI_CFG_SPACE_SIZE];
    bool     intx_level;           /* 设备内部的中断请求电平 */
    bool     dma_blocked_logged;   /* Bus Master 关闭时的 DMA 只记一次日志 */
    bool     msi_was_enabled;      /* 上次配置写之后的 MSI Enable，用来记切换 */
    uint64_t msi_sent;             /* 交给 KVM 并被收下的消息数 */
    uint64_t msi_dropped;          /* 没有送达的消息数 */
    uint64_t msi_drop_addr;        /* 最近一次没送达的消息，日志节流用 */
    uint32_t msi_drop_data;
    uint64_t msi_drop_run;         /* 这个组合连续没送达的次数 */
};

struct pci_bus {
    struct vmm_vm     *vm;
    struct pci_device *devs[PCI_MAX_SLOTS];
    struct pci_device  host_bridge;
    uint32_t           conf1_addr;    /* CONFIG_ADDRESS 当前值 */
    uint64_t           mmio_next;     /* BAR 分配游标 */
    uint32_t           io_next;
    bool               intx_line[PCI_INTX_LINES];  /* 上次送给 KVM 的线电平 */
};

/* 注册 conf1 两个端口，把 host bridge 放进 slot 0 */
int  pci_bus_init(struct pci_bus *bus, struct vmm_vm *vm);

/* 把设备放进下一个空 slot：生成配置空间，为每个 BAR 占一个 I/O 表项，
 * 分配地址并打开 decode。dev 的生命周期必须覆盖整个 VM 运行期。 */
int  pci_register_device(struct pci_bus *bus, struct pci_device *dev);

/* 配置空间访问。conf1 端口在内部调用它们，以后的 ECAM 也会用；单元测试
 * 直接驱动这两个函数。不存在的设备读回全 1（master abort），写被丢弃。
 * reg + size 必须落在同一个 dword 内，size 为 1/2/4。 */
uint32_t pci_cfg_read(struct pci_bus *bus, uint8_t busno, uint8_t devfn,
                      uint16_t reg, uint32_t size);
void     pci_cfg_write(struct pci_bus *bus, uint8_t busno, uint8_t devfn,
                       uint16_t reg, uint32_t size, uint32_t val);

/* 设备报告自己的中断请求电平。重复设置同一电平没有副作用。
 * 设备没有 INTx（intx_pin == 0）时调用是设备模型的 bug，只记日志。 */
void pci_set_irq(struct pci_device *dev, bool level);

/* MSI capability 在配置空间里的位置（见文件头） */
#define PCI_MSI_CAP_OFFSET  0x40

/* guest 是否打开了 MSI。设备没有 MSI capability 时恒为 false。 */
bool pci_msi_enabled(const struct pci_device *dev);

/* 按当前的地址和数据发一条 MSI。只在 pci_msi_enabled 为真时调用；发不出去
 * 的原因见文件头，记日志、计入 msi_dropped，不返回错误：设备对一次丢掉的
 * 中断也做不了什么，它该做的状态更新照常做。 */
void pci_msi_notify(struct pci_device *dev);

/* 设备发起的 DMA。Bus Master 关闭返回 VMM_ERR_GUEST；地址范围的检查和
 * 失败原因同 mem_read/mem_write。 */
int  pci_dma_read(struct pci_device *dev, uint64_t gpa, void *dst, size_t len);
int  pci_dma_write(struct pci_device *dev, uint64_t gpa, const void *src,
                   size_t len);

/* 把 [gpa, gpa+len) 映射成 host 指针，给设备直接读写（例如拼成 iovec
 * 交给 preadv），省掉一次经中间缓冲的拷贝。检查同上：Bus Master 关闭或
 * 范围没有完整落在一个 RAM slot 里时返回 VMM_ERR_GUEST，*hva 不变，失败
 * 原因记进日志。guest 内存布局在 VM 运行期间不变，所以指针一直有效；但
 * 调用方只在处理这一次请求时使用，不把它保存下来，免得 guest 改写描述符
 * 之后设备还在按旧地址访问。 */
int  pci_dma_map(struct pci_device *dev, uint64_t gpa, size_t len, void **hva);

#endif /* PCI_PCI_H */
