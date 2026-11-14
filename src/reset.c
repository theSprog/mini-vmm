/* src/reset.c — 整机复位，设计见 include/reset.h */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "vmm.h"
#include "kvm_wrappers.h"
#include "devices/irqchip.h"
#include "gdb/gdbstub.h"
#include "reset.h"

struct vcpu_power_on {
    struct kvm_regs         regs;
    struct kvm_sregs        sregs;
    struct kvm_xsave       *xsave;
    struct kvm_xcrs         xcrs;
    struct kvm_msrs        *msrs;          /* 只含能读也能写回的 MSR */
    struct kvm_lapic_state  lapic;
    struct kvm_mp_state     mp;
    struct kvm_vcpu_events  events;
    struct kvm_debugregs    debugregs;
};

struct vm_power_on {
    struct vcpu_power_on   vcpu[VMM_MAX_VCPUS];
    size_t                 xsave_size;
    bool                   has_irqchip;
    struct kvm_irqchip     chip[3];        /* PIC master、PIC slave、IOAPIC */
    struct kvm_pit_state2  pit;
};

/* 失败时带上 ioctl 名字和 errno，和 kvm_wrappers.c 的约定一致 */
#define STATE_IOCTL(fd, req, arg, who) ({                                 \
    int _r = ioctl((fd), (req), (arg));                                \
    if (_r < 0)                                                        \
        vmm_err("%s: ioctl(%s) failed: %s", (who), #req, strerror(errno)); \
    _r;                                                                \
})

int vmm_register_reset(struct vmm_vm *vm, const char *name, vmm_reset_fn fn,
                       void *opaque)
{
    struct vmm_reset_handler *h;

    if (vm->nr_reset_handlers == VMM_MAX_RESET_HANDLERS) {
        vmm_err("reset: too many reset handlers, cannot add %s", name);
        return VMM_ERR_NOMEM;
    }
    h = &vm->reset_handlers[vm->nr_reset_handlers++];
    h->name   = name;
    h->fn     = fn;
    h->opaque = opaque;
    return VMM_OK;
}

/* ------------------------------------------------------------------ */
/* MSR                                                                 */
/* ------------------------------------------------------------------ */

/* KVM_GET_MSR_INDEX_LIST 给出 KVM 认为需要保存的 MSR（硬件 MSR 加 KVM 自己
 * 模拟的 PV MSR）。返回 kvm_msr_list，调用方 free。 */
static struct kvm_msr_list *msr_index_list(int kvm_fd)
{
    struct kvm_msr_list probe = { .nmsrs = 0 }, *list;

    /* 第一次调用必然以 E2BIG 失败，作用是拿到个数 */
    if (ioctl(kvm_fd, KVM_GET_MSR_INDEX_LIST, &probe) < 0 && errno != E2BIG) {
        vmm_err("reset: ioctl(KVM_GET_MSR_INDEX_LIST) failed: %s",
                strerror(errno));
        return NULL;
    }
    list = calloc(1, sizeof(*list) + probe.nmsrs * sizeof(list->indices[0]));
    if (!list)
        return NULL;
    list->nmsrs = probe.nmsrs;
    if (STATE_IOCTL(kvm_fd, KVM_GET_MSR_INDEX_LIST, list, "reset") < 0) {
        free(list);
        return NULL;
    }
    return list;
}

/* KVM_GET_MSRS / KVM_SET_MSRS 返回成功处理的条目数，在第一个失败的条目处
 * 停下，所以逐个试：列表里有些 MSR 在这颗 CPU 或这份 CPUID 下不存在（读
 * 不出来），有些只能读不能由 host 写回。两样都过的才存，复位时一次写回
 * 就不会在中途停下。写回的是刚读出的值，此时 vCPU 还没跑过，等于没改。 */
static struct kvm_msrs *msrs_save(int vcpu_fd, const struct kvm_msr_list *list,
                                  int id)
{
    struct kvm_msrs *out;
    struct {
        struct kvm_msrs hdr;
        struct kvm_msr_entry ent;
    } one;
    unsigned int i, skipped = 0;

    out = calloc(1, sizeof(*out) + list->nmsrs * sizeof(out->entries[0]));
    if (!out)
        return NULL;
    for (i = 0; i < list->nmsrs; i++) {
        memset(&one, 0, sizeof(one));
        one.hdr.nmsrs = 1;
        one.ent.index = list->indices[i];
        if (ioctl(vcpu_fd, KVM_GET_MSRS, &one.hdr) != 1 ||
            ioctl(vcpu_fd, KVM_SET_MSRS, &one.hdr) != 1) {
            skipped++;
            continue;
        }
        out->entries[out->nmsrs++] = one.ent;
    }
    vmm_dbg("reset: vCPU %d: saved %u MSRs, %u in KVM's list not readable "
            "and writable", id, out->nmsrs, skipped);
    return out;
}

static int msrs_restore(int vcpu_fd, struct kvm_msrs *msrs, int id)
{
    int n = ioctl(vcpu_fd, KVM_SET_MSRS, msrs);

    if (n < 0) {
        vmm_err("reset: vCPU %d: ioctl(KVM_SET_MSRS) failed: %s", id,
                strerror(errno));
        return VMM_ERR_SYS;
    }
    if ((unsigned int)n != msrs->nmsrs) {
        vmm_err("reset: vCPU %d: KVM_SET_MSRS stopped at entry %d of %u, "
                "MSR 0x%x value 0x%llx", id, n, msrs->nmsrs,
                msrs->entries[n].index,
                (unsigned long long)msrs->entries[n].data);
        return VMM_ERR_SYS;
    }
    return VMM_OK;
}

/* ------------------------------------------------------------------ */
/* 保存                                                                */
/* ------------------------------------------------------------------ */

static int vcpu_save(struct vm_power_on *po, struct vmm_vcpu *v,
                     const struct kvm_msr_list *list)
{
    struct vcpu_power_on *s = &po->vcpu[v->id];
    char who[24];

    snprintf(who, sizeof(who), "reset: vCPU %d", v->id);
    s->xsave = calloc(1, po->xsave_size);
    if (!s->xsave)
        return VMM_ERR_NOMEM;
    if (STATE_IOCTL(v->fd, KVM_GET_REGS, &s->regs, who) < 0 ||
        STATE_IOCTL(v->fd, KVM_GET_SREGS, &s->sregs, who) < 0 ||
        STATE_IOCTL(v->fd, po->xsave_size > sizeof(struct kvm_xsave)
                    ? KVM_GET_XSAVE2 : KVM_GET_XSAVE, s->xsave, who) < 0 ||
        STATE_IOCTL(v->fd, KVM_GET_XCRS, &s->xcrs, who) < 0 ||
        STATE_IOCTL(v->fd, KVM_GET_LAPIC, &s->lapic, who) < 0 ||
        STATE_IOCTL(v->fd, KVM_GET_MP_STATE, &s->mp, who) < 0 ||
        STATE_IOCTL(v->fd, KVM_GET_VCPU_EVENTS, &s->events, who) < 0 ||
        STATE_IOCTL(v->fd, KVM_GET_DEBUGREGS, &s->debugregs, who) < 0)
        return VMM_ERR_SYS;
    /* GET 不返回 NMI_PENDING 这个标志，它在 SET 里的意思是"按 nmi.pending
     * 设置"。带上它，复位时挂起的 NMI 才会被清掉（这里存的值是 0） */
    s->events.flags |= KVM_VCPUEVENT_VALID_NMI_PENDING;
    s->msrs = msrs_save(v->fd, list, v->id);
    return s->msrs ? VMM_OK : VMM_ERR_SYS;
}

int vm_power_on_save(struct vmm_vm *vm)
{
    static const int caps[] = { KVM_CAP_XSAVE, KVM_CAP_XCRS,
                                KVM_CAP_VCPU_EVENTS, KVM_CAP_DEBUGREGS };
    static const char *const cap_names[] = { "KVM_CAP_XSAVE", "KVM_CAP_XCRS",
                                "KVM_CAP_VCPU_EVENTS", "KVM_CAP_DEBUGREGS" };
    struct vm_power_on *po;
    struct kvm_msr_list *list;
    int i, sz, r = VMM_OK;

    for (i = 0; i < (int)(sizeof(caps) / sizeof(caps[0])); i++) {
        if (!kvm_check_extension(vm->kvm_fd, caps[i])) {
            vmm_err("reset: KVM lacks %s, cannot save the power-on state",
                    cap_names[i]);
            return VMM_ERR_UNSUPPORTED;
        }
    }
    po = calloc(1, sizeof(*po));
    if (!po)
        return VMM_ERR_NOMEM;
    vm->power_on = po;

    /* 打开了 AMX 这类大状态时 XSAVE 区超过 4 KiB，要用 KVM_GET_XSAVE2，
     * 大小由 KVM_CAP_XSAVE2 给出；SET 仍是 KVM_SET_XSAVE，内核按同一个大小
     * 拷贝（Documentation/virt/kvm/api.rst 的 KVM_GET_XSAVE2） */
    sz = kvm_check_extension(vm->vm_fd, KVM_CAP_XSAVE2);
    po->xsave_size = sz > (int)sizeof(struct kvm_xsave)
                   ? (size_t)sz : sizeof(struct kvm_xsave);

    list = msr_index_list(vm->kvm_fd);
    if (!list)
        return VMM_ERR_SYS;
    for (i = 0; i < vm->nr_vcpus && r == VMM_OK; i++)
        r = vcpu_save(po, &vm->vcpus[i], list);
    free(list);
    if (r != VMM_OK)
        return r;

    po->has_irqchip = vm->has_irqchip;
    if (po->has_irqchip) {
        for (i = 0; i < 3; i++) {
            po->chip[i].chip_id = (uint32_t)i;
            if (STATE_IOCTL(vm->vm_fd, KVM_GET_IRQCHIP, &po->chip[i],
                            "reset") < 0)
                return VMM_ERR_SYS;
        }
        if (STATE_IOCTL(vm->vm_fd, KVM_GET_PIT2, &po->pit, "reset") < 0)
            return VMM_ERR_SYS;
    }
    vmm_info("reset: saved power-on state of %d vCPU(s), %u MSRs each, "
             "XSAVE %zu bytes", vm->nr_vcpus, po->vcpu[0].msrs->nmsrs,
             po->xsave_size);
    return VMM_OK;
}

void vm_power_on_free(struct vmm_vm *vm)
{
    struct vm_power_on *po = vm->power_on;
    int i;

    if (!po)
        return;
    for (i = 0; i < VMM_MAX_VCPUS; i++) {
        free(po->vcpu[i].xsave);
        free(po->vcpu[i].msrs);
    }
    free(po);
    vm->power_on = NULL;
}

/* ------------------------------------------------------------------ */
/* 复位                                                                */
/* ------------------------------------------------------------------ */

int vm_request_reset(struct vmm_vm *vm, const char *source)
{
    vmm_info("guest requested reset via %s", source);
    vm->exit_code = 0;
    if (vm->cfg.no_reboot)
        vmm_info("--no-reboot: stopping instead of resetting");
    else
        __atomic_store_n(&vm->reset_requested, true, __ATOMIC_SEQ_CST);
    return VMM_ERR_EXIT;
}

/* 顺序：SREGS 带着 APIC base，要在 LAPIC 之前；LAPIC 的定时器模式要在
 * TSC deadline MSR 之前（MSR 里存的是 0，顺序不对也只是多一次无效写，但
 * 保持这个依赖顺序，存进来的值不为 0 时也成立）；MP_STATE 放在最后，
 * AP 回到 UNINITIALIZED 等 INIT/SIPI。 */
static int vcpu_restore(struct vm_power_on *po, struct vmm_vcpu *v)
{
    struct vcpu_power_on *s = &po->vcpu[v->id];
    char who[24];

    snprintf(who, sizeof(who), "reset: vCPU %d", v->id);
    if (STATE_IOCTL(v->fd, KVM_SET_SREGS, &s->sregs, who) < 0 ||
        STATE_IOCTL(v->fd, KVM_SET_REGS, &s->regs, who) < 0 ||
        STATE_IOCTL(v->fd, KVM_SET_XSAVE, s->xsave, who) < 0 ||
        STATE_IOCTL(v->fd, KVM_SET_XCRS, &s->xcrs, who) < 0 ||
        STATE_IOCTL(v->fd, KVM_SET_LAPIC, &s->lapic, who) < 0)
        return VMM_ERR_SYS;
    if (msrs_restore(v->fd, s->msrs, v->id) != VMM_OK)
        return VMM_ERR_SYS;
    if (STATE_IOCTL(v->fd, KVM_SET_VCPU_EVENTS, &s->events, who) < 0 ||
        STATE_IOCTL(v->fd, KVM_SET_DEBUGREGS, &s->debugregs, who) < 0 ||
        STATE_IOCTL(v->fd, KVM_SET_MP_STATE, &s->mp, who) < 0)
        return VMM_ERR_SYS;
    return VMM_OK;
}

static int irqchip_restore(struct vmm_vm *vm, struct vm_power_on *po)
{
    int gsi, i;

    /* 先拉低每根线，理由见 reset.h。KVM_IRQ_LINE 同时作用于 PIC 和 IOAPIC
     * 上这个 GSI 的路由，两边的 irq_states 一起清掉 */
    for (gsi = 0; gsi < KVM_IOAPIC_NUM_PINS; gsi++)
        if (irqchip_set_irq(vm, gsi, 0) != VMM_OK)
            return VMM_ERR_SYS;
    for (i = 0; i < 3; i++)
        if (STATE_IOCTL(vm->vm_fd, KVM_SET_IRQCHIP, &po->chip[i], "reset") < 0)
            return VMM_ERR_SYS;
    if (STATE_IOCTL(vm->vm_fd, KVM_SET_PIT2, &po->pit, "reset") < 0)
        return VMM_ERR_SYS;
    return VMM_OK;
}

int vm_reset(struct vmm_vm *vm, vmm_reload_fn reload, void *opaque)
{
    struct vm_power_on *po = vm->power_on;
    int i, r = VMM_OK;

    if (!po) {
        vmm_err("reset: no power-on state saved, cannot reset");
        return VMM_ERR_INVAL;
    }
    pthread_mutex_lock(&vm->io_lock);
    if (po->has_irqchip)
        r = irqchip_restore(vm, po);
    for (i = 0; i < vm->nr_vcpus && r == VMM_OK; i++)
        r = vcpu_restore(po, &vm->vcpus[i]);
    if (r == VMM_OK) {
        for (i = 0; i < vm->nr_reset_handlers; i++) {
            vmm_dbg("reset: device %s", vm->reset_handlers[i].name);
            vm->reset_handlers[i].fn(vm->reset_handlers[i].opaque);
        }
        /* 软件断点的 int3 在 guest 内存里，镜像重新装载前后要分别撤下和
         * 重新写入，见 gdb/gdbstub.h */
        if (vm->gdb)
            gdbstub_reset_before(vm->gdb);
        r = reload(opaque);
    }
    if (r == VMM_OK) {
        for (i = 0; i < vm->nr_vcpus; i++) {
            struct vmm_vcpu *v = &vm->vcpus[i];

            v->should_stop = false;
            __atomic_store_n(&v->run->immediate_exit, 0, __ATOMIC_SEQ_CST);
        }
        vm->reset_count++;
        /* running 先于 reset_requested 改变：gdbstub 线程看到两者同时为
         * false 才认为 VM 结束了，这个顺序保证它在复位过程中的任何时刻
         * 都不会误判 */
        __atomic_store_n(&vm->running, true, __ATOMIC_SEQ_CST);
        __atomic_store_n(&vm->reset_requested, false, __ATOMIC_SEQ_CST);
        if (vm->gdb)
            gdbstub_reset_after(vm->gdb);
    }
    pthread_mutex_unlock(&vm->io_lock);

    if (r != VMM_OK)
        vmm_err("reset: failed, stopping the VM");
    else
        vmm_info("reset: guest reset #%u done, restarting vCPUs",
                 vm->reset_count);
    return r;
}
