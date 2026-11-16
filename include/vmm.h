/* include/vmm.h — 全局 VM/vCPU 上下文、公共宏、错误码、IO 总线
 *
 * 本文件是整个 VMM 的“地基头文件”，后续所有模块（mem/boot/virtio/vhost/
 * vfio/smp/snapshot）都直接或间接依赖这里的 struct vmm_vm / struct vmm_vcpu。
 * 结构体里有一部分字段是为后续里程碑预留的占位，目的是避免后面反复重构
 * 核心结构体。
 */
#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <linux/kvm.h>

#include "memory.h"
#include "devices/device.h"
#include "reset.h"

/* ------------------------------------------------------------------ */
/* 常量与错误码                                                        */
/* ------------------------------------------------------------------ */

#define VMM_MAX_VCPUS        32
#define VMM_MAX_PIO_DEVS     16
#define VMM_MAX_MMIO_DEVS    32
/* --device 的上限：PCI 总线 32 个 slot 去掉 host bridge（pci/pci.h） */
#define VMM_MAX_DEVICES      31

/* 按 exit_reason 计数的格数。<linux/kvm.h> 里 KVM_EXIT_* 目前不到 50 个，
 * 留出余量；超出的值都记进最后一格，不会越界。 */
#define VMM_EXIT_REASON_SLOTS 64

/* 默认 guest RAM：128 MiB。
 * 注意：长模式 payload 要往 GPA 0x500000 (5 MiB) 写数据，2 MiB 内存是不够的，
 * 所以 --mem 是命令行可调的，只有实模式 payload 才用 2M。 */
#define VMM_DEFAULT_RAM_SIZE (128ULL << 20)

enum vmm_errno {
    VMM_OK              =  0,
    VMM_ERR_SYS         = -1,  /* errno 已设置，看 errno */
    VMM_ERR_INVAL       = -2,  /* 参数非法 */
    VMM_ERR_NOMEM       = -3,
    VMM_ERR_UNSUPPORTED = -4,  /* KVM capability 缺失 */
    VMM_ERR_GUEST       = -5,  /* guest 行为异常（越界访问等） */
    VMM_ERR_EXIT        = -6,  /* 正常退出信号（HLT / shutdown） */
    VMM_ERR_INTR        = -7,  /* KVM_RUN 被信号或 immediate_exit 打断 */
};

/* ------------------------------------------------------------------ */
/* 日志                                                                */
/* ------------------------------------------------------------------ */

extern int vmm_log_level;   /* 0=err 1=warn 2=info 3=debug */

void vmm_log(int level, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

#define vmm_err(...)   vmm_log(0, __FILE__, __LINE__, __VA_ARGS__)
#define vmm_warn(...)  vmm_log(1, __FILE__, __LINE__, __VA_ARGS__)
#define vmm_info(...)  vmm_log(2, __FILE__, __LINE__, __VA_ARGS__)
#define vmm_dbg(...)   vmm_log(3, __FILE__, __LINE__, __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* 前向声明                                                            */
/* ------------------------------------------------------------------ */

struct vmm_vm;
struct vmm_vcpu;
struct arch_cpu_ops;
struct gdbstub;
struct split_irqchip;

/* ------------------------------------------------------------------ */
/* IO 总线（PIO / MMIO 统一抽象）                                       */
/* ------------------------------------------------------------------ */
/*
 * 设计意图：KVM_EXIT_IO 和 KVM_EXIT_MMIO 的语义高度同构——
 * “某个地址区间上发生了一次 size 字节的读或写”。用同一个 vmm_io_dev
 * 描述二者，可以让 8250 串口（PIO 0x3f8）和后面的 PCI BAR、
 * virtio-mmio 等 MMIO 设备复用同一套注册/查找/分发路径。
 *
 * 回调里传的是 offset（相对 base 的偏移）而不是绝对地址，
 * 这样设备实现不需要知道自己被挂在哪里 —— virtio-mmio 有多个实例时
 * 这一点是必需的。
 */
struct vmm_io_dev {
    const char *name;
    uint64_t    base;
    uint64_t    len;
    void       *opaque;            /* 指向具体设备实例 */

    /* 返回 VMM_OK 表示已处理；返回负值表示错误。
     * data 由调用方提供，长度为 size（1/2/4/8）。 */
    int (*read)(void *opaque, uint64_t offset, uint32_t size, void *data);
    int (*write)(void *opaque, uint64_t offset, uint32_t size,
                 const void *data);
};

/* 一个设备（或一类未注册地址）引起的用户态 exit 次数，按方向分开。
 * 一次 KVM_EXIT_IO / KVM_EXIT_MMIO 记一次：ins/outs 一次 exit 里带的
 * 多次访问只算一次，因为这里要回答的是“exit 的开销花在了哪个设备上”，
 * 而开销按 exit 计，不按访问计。由 vm->io_lock 保护。 */
struct vmm_io_stats {
    uint64_t reads;
    uint64_t writes;
};

/* IO 总线分发。handle_io()/handle_mmio() 只是它们在 KVM_EXIT 路径上的
 * 薄封装——从 kvm_run 里取出字段，然后调这里。
 *
 * 单独暴露出来有两个理由：一是这条路径是所有设备（含后续每一个 VirtIO
 * transport）唯一的入口，值得能在不启动 VM 的情况下直接驱动它做单元测试
 * （见 tests/unit/io_bus_test.c）；二是 event-loop 里程碑的事件循环也要复用它。
 *
 * data 的长度：PIO 是 size * count（串操作逐次前进 size 字节），
 * MMIO 是 len。返回值语义同设备回调：VMM_OK 表示已处理，
 * VMM_ERR_EXIT 是 guest 请求停机的约定信号，其余负值是错误。 */
int  io_bus_dispatch_pio(struct vmm_vcpu *vcpu, uint16_t port, bool is_write,
                         uint32_t size, uint32_t count, uint8_t *data);
int  io_bus_dispatch_mmio(struct vmm_vcpu *vcpu, uint64_t addr, bool is_write,
                          uint32_t len, uint8_t *data);

int  vmm_register_pio(struct vmm_vm *vm, const struct vmm_io_dev *dev);
int  vmm_register_mmio(struct vmm_vm *vm, const struct vmm_io_dev *dev);

/* 地址由 guest 编程的区域（PCI BAR）。
 *
 * 和上面的静态注册不同，这类区域的位置在 guest 运行中会变：guest 写 BAR
 * 或开关 command 寄存器的 decode 位，区域就搬家或消失。所以分两步：
 * vmm_io_reserve 先占一个表项，dev->base/len 被忽略，表项以 len = 0 登记
 * （查不到、也不参与冲突检查），*slot 返回表项下标；之后 vmm_io_move
 * 改它的基址和长度，len = 0 表示停止响应。
 *
 * 表项占位而不是增删，是因为 exit 统计按表项下标记账（pio_stats 与
 * pio_devs 同下标），删掉再插入会让计数挪到别的设备名下。
 *
 * vmm_io_move 不做冲突检查：guest 可以把 BAR 编到任何位置，真实硬件上
 * 地址重叠的结果是未定义的，VMM 能做的只是给出确定的行为——查找按注册
 * 顺序取第一个命中的表项，先注册的静态设备优先。重叠时打一条 warn。
 *
 * 调用时机：guest 启动前（单线程），或者在持有 vm->io_lock 的分发路径里
 * （配置空间写由 exit 分发进来，本来就持锁）。查找也只在持锁时发生，
 * 所以表项改动对 vCPU 线程是原子可见的。 */
enum vmm_io_space { VMM_IO_PIO, VMM_IO_MMIO };

int  vmm_io_reserve(struct vmm_vm *vm, enum vmm_io_space space,
                    const struct vmm_io_dev *dev, int *slot);
void vmm_io_move(struct vmm_vm *vm, enum vmm_io_space space, int slot,
                 uint64_t base, uint64_t len);
const struct vmm_io_dev *vmm_lookup_pio(struct vmm_vm *vm, uint64_t port);
const struct vmm_io_dev *vmm_lookup_mmio(struct vmm_vm *vm, uint64_t addr);

/* ------------------------------------------------------------------ */
/* 运行模式与配置                                                      */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* vCPU                                                                */
/* ------------------------------------------------------------------ */

/* vCPU 线程此刻在做什么，monitor 的 info cpus 显示。只有本 vCPU 线程写 */
enum vcpu_state {
    VCPU_STOPPED = 0,   /* 线程没有运行（尚未启动、已退出、复位期间） */
    VCPU_GUEST,         /* 在 KVM_RUN 里：执行 guest 代码，或在 KVM 内部处理 HLT 等 */
    VCPU_VMM,           /* 在用户态处理 exit 或执行 vcpu_work */
    VCPU_PAUSED,        /* 被 gdbstub 暂停 */
};

struct vcpu_work;

struct vmm_vcpu {
    struct vmm_vm  *vm;
    int             id;         /* vCPU index，等于 APIC id */
    int             fd;         /* KVM vCPU fd */
    struct kvm_run *run;        /* mmap 出来的共享通信页 */
    size_t          run_size;

    pthread_t       thread;     /* 每个 vCPU 一个线程，见 smp.c */
    bool            thread_started;
    /* 用 __atomic 读写：请求停机的线程和 vCPU 线程之间的顺序要求见
     * smp.c 的 vm_request_stop() */
    bool            should_stop;
    int             result;     /* vcpu_run_loop 的返回值 */

    /* 回到用户态的 exit 按 kvm_run->exit_reason 计数，下标就是
     * KVM_EXIT_* 的值，最后一格收容超出范围的值。KVM_RUN 被信号打断
     * （EINTR）不算：那时 exit_reason 无效，见 vcpu_run_loop()。
     * 只有本 vCPU 线程写，monitor 在 I/O 线程上随时读，所以读写都用
     * relaxed 原子操作（vcpu_count_exit / vcpu_exit_total）。只有一个写者，
     * 写的一侧是“原子读、加一、原子写”，x86 上就是普通的 load 和 store，
     * 不需要带 lock 前缀的读改写，exit 路径不会因此变慢。各格之间不保证
     * 是同一时刻的快照。 */
    uint64_t        exits[VMM_EXIT_REASON_SLOTS];
    uint32_t        last_exit;  /* 最近一次 exit_reason，同样 relaxed 原子 */
    int             state;      /* enum vcpu_state，relaxed 原子 */
    int             tid;        /* vCPU 线程的 host 线程号，线程启动时写 */

    /* KVM_GET_STATS_FD 的 fd，创建 vCPU 时打开、vm_destroy 时关闭，
     * 打不开（host 内核早于 5.14）时为 -1。拿 fd 的 ioctl 要持 vcpu->mutex，
     * vCPU 在 KVM_RUN 里（包括 guest HLT 时）一直占着这把锁，运行中从别的
     * 线程调会一直阻塞；读这个 fd 不拿锁（host 内核 virt/kvm/kvm_main.c 的
     * kvm_vcpu_stats_read），所以在 vCPU 线程启动前打开好，之后随时可读 */
    int             stats_fd;

    /* 交给本 vCPU 线程执行的工作，见 smp.h 的 vcpu_queue_work() */
    pthread_mutex_t   work_lock;
    struct vcpu_work *work_head, **work_tail;
    bool              work_open;    /* 线程在运行、接受新工作 */
};

/* exit 计数的读写，理由见 struct vmm_vcpu.exits */
void     vcpu_count_exit(struct vmm_vcpu *vcpu, uint32_t reason);
uint64_t vcpu_exit_count(const struct vmm_vcpu *vcpu, int slot);

uint64_t vcpu_exit_total(const struct vmm_vcpu *vcpu);

/* ------------------------------------------------------------------ */
/* VM                                                                  */
/* ------------------------------------------------------------------ */

struct vmm_vm {
    int             kvm_fd;
    int             vm_fd;
    int             api_version;
    size_t          vcpu_mmap_size;

    struct vmm_mem  mem;

    struct vmm_vcpu vcpus[VMM_MAX_VCPUS];
    int             nr_vcpus;

    const struct arch_cpu_ops *arch;   /* 运行时探测出的 CPU 厂商后端 */

    /* KVM 里有 LAPIC、能投递 MSI：KVM_CREATE_IRQCHIP 或 KVM_CAP_SPLIT_IRQCHIP
     * 已完成。split 时 split 非 NULL，PIC/IOAPIC/PIT 在用户态，
     * 见 devices/split_irqchip.h */
    bool            has_irqchip;
    struct split_irqchip *split;

    struct vmm_io_dev pio_devs[VMM_MAX_PIO_DEVS];
    int               nr_pio_devs;
    struct vmm_io_dev mmio_devs[VMM_MAX_MMIO_DEVS];
    int               nr_mmio_devs;

    /* 与 pio_devs/mmio_devs 同下标的 exit 计数，以及落在未注册地址上的
     * exit。计数在分发路径上持 io_lock 更新。 */
    struct vmm_io_stats pio_stats[VMM_MAX_PIO_DEVS];
    struct vmm_io_stats mmio_stats[VMM_MAX_MMIO_DEVS];
    struct vmm_io_stats pio_unhandled;
    struct vmm_io_stats mmio_unhandled;

    /* 多个 vCPU 线程会同时陷出到用户态访问设备。设备模型本身不做
     * 并发保护，统一由 exit 分发路径持有这把大锁串行化（和 QEMU 的
     * BQL 同一个思路）。设备表在 guest 启动前注册完毕，之后表项个数
     * 不再变；PCI BAR 表项的基址和长度会随 guest 编程改变，改动和查找
     * 都在这把锁下进行（见 vmm_io_move）。
     * I/O 线程的回调也遵守“先 io_lock、后设备锁”，见 io/event_loop.h。 */
    pthread_mutex_t   io_lock;

    struct vmm_config cfg;

    /* --gdb 时非 NULL，见 gdb/gdbstub.h。vCPU 运行循环和 vm_request_stop()
     * 据此进入 gdbstub 的暂停逻辑 */
    struct gdbstub *gdb;

    volatile bool   running;
    volatile bool   user_stop;         /* Ctrl-A x 触发的停机 */
    /* monitor quit 置位，之后不再清零。vm_reset 会清掉每个 vCPU 的
     * should_stop，quit 恰好落在复位过程中时，停机请求会被这次清零抹掉，
     * main 的复位循环在复位前后都检查这个标志，见 main.c */
    bool            quit_requested;
    int             exit_code;
    struct timespec started;           /* 第一次进入 guest 的时刻（CLOCK_MONOTONIC） */
    int             stats_fd;          /* VM 级 KVM_GET_STATS_FD，打不开时为 -1 */

    /* 整机复位，见 reset.h。reset_requested 由请求复位的 vCPU 置位，
     * vm_reset 完成时清零；gdbstub 据此区分"VM 在复位"和"VM 结束了" */
    struct vmm_reset_handler reset_handlers[VMM_MAX_RESET_HANDLERS];
    int             nr_reset_handlers;
    struct vm_power_on *power_on;      /* vm_power_on_save 存下的 KVM 状态 */
    volatile bool   reset_requested;
    unsigned int    reset_count;       /* relaxed 原子：monitor 在别的线程读 */
};

/* 生命周期 */
int  vm_create(struct vmm_vm *vm, const struct vmm_config *cfg);
int  vm_create_vcpu(struct vmm_vm *vm, int id);
void vm_destroy(struct vmm_vm *vm);

/* 单个 vCPU 的主循环，由 smp.c 的 vCPU 线程调用 */
int  vcpu_run_loop(struct vmm_vcpu *vcpu);

/* 单次 exit 的分发，独立出来便于事件循环复用 */
int  vcpu_handle_exit(struct vmm_vcpu *vcpu);

/* 调试辅助：把 regs/sregs 打出来，KVM_EXIT_SHUTDOWN / 内部错误时必用。
 * 读寄存器的 ioctl 要持 vcpu->mutex，所以只能在 vCPU 自己的线程上调用，
 * 或者在 vCPU 线程停下之后调用；monitor 经 vcpu_queue_work() 满足这一点。
 * vcpu_dump_state 写到 stderr。 */
void vcpu_print_state(FILE *out, struct vmm_vcpu *vcpu);
void vcpu_dump_state(struct vmm_vcpu *vcpu);
const char *kvm_exit_reason_str(uint32_t reason);

#endif /* VMM_H */
