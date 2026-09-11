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

#endif /* SMP_H */
