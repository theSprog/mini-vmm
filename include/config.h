/* include/config.h — 命令行给出的整机配置
 *
 * struct vmm_config 是 argv 解析后的结果，由 src/config.c 填写，之后只读：
 * vm_create 把它整份拷进 vm->cfg，各模块从那里取自己的选项。选项之间的
 * 组合（某选项只在 linux 模式下有意义、--device 引用的 netdev 存在）在
 * 解析时一次查完，后面的代码可以假设配置是自洽的。
 */
#ifndef VMM_CONFIG_H
#define VMM_CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "boot/gdt_pgtable.h"
#include "devices/device.h"

#define VMM_MAX_VCPUS        32
/* --device 的上限：PCI 总线 32 个 slot 去掉 host bridge（pci/pci.h） */
#define VMM_MAX_DEVICES      31

/* 默认 guest RAM：128 MiB。
 * 注意：长模式 payload 要往 GPA 0x500000 (5 MiB) 写数据，2 MiB 内存是不够的，
 * 所以 --mem 是命令行可调的，只有实模式 payload 才用 2M。 */
#define VMM_DEFAULT_RAM_SIZE (128ULL << 20)

enum vmm_boot_mode {
    VMM_BOOT_REAL16 = 0,  /* 16 位实模式 payload，CS:IP = 0000:entry */
    VMM_BOOT_LONG64,      /* 直接进 64 位长模式的 payload */
    VMM_BOOT_LINUX,       /* Linux 直启：vmlinux/bzImage + zeropage */
};

/* 取值沿用 QEMU 的 kernel-irqchip=on|split|off。off（LAPIC 也在用户态）
 * 属于 user-lapic 里程碑，命令行解析时拒绝 */
enum vmm_irqchip_mode {
    VMM_IRQCHIP_ON = 0,     /* PIC、IOAPIC、PIT、LAPIC 全在 KVM */
    VMM_IRQCHIP_SPLIT,      /* 只有 LAPIC 在 KVM */
};

struct vmm_config {
    enum vmm_boot_mode mode;
    uint64_t    ram_size;      /* 字节 */
    int         nr_vcpus;
    const char *payload_path;  /* 裸机器码 .bin，或 vmlinux/bzImage */
    uint64_t    payload_gpa;   /* 载入地址；实模式 payload 为 0x0 */
    uint64_t    entry_gpa;     /* 首条指令的 GPA，默认 = payload_gpa */
    int         log_level;
    bool        dump_on_exit;  /* 退出时 dump 一段 guest 内存（验证长模式 payload 用） */
    uint64_t    dump_gpa;
    size_t      dump_len;

    /* long64 payload 的启动页表：粒度（--gran）和恒等映射的范围（--map）。
     * walk_gva 是启动前要打印页表遍历过程的 GVA（--walk），UINT64_MAX
     * 表示不打印 */
    enum boot_page_gran pt_gran;
    uint64_t    map_size;
    uint64_t    walk_gva;

    /* Linux 直启 */
    const char *initrd_path;
    const char *cmdline;

    /* 把每一次陷出到用户态的 PIO 访问写进这个文件（--trace-pio）。
     * 用来回答“驱动到底按什么顺序读写了哪些寄存器”，见 learning-note 卷七。 */
    const char *trace_pio_path;
    /* 同上，记录陷出到用户态的 MMIO 访问（--trace-mmio） */
    const char *trace_mmio_path;

    /* VM 停下后把 exit 统计写到这里（--exit-stats），"-" 表示 stderr */
    const char *exit_stats_path;

    /* --device 给出的 PCI 设备，按命令行顺序占 slot 1 起的位置。
     * 上限是总线上除 host bridge 之外的 slot 数。 */
    struct vmm_device devices[VMM_MAX_DEVICES];
    int         nr_devices;

    /* --netdev 给出的网络后端。每个后端恰好接一个网卡，所以上限和设备数
     * 相同；对象在解析阶段分配，见 net/netdev.h */
    struct netdev *netdevs[VMM_MAX_DEVICES];
    int         nr_netdevs;

    /* --drive 给出的磁盘后端，道理同 netdevs，见 block/blockdev.h */
    struct blockdev *drives[VMM_MAX_DEVICES];
    int         nr_drives;

    /* --gdb 的 TCP 端口，0 表示由内核分配，-1 表示不启用 gdbstub */
    int         gdb_port;

    /* --no-reboot：guest 请求复位时 VMM 退出，而不是复位后再启动 */
    bool        no_reboot;

    /* --monitor 的 unix socket 路径，NULL 表示不启用，见 monitor.h */
    const char *monitor_path;

    /* --kernel-irqchip：PIC/IOAPIC/PIT 在 KVM 里还是在用户态 */
    enum vmm_irqchip_mode irqchip;

    /* --pit-reinject：PIT 通道 0 的 tick 因为 guest 没及时确认而积压时，
     * true 逐个补发（KVM 内核 PIT 的默认），false 合并成一次。两种 irqchip
     * 模式语义相同，见 devices/split_irqchip.h */
    bool pit_reinject;
};

void vmm_config_default(struct vmm_config *cfg);
int  vmm_config_parse_args(struct vmm_config *cfg, int argc, char **argv);
void vmm_usage(const char *prog);

#endif /* VMM_CONFIG_H */
