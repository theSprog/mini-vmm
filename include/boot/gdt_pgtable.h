/* include/boot/gdt_pgtable.h — GDT + 4 级恒等映射页表 + 长模式进入
 *
 * Step 1.2 的全部内容。核心事实：KVM 允许我们“作弊”——
 * 不需要真的在 guest 里执行 lgdt / mov cr0 / jmp far 那套实模式切换代码，
 * 而是通过 KVM_SET_SREGS 直接把 vCPU 的段寄存器影子缓存（segment cache）、
 * CR0/CR3/CR4/EFER 一次性设成“已经处于长模式”的样子，
 * 然后第一条指令就是 64 位代码。这是 Firecracker/kvmtool 的标准做法。
 *
 * 但“作弊”有个前提必须满足：内存里还是要有一份真实的、和 sregs 一致的
 * GDT，并且 GDTR.base/limit 要指向它。因为 guest 后续一旦自己执行
 * lgdt/reload segment（Linux 内核启动早期就会做），硬件会去内存里查表；
 * 如果内存里没有对应表项，立刻 #GP。
 */
#ifndef BOOT_GDT_PGTABLE_H
#define BOOT_GDT_PGTABLE_H

#include <stdint.h>
#include <stdbool.h>

struct vmm_vm;
struct vmm_vcpu;

/* ------------------------------------------------------------------ */
/* Guest 物理地址布局（低 1 MiB 以下的“VMM 私有区”）                    */
/* ------------------------------------------------------------------ */
/* 布局原则：避开 payload（Step 1 放在 0x0，Step 2 vmlinux 放 0x100000），
 * 并且全部页对齐。0x0 处保留一页给 payload/IVT。                        */
#define BOOT_GDT_GPA     0x1000ULL   /* GDT，1 页足够（最多 512 项） */
#define BOOT_PML4_GPA    0x2000ULL
#define BOOT_PDPT_GPA    0x3000ULL
#define BOOT_PD_GPA      0x4000ULL
#define BOOT_PT_GPA      0x5000ULL   /* 仅 4K 粒度模式使用，占 512 页 = 2 MiB */
#define BOOT_PT_END_GPA  (BOOT_PT_GPA + 512ULL * 0x1000)  /* = 0x205000 */

/* 恒等映射范围：1 GiB */
#define BOOT_IDENTITY_MAP_SIZE (1ULL << 30)

/* ------------------------------------------------------------------ */
/* 分页结构项标志位（AMD64 APM Vol.2 §5.3）                             */
/* ------------------------------------------------------------------ */
#define PTE_P     (1ULL << 0)   /* Present */
#define PTE_RW    (1ULL << 1)   /* Writable */
#define PTE_US    (1ULL << 2)   /* User/Supervisor */
#define PTE_PWT   (1ULL << 3)
#define PTE_PCD   (1ULL << 4)
#define PTE_A     (1ULL << 5)   /* Accessed */
#define PTE_D     (1ULL << 6)   /* Dirty（仅末级） */
#define PTE_PS    (1ULL << 7)   /* Page Size：在 PDPE=1GiB页，在 PDE=2MiB页 */
#define PTE_G     (1ULL << 8)   /* Global */
#define PTE_NX    (1ULL << 63)  /* No-Execute，需 EFER.NXE */

#define PTE_ADDR_MASK 0x000ffffffffff000ULL

/* ------------------------------------------------------------------ */
/* CR0 / CR4 / EFER 位                                                 */
/* ------------------------------------------------------------------ */
#define X86_CR0_PE (1ULL << 0)
#define X86_CR0_MP (1ULL << 1)
#define X86_CR0_ET (1ULL << 4)
#define X86_CR0_NE (1ULL << 5)
#define X86_CR0_WP (1ULL << 16)
#define X86_CR0_AM (1ULL << 18)
#define X86_CR0_PG (1ULL << 31)

#define X86_CR4_PAE (1ULL << 5)
#define X86_CR4_PGE (1ULL << 7)

#define X86_EFER_SCE (1ULL << 0)
#define X86_EFER_LME (1ULL << 8)   /* Long Mode Enable */
#define X86_EFER_LMA (1ULL << 10)  /* Long Mode Active（硬件置位，但 KVM
                                    * 走 SET_SREGS 时需要我们一起给上） */
#define X86_EFER_NXE (1ULL << 11)

/* ------------------------------------------------------------------ */
/* GDT 选择子（索引 × 8）                                               */
/* ------------------------------------------------------------------ */
#define BOOT_GDT_ENTRY_NULL  0
#define BOOT_GDT_ENTRY_CODE  1
#define BOOT_GDT_ENTRY_DATA  2
#define BOOT_GDT_ENTRY_TSS   3   /* Step 2 之后 Linux 需要，先占位 */
#define BOOT_GDT_NR_ENTRIES  4

#define BOOT_SEL_CODE (BOOT_GDT_ENTRY_CODE * 8)  /* 0x08 */
#define BOOT_SEL_DATA (BOOT_GDT_ENTRY_DATA * 8)  /* 0x10 */

/* ------------------------------------------------------------------ */
/* 页表粒度                                                            */
/* ------------------------------------------------------------------ */
enum boot_page_gran {
    /* PML4 → PDPT → PD(PS=1)，3 张表共 12 KiB 映射 1 GiB。
     * 生产做法（kvmtool/Firecracker 都是这个）。 */
    BOOT_PG_2M = 0,
    /* PML4 → PDPT → PD → PT，完整四级，1 GiB 需要 512 张 PT（2 MiB）。
     * 慢且费内存，但能真正走一遍四级页表构建，博客原理篇用它讲清楚
     * 每一级 9 位索引是怎么切分的。 */
    BOOT_PG_4K,
};

struct boot_mem_layout {
    uint64_t gdt_gpa;
    uint64_t pml4_gpa;
    uint64_t pdpt_gpa;
    uint64_t pd_gpa;
    uint64_t pt_gpa;            /* 仅 BOOT_PG_4K 使用 */
    uint64_t map_size;          /* 恒等映射多大，须 2 MiB 对齐 */
    enum boot_page_gran gran;
};

/* 填入默认布局（上面那堆 BOOT_*_GPA + 1GiB + 2M 粒度） */
void boot_layout_default(struct boot_mem_layout *l);

/* 校验布局是否自洽（不重叠、页对齐、RAM 够大） */
int  boot_layout_validate(struct vmm_vm *vm, const struct boot_mem_layout *l);

/* ------------------------------------------------------------------ */
/* 构建：往 guest 物理内存里写表                                        */
/* ------------------------------------------------------------------ */

/* 在 l->pml4_gpa 等处构建恒等映射页表 */
int  boot_build_page_table(struct vmm_vm *vm, const struct boot_mem_layout *l);

/* 在 l->gdt_gpa 处写入 GDT（null / 64 位 code / 64 位 data / TSS 占位） */
int  boot_build_gdt(struct vmm_vm *vm, const struct boot_mem_layout *l);

/* ------------------------------------------------------------------ */
/* 应用：设置 vCPU 寄存器                                               */
/* ------------------------------------------------------------------ */

/* Step 1.1：把 vCPU 摆成 16 位实模式，CS.base=0，CS.selector=0，RIP=entry。
 * 必须显式覆盖 KVM 的 reset 值（CS.base=0xffff0000, RIP=0xfff0）。 */
int  boot_setup_real_mode(struct vmm_vcpu *vcpu, uint64_t entry_gpa);

/* Step 1.2：设置 CR0(PE|PG)、CR3=pml4、CR4(PAE)、EFER(LME|LMA)，
 * 段寄存器按 64 位平坦模型填，GDTR 指向 l->gdt_gpa，RIP=entry。
 * 调用前 boot_build_page_table / boot_build_gdt 必须已经执行。 */
int  boot_setup_long_mode(struct vmm_vcpu *vcpu,
                          const struct boot_mem_layout *l,
                          uint64_t entry_gpa);

/* 调试：把已构建的页表按四级走一遍，打印 GPA 的翻译路径。
 * 排“写进去了但地址不对”这类 bug 时是救命工具。 */
void boot_dump_page_walk(struct vmm_vm *vm, const struct boot_mem_layout *l,
                         uint64_t gva);

#endif /* BOOT_GDT_PGTABLE_H */
