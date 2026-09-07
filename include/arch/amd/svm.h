/* include/arch/amd/svm.h — AMD SVM（含海光）相关定义与能力探测
 *
 * 参考：AMD64 Architecture Programmer's Manual Vol.2, Chapter 15 (SVM)
 * 注意：本文件只放“host 侧用户态需要感知的”SVM 信息。VMCB 的实际布局由
 * host 内核的 kvm_amd 模块维护，用户态 VMM 永远看不到 VMCB —— 这一点
 * 在博客原理篇里要专门澄清，很多人以为写 VMM 就是自己填 VMCB。
 */
#ifndef ARCH_AMD_SVM_H
#define ARCH_AMD_SVM_H

#include <stdint.h>
#include <stdbool.h>

/* CPUID vendor string（leaf 0，EBX:EDX:ECX 顺序） */
#define SVM_VENDOR_AMD   "AuthenticAMD"
#define SVM_VENDOR_HYGON "HygonGenuine"

/* CPUID Fn8000_0001_ECX */
#define CPUID_EXT_FEAT_SVM      (1u << 2)   /* SVM 可用 */

/* CPUID Fn8000_000A：SVM Revision and Feature Identification */
#define CPUID_SVM_LEAF          0x8000000Au
#define SVM_FEAT_NP             (1u << 0)   /* Nested Paging (NPT) */
#define SVM_FEAT_LBR_VIRT       (1u << 1)
#define SVM_FEAT_SVM_LOCK       (1u << 2)
#define SVM_FEAT_NRIP_SAVE      (1u << 3)   /* 下一条指令 RIP 保存 */
#define SVM_FEAT_TSC_RATE_MSR   (1u << 4)
#define SVM_FEAT_VMCB_CLEAN     (1u << 5)
#define SVM_FEAT_FLUSH_BY_ASID  (1u << 6)
#define SVM_FEAT_DECODE_ASSISTS (1u << 7)   /* 影响 MMIO exit 是否带解码信息 */
#define SVM_FEAT_PAUSE_FILTER   (1u << 10)
#define SVM_FEAT_AVIC           (1u << 13)  /* 硬件中断虚拟化，Step 6 相关 */
#define SVM_FEAT_V_VMSAVE_VMLOAD (1u << 15)
#define SVM_FEAT_VGIF           (1u << 16)

struct svm_caps {
    bool     svm_supported;
    uint32_t svm_rev;        /* Fn8000_000A EAX[7:0] */
    uint32_t nr_asids;       /* EBX */
    uint32_t features;       /* EDX，上面 SVM_FEAT_* 位 */
};

/* 直接跑 CPUID 填充 caps；不依赖 /dev/kvm */
int  svm_query_caps(struct svm_caps *caps);
void svm_dump_caps(const struct svm_caps *caps);

#endif /* ARCH_AMD_SVM_H */
