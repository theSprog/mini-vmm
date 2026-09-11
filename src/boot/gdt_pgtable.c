/* src/boot/gdt_pgtable.c — GDT、4 级恒等映射页表、长模式进入 */
#include <stdio.h>
#include <string.h>

#include "vmm.h"
#include "memory.h"
#include "kvm_wrappers.h"
#include "boot/gdt_pgtable.h"

/* ------------------------------------------------------------------ */
/* GDT 描述符                                                          */
/* ------------------------------------------------------------------ */
/*
 * 64 位模式下的段描述符几乎所有字段都被硬件忽略（base 强制为 0，
 * limit 不检查），但 L、D/B、S、P、DPL、type 这几位仍然有效，
 * 而且必须自洽，否则 far jump / 段加载会 #GP。
 *
 *   code: type=0xB(执行/可读/已访问) S=1 DPL=0 P=1 L=1 D=0 G=1 limit=0xFFFFF
 *   data: type=0x3(读写/已访问)      S=1 DPL=0 P=1 L=0 D=1 G=1 limit=0xFFFFF
 *
 * 注意 code 段 L=1 时 D 必须为 0，L=1&&D=1 是保留组合，会 #GP。
 */
#define GDT_DESC_CODE64 0x00AF9B000000FFFFULL
#define GDT_DESC_DATA64 0x00CF93000000FFFFULL
#define GDT_DESC_NULL   0x0000000000000000ULL

/*
 * 从 GDT 描述符反解出 struct kvm_segment。
 *
 * 之所以要"反解"而不是两边各写一份常量：sregs 里的段影子缓存和内存里的
 * GDT 必须严格一致。手写两份常量，一旦改了一处忘了另一处，症状是
 * guest 跑到某个 reload 段寄存器的地方才崩，极难定位。
 *
 * 一个关键坑：kvm_segment.limit 存的是**已按 G 位缩放后的有效界限**，
 * 不是描述符里那个 20 位原始字段。KVM 内部 emulator_get_segment() 里
 * 有一行 `if (var.g) var.limit >>= 12;` 反向印证了这一点。
 * 所以 G=1 时这里要做 (raw << 12) | 0xFFF。
 */
static void seg_from_desc(struct kvm_segment *s, uint64_t d, uint16_t sel)
{
    uint32_t limit_raw;

    memset(s, 0, sizeof(*s));
    s->selector = sel;
    s->base = ((d >> 16) & 0xFFFFFF) | (((d >> 56) & 0xFF) << 24);

    limit_raw = (uint32_t)((d & 0xFFFF) | (((d >> 48) & 0xF) << 16));

    s->type    = (d >> 40) & 0xF;
    s->s       = (d >> 44) & 0x1;
    s->dpl     = (d >> 45) & 0x3;
    s->present = (d >> 47) & 0x1;
    s->avl     = (d >> 52) & 0x1;
    s->l       = (d >> 53) & 0x1;
    s->db      = (d >> 54) & 0x1;
    s->g       = (d >> 55) & 0x1;

    s->limit = s->g ? ((limit_raw << 12) | 0xFFF) : limit_raw;
    s->unusable = 0;
}

void boot_layout_default(struct boot_mem_layout *l)
{
    memset(l, 0, sizeof(*l));
    l->gdt_gpa  = BOOT_GDT_GPA;
    l->pml4_gpa = BOOT_PML4_GPA;
    l->pdpt_gpa = BOOT_PDPT_GPA;
    l->pd_gpa   = BOOT_PD_GPA;
    l->pt_gpa   = BOOT_PT_GPA;
    l->map_size = BOOT_IDENTITY_MAP_SIZE;
    l->gran     = BOOT_PG_2M;
}

int boot_layout_validate(struct vmm_vm *vm, const struct boot_mem_layout *l)
{
    uint64_t need_top;

    if (l->map_size & ((2ULL << 20) - 1)) {
        vmm_err("map_size 0x%llx is not 2MiB aligned",
                (unsigned long long)l->map_size);
        return VMM_ERR_INVAL;
    }
    /* 单张 PDPT 只有 512 项，每项覆盖 1GiB */
    if (l->map_size > (512ULL << 30)) {
        vmm_err("map_size exceeds the 512GiB a single PDPT can cover");
        return VMM_ERR_INVAL;
    }

    if (l->gran == BOOT_PG_4K) {
        uint64_t nr_pt = l->map_size / (2ULL << 20);
        need_top = l->pt_gpa + nr_pt * 0x1000;
    } else {
        need_top = l->pd_gpa + 0x1000;
    }

    if (!gpa_to_hva(vm, l->gdt_gpa, need_top - l->gdt_gpa)) {
        vmm_err("page table / GDT area [0x%llx, 0x%llx) exceeds "
                "guest RAM, increase --mem",
                (unsigned long long)l->gdt_gpa,
                (unsigned long long)need_top);
        return VMM_ERR_INVAL;
    }

    /* 恒等映射范围超过实际 RAM 是允许的：访问超出部分的 GPA 不会
     * 产生 #PF（页表里有映射），而是 NPT violation -> KVM_EXIT_MMIO。
     * 这正是后面把 virtio-mmio 挂到 0xd0000000 的前提。 */
    if (l->map_size > vm->mem.ram_size)
        vmm_dbg("identity map %llu MiB > RAM %llu MiB; accesses "
                "beyond RAM will produce KVM_EXIT_MMIO",
                (unsigned long long)(l->map_size >> 20),
                (unsigned long long)(vm->mem.ram_size >> 20));

    return VMM_OK;
}

int boot_build_gdt(struct vmm_vm *vm, const struct boot_mem_layout *l)
{
    uint64_t gdt[BOOT_GDT_NR_ENTRIES];

    memset(gdt, 0, sizeof(gdt));
    gdt[BOOT_GDT_ENTRY_NULL] = GDT_DESC_NULL;
    gdt[BOOT_GDT_ENTRY_CODE] = GDT_DESC_CODE64;
    gdt[BOOT_GDT_ENTRY_DATA] = GDT_DESC_DATA64;
    gdt[BOOT_GDT_ENTRY_TSS]  = 0;   /* Step 2 再填 */

    if (mem_write(vm, l->gdt_gpa, gdt, sizeof(gdt)) != VMM_OK)
        return VMM_ERR_INVAL;

    vmm_info("GDT @ GPA 0x%llx: %d entries (CS=0x%02x DS=0x%02x)",
             (unsigned long long)l->gdt_gpa, BOOT_GDT_NR_ENTRIES,
             BOOT_SEL_CODE, BOOT_SEL_DATA);
    return VMM_OK;
}

/*
 * 恒等映射（identity mapping）：让 GVA == GPA。
 *
 * 为什么必须建这个？因为一旦置上 CR0.PG，CPU 取指和取数用的地址就都
 * 是虚拟地址了，要过 CR3 指向的页表。如果不建映射，第一条指令的取指
 * 就会 #PF；而我们又没装 IDT，#PF 处理不了就变 #DF，再不行就 triple
 * fault，最终以 KVM_EXIT_SHUTDOWN 收场。恒等映射是让"开分页"这件事
 * 对已有代码完全透明的最简单办法。
 */
int boot_build_page_table(struct vmm_vm *vm, const struct boot_mem_layout *l)
{
    uint64_t *pml4, *pdpt, *pd;
    uint64_t nr_1g, nr_2m, i;

    pml4 = gpa_to_hva(vm, l->pml4_gpa, 0x1000);
    pdpt = gpa_to_hva(vm, l->pdpt_gpa, 0x1000);
    pd   = gpa_to_hva(vm, l->pd_gpa,   0x1000);
    if (!pml4 || !pdpt || !pd)
        return VMM_ERR_INVAL;

    memset(pml4, 0, 0x1000);
    memset(pdpt, 0, 0x1000);
    memset(pd,   0, 0x1000);

    /* 48 位线性地址切分：[47:39] PML4E | [38:30] PDPE | [29:21] PDE
     *                    | [20:12] PTE  | [11:0] 页内偏移
     * 每级 9 位 = 512 项，每项 8 字节 = 正好 4KiB 一页。 */

    nr_1g = (l->map_size + ((1ULL << 30) - 1)) >> 30;
    if (nr_1g > 512)
        return VMM_ERR_INVAL;
    if (nr_1g > 1) {
        vmm_err("only <=1GiB identity map is supported (more needs multiple PDs)");
        return VMM_ERR_UNSUPPORTED;
    }

    pml4[0] = (l->pdpt_gpa & PTE_ADDR_MASK) | PTE_P | PTE_RW;
    pdpt[0] = (l->pd_gpa   & PTE_ADDR_MASK) | PTE_P | PTE_RW;

    nr_2m = l->map_size >> 21;

    if (l->gran == BOOT_PG_2M) {
        /* PDE 置 PS=1，这一项就直接是一个 2MiB 页，不再往下走 PT。
         * 三张表 12KiB 覆盖 1GiB。 */
        for (i = 0; i < nr_2m; i++)
            pd[i] = ((i << 21) & PTE_ADDR_MASK)
                    | PTE_P | PTE_RW | PTE_PS;

        vmm_info("page tables @ PML4=0x%llx PDPT=0x%llx PD=0x%llx: "
                 "identity map %llu MiB via %llu 2MiB pages",
                 (unsigned long long)l->pml4_gpa,
                 (unsigned long long)l->pdpt_gpa,
                 (unsigned long long)l->pd_gpa,
                 (unsigned long long)(l->map_size >> 20),
                 (unsigned long long)nr_2m);
    } else {
        /* 完整四级：每个 PDE 指向一张 PT，每张 PT 512 项 × 4KiB。
         * 1GiB 需要 512 张 PT = 2MiB 页表内存，比大页方案多 170 倍。
         * 生产环境没人这么干，这里保留是为了把四级走通看清楚。 */
        for (i = 0; i < nr_2m; i++) {
            uint64_t pt_gpa = l->pt_gpa + i * 0x1000;
            uint64_t *pt = gpa_to_hva(vm, pt_gpa, 0x1000);
            uint64_t j;

            if (!pt) {
                vmm_err("PT #%llu @ 0x%llx exceeds guest RAM",
                        (unsigned long long)i,
                        (unsigned long long)pt_gpa);
                return VMM_ERR_INVAL;
            }
            for (j = 0; j < 512; j++)
                pt[j] = (((i << 21) | (j << 12)) & PTE_ADDR_MASK)
                        | PTE_P | PTE_RW;

            pd[i] = (pt_gpa & PTE_ADDR_MASK) | PTE_P | PTE_RW;
        }

        vmm_info("page tables @ PML4=0x%llx PDPT=0x%llx PD=0x%llx "
                 "PT[%llu]=0x%llx..0x%llx: identity map %llu MiB, "
                 "4KiB granularity",
                 (unsigned long long)l->pml4_gpa,
                 (unsigned long long)l->pdpt_gpa,
                 (unsigned long long)l->pd_gpa,
                 (unsigned long long)nr_2m,
                 (unsigned long long)l->pt_gpa,
                 (unsigned long long)(l->pt_gpa + nr_2m * 0x1000),
                 (unsigned long long)(l->map_size >> 20));
    }

    return VMM_OK;
}

int boot_setup_real_mode(struct vmm_vcpu *vcpu, uint64_t entry_gpa)
{
    struct kvm_sregs sregs;
    struct kvm_regs regs;
    int r;

    if (entry_gpa > 0xFFFF) {
        vmm_err("real mode entry 0x%llx exceeds 64KiB (IP is 16-bit when CS.base=0)",
                (unsigned long long)entry_gpa);
        return VMM_ERR_INVAL;
    }

    r = kvm_get_sregs(vcpu->fd, &sregs);
    if (r != VMM_OK)
        return r;

    /* KVM 给 vCPU 的 reset 值模拟真实 x86 上电状态：
     *   CS.selector = 0xF000, CS.base = 0xFFFF0000, RIP = 0xFFF0
     * 也就是 reset vector 0xFFFFFFF0，那里在真机上是 BIOS ROM。
     * 我们没有 BIOS，所以直接把 CS.base 和 selector 清零，
     * 让 CS:IP 落在 GPA 0 附近。
     *
     * 除此之外的字段（limit=0xFFFF, type=11, s=1, present=1, g=0）
     * reset 值本身就是对的，不动它们比自己重填更不容易出错。 */
    sregs.cs.selector = 0;
    sregs.cs.base     = 0;

    r = kvm_set_sregs(vcpu->fd, &sregs);
    if (r != VMM_OK)
        return r;

    memset(&regs, 0, sizeof(regs));
    regs.rip = entry_gpa;
    /* RFLAGS 的 bit1 是保留位且恒为 1，写 0 会被 KVM 拒绝 */
    regs.rflags = 0x2;
    regs.rsp = 0x8000;   /* 实模式栈随便找个不冲突的地方 */

    r = kvm_set_regs(vcpu->fd, &regs);
    if (r != VMM_OK)
        return r;

    vmm_info("vCPU %d: 16-bit real mode, CS:IP = 0000:%04llx",
             vcpu->id, (unsigned long long)entry_gpa);
    return VMM_OK;
}

/* 长模式公共部分：选择子和 RSI 由调用者决定 */
static int setup_long_mode_common(struct vmm_vcpu *vcpu,
                                  const struct boot_mem_layout *l,
                                  uint64_t entry_gpa, uint16_t sel_code,
                                  uint16_t sel_data, uint16_t gdt_limit,
                                  uint64_t rsi)
{
    struct kvm_sregs sregs;
    struct kvm_regs regs;
    struct kvm_segment code, data;
    int r;

    r = kvm_get_sregs(vcpu->fd, &sregs);
    if (r != VMM_OK)
        return r;

    /*
     * 真机上从实模式进长模式要按顺序做：设 CR4.PAE -> 填 CR3 ->
     * 置 EFER.LME -> 置 CR0.PG -> far jump 到 64 位代码段。硬件在
     * CR0.PG 置位那一刻自动把 EFER.LMA 设成 1。
     *
     * 但通过 KVM_SET_SREGS，我们是在 vCPU 停止时直接改它的架构状态，
     * 不经过硬件的状态转换逻辑，所以 LMA 得我们自己一起给上。
     * KVM 的 kvm_is_valid_sregs() 会校验这组值是否自洽：
     * EFER.LME && CR0.PG 时，必须同时有 CR4.PAE、EFER.LMA、CR0.PE，
     * 少一个就 EINVAL。
     */
    sregs.cr0  = X86_CR0_PE | X86_CR0_MP | X86_CR0_ET | X86_CR0_NE
               | X86_CR0_WP | X86_CR0_AM | X86_CR0_PG;
    sregs.cr3  = l->pml4_gpa;
    sregs.cr4  = X86_CR4_PAE;
    sregs.efer = X86_EFER_LME | X86_EFER_LMA;

    /* GDTR 必须指向我们刚写进内存的那张表。
     * 严格说这一步对"第一条指令能不能跑"没影响——段影子缓存已经
     * 由下面的 sregs.cs/ds 直接给定了。但 guest 一旦执行 lgdt 之外的
     * 任何段加载（Linux 内核早期一定会），硬件就会真的去查这张表。 */
    sregs.gdt.base  = l->gdt_gpa;
    sregs.gdt.limit = gdt_limit;

    /* 不装 IDT：Step 1.2 的 payload 不该产生任何异常。
     * 万一产生了，limit=0 会导致 #GP -> #DF -> triple fault，
     * 表现为 KVM_EXIT_SHUTDOWN。这其实是个好事——出错立刻可见，
     * 而不是静默地跑飞。 */
    sregs.idt.base  = 0;
    sregs.idt.limit = 0;

    seg_from_desc(&code, GDT_DESC_CODE64, sel_code);
    seg_from_desc(&data, GDT_DESC_DATA64, sel_data);

    sregs.cs = code;
    sregs.ds = sregs.es = sregs.fs = sregs.gs = sregs.ss = data;

    r = kvm_set_sregs(vcpu->fd, &sregs);
    if (r != VMM_OK) {
        vmm_err("KVM_SET_SREGS failed: check CR0/CR4/EFER consistency");
        return r;
    }

    memset(&regs, 0, sizeof(regs));
    regs.rip    = entry_gpa;
    regs.rflags = 0x2;
    regs.rsp    = 0x200000;   /* 2MiB 处向下growing，够 payload 用 */
    regs.rsi    = rsi;

    r = kvm_set_regs(vcpu->fd, &regs);
    if (r != VMM_OK)
        return r;

    vmm_info("vCPU %d: 64-bit long mode, CR3=0x%llx RIP=0x%llx",
             vcpu->id, (unsigned long long)sregs.cr3,
             (unsigned long long)entry_gpa);
    return VMM_OK;
}

int boot_setup_long_mode(struct vmm_vcpu *vcpu,
                         const struct boot_mem_layout *l,
                         uint64_t entry_gpa)
{
    return setup_long_mode_common(vcpu, l, entry_gpa,
                                  BOOT_SEL_CODE, BOOT_SEL_DATA,
                                  BOOT_GDT_NR_ENTRIES * 8 - 1, 0);
}

int boot_setup_linux64(struct vmm_vcpu *vcpu,
                       const struct boot_mem_layout *l,
                       uint64_t entry_gpa, uint64_t boot_params_gpa)
{
    uint64_t gdt[4] = {
        GDT_DESC_NULL,
        GDT_DESC_NULL,
        GDT_DESC_CODE64,    /* 0x10 = __BOOT_CS */
        GDT_DESC_DATA64,    /* 0x18 = __BOOT_DS */
    };
    int r;

    /* 内核 startup_64 很快会 lgdt 自己的 GDT，这张表只需要撑到那一刻；
     * 但在那之前如果有任何段重载，选择子必须对得上，所以还是按协议给。 */
    if (mem_write(vcpu->vm, l->gdt_gpa, gdt, sizeof(gdt)) != VMM_OK)
        return VMM_ERR_INVAL;

    r = setup_long_mode_common(vcpu, l, entry_gpa,
                               BOOT_LINUX_SEL_CODE, BOOT_LINUX_SEL_DATA,
                               sizeof(gdt) - 1, boot_params_gpa);
    if (r == VMM_OK)
        vmm_info("vCPU %d: Linux boot protocol, CS=0x%02x DS=0x%02x RSI=0x%llx",
                 vcpu->id, BOOT_LINUX_SEL_CODE, BOOT_LINUX_SEL_DATA,
                 (unsigned long long)boot_params_gpa);
    return r;
}

void boot_dump_page_walk(struct vmm_vm *vm, const struct boot_mem_layout *l,
                         uint64_t gva)
{
    uint64_t *tbl;
    uint64_t e, next = l->pml4_gpa;
    const char *names[] = { "PML4E", "PDPE", "PDE", "PTE" };
    int shift[] = { 39, 30, 21, 12 };
    int lvl;

    fprintf(stderr, "page walk for GVA 0x%llx (CR3=0x%llx):\n",
            (unsigned long long)gva, (unsigned long long)l->pml4_gpa);

    for (lvl = 0; lvl < 4; lvl++) {
        uint64_t idx = (gva >> shift[lvl]) & 0x1FF;

        tbl = gpa_to_hva(vm, next, 0x1000);
        if (!tbl) {
            fprintf(stderr, "  %s: table @ 0x%llx is not accessible\n",
                    names[lvl], (unsigned long long)next);
            return;
        }
        e = tbl[idx];
        fprintf(stderr, "  %-5s [%3llu] @ 0x%llx = 0x%016llx  %s%s%s\n",
                names[lvl], (unsigned long long)idx,
                (unsigned long long)next, (unsigned long long)e,
                (e & PTE_P)  ? "P "  : "!P ",
                (e & PTE_RW) ? "RW " : "RO ",
                (e & PTE_PS) ? "PS"  : "");

        if (!(e & PTE_P))
            return;

        /* PS=1 时这一级就是末级，直接算出物理地址 */
        if ((lvl == 1 || lvl == 2) && (e & PTE_PS)) {
            uint64_t mask = (1ULL << shift[lvl]) - 1;
            fprintf(stderr, "  => GPA 0x%llx (large page)\n",
                    (unsigned long long)((e & PTE_ADDR_MASK & ~mask)
                                         | (gva & mask)));
            return;
        }
        if (lvl == 3) {
            fprintf(stderr, "  => GPA 0x%llx\n",
                    (unsigned long long)((e & PTE_ADDR_MASK)
                                         | (gva & 0xFFF)));
            return;
        }
        next = e & PTE_ADDR_MASK;
    }
}
