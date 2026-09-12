/* src/smp.c — 多 vCPU 线程、CPUID 拓扑、停机协调
 *
 * AP 启动流程里 VMM 不需要做任何事情：
 *   - 内核态 irqchip 存在时，KVM 在 KVM_CREATE_VCPU 里把 vcpu 0 设为
 *     RUNNABLE（BSP），其余 vCPU 设为 KVM_MP_STATE_UNINITIALIZED；
 *   - AP 线程调用 KVM_RUN 后阻塞在 KVM 内部，直到 BSP 通过 LAPIC ICR
 *     发出 INIT + SIPI。KVM 收到 SIPI 后把 AP 置成实模式、
 *     CS:IP = (vector << 8):0，AP 从 Linux 放在低 1MiB 的 trampoline 起跑。
 * 所以 VMM 的职责只有线程、CPUID 和固件表（MADT/MP table）三件事。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>

#include "vmm.h"
#include "smp.h"
#include "kvm_wrappers.h"
#include "arch/arch.h"

/* ------------------------------------------------------------------ */
/* CPUID 拓扑                                                          */
/* ------------------------------------------------------------------ */

#define CPUID_1_EDX_HTT        (1u << 28)

/* CPUID 叶 0xB（Intel 定义的扩展拓扑枚举）的子叶编码 */
#define CPUID_TOPO_LEAF        0xb
#define CPUID_TOPO_TYPE_INVAL  0       /* 类型为 0 表示子叶列表结束 */
#define CPUID_TOPO_TYPE_SMT    1
#define CPUID_TOPO_TYPE_CORE   2

static uint32_t order_base2(uint32_t n)
{
    uint32_t b = 0;

    while ((1u << b) < n)
        b++;
    return b;
}

/*
 * 填一条 0xB 的子叶。字段含义见 Intel SDM Vol.2 CPUID 指令的
 * "Extended Topology Enumeration Leaf"，guest 侧的解析在
 * linux-6.6 arch/x86/kernel/cpu/topology.c detect_extended_topology()：
 *   EAX[4:0]    从本层算到下一层要把 APIC ID 右移几位
 *   EBX[15:0]   本层的逻辑处理器数；为 0 表示这一层无效
 *   ECX[7:0]    子叶号（原样回显），ECX[15:8] 层类型
 *   EDX         完整的 32 位 x2APIC ID（xAPIC 下就是 8 位 APIC ID）
 */
static void topo_leaf_set(struct kvm_cpuid_entry2 *e, uint32_t index,
                          uint32_t shift, uint32_t nr_lp, uint32_t type,
                          uint32_t apic_id)
{
    memset(e, 0, sizeof(*e));
    e->function = CPUID_TOPO_LEAF;
    e->index    = index;
    e->flags    = KVM_CPUID_FLAG_SIGNIFCANT_INDEX;
    e->eax      = shift;
    e->ebx      = nr_lp;
    e->ecx      = index | (type << 8);
    e->edx      = apic_id;
}

/*
 * 补齐 CPUID 叶 0xB。
 *
 * KVM_GET_SUPPORTED_CPUID 只给出 0xB 的子叶 0 且内容全零（海光 host 上
 * 实测如此），而 Linux 判断“有没有这套枚举”的条件是子叶 0 的 EBX 非零
 * 且层类型为 SMT（topology.c check_extended_topology_leaf()），所以原样
 * 透传等于告诉 guest“不提供”。这对 AMD/海光 guest 没有影响——它们还有
 * 0x80000008 + 0x8000001E 这条退路；但 Intel guest 只认 0x1F/0xB，退路
 * 是 detect_ht() 读 CPUID.1:EBX[23:16] 加 CPUID.4，更难填对。
 *
 * 这里按 1 socket × n core × 1 thread 填三条子叶：
 *   0  SMT  层：每核 1 个线程，移位 0
 *   1  Core 层：每 package n 个核，移位 order_base2(n)
 *   2  终止：类型 0
 * 解析出来的结果与 0x8000001E 那条路完全一致（cpu_core_id = APIC ID，
 * phys_proc_id = 0，x86_max_cores = n，__max_die_per_package = 1），
 * 两条路并存也不会打架。
 *
 * 0x1F（更多层级的版本）仍然整体清零：KVM 也只给子叶 0，填一半反而会让
 * detect_extended_topology() 算出错误的掩码，不如让它明确地走不通。
 *
 * cpuid 缓冲区必须来自 kvm_get_supported_cpuid()，末尾有
 * KVM_CPUID_SPARE 个空位可供追加。
 */
static void cpuid_fill_topology_leaf(struct kvm_cpuid2 *cpuid,
                                     uint32_t id, uint32_t n)
{
    const uint32_t bits = order_base2(n);
    const struct {
        uint32_t shift, nr_lp, type;
    } level[3] = {
        { 0,    1, CPUID_TOPO_TYPE_SMT   },
        { bits, n, CPUID_TOPO_TYPE_CORE  },
        { 0,    0, CPUID_TOPO_TYPE_INVAL },
    };
    uint32_t sub, i;
    uint32_t appended = 0;

    for (sub = 0; sub < 3; sub++) {
        struct kvm_cpuid_entry2 *e = NULL;

        for (i = 0; i < cpuid->nent; i++) {
            if (cpuid->entries[i].function == CPUID_TOPO_LEAF &&
                cpuid->entries[i].index == sub) {
                e = &cpuid->entries[i];
                break;
            }
        }
        if (!e) {
            if (appended >= KVM_CPUID_SPARE) {
                vmm_warn("no room to add CPUID leaf 0x%x subleaf %u",
                         CPUID_TOPO_LEAF, sub);
                return;
            }
            e = &cpuid->entries[cpuid->nent++];
            appended++;
        }
        topo_leaf_set(e, sub, level[sub].shift, level[sub].nr_lp,
                      level[sub].type, id);
    }
}

/*
 * vCPU 数超过 8 时提醒一句：部分 guest 内核不看 CPUID 0x8000001D 报的
 * 缓存共享关系，而是直接拿 APIC ID 的某一位当核簇号。海光 model < 5
 * （mini-vmm 在 soc63 上引导的正是这一类）走的是
 *   linux-6.6 arch/x86/kernel/cpu/cacheinfo.c cacheinfo_hygon_init_llc_id()
 *     per_cpu(cpu_llc_id, cpu) = c->apicid >> 3;
 * 而 mini-vmm 让 APIC ID 等于 vCPU 序号，于是 vCPU 超过 8 个时 guest 会
 * 按 8 个一组切出多个 LLC 域（实测 --smp 16 得到 0-7 和 8-15 两块），
 * 尽管 CPUID 里报的是所有 vCPU 共享同一个 L3。
 *
 * 功能上没有问题，但会改变调度域的形状，拿 mini-vmm 做调度或缓存亲和性
 * 实验时必须知道，所以这里只提示、不改行为——改 CPUID 里的 model 确实能
 * 绕开这条捷径，但会连带影响 guest 的一堆勘误判断，代价更大。
 */
#define LLC_APICID_GROUP_SIZE  8

static void smp_warn_llc_split(int nr_vcpus)
{
    uint32_t r[4], family, model;
    char vendor[13];
    enum arch_vendor v;

    if (nr_vcpus <= LLC_APICID_GROUP_SIZE)
        return;

    v = arch_detect_vendor(vendor);
    if (v != ARCH_VENDOR_HYGON)
        return;

    arch_cpuid(0x1, 0, r);
    family = (r[0] >> 8) & 0xf;
    if (family == 0xf)
        family += (r[0] >> 20) & 0xff;
    model = (r[0] >> 4) & 0xf;
    if (family >= 0xf)
        model |= ((r[0] >> 16) & 0xf) << 4;

    if (model >= 0x5 && !(model >= 0x10 && model <= 0x1f))
        return;

    vmm_warn("%d vCPUs: this guest CPU model derives the LLC id from "
             "APIC ID bit 3, so the guest will split the vCPUs into "
             "%d-CPU last-level-cache domains regardless of what CPUID "
             "leaf 0x8000001D reports", nr_vcpus, LLC_APICID_GROUP_SIZE);
}

/*
 * 拓扑固定为 1 socket × N core × 1 thread，APIC ID = vcpu id。
 *
 * KVM_GET_SUPPORTED_CPUID 返回的拓扑字段是 host 的原始值（例如 leaf 1
 * 里的 initial APIC ID 是 host 上执行这条 ioctl 的那颗 CPU 的），
 * 原样透传会让所有 vCPU 自报同一个 APIC ID、同一套 host 拓扑。
 * 需要改写的 leaf：
 *   1          EBX[31:24] initial APIC ID，EBX[23:16] 逻辑处理器数，
 *              EDX.HTT 表示后者有效
 *   0xB        Intel 扩展拓扑。KVM 只给出全零的 subleaf 0，这里按
 *              1 socket × n core × 1 thread 补齐三条子叶，见
 *              cpuid_fill_topology_leaf()
 *   0x1F       0xB 的多层级版本，整体清零表示“不提供”，同上函数的说明
 *   0x80000008 ECX[7:0] NC = 核数 - 1，ECX[15:12] ApicIdSize
 *   0x8000001D 每级 cache 的 EAX[25:14] 共享线程数 - 1：
 *              L1/L2 每核私有，L3 所有 vCPU 共享
 *   0x8000001E EAX 扩展 APIC ID，EBX[7:0] core id、EBX[15:8] 每核线程数 - 1，
 *              ECX[7:0] node id
 * Hygon 的 guest 拓扑代码见 linux-6.6 arch/x86/kernel/cpu/hygon.c
 * hygon_get_topology() / hygon_detect_cmp()。
 */
void smp_fixup_cpuid(const struct vmm_vcpu *vcpu, struct kvm_cpuid2 *cpuid)
{
    uint32_t id = (uint32_t)vcpu->id;
    uint32_t n  = (uint32_t)vcpu->vm->cfg.nr_vcpus;
    uint32_t i;

    for (i = 0; i < cpuid->nent; i++) {
        struct kvm_cpuid_entry2 *e = &cpuid->entries[i];

        switch (e->function) {
        case 0x1:
            e->ebx = (e->ebx & 0x0000ffffu) | (id << 24) |
                     ((n > 255 ? 255 : n) << 16);
            if (n > 1)
                e->edx |= CPUID_1_EDX_HTT;
            else
                e->edx &= ~CPUID_1_EDX_HTT;
            break;
        case 0x1f:
            e->eax = e->ebx = e->ecx = e->edx = 0;
            break;
        case 0x80000008:
            e->ecx = (e->ecx & ~0xf0ffu) | (order_base2(n) << 12) | (n - 1);
            break;
        case 0x8000001d: {
            uint32_t level = (e->eax >> 5) & 0x7;
            uint32_t share = (level >= 3) ? n - 1 : 0;

            if (e->eax & 0x1f)          /* cache type = 0 表示列表结束 */
                e->eax = (e->eax & ~(0xfffu << 14)) | (share << 14);
            break;
        }
        case 0x8000001e:
            e->eax = id;
            e->ebx = id & 0xff;
            e->ecx = 0;
            break;
        default:
            break;
        }
    }

    /* 0xB 要追加子叶，放在遍历之后做，避免边遍历边改 nent */
    cpuid_fill_topology_leaf(cpuid, id, n);
}

/* ------------------------------------------------------------------ */
/* 线程生命周期                                                        */
/* ------------------------------------------------------------------ */

/* 处理函数什么都不做，作用全在让 KVM_RUN 以 EINTR 返回 */
static void kick_handler(int sig)
{
    (void)sig;
}

/*
 * 停机要解决的竞态：目标线程刚检查完 should_stop（false），还没进
 * KVM_RUN，此时发信号只会被 handler 吃掉，随后它进入 KVM_RUN 并在
 * guest 的 HLT 里无限期睡下去。
 * kvm_run->immediate_exit（KVM_CAP_IMMEDIATE_EXIT）正是为此设计的：
 * KVM_RUN 入口处看到它为 1 就直接返回 -EINTR。先置位再发信号，两个窗口
 * 都被覆盖——没进 KVM_RUN 的被 immediate_exit 挡住，已经在里面的被
 * 信号踢出来。
 */
void vm_request_stop(struct vmm_vm *vm)
{
    int i;

    vm->running = false;
    for (i = 0; i < vm->nr_vcpus; i++) {
        struct vmm_vcpu *v = &vm->vcpus[i];

        v->should_stop = true;
        if (v->run)
            __atomic_store_n(&v->run->immediate_exit, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&v->thread_started, __ATOMIC_ACQUIRE) &&
            !pthread_equal(v->thread, pthread_self()))
            pthread_kill(v->thread, SIGUSR1);
    }
}

static void *vcpu_thread(void *arg)
{
    struct vmm_vcpu *v = arg;
    char name[16];

    snprintf(name, sizeof(name), "vcpu%d", v->id);
    pthread_setname_np(pthread_self(), name);

    v->result = vcpu_run_loop(v);
    if (v->result != VMM_OK)
        vmm_err("vCPU %d stopped with error %d", v->id, v->result);

    /* 任何一个 vCPU 退出（poweroff、reset、出错）都意味着整机停下 */
    vm_request_stop(v->vm);
    return NULL;
}

int vm_run(struct vmm_vm *vm)
{
    struct sigaction sa;
    int i, started = 0, r = VMM_OK;

    /* 不带 SA_RESTART，保证 KVM_RUN 返回 EINTR */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = kick_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) < 0) {
        vmm_err("sigaction(SIGUSR1) failed");
        return VMM_ERR_SYS;
    }

    smp_warn_llc_split(vm->nr_vcpus);

    for (i = 0; i < vm->nr_vcpus; i++) {
        struct vmm_vcpu *v = &vm->vcpus[i];

        if (pthread_create(&v->thread, NULL, vcpu_thread, v) != 0) {
            vmm_err("pthread_create for vCPU %d failed", i);
            r = VMM_ERR_SYS;
            vm_request_stop(vm);
            break;
        }
        __atomic_store_n(&v->thread_started, true, __ATOMIC_RELEASE);
        started++;
    }
    vmm_info("%d vCPU thread(s) running", started);

    for (i = 0; i < started; i++) {
        struct vmm_vcpu *v = &vm->vcpus[i];

        pthread_join(v->thread, NULL);
        __atomic_store_n(&v->thread_started, false, __ATOMIC_RELEASE);
        if (r == VMM_OK && v->result != VMM_OK)
            r = v->result;
    }
    return r;
}
