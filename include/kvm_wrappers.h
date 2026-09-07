/* include/kvm_wrappers.h — KVM ioctl 的薄封装
 *
 * 原则：这一层不做任何策略，只做三件事——
 *   1) 把 ioctl 的裸整数返回值统一成 VMM_ERR_* 语义
 *   2) 出错时打一条带 ioctl 名字的日志（否则一堆 -1 根本没法定位）
 *   3) 把“需要先 memset 结构体再填字段”这类样板代码收敛掉
 * 结构体定义一律直接用 <linux/kvm.h>，绝不自己抄一份，避免和内核版本漂移。
 */
#ifndef KVM_WRAPPERS_H
#define KVM_WRAPPERS_H

#include <stdint.h>
#include <stddef.h>
#include <linux/kvm.h>

/* ---- 系统级（/dev/kvm fd） ---- */
int  kvm_open(void);                              /* 返回 kvm_fd */
int  kvm_get_api_version(int kvm_fd);             /* 应为 12 */
int  kvm_check_extension(int kvm_fd, int cap);    /* KVM_CHECK_EXTENSION */
int  kvm_get_vcpu_mmap_size(int kvm_fd);          /* kvm_run 页大小 */
int  kvm_create_vm(int kvm_fd);                   /* 返回 vm_fd */

/* ---- VM 级（vm_fd） ---- */
int  kvm_create_vcpu(int vm_fd, int vcpu_id);     /* 返回 vcpu_fd */
int  kvm_set_user_memory_region(int vm_fd, uint32_t slot, uint32_t flags,
                                uint64_t gpa, uint64_t size, uint64_t hva);
/* Intel 专用，AMD/海光上不需要调；由 arch 层决定是否调用 */
int  kvm_set_tss_addr(int vm_fd, uint64_t addr);
int  kvm_set_identity_map_addr(int vm_fd, uint64_t addr);

/* ---- vCPU 级（vcpu_fd） ---- */
struct kvm_run *kvm_mmap_run(int vcpu_fd, size_t size);
int  kvm_get_regs(int vcpu_fd, struct kvm_regs *regs);
int  kvm_set_regs(int vcpu_fd, const struct kvm_regs *regs);
int  kvm_get_sregs(int vcpu_fd, struct kvm_sregs *sregs);
int  kvm_set_sregs(int vcpu_fd, const struct kvm_sregs *sregs);
int  kvm_run(int vcpu_fd);                        /* 处理 EINTR 重试 */

/* CPUID 透传：把 host 支持的 CPUID 叶子设给 vCPU。Step 1.2 之后需要，
 * 因为长模式要求 guest 能看到 CPUID.80000001h:EDX.LM。 */
int  kvm_get_supported_cpuid(int kvm_fd, struct kvm_cpuid2 **out);
int  kvm_set_cpuid2(int vcpu_fd, struct kvm_cpuid2 *cpuid);

/* MSR 读写（Step 1.2 可用 sregs.efer 代替；snapshot 阶段必需） */
int  kvm_get_msr(int vcpu_fd, uint32_t index, uint64_t *value);
int  kvm_set_msr(int vcpu_fd, uint32_t index, uint64_t value);

#endif /* KVM_WRAPPERS_H */
