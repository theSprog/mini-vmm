/* src/vcpu.c — vCPU 运行循环：KVM_RUN、exit 分发、exit 计数、寄存器转储
 *
 * 每个 vCPU 线程（smp.c）都在 vcpu_run_loop 里循环：进 KVM_RUN，回到用户态
 * 后按 exit_reason 分发，PIO/MMIO 交给 io_bus.c。 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "vmm.h"
#include "kvm_wrappers.h"
#include "devices/irqchip.h"
#include "gdb/gdbstub.h"
#include "smp.h"

/* ------------------------------------------------------------------ */
/* 调试输出                                                            */
/* ------------------------------------------------------------------ */

const char *kvm_exit_reason_str(uint32_t r)
{
    switch (r) {
    case KVM_EXIT_UNKNOWN:        return "UNKNOWN";
    case KVM_EXIT_EXCEPTION:      return "EXCEPTION";
    case KVM_EXIT_IO:             return "IO";
    case KVM_EXIT_HYPERCALL:      return "HYPERCALL";
    case KVM_EXIT_DEBUG:          return "DEBUG";
    case KVM_EXIT_HLT:            return "HLT";
    case KVM_EXIT_MMIO:           return "MMIO";
    case KVM_EXIT_IRQ_WINDOW_OPEN:return "IRQ_WINDOW_OPEN";
    case KVM_EXIT_SHUTDOWN:       return "SHUTDOWN";
    case KVM_EXIT_FAIL_ENTRY:     return "FAIL_ENTRY";
    case KVM_EXIT_INTR:           return "INTR";
    case KVM_EXIT_SET_TPR:        return "SET_TPR";
    case KVM_EXIT_TPR_ACCESS:     return "TPR_ACCESS";
    case KVM_EXIT_NMI:            return "NMI";
    case KVM_EXIT_INTERNAL_ERROR: return "INTERNAL_ERROR";
    case KVM_EXIT_SYSTEM_EVENT:   return "SYSTEM_EVENT";
    case KVM_EXIT_IOAPIC_EOI:     return "IOAPIC_EOI";
    case KVM_EXIT_HYPERV:         return "HYPERV";
    case KVM_EXIT_X86_RDMSR:      return "X86_RDMSR";
    case KVM_EXIT_X86_WRMSR:      return "X86_WRMSR";
    case KVM_EXIT_DIRTY_RING_FULL:return "DIRTY_RING_FULL";
    case KVM_EXIT_AP_RESET_HOLD:  return "AP_RESET_HOLD";
    case KVM_EXIT_X86_BUS_LOCK:   return "X86_BUS_LOCK";
    default:                      return "?";
    }
}

static void dump_seg(FILE *out, const char *name, const struct kvm_segment *s)
{
    fprintf(out, "  %-3s sel=%04x base=%016llx limit=%08x "
                    "type=%x s=%u dpl=%u p=%u l=%u db=%u g=%u\n",
            name, s->selector, (unsigned long long)s->base, s->limit,
            s->type, s->s, s->dpl, s->present, s->l, s->db, s->g);
}

void vcpu_print_state(FILE *out, struct vmm_vcpu *vcpu)
{
    struct kvm_regs r;
    struct kvm_sregs s;

    if (kvm_get_regs(vcpu->fd, &r) != VMM_OK ||
        kvm_get_sregs(vcpu->fd, &s) != VMM_OK) {
        fprintf(out, "cannot read vCPU %d state\n", vcpu->id);
        return;
    }

    fprintf(out, "=== vCPU %d state ===\n", vcpu->id);
    fprintf(out, "  rip=%016llx rflags=%016llx\n",
            (unsigned long long)r.rip, (unsigned long long)r.rflags);
    fprintf(out, "  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n",
            (unsigned long long)r.rax, (unsigned long long)r.rbx,
            (unsigned long long)r.rcx, (unsigned long long)r.rdx);
    fprintf(out, "  rsi=%016llx rdi=%016llx rsp=%016llx rbp=%016llx\n",
            (unsigned long long)r.rsi, (unsigned long long)r.rdi,
            (unsigned long long)r.rsp, (unsigned long long)r.rbp);
    fprintf(out, "  cr0=%016llx cr2=%016llx cr3=%016llx cr4=%016llx\n",
            (unsigned long long)s.cr0, (unsigned long long)s.cr2,
            (unsigned long long)s.cr3, (unsigned long long)s.cr4);
    fprintf(out, "  efer=%016llx  (LME=%d LMA=%d)  apic_base=%016llx\n",
            (unsigned long long)s.efer,
            !!(s.efer & (1ULL << 8)), !!(s.efer & (1ULL << 10)),
            (unsigned long long)s.apic_base);
    dump_seg(out, "cs", &s.cs);
    dump_seg(out, "ss", &s.ss);
    dump_seg(out, "ds", &s.ds);
    fprintf(out, "  gdt base=%016llx limit=%04x  idt base=%016llx limit=%04x\n",
            (unsigned long long)s.gdt.base, s.gdt.limit,
            (unsigned long long)s.idt.base, s.idt.limit);
    fprintf(out, "  exits=%llu (io=%llu mmio=%llu hlt=%llu)\n",
            (unsigned long long)vcpu_exit_total(vcpu),
            (unsigned long long)vcpu_exit_count(vcpu, KVM_EXIT_IO),
            (unsigned long long)vcpu_exit_count(vcpu, KVM_EXIT_MMIO),
            (unsigned long long)vcpu_exit_count(vcpu, KVM_EXIT_HLT));
}

void vcpu_dump_state(struct vmm_vcpu *vcpu)
{
    vcpu_print_state(stderr, vcpu);
}

void vcpu_count_exit(struct vmm_vcpu *vcpu, uint32_t reason)
{
    int slot = reason < VMM_EXIT_REASON_SLOTS - 1 ? (int)reason
                                                  : VMM_EXIT_REASON_SLOTS - 1;
    uint64_t n = __atomic_load_n(&vcpu->exits[slot], __ATOMIC_RELAXED);

    __atomic_store_n(&vcpu->exits[slot], n + 1, __ATOMIC_RELAXED);
    __atomic_store_n(&vcpu->last_exit, reason, __ATOMIC_RELAXED);
}

uint64_t vcpu_exit_count(const struct vmm_vcpu *vcpu, int slot)
{
    return __atomic_load_n(&vcpu->exits[slot], __ATOMIC_RELAXED);
}

uint64_t vcpu_exit_total(const struct vmm_vcpu *vcpu)
{
    uint64_t sum = 0;
    int i;

    for (i = 0; i < VMM_EXIT_REASON_SLOTS; i++)
        sum += vcpu_exit_count(vcpu, i);
    return sum;
}


/* ------------------------------------------------------------------ */
/* exit 处理                                                           */
/* ------------------------------------------------------------------ */


/* 以下两个是 KVM_EXIT 路径上的薄封装：只负责从 kvm_run 里把字段取出来。 */

static int handle_io(struct vmm_vcpu *vcpu)
{
    struct kvm_run *run = vcpu->run;
    uint8_t *data;

    /* 数据缓冲区在 kvm_run 页内，偏移由内核填在 io.data_offset。
     * 不能假设它紧跟在结构体后面——必须按 data_offset 算。 */
    data = (uint8_t *)run + run->io.data_offset;

    return io_bus_dispatch_pio(vcpu, (uint16_t)run->io.port,
                               run->io.direction == KVM_EXIT_IO_OUT,
                               run->io.size, run->io.count, data);
}

static int handle_mmio(struct vmm_vcpu *vcpu)
{
    struct kvm_run *run = vcpu->run;

    return io_bus_dispatch_mmio(vcpu, run->mmio.phys_addr,
                                run->mmio.is_write != 0,
                                run->mmio.len, run->mmio.data);
}

static void report_internal_error(struct vmm_vcpu *vcpu)
{
    struct kvm_run *run = vcpu->run;
    uint32_t i;

    vmm_err("KVM_EXIT_INTERNAL_ERROR suberror=%u ndata=%u",
            run->internal.suberror, run->internal.ndata);

    switch (run->internal.suberror) {
    case KVM_INTERNAL_ERROR_EMULATION:
        vmm_err("  KVM tried to emulate an instruction in software and failed.");
        vmm_err("  likely causes: RIP does not point at a valid "
                "instruction (payload not loaded? wrong entry "
                "address? broken page table?)");
        if (run->emulation_failure.flags &
            KVM_INTERNAL_ERROR_EMULATION_FLAG_INSTRUCTION_BYTES) {
            fprintf(stderr, "  instruction bytes:");
            for (i = 0; i < run->emulation_failure.insn_size; i++)
                fprintf(stderr, " %02x",
                        run->emulation_failure.insn_bytes[i]);
            fputc('\n', stderr);
        }
        break;
    case KVM_INTERNAL_ERROR_SIMUL_EX:
        vmm_err("  multiple simultaneous exceptions");
        break;
    case KVM_INTERNAL_ERROR_DELIVERY_EV:
        vmm_err("  unexpected VM exit while delivering an event");
        break;
    default:
        for (i = 0; i < run->internal.ndata && i < 16; i++)
            vmm_err("  data[%u] = 0x%llx", i,
                    (unsigned long long)run->internal.data[i]);
        break;
    }
}

int vcpu_handle_exit(struct vmm_vcpu *vcpu)
{
    struct kvm_run *run = vcpu->run;
    const struct irqchip_ops *irqchip = vcpu->vm->irqchip;

    vcpu_count_exit(vcpu, run->exit_reason);

    switch (run->exit_reason) {
    case KVM_EXIT_IO: {
        int r;

        pthread_mutex_lock(&vcpu->vm->io_lock);
        r = handle_io(vcpu);
        pthread_mutex_unlock(&vcpu->vm->io_lock);
        return r;
    }

    case KVM_EXIT_MMIO: {
        int r;

        pthread_mutex_lock(&vcpu->vm->io_lock);
        r = handle_mmio(vcpu);
        pthread_mutex_unlock(&vcpu->vm->io_lock);
        return r;
    }

    case KVM_EXIT_HLT:
        vmm_info("vCPU %d: HLT, stopping", vcpu->id);
        return VMM_ERR_EXIT;

    case KVM_EXIT_INTR:
        /* 被信号打断，正常继续 */
        return VMM_OK;

    case KVM_EXIT_SHUTDOWN:
        /* x86 上这基本等同于 triple fault。没装 IDT 时，任何异常
         * 都会走到这里，是长模式 payload 最常见的失败形式。 */
        vmm_err("vCPU %d: SHUTDOWN (usually a triple fault)", vcpu->id);
        vcpu_dump_state(vcpu);
        return VMM_ERR_GUEST;

    case KVM_EXIT_FAIL_ENTRY:
        vmm_err("vCPU %d: VM entry failed, reason=0x%llx cpu=%u",
                vcpu->id,
                (unsigned long long)run->fail_entry.hardware_entry_failure_reason,
                run->fail_entry.cpu);
        vcpu_dump_state(vcpu);
        return VMM_ERR_GUEST;

    case KVM_EXIT_INTERNAL_ERROR:
        report_internal_error(vcpu);
        vcpu_dump_state(vcpu);
        return VMM_ERR_GUEST;

    case KVM_EXIT_DEBUG:
        /* 只有 gdbstub 打开 guest debug 后才会出现 */
        if (vcpu->vm->gdb)
            return gdbstub_vcpu_debug_exit(vcpu);
        vmm_err("vCPU %d: KVM_EXIT_DEBUG without gdbstub: exception %u "
                "pc 0x%llx", vcpu->id, run->debug.arch.exception,
                (unsigned long long)run->debug.arch.pc);
        vcpu_dump_state(vcpu);
        return VMM_ERR_GUEST;

    case KVM_EXIT_SYSTEM_EVENT:
        vmm_info("vCPU %d: SYSTEM_EVENT type=%u", vcpu->id,
                 run->system_event.type);
        return VMM_ERR_EXIT;

    case KVM_EXIT_IRQ_WINDOW_OPEN:
        /* 用户态中断控制器请求的中断窗口开了：回到 run loop 顶部，由
         * pre_run 注入 */
        if (irqchip && irqchip->pre_run)
            return VMM_OK;
        break;

    case KVM_EXIT_IOAPIC_EOI:
        if (irqchip && irqchip->eoi)
            return irqchip->eoi(vcpu, run->eoi.vector);
        break;

    default:
        break;
    }

    vmm_err("vCPU %d: unhandled exit_reason=%u (%s)", vcpu->id,
            run->exit_reason, kvm_exit_reason_str(run->exit_reason));
    vcpu_dump_state(vcpu);
    return VMM_ERR_GUEST;
}

int vcpu_run_loop(struct vmm_vcpu *vcpu)
{
    const struct irqchip_ops *irqchip = vcpu->vm->irqchip;
    int r;

    while (!__atomic_load_n(&vcpu->should_stop, __ATOMIC_SEQ_CST)) {
        /* 中断控制器在用户态时，外部中断由 VMM 在进 KVM_RUN 之前注入 */
        if (irqchip && irqchip->pre_run) {
            r = irqchip->pre_run(vcpu);
            if (r != VMM_OK)
                return r;
        }
        __atomic_store_n(&vcpu->state, VCPU_GUEST, __ATOMIC_RELAXED);
        r = kvm_run(vcpu->fd);
        __atomic_store_n(&vcpu->state, VCPU_VMM, __ATOMIC_RELAXED);
        /* 被打断时 kvm_run 页里的 exit_reason 不一定有效（immediate_exit
         * 路径下 KVM 不会改写它，还是上一次的值），不能交给
         * vcpu_handle_exit，否则会把上一次的 IO 再执行一遍 */
        if (r == VMM_ERR_INTR) {
            /* 被踢出来的原因要么是停机，要么是有工作（或 gdb 要暂停）。
             * 先清 immediate_exit 再看工作队列，顺序的理由见 smp.c 的
             * vcpu_kick()；停机时不清，正在退出的线程不能再睡进 KVM_RUN */
            if (!__atomic_load_n(&vcpu->should_stop, __ATOMIC_SEQ_CST))
                __atomic_store_n(&vcpu->run->immediate_exit, 0,
                                 __ATOMIC_SEQ_CST);
            vcpu_run_work(vcpu);
            /* AP 唤醒（EAGAIN）和停机（EINTR）都走这条路，每个 vCPU 整个
             * 生命周期里只会走几次，所以在这里查一次 mp_state 的代价可以
             * 忽略，而它是判断 AP 卡在哪一步的决定性证据：
             *   UNINITIALIZED   还没收到 INIT，查 MADT 里这颗 CPU 的登记
             *   INIT_RECEIVED   收到 INIT 在等 SIPI
             *   RUNNABLE        已经在跑或正要跑
             * 必须在本 vCPU 的线程里调，见 kvm_get_mp_state() 的说明。 */
            if (vmm_log_level >= 3) {
                uint32_t st;

                if (kvm_get_mp_state(vcpu->fd, &st) == VMM_OK)
                    vmm_dbg("vCPU %d: KVM_RUN interrupted, mp_state=%u (%s)",
                            vcpu->id, st, kvm_mp_state_str(st));
            }
            /* gdb 要求暂停时停在这里。这一刻挂起的 IO 已经被 KVM 完成，
             * 寄存器是一致的，见 gdb/gdbstub.h */
            if (vcpu->vm->gdb)
                gdbstub_vcpu_interrupted(vcpu);
            continue;
        }
        if (r != VMM_OK)
            return r;

        r = vcpu_handle_exit(vcpu);
        if (r == VMM_ERR_EXIT)
            return VMM_OK;      /* 正常停机 */
        if (r != VMM_OK)
            return r;
    }
    return VMM_OK;
}
