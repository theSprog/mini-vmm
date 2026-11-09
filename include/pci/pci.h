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
 * 并发：配置空间和 BAR 状态只在 vm->io_lock 下访问。conf1 端口的访问本来
 * 就经 exit 分发进来、持着这把锁；注册发生在 guest 启动之前。
 *
 * 参考：PCI Local Bus Spec 3.0 第 3.2.2.3.2 节（Type 1 配置机制）、
 * 第 6.1 节（配置空间头）、第 6.2.5 节（BAR）；linux/pci_regs.h；
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
 * 唯一按 0x1234 匹配的驱动是 bochs（device 0x1111），下面的 device ID 都
 * 避开了它，guest 不会给这些设备绑定任何驱动。 */
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
    uint8_t  intx_pin;             /* 0 = 不用 INTx，1..4 = INTA..INTD */
    struct pci_bar bars[PCI_NUM_BARS];

    /* 以下由 PCI 核心维护 */
    struct pci_bus *bus;
    int      slot;
    uint8_t  cfg[PCI_CFG_SPACE_SIZE];
    uint8_t  wmask[PCI_CFG_SPACE_SIZE];
};

struct pci_bus {
    struct vmm_vm     *vm;
    struct pci_device *devs[PCI_MAX_SLOTS];
    struct pci_device  host_bridge;
    uint32_t           conf1_addr;    /* CONFIG_ADDRESS 当前值 */
    uint64_t           mmio_next;     /* BAR 分配游标 */
    uint32_t           io_next;
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

#endif /* PCI_PCI_H */
