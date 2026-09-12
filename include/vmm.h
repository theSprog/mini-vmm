/* include/vmm.h — 全局 VM/vCPU 上下文、公共宏、错误码、IO 总线
 *
 * 本文件是整个 VMM 的“地基头文件”，后续所有模块（mem/boot/virtio/vhost/
 * vfio/smp/snapshot）都直接或间接依赖这里的 struct vmm_vm / struct vmm_vcpu。
 * Step 1 只会用到其中一小部分字段，其余字段是为后续 Step 预留的占位，
 * 目的是避免后面反复重构核心结构体。
 */
#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>
#include <linux/kvm.h>

#include "memory.h"

/* ------------------------------------------------------------------ */
/* 常量与错误码                                                        */
/* ------------------------------------------------------------------ */

#define VMM_MAX_VCPUS        32
#define VMM_MAX_PIO_DEVS     16
#define VMM_MAX_MMIO_DEVS    32

/* 默认 guest RAM：128 MiB。
 * 注意：Step 1.2 要往 GPA 0x500000 (5 MiB) 写数据，2 MiB 内存是不够的，
 * 所以 --mem 是命令行可调的，Step 1.1 才用 2M。 */
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
 * 描述二者，可以让 Step 2 的 8250 串口（PIO 0x3f8）和 Step 3 的
 * virtio-mmio（MMIO 0xd0000000）复用同一套注册/查找/分发路径。
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

int  vmm_register_pio(struct vmm_vm *vm, const struct vmm_io_dev *dev);
int  vmm_register_mmio(struct vmm_vm *vm, const struct vmm_io_dev *dev);
const struct vmm_io_dev *vmm_lookup_pio(struct vmm_vm *vm, uint64_t port);
const struct vmm_io_dev *vmm_lookup_mmio(struct vmm_vm *vm, uint64_t addr);

/* ------------------------------------------------------------------ */
/* 运行模式与配置                                                      */
/* ------------------------------------------------------------------ */

enum vmm_boot_mode {
    VMM_BOOT_REAL16 = 0,  /* Step 1.1：16 位实模式，CS:IP = 0000:entry */
    VMM_BOOT_LONG64,      /* Step 1.2：直接进 64 位长模式 */
    VMM_BOOT_LINUX,       /* Step 2+：vmlinux + zeropage */
};

struct vmm_config {
    enum vmm_boot_mode mode;
    uint64_t    ram_size;      /* 字节 */
    int         nr_vcpus;
    const char *payload_path;  /* 裸机器码 .bin（Step 1）或 vmlinux（Step 2） */
    uint64_t    payload_gpa;   /* 载入地址；Step 1.1 = 0x0 */
    uint64_t    entry_gpa;     /* 首条指令的 GPA，默认 = payload_gpa */
    int         log_level;
    bool        dump_on_exit;  /* 退出时 dump 一段 guest 内存（Step 1.2 验证用） */
    uint64_t    dump_gpa;
    size_t      dump_len;

    /* Step 2：Linux 直启 */
    const char *initrd_path;
    const char *cmdline;

    /* 把每一次陷出到用户态的 PIO 访问写进这个文件（--trace-pio）。
     * 用来回答“驱动到底按什么顺序读写了哪些寄存器”，见 learning-note 卷七。 */
    const char *trace_pio_path;
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

    /* 统计，方便排错和后面写博客画图 */
    uint64_t        n_exits;
    uint64_t        n_exit_io;
    uint64_t        n_exit_mmio;
    uint64_t        n_exit_hlt;
};

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

    /* 多个 vCPU 线程会同时陷出到用户态访问设备。设备模型本身不做
     * 并发保护，统一由 exit 分发路径持有这把大锁串行化（和 QEMU 的
     * BQL 同一个思路）。设备表在 guest 启动前注册完毕，之后只读。 */
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

/* 单次 exit 的分发，独立出来便于 Step 4 事件循环复用 */
int  vcpu_handle_exit(struct vmm_vcpu *vcpu);

/* 调试辅助：把 regs/sregs 打出来，KVM_EXIT_SHUTDOWN / 内部错误时必用 */
void vcpu_dump_state(struct vmm_vcpu *vcpu);
const char *kvm_exit_reason_str(uint32_t reason);

#endif /* VMM_H */
