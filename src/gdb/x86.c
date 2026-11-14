/* src/gdb/x86.c — 断点表、DR7、KVM_EXIT_DEBUG 归类、寄存器换算，说明见 gdb/x86.h */
#include <errno.h>
#include <string.h>

#include "gdb/x86.h"

/* ------------------------------------------------------------------ */
/* 软件断点                                                            */
/* ------------------------------------------------------------------ */

int gdb_x86_sw_find(const struct gdb_x86_bps *b, uint64_t gva)
{
    int i;

    for (i = 0; i < b->nr_sw; i++)
        if (b->sw[i].gva == gva)
            return i;
    return -1;
}

int gdb_x86_sw_add(struct gdb_x86_bps *b, uint64_t gva, uint64_t gpa,
                   uint8_t orig)
{
    if (gdb_x86_sw_find(b, gva) >= 0)
        return 0;
    if (b->nr_sw >= GDB_X86_MAX_SW_BP)
        return -ENOSPC;
    b->sw[b->nr_sw].gva  = gva;
    b->sw[b->nr_sw].gpa  = gpa;
    b->sw[b->nr_sw].orig = orig;
    b->nr_sw++;
    return 0;
}

int gdb_x86_sw_del(struct gdb_x86_bps *b, uint64_t gva,
                   struct gdb_x86_sw_bp *out)
{
    int i = gdb_x86_sw_find(b, gva);

    if (i < 0)
        return -ENOENT;
    *out = b->sw[i];
    b->sw[i] = b->sw[--b->nr_sw];
    return 0;
}

void gdb_x86_sw_hide(const struct gdb_x86_bps *b, uint64_t gpa,
                     uint8_t *buf, size_t len)
{
    int i;

    for (i = 0; i < b->nr_sw; i++) {
        uint64_t off = b->sw[i].gpa - gpa;     /* gpa 之前的断点回绕成大数 */

        if (off < len)
            buf[off] = b->sw[i].orig;
    }
}

void gdb_x86_sw_shadow_write(struct gdb_x86_bps *b, uint64_t gpa,
                             uint8_t *buf, size_t len)
{
    int i;

    for (i = 0; i < b->nr_sw; i++) {
        uint64_t off = b->sw[i].gpa - gpa;

        if (off < len) {
            b->sw[i].orig = buf[off];
            buf[off] = GDB_X86_INT3;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 硬件断点                                                            */
/* ------------------------------------------------------------------ */

int gdb_x86_hw_add(struct gdb_x86_bps *b, uint64_t addr)
{
    int i, free_slot = -1;

    for (i = 0; i < GDB_X86_NR_HW_BP; i++) {
        if (b->hw_used[i] && b->hw[i] == addr)
            return i;
        if (!b->hw_used[i] && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        return -ENOSPC;
    b->hw[free_slot] = addr;
    b->hw_used[free_slot] = true;
    return free_slot;
}

int gdb_x86_hw_del(struct gdb_x86_bps *b, uint64_t addr)
{
    int i;

    for (i = 0; i < GDB_X86_NR_HW_BP; i++) {
        if (b->hw_used[i] && b->hw[i] == addr) {
            b->hw_used[i] = false;
            b->hw[i] = 0;
            return 0;
        }
    }
    return -ENOENT;
}

uint64_t gdb_x86_dr7(const struct gdb_x86_bps *b)
{
    uint64_t dr7 = GDB_X86_DR7_FIXED_1;
    int i;

    /* Ln 在 bit 2n；R/Wn 在 bit 16+4n、LENn 在 bit 18+4n，都保持 00 */
    for (i = 0; i < GDB_X86_NR_HW_BP; i++)
        if (b->hw_used[i])
            dr7 |= 1ull << (2 * i);
    return dr7;
}

static bool have_hw(const struct gdb_x86_bps *b)
{
    int i;

    for (i = 0; i < GDB_X86_NR_HW_BP; i++)
        if (b->hw_used[i])
            return true;
    return false;
}

void gdb_x86_guest_debug(const struct gdb_x86_bps *b, bool step,
                         uint32_t step_extra, struct kvm_guest_debug *dbg)
{
    int i;

    memset(dbg, 0, sizeof(*dbg));
    if (b->nr_sw)
        dbg->control |= KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_SW_BP;
    if (have_hw(b)) {
        dbg->control |= KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_HW_BP;
        for (i = 0; i < GDB_X86_NR_HW_BP; i++)
            dbg->arch.debugreg[i] = b->hw_used[i] ? b->hw[i] : 0;
        dbg->arch.debugreg[7] = gdb_x86_dr7(b);
    }
    if (step)
        dbg->control |= KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP |
                        step_extra;
}

/* ------------------------------------------------------------------ */
/* KVM_EXIT_DEBUG 归类                                                 */
/* ------------------------------------------------------------------ */

/*
 * USE_SW_BP 打开后 guest 执行的每一条 int3 都会退出到这里，包括 guest 自己
 * 的：Linux 启动时 int3_selftest() 故意执行一条 int3 并检查自己的 #BP
 * 处理函数被调用了（linux-6.6 arch/x86/kernel/alternative.c），运行中
 * text_poke_bp() 改代码时也临时插 int3。所以 pc 不在断点表里的 #BP 必须
 * 还给 guest，否则 guest 会 BUG。
 *
 * #DB 同理：USE_HW_BP 或 SINGLESTEP 打开时所有 #DB 都退出到这里，DR6 里
 * 既不是我们的断点槽、也不是我们要的单步的，还给 guest。
 */
enum gdb_x86_event gdb_x86_classify(const struct gdb_x86_bps *b,
                                    const struct kvm_debug_exit_arch *ex,
                                    bool stepping)
{
    int i;

    if (ex->exception == GDB_X86_BP_VECTOR)
        return gdb_x86_sw_find(b, ex->pc) >= 0 ? GDB_X86_EV_SWBREAK
                                               : GDB_X86_EV_REINJECT_BP;

    for (i = 0; i < GDB_X86_NR_HW_BP; i++)
        if (b->hw_used[i] && (ex->dr6 & (1u << i)))
            return GDB_X86_EV_HWBREAK;
    if (stepping && (ex->dr6 & GDB_X86_DR6_BS))
        return GDB_X86_EV_STEP;
    return GDB_X86_EV_REINJECT_DB;
}

/* ------------------------------------------------------------------ */
/* 寄存器换算                                                          */
/* ------------------------------------------------------------------ */

void gdb_x86_regs_from_kvm(struct rsp_regs *r, const struct kvm_regs *kr,
                           const struct kvm_sregs *ks,
                           const struct kvm_fpu *kf)
{
    int i;

    memset(r, 0, sizeof(*r));
    r->gpr[0]  = kr->rax;  r->gpr[1]  = kr->rbx;
    r->gpr[2]  = kr->rcx;  r->gpr[3]  = kr->rdx;
    r->gpr[4]  = kr->rsi;  r->gpr[5]  = kr->rdi;
    r->gpr[6]  = kr->rbp;  r->gpr[7]  = kr->rsp;
    r->gpr[8]  = kr->r8;   r->gpr[9]  = kr->r9;
    r->gpr[10] = kr->r10;  r->gpr[11] = kr->r11;
    r->gpr[12] = kr->r12;  r->gpr[13] = kr->r13;
    r->gpr[14] = kr->r14;  r->gpr[15] = kr->r15;
    r->rip    = kr->rip;
    r->eflags = (uint32_t)kr->rflags;   /* RFLAGS 高 32 位是保留位 */

    r->seg[0] = ks->cs.selector;  r->seg[1] = ks->ss.selector;
    r->seg[2] = ks->ds.selector;  r->seg[3] = ks->es.selector;
    r->seg[4] = ks->fs.selector;  r->seg[5] = ks->gs.selector;
    /* 64 位模式下 fs/gs 的基址取自 MSR，KVM 在 sregs 的影子描述符里给出 */
    r->fs_base = ks->fs.base;
    r->gs_base = ks->gs.base;

    /* FXSAVE 布局里 fpr[i] 是 ST(i)，80 位值占前 10 字节 */
    for (i = 0; i < 8; i++)
        memcpy(r->st[i], kf->fpr[i], sizeof(r->st[i]));
    r->fctrl = kf->fcw;
    r->fstat = kf->fsw;
    r->fop   = kf->last_opcode;
    memcpy(r->xmm, kf->xmm, sizeof(r->xmm));
    r->mxcsr = kf->mxcsr;
}

void gdb_x86_regs_to_kvm(const struct rsp_regs *r, struct kvm_regs *kr)
{
    kr->rax = r->gpr[0];   kr->rbx = r->gpr[1];
    kr->rcx = r->gpr[2];   kr->rdx = r->gpr[3];
    kr->rsi = r->gpr[4];   kr->rdi = r->gpr[5];
    kr->rbp = r->gpr[6];   kr->rsp = r->gpr[7];
    kr->r8  = r->gpr[8];   kr->r9  = r->gpr[9];
    kr->r10 = r->gpr[10];  kr->r11 = r->gpr[11];
    kr->r12 = r->gpr[12];  kr->r13 = r->gpr[13];
    kr->r14 = r->gpr[14];  kr->r15 = r->gpr[15];
    kr->rip    = r->rip;
    kr->rflags = r->eflags;
}
