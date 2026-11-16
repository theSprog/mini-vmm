/* include/smp.h — 多 vCPU：线程生命周期、CPUID 拓扑
 *
 * AP 的唤醒（INIT-SIPI-SIPI）完全由内核态 LAPIC 完成，VMM 这边只需要：
 *   1. 每个 vCPU 一个线程，都进 KVM_RUN。AP 在收到 SIPI 之前阻塞在
 *      KVM 内部（mp_state = UNINITIALIZED），不占 CPU；
 *   2. 每个 vCPU 的 CPUID 自报家门：APIC ID 和拓扑必须和 MADT/MP table
 *      以及 KVM 给 LAPIC 设的 ID（= vcpu_id）一致；
 *   3. 任意一个 vCPU 要求退出时，把其余 vCPU 也可靠地停下来。
 */
#ifndef SMP_H
#define SMP_H

#include <linux/kvm.h>

struct vmm_vm;
struct vmm_vcpu;

/* 按 vcpu->id 和 vm->nr_vcpus 改写拓扑相关的 CPUID leaf。
 * 在 KVM_SET_CPUID2 之前调用；拓扑是“1 socket × N core × 1 thread”。 */
void smp_fixup_cpuid(const struct vmm_vcpu *vcpu, struct kvm_cpuid2 *cpuid);

/* 为每个 vCPU 启动一个线程并等全部结束。
 * 返回第一个出错 vCPU 的错误码；全部正常停机时返回 VMM_OK。 */
int  vm_run(struct vmm_vm *vm);

/* 让所有 vCPU 线程尽快离开 KVM_RUN。可以在任意线程里调用，可重入。 */
void vm_request_stop(struct vmm_vm *vm);

/*
 * 把一个函数交给指定 vCPU 的线程执行（QEMU 叫 run_on_cpu）。
 *
 * 读写寄存器这类 vCPU ioctl 要持 vcpu->mutex，vCPU 在 KVM_RUN 里（包括
 * guest 执行 HLT、在 KVM 内部睡眠时）一直占着它，别的线程调用会一直阻塞。
 * 所以由 vCPU 自己的线程来做：vcpu_queue_work 把工作挂进队列，然后像停机
 * 一样置 immediate_exit 并发 SIGUSR1，把目标 vCPU 踢出 KVM_RUN；vCPU 线程
 * 在 KVM_RUN 返回 EINTR 后清掉 immediate_exit，执行队列里的工作，再回到
 * guest。KVM 在返回 EINTR 之前完成了挂起的 PIO/MMIO，此时寄存器是一致的；
 * HLT 中的 vCPU 回到 KVM_RUN 后继续 HLT，guest 看不出被打断过。只有目标
 * vCPU 离开 guest，其他 vCPU 照常运行。
 *
 * 保证：被接受的工作恰好执行一次。vCPU 线程退出（停机、复位、出错）时先
 * 关闭队列，再把剩下的工作执行完，所以发起方总能等到 fn 被调用，不会有
 * 请求永远没有下文；vm_run 返回时所有被接受的工作都已执行完。
 * 被 gdbstub 暂停的 vCPU 不在 KVM_RUN 里，也不处理队列，工作等到 gdb 让
 * 它继续运行（或 VM 结束）时才执行。
 *
 * fn 在 vCPU 线程上运行，调用时不持有任何 VMM 的锁；fn 返回后 vCPU 线程
 * 不再访问 w，fn 可以把 w 交给别的线程或释放它。
 */
struct vcpu_work {
    void (*fn)(struct vmm_vcpu *v, struct vcpu_work *w);
    struct vcpu_work *next;
};

/* 任意线程调用（目标 vCPU 自己的线程除外）。vCPU 线程没有在运行时返回
 * VMM_ERR_EXIT，工作不会执行 */
int  vcpu_queue_work(struct vmm_vcpu *v, struct vcpu_work *w);
/* vCPU 线程调用：执行队列里当前所有的工作 */
void vcpu_run_work(struct vmm_vcpu *v);

#endif /* SMP_H */
