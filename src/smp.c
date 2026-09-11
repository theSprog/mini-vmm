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

/* ------------------------------------------------------------------ */
/* CPUID 拓扑                                                          */
/* ------------------------------------------------------------------ */

#define CPUID_1_EDX_HTT        (1u << 28)

static uint32_t order_base2(uint32_t n)
{
    uint32_t b = 0;

    while ((1u << b) < n)
        b++;
    return b;
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
 *   0xB / 0x1F Intel 扩展拓扑。这里整体清零表示“不提供”，guest 退回
 *              AMD 的 0x80000008 / 0x8000001E 路径；KVM 只给了
 *              subleaf 0，补全需要扩容 cpuid 数组，等 Intel 后端再做
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
        case 0xb:
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
