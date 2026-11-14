/* include/gdb/x86.h — gdbstub 里和 x86 / KVM 数据结构相关、但不发 ioctl 的部分
 *
 * 断点表、DR7 编码、KVM_EXIT_DEBUG 的归类、kvm_regs 与 gdb 寄存器布局的
 * 换算。这些都是纯函数，从 gdbstub.c 里拆出来是为了能在单元测试里直接
 * 断言，不必起一台 VM。
 *
 * 依据：
 *   - Intel SDM Vol.3 的 Debug Registers 一节：DR7 的 L0..L3、R/W0..3、
 *     LEN0..3 字段，DR6 的 B0..B3、BS 位。位的取值和 Linux 的
 *     arch/x86/include/uapi/asm/debugreg.h（DR_TRAP0..3、DR_STEP、
 *     DR_LOCAL_ENABLE、DR_CONTROL_SHIFT）以及 KVM 的 DR7_FIXED_1
 *     （arch/x86/include/asm/kvm_host.h）一致，换 SDM 版本时以这两个头文件
 *     核对
 *   - linux/kvm.h、asm/kvm.h：struct kvm_guest_debug、kvm_debug_exit_arch、
 *     KVM_GUESTDBG_*
 *   - KVM 的实现（linux-6.6 arch/x86/kvm/svm/svm.c bp_interception()、
 *     db_interception()）：#BP 退出时 debug.arch.pc 是 int3 指令本身的地址，
 *     #DB 退出时带回 dr6
 */
#ifndef GDB_X86_H
#define GDB_X86_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <linux/kvm.h>

#include "gdb/rsp.h"

#define GDB_X86_INT3        0xCC
#define GDB_X86_NR_HW_BP    4       /* DR0..DR3 */
/* 软件断点个数上限。gdb 每个断点位置插一次，内核调试时同时存在的断点
 * 一般是个位数，64 足够，满了返回错误而不是动态扩容，表就能放在结构体里 */
#define GDB_X86_MAX_SW_BP   64

#define GDB_X86_BP_VECTOR   3
#define GDB_X86_DB_VECTOR   1

/* DR6 / DR7 位：asm/debugreg.h 的 DR_STEP，kvm_host.h 的 DR7_FIXED_1 */
#define GDB_X86_DR6_BS          (1u << 14)  /* 单步陷阱 */
#define GDB_X86_DR7_FIXED_1     (1u << 10)  /* 保留位，读出恒为 1 */

struct gdb_x86_sw_bp {
    uint64_t gva;       /* gdb 给的地址，#BP 退出时按它匹配 pc */
    uint64_t gpa;       /* 插入时翻译出的物理地址，撤销和遮蔽都按它 */
    uint8_t  orig;      /* 被 int3 覆盖的原字节 */
};

/*
 * 断点表由 gdbstub 的锁保护。软件断点按 GVA 识别，地址翻译只在插入时做
 * 一次：撤销时直接写回记下的 GPA，因为撤销时 guest 页表可能已经变了
 * （不同进程的同一 GVA 指向不同物理页），重新翻译可能写到别的页上。
 */
struct gdb_x86_bps {
    struct gdb_x86_sw_bp sw[GDB_X86_MAX_SW_BP];
    int      nr_sw;
    uint64_t hw[GDB_X86_NR_HW_BP];
    bool     hw_used[GDB_X86_NR_HW_BP];
};

/* 软件断点。add 对已存在的 GVA 返回 0 且不改表，表满返回 -ENOSPC；
 * del 返回被删条目的副本，地址不在表里返回 -ENOENT。 */
int  gdb_x86_sw_find(const struct gdb_x86_bps *b, uint64_t gva);
int  gdb_x86_sw_add(struct gdb_x86_bps *b, uint64_t gva, uint64_t gpa,
                    uint8_t orig);
int  gdb_x86_sw_del(struct gdb_x86_bps *b, uint64_t gva,
                    struct gdb_x86_sw_bp *out);

/* 读内存时把 [gpa, gpa+len) 里被 int3 覆盖的字节换回原字节，gdb 看到的
 * 是没插断点时的内存。 */
void gdb_x86_sw_hide(const struct gdb_x86_bps *b, uint64_t gpa,
                     uint8_t *buf, size_t len);
/* 写内存前调用：buf 里落在断点上的字节记成新的原字节，buf 里换成 int3，
 * 写下去之后断点仍然有效，撤销时恢复的是 gdb 写的新值。 */
void gdb_x86_sw_shadow_write(struct gdb_x86_bps *b, uint64_t gpa,
                             uint8_t *buf, size_t len);

/* 硬件断点。add 返回槽号，已存在时返回原槽号，槽满返回 -ENOSPC。 */
int  gdb_x86_hw_add(struct gdb_x86_bps *b, uint64_t addr);
int  gdb_x86_hw_del(struct gdb_x86_bps *b, uint64_t addr);
/* 由占用的槽算出 DR7：每个槽置局部使能位 Ln，R/Wn = 00（指令执行），
 * LENn = 00（指令断点的长度必须编码成 00；Linux 的 hw_breakpoint.c 对执行
 * 断点用 X86_BREAKPOINT_LEN_X，编码进 DR7 时 & 0xf 后也是 00） */
uint64_t gdb_x86_dr7(const struct gdb_x86_bps *b);

/*
 * 给一个 vCPU 准备 KVM_SET_GUEST_DEBUG 的参数。
 *   有软件断点     ENABLE | USE_SW_BP：所有 #BP 都退出到用户态
 *   有硬件断点     ENABLE | USE_HW_BP，debugreg[0..3] 和 [7]
 *   step           ENABLE | SINGLESTEP，加上 extra（KVM 支持时传
 *                  KVM_GUESTDBG_BLOCKIRQ，单步时不注入中断）
 * 三者都没有时 control = 0，KVM 关闭 guest debug。
 */
void gdb_x86_guest_debug(const struct gdb_x86_bps *b, bool step,
                         uint32_t step_extra, struct kvm_guest_debug *dbg);

enum gdb_x86_event {
    GDB_X86_EV_SWBREAK,     /* 撞上我们插的 int3 */
    GDB_X86_EV_HWBREAK,     /* DR0..DR3 之一命中 */
    GDB_X86_EV_STEP,        /* 单步完成 */
    GDB_X86_EV_REINJECT_BP, /* guest 自己的 int3，原样还给 guest */
    GDB_X86_EV_REINJECT_DB, /* 不是我们引起的 #DB，原样还给 guest */
};

/* stepping：这个 vCPU 这次是带 SINGLESTEP 恢复的 */
enum gdb_x86_event gdb_x86_classify(const struct gdb_x86_bps *b,
                                    const struct kvm_debug_exit_arch *ex,
                                    bool stepping);

/* kvm_regs/sregs/fpu → gdb 寄存器布局；反方向只换算可写的 gpr、rip、eflags */
void gdb_x86_regs_from_kvm(struct rsp_regs *r, const struct kvm_regs *kr,
                           const struct kvm_sregs *ks,
                           const struct kvm_fpu *kf);
void gdb_x86_regs_to_kvm(const struct rsp_regs *r, struct kvm_regs *kr);

#endif /* GDB_X86_H */
