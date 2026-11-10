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
#include <pthread.h>
#include <linux/kvm.h>

#include "memory.h"
#include "devices/device.h"

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

    /* VM 停下后把 exit 统计写到这里（--exit-stats），"-" 表示 stderr */
    const char *exit_stats_path;

    /* --device 给出的 PCI 设备，按命令行顺序占 slot 1 起的位置。
     * 上限是总线上除 host bridge 之外的 slot 数。 */
    struct vmm_device devices[VMM_MAX_DEVICES];
    int         nr_devices;
};

void vmm_config_default(struct vmm_config *cfg);
int  vmm_config_parse_args(struct vmm_config *cfg, int argc, char **argv);
void vmm_usage(const char *prog);

/* ------------------------------------------------------------------ */
/* vCPU                                                                */
/* ------------------------------------------------------------------ */

struct vmm_vcpu {
    struct vmm_vm  *vm;
    int             id;         /* vCPU index，等于 APIC id */
    int             fd;         /* KVM vCPU fd */
    struct kvm_run *run;        /* mmap 出来的共享通信页 */
    size_t          run_size;

    pthread_t       thread;     /* 每个 vCPU 一个线程，见 smp.c */
    bool            thread_started;
    volatile bool   should_stop;
    int             result;     /* vcpu_run_loop 的返回值 */

    /* 回到用户态的 exit 按 kvm_run->exit_reason 计数，下标就是
     * KVM_EXIT_* 的值，最后一格收容超出范围的值。只有本 vCPU 线程写，
     * VM 停下、线程 join 之后才读（--exit-stats），所以不需要锁。
     * KVM_RUN 被信号打断（EINTR）不算：那时 exit_reason 无效，见
     * vcpu_run_loop()。 */
    uint64_t        exits[VMM_EXIT_REASON_SLOTS];
};

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

    bool            has_irqchip;       /* KVM_CREATE_IRQCHIP 已完成 */

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

    volatile bool   running;
    volatile bool   user_stop;         /* Ctrl-A x 触发的停机 */
    int             exit_code;
};

/* 生命周期 */
int  vm_create(struct vmm_vm *vm, const struct vmm_config *cfg);
int  vm_create_vcpu(struct vmm_vm *vm, int id);
void vm_destroy(struct vmm_vm *vm);

/* 单个 vCPU 的主循环，由 smp.c 的 vCPU 线程调用 */
int  vcpu_run_loop(struct vmm_vcpu *vcpu);

/* 单次 exit 的分发，独立出来便于事件循环复用 */
int  vcpu_handle_exit(struct vmm_vcpu *vcpu);

/* 调试辅助：把 regs/sregs 打出来，KVM_EXIT_SHUTDOWN / 内部错误时必用 */
void vcpu_dump_state(struct vmm_vcpu *vcpu);
const char *kvm_exit_reason_str(uint32_t reason);

#endif /* VMM_H */
