/* include/io_bus.h — IO 总线（PIO / MMIO 统一抽象） */
#ifndef VMM_IO_BUS_H
#define VMM_IO_BUS_H

#include <stdint.h>
#include <stdbool.h>

struct vmm_vm;
struct vmm_vcpu;

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

/* --trace-pio / --trace-mmio 的文件。vm_create 打开，vm_destroy 关闭；
 * 打不开只打一条 warn，VM 照常运行 */
void io_trace_open(struct vmm_vm *vm);
void io_trace_close(void);

#endif /* VMM_IO_BUS_H */
