/* src/arch/amd/svm.c — AMD/海光 SVM 后端
 *
 * 参考 AMD64 APM Vol.2 Ch.15。这里做的事情非常少，因为 VMCB 的构造、
 * VMRUN 的执行、#VMEXIT 的解码全部在 host 内核的 kvm_amd 模块里完成。
 * 用户态 VMM 能做的只有：确认硬件能力、告诉 KVM 我们要什么。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vmm.h"
#include "kvm_wrappers.h"
#include "arch/arch.h"
#include "arch/amd/svm.h"
#include "smp.h"

int svm_query_caps(struct svm_caps *caps)
{
    uint32_t r[4], max_ext;

    memset(caps, 0, sizeof(*caps));

    arch_cpuid(0x80000000, 0, r);
    max_ext = r[0];
    if (max_ext < CPUID_SVM_LEAF) {
        vmm_err("CPU lacks extended CPUID leaf 0x%x (max=0x%x)",
                CPUID_SVM_LEAF, max_ext);
        return VMM_ERR_UNSUPPORTED;
    }

    arch_cpuid(0x80000001, 0, r);
    if (!(r[2] & CPUID_EXT_FEAT_SVM)) {
        vmm_err("CPUID Fn8000_0001_ECX.SVM is clear: virtualization disabled in BIOS?");
        return VMM_ERR_UNSUPPORTED;
    }
    caps->svm_supported = true;

    arch_cpuid(CPUID_SVM_LEAF, 0, r);
    caps->svm_rev   = r[0] & 0xff;
    caps->nr_asids  = r[1];
    caps->features  = r[3];
    return VMM_OK;
}

/*
 * 这里输出的特性名严格照抄 Linux 在 /proc/cpuinfo 里用的拼写，
 * 见 arch/x86/include/asm/cpufeatures.h 的 word 15：
 *
 *     X86_FEATURE_NPT           (15*32+0)  "npt"
 *     X86_FEATURE_LBRV          (15*32+1)  "lbrv"
 *     X86_FEATURE_SVML          (15*32+2)  "svm_lock"
 *     X86_FEATURE_NRIPS         (15*32+3)  "nrip_save"
 *     X86_FEATURE_TSCRATEMSR    (15*32+4)  "tsc_scale"
 *     X86_FEATURE_VMCBCLEAN     (15*32+5)  "vmcb_clean"   <- 带下划线
 *     X86_FEATURE_FLUSHBYASID   (15*32+6)  "flushbyasid"
 *     X86_FEATURE_DECODEASSISTS (15*32+7)  "decodeassists"
 *     X86_FEATURE_AVIC          (15*32+13) "avic"
 *
 * 为什么要跟内核对齐：我们是自己跑 CPUID Fn8000_000A 读的位，内核是
 * 另一条独立路径读的同一份硬件寄存器。名字一致，两边输出才能直接
 * 做集合比对，位定义抄错时能当场发现（见 tests/step1.sh 的人工验收）。
 * 名字不一致的话，这个交叉验证就退化成人肉对照两份命名表，
 * 迟早对错。
 */
void svm_dump_caps(const struct svm_caps *c)
{
    /* 位 -> 名字的映射表。分散在一串三元表达式里的话，加一个特性
     * 就要同时改格式串的 %s 个数和参数列表，非常容易漏。 */
    static const struct {
        uint32_t    bit;
        const char *name;
    } tbl[] = {
        { SVM_FEAT_NP,              "npt"             },
        { SVM_FEAT_LBR_VIRT,        "lbrv"            },
        { SVM_FEAT_SVM_LOCK,        "svm_lock"        },
        { SVM_FEAT_NRIP_SAVE,       "nrip_save"       },
        { SVM_FEAT_TSC_RATE_MSR,    "tsc_scale"       },
        { SVM_FEAT_VMCB_CLEAN,      "vmcb_clean"      },
        { SVM_FEAT_FLUSH_BY_ASID,   "flushbyasid"     },
        { SVM_FEAT_DECODE_ASSISTS,  "decodeassists"   },
        { SVM_FEAT_PAUSE_FILTER,    "pausefilter"     },
        { SVM_FEAT_AVIC,            "avic"            },
        { SVM_FEAT_V_VMSAVE_VMLOAD, "v_vmsave_vmload" },
        { SVM_FEAT_VGIF,            "vgif"            },
    };
    char buf[256];
    size_t off = 0, i;

    vmm_info("SVM rev=%u, ASIDs=%u", c->svm_rev, c->nr_asids);

    for (i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        int n;

        if (!(c->features & tbl[i].bit))
            continue;
        n = snprintf(buf + off, sizeof(buf) - off, " %s", tbl[i].name);
        if (n < 0 || (size_t)n >= sizeof(buf) - off)
            break;
        off += (size_t)n;
    }
    buf[off] = '\0';

    /* 必须是单独一行、单一前缀，tests/step1.sh 的人工验收会用
     * sed 把这行剪下来跟 /proc/cpuinfo 做集合比对。 */
    vmm_info("SVM features:%s", off ? buf : " (none)");
}

static bool amd_match(enum arch_vendor v)
{
    /* 海光 CPU 的 vendor string 是 HygonGenuine，但微架构派生自
     * Zen1，SVM 行为与 AMD 一致，host 侧也是同一个 kvm_amd 模块
     * （驱动里 hygon 走的就是 svm.c）。所以共用 AMD 后端。 */
    return v == ARCH_VENDOR_AMD || v == ARCH_VENDOR_HYGON;
}

static int amd_check_features(struct vmm_vm *vm)
{
    struct svm_caps caps;
    int r;

    (void)vm;

    r = svm_query_caps(&caps);
    if (r != VMM_OK)
        return r;

    svm_dump_caps(&caps);

    if (!(caps.features & SVM_FEAT_NP)) {
        /* 没有 NPT 的话 KVM 会退回影子页表（shadow paging）。功能上
         * 仍然能跑，但 Step 1.2 的恒等映射页表会被 KVM 接管重写，
         * 调试时看到的现象会非常反直觉，这里显式警告。 */
        vmm_warn("no NPT support: KVM falls back to shadow paging, "
                 "slower and much harder to debug");
    }
    if (!(caps.features & SVM_FEAT_NRIP_SAVE))
        vmm_warn("no nrip_save: KVM must decode instruction length in "
                 "software, the MMIO path will be slower");

    return VMM_OK;
}

static int amd_prepare_vm(struct vmm_vm *vm)
{
    (void)vm;
    /* SVM 不需要 KVM_SET_TSS_ADDR / KVM_SET_IDENTITY_MAP_ADDR。
     *
     * 这两个 ioctl 是 Intel VMX 的历史包袱：早期 VT-x 不支持
     * "unrestricted guest"，CPU 无法在 CR0.PE=0（实模式）下直接进
     * VMX non-root，KVM 只能用 vm86 模式模拟实模式，而 vm86 需要一个
     * 真实的 TSS；EPT 开启后还需要一块恒等映射页给实模式过渡用。
     * SVM 从第一代起就能直接跑实模式，所以这里什么都不用做。 */
    vmm_dbg("AMD/SVM: TSS_ADDR / IDENTITY_MAP_ADDR not needed");
    return VMM_OK;
}

/*
 * CPUID.1:ECX[31]，Linux 里叫 X86_FEATURE_HYPERVISOR（/proc/cpuinfo 的
 * "hypervisor" 标志）。物理 CPU 上这一位恒为 0（Intel SDM 标为未使用，
 * AMD APM 写明保留给 hypervisor 表示 guest 状态），虚拟化软件用它告诉
 * guest“你在虚拟机里”。KVM_GET_SUPPORTED_CPUID 不报告这一位，要 VMM
 * 自己置上（QEMU 默认也会置上）。
 *
 * 写 0（不置位）：guest 认为自己跑在物理机上。以 guest 的 v6.6 为例：
 *   - 早期微码加载器启用，会去 initrd 里找微码并尝试更新（虚拟机里
 *     改不了物理 CPU 的微码，只是白做）；
 *   - 按物理机处理勘误：AMD 上会直接写 host 才有意义的 MSR（如
 *     Zenbleed 的 chicken bit、erratum 1485 的 BP_CFG），海光
 *     model <= 3 会按 APIC ID 第 6 位推算 socket 编号；
 *   - ACPI 进入 S1-S3 前执行 WBINVD（ACPI_FLUSH_CPU_CACHE）；
 *   - 漏洞与缓解按“裸机、host 状态已知”报告；
 *   - 用户态的 systemd-detect-virt 之类工具判定为 "none"。
 *
 * 写 1（置位）：上面几项都按虚拟机处理：跳过微码加载和 host 专属的
 * 勘误 MSR，不在睡眠前 WBINVD，漏洞按“可能迁移到有漏洞的主机”保守
 * 报告（Intel 上的 ITS、AMD/海光上的 NULL_SEG），MDS 等条目注明
 * "SMT Host state unknown"。
 * 另外，开了 CONFIG_KVM_GUEST 的内核（发行版内核都开）只有看到这一位
 * 才会继续查 0x40000000 叶的 "KVMKVMKVM" 签名，进而启用 kvmclock、
 * PV EOI、steal time 等半虚拟化特性；这两片叶子 KVM_GET_SUPPORTED_CPUID
 * 已经给出，功能本身也都由 KVM 内核实现，VMM 不需要额外做什么。
 * mini-vmm 目前的 guest 没开 CONFIG_HYPERVISOR_GUEST，不受这一条影响。
 *
 * 顺带说明为什么不去改 HWCR（MSR 0xC0010015）的 bit 24 TscFreqSel：
 * guest 内核在 constant_tsc 时检查它，为 0 就打印
 * "[Firmware Bug]: TSC doesn't count with P0 frequency!"（写 1 表示
 * TSC 按 P0 频率计数，这条日志就会消失；写 0 只是多一行日志，行为不变，
 * TSC 在 KVM 下本来就恒定）。但 KVM 的 MSR_K7_HWCR 写处理（host v5.15
 * 的 arch/x86/kvm/x86.c，到 v6.6 仍然一样）只接受 bit 18 McStatusWrEn，
 * 其余非零位一律拒绝，KVM_SET_MSRS 会返回 0 表示一个都没写成功。
 * 所以这一位用户态改不了，只能保持 KVM 的初值 0。
 */
#define CPUID_1_ECX_HYPERVISOR (1u << 31)

static void cpuid_mark_hypervisor(struct kvm_cpuid2 *cpuid)
{
    uint32_t i;

    for (i = 0; i < cpuid->nent; i++) {
        if (cpuid->entries[i].function == 0x1)
            cpuid->entries[i].ecx |= CPUID_1_ECX_HYPERVISOR;
    }
}

static int amd_init_vcpu(struct vmm_vcpu *vcpu)
{
    struct kvm_cpuid2 *cpuid = NULL;
    int r;

    /* 把 host 支持的 CPUID 透传给 guest，只改写拓扑相关的 leaf，
     * 再置上 hypervisor 位（见 cpuid_mark_hypervisor()）。
     *
     * Step 1.2 严格说不依赖它：KVM_SET_SREGS 这条路径不会去校验
     * guest CPUID 里有没有 LM 位。但 guest 里一旦执行 CPUID 指令
     * （Linux 内核启动第一件事就是），不设的话拿到的全是 0，
     * 内核会直接判定 CPU 不支持长模式然后停住。所以现在就设上。
     *
     * 拓扑字段（initial APIC ID、核数等）是 host 的原始值，必须按
     * vcpu_id 改写，否则多个 vCPU 会自称同一个 APIC ID，见 smp.c。 */
    r = kvm_get_supported_cpuid(vcpu->vm->kvm_fd, &cpuid);
    if (r != VMM_OK) {
        vmm_warn("KVM_GET_SUPPORTED_CPUID failed, skipping SET_CPUID2");
        return VMM_OK;
    }

    smp_fixup_cpuid(vcpu, cpuid);
    cpuid_mark_hypervisor(cpuid);
    r = kvm_set_cpuid2(vcpu->fd, cpuid);
    free(cpuid);
    if (r != VMM_OK) {
        vmm_warn("KVM_SET_CPUID2 failed (harmless for Step 1)");
        return VMM_OK;
    }

    vmm_dbg("vCPU %d: host CPUID passed through, topology rewritten",
            vcpu->id);
    return VMM_OK;
}

const struct arch_cpu_ops arch_ops_amd = {
    .name           = "amd-svm",
    .vendor         = ARCH_VENDOR_AMD,
    .match          = amd_match,
    .check_features = amd_check_features,
    .prepare_vm     = amd_prepare_vm,
    .init_vcpu      = amd_init_vcpu,
};