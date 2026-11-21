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
#include "config.h"
#include "io_bus.h"
#include "devices/device.h"
#include "reset.h"

/* ------------------------------------------------------------------ */
/* 常量与错误码                                                        */
/* ------------------------------------------------------------------ */

#define VMM_MAX_PIO_DEVS     16
#define VMM_MAX_MMIO_DEVS    32

/* 按 exit_reason 计数的格数。<linux/kvm.h> 里 KVM_EXIT_* 目前不到 50 个，
 * 留出余量；超出的值都记进最后一格，不会越界。 */
#define VMM_EXIT_REASON_SLOTS 64

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

/* 进程退出码（vm->exit_code）里有约定含义的值，完整列表见 --help 的
 * "Exit status"。0 是正常结束，1 是 VMM 出错（main 的错误返回）。
 * 裸 payload 经调试端口 0xF4 可以写任意值，Linux guest 不碰这个端口。 */
#define VMM_EXIT_GUEST_PANIC 3     /* guest 经 pvpanic 报告 PANICKED */

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
struct irqchip_ops;

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

    /* 中断控制器的放置方式，创建成功后才非 NULL；裸 payload 模式为 NULL。
     * 非 NULL 就说明 KVM 里有 LAPIC、能投递 MSI，见 devices/irqchip.h */
    const struct irqchip_ops *irqchip;
    /* irqchip_split_ops 的私有状态，只有 split_irqchip.c 使用 */
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
     * main.c 的 run_guest 在复位前后都检查这个标志 */
    bool            quit_requested;
    int             exit_code;         /* 取值约定见 VMM_EXIT_* */
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
