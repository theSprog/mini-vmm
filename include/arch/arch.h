/* include/arch/arch.h — CPU 厂商抽象层
 *
 * 为什么 Step 1 就要有这一层？
 * 因为从创建 VM 的第一步开始，Intel 和 AMD 的准备工作就不同：
 *   - Intel (VMX)：老 CPU 无 unrestricted guest 时跑实模式需要 vm86 兜底，
 *     且 KVM 要求先用 KVM_SET_TSS_ADDR 划一块 3 页的 TSS 区域，
 *     用 EPT 时还要 KVM_SET_IDENTITY_MAP_ADDR。
 *   - AMD (SVM)：SVM 天生能直接跑实模式，上面两个 ioctl 都不需要。
 * 如果不抽象，这些差异会以 #ifdef 的形式散落在 vm.c 里。
 *
 * 关键决策：运行时 CPUID 探测，不用编译期 #ifdef。
 * 原因是同一份二进制要能在不同机器上跑，而且海光 CPU 的 vendor string
 * 是 "HygonGenuine" 而非 "AuthenticAMD"，编译期宏根本区分不了。
 */
#ifndef ARCH_ARCH_H
#define ARCH_ARCH_H

#include <stdint.h>
#include <stdbool.h>

struct vmm_vm;
struct vmm_vcpu;

enum arch_vendor {
    ARCH_VENDOR_UNKNOWN = 0,
    ARCH_VENDOR_AMD,      /* AuthenticAMD */
    ARCH_VENDOR_HYGON,    /* HygonGenuine —— 走 AMD/SVM 后端 */
    ARCH_VENDOR_INTEL,    /* GenuineIntel */
};

struct arch_cpu_ops {
    const char      *name;
    enum arch_vendor vendor;

    /* 探测：本后端是否匹配当前 host CPU */
    bool (*match)(enum arch_vendor v);

    /* 检查虚拟化相关能力是否齐备（SVM/NPT 或 VMX/EPT），
     * 不满足时给出人话级别的错误提示 */
    int  (*check_features)(struct vmm_vm *vm);

    /* 在 KVM_CREATE_VM 之后、创建 vCPU 之前做厂商相关准备。
     * AMD/Hygon 上是 no-op；Intel 上做 TSS_ADDR / IDENTITY_MAP_ADDR。 */
    int  (*prepare_vm)(struct vmm_vm *vm);

    /* 每个 vCPU 创建后的厂商相关初始化（目前主要是 CPUID 整形） */
    int  (*init_vcpu)(struct vmm_vcpu *vcpu);
};

/* 裸 CPUID 指令封装 */
void arch_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4]);

/* 读 CPUID leaf 0 的 vendor string，返回 12 字节 + '\0' 到 buf[13] */
enum arch_vendor arch_detect_vendor(char buf[13]);

/* 选出匹配的后端；失败返回 NULL */
const struct arch_cpu_ops *arch_probe(void);

const char *arch_vendor_str(enum arch_vendor v);

/* 各后端导出符号 */
extern const struct arch_cpu_ops arch_ops_amd;
/* extern const struct arch_cpu_ops arch_ops_intel;  // 暂未实现 */

#endif /* ARCH_ARCH_H */
