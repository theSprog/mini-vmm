/* src/boot/payload.c — 裸 payload 装载器，约定见 include/boot/payload.h */
#include <stdint.h>

#include "vmm.h"
#include "memory.h"
#include "boot/gdt_pgtable.h"
#include "boot/payload.h"

/* 检查 payload 是否和 GDT/页表区域打架 */
static int check_payload_overlap(const struct boot_mem_layout *l,
                                 uint64_t gpa, size_t len)
{
    uint64_t boot_lo = l->gdt_gpa;
    uint64_t boot_hi = (l->gran == BOOT_PG_4K)
                     ? l->pt_gpa + (l->map_size / (2ULL << 20)) * 0x1000
                     : l->pd_gpa + 0x1000;

    if (len == 0)
        return VMM_OK;
    if (gpa < boot_hi && boot_lo < gpa + len) {
        vmm_err("payload [0x%llx, 0x%llx) overlaps GDT/page table area "
                "[0x%llx, 0x%llx)",
                (unsigned long long)gpa,
                (unsigned long long)(gpa + len),
                (unsigned long long)boot_lo,
                (unsigned long long)boot_hi);
        vmm_err("  use --load to move the payload above 0x%llx",
                (unsigned long long)boot_hi);
        return VMM_ERR_INVAL;
    }
    return VMM_OK;
}

int payload_boot(struct vmm_vm *vm, const struct vmm_config *cfg)
{
    struct vmm_vcpu *vcpu = &vm->vcpus[0];
    struct boot_mem_layout layout;
    size_t payload_len = 0;
    int r;

    r = mem_load_file(vm, cfg->payload_gpa, cfg->payload_path, &payload_len);
    if (r != VMM_OK)
        return r;

    if (cfg->mode == VMM_BOOT_REAL16)
        return boot_setup_real_mode(vcpu, cfg->entry_gpa);

    boot_layout_default(&layout);
    layout.gran     = cfg->pt_gran;
    layout.map_size = cfg->map_size;

    r = boot_layout_validate(vm, &layout);
    if (r != VMM_OK)
        return r;

    /* payload 已经载入内存了，页表马上要往同一片 RAM 里写。重叠的话页表
     * 会把 payload 覆盖掉，症状是 guest 一启动就执行到页表数据当指令，
     * 报 INTERNAL_ERROR/EMULATION，非常难猜。这里提前拦下来。 */
    r = check_payload_overlap(&layout, cfg->payload_gpa, payload_len);
    if (r != VMM_OK)
        return r;

    r = boot_build_page_table(vm, &layout);
    if (r != VMM_OK)
        return r;
    r = boot_build_gdt(vm, &layout);
    if (r != VMM_OK)
        return r;

    if (cfg->walk_gva != UINT64_MAX)
        boot_dump_page_walk(vm, &layout, cfg->walk_gva);

    return boot_setup_long_mode(vcpu, &layout, cfg->entry_gpa);
}
