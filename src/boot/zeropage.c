/* src/boot/zeropage.c — 构造 boot_params / E820 内存图 / initrd  [linux-boot] */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <asm/bootparam.h>

#include "vmm.h"
#include "memory.h"
#include "boot/zeropage.h"

#define E820_TYPE_RAM      1
#define E820_TYPE_RESERVED 2

#define ALIGN_DOWN(x, a) ((x) & ~((uint64_t)(a) - 1))

static void e820_add(struct boot_params *bp, uint64_t addr, uint64_t size,
                     uint32_t type)
{
    struct boot_e820_entry *e = &bp->e820_table[bp->e820_entries++];

    e->addr = addr;
    e->size = size;
    e->type = type;
    vmm_info("  e820: [0x%012llx, 0x%012llx) %s",
             (unsigned long long)addr, (unsigned long long)(addr + size),
             type == E820_TYPE_RAM ? "RAM" : "reserved");
}

/* initrd 摆在低于 initrd_addr_max 的 RAM 最高处，按 2MiB 对齐往下取。
 * 放高处的理由：内核解压、建直接映射、分配早期页表都从低处往上用，
 * initrd 在顶上不容易被踩。 */
static int load_initrd(struct vmm_vm *vm, struct boot_params *bp,
                       const struct zp_config *zc)
{
    const struct vmm_mem_slot *low;
    struct stat st;
    uint64_t top, gpa, limit;
    size_t len;
    int r;

    if (stat(zc->initrd_path, &st) < 0) {
        vmm_err("stat %s: %s", zc->initrd_path, strerror(errno));
        return VMM_ERR_SYS;
    }

    /* initrd_addr_max 是 32 位字段，所以 initrd 总在 hole 以下的低段 RAM 里 */
    low = mem_find_slot(vm, 0);
    if (!low) {
        vmm_err("no RAM at GPA 0");
        return VMM_ERR_INVAL;
    }
    top = low->gpa + low->size;
    limit = zc->initrd_addr_max ? zc->initrd_addr_max : ZP_INITRD_ADDR_MAX;
    if (top > limit + 1)
        top = limit + 1;
    if ((uint64_t)st.st_size > top) {
        vmm_err("initrd %s (%lld bytes) is larger than usable RAM",
                zc->initrd_path, (long long)st.st_size);
        return VMM_ERR_INVAL;
    }
    gpa = ALIGN_DOWN(top - (uint64_t)st.st_size, 2ULL << 20);
    if (gpa < zc->kernel_end) {
        vmm_err("initrd [0x%llx, +0x%llx) overlaps the kernel image "
                "(ends at 0x%llx), increase --mem",
                (unsigned long long)gpa, (unsigned long long)st.st_size,
                (unsigned long long)zc->kernel_end);
        return VMM_ERR_INVAL;
    }

    r = mem_load_file(vm, gpa, zc->initrd_path, &len);
    if (r != VMM_OK)
        return r;

    bp->hdr.ramdisk_image = (uint32_t)gpa;
    bp->hdr.ramdisk_size  = (uint32_t)len;
    bp->ext_ramdisk_image = (uint32_t)(gpa >> 32);
    bp->ext_ramdisk_size  = (uint32_t)((uint64_t)len >> 32);
    vmm_info("initrd @ GPA 0x%llx, %zu bytes", (unsigned long long)gpa, len);
    return VMM_OK;
}

int zeropage_setup(struct vmm_vm *vm, const struct zp_config *zc)
{
    const struct vmm_mem_slot *s;
    struct boot_params *bp;
    enum gpa_status gs;
    size_t cl_len;

    s = mem_find_slot(vm, 0);
    if (!s || !s->ram || s->size <= ZP_HIGH_MEM_START) {
        vmm_err("Linux needs more than 1MiB of RAM starting at GPA 0");
        return VMM_ERR_INVAL;
    }

    gs = gpa_to_hva(vm, ZP_BOOT_PARAMS_GPA, sizeof(*bp), (void **)&bp);
    if (gs != GPA_OK) {
        vmm_err("boot_params at GPA 0x%llx: %s",
                (unsigned long long)ZP_BOOT_PARAMS_GPA, gpa_status_str(gs));
        return VMM_ERR_INVAL;
    }
    memset(bp, 0, sizeof(*bp));

    if (zc->kernel_hdr) {
        /* bzImage：boot.rst 要求把内核自带的 setup header 原样拷进清零过的
         * boot_params，内核声明的字段（version、initrd_addr_max、
         * cmdline_size、init_size……）一个都不改，下面只填 loader 负责的。 */
        if (zc->kernel_hdr_len > sizeof(bp->hdr)) {
            vmm_err("setup header is %zu bytes, larger than boot_params.hdr",
                    zc->kernel_hdr_len);
            return VMM_ERR_INVAL;
        }
        memcpy(&bp->hdr, zc->kernel_hdr, zc->kernel_hdr_len);
        bp->hdr.loadflags |= LOADED_HIGH;
    } else {
        /* vmlinux：没有 setup header，伪造一份。内核本体（跳过解压器直接进
         * startup_64 时）真正读的只有 version、type_of_loader（为 0 时
         * initrd 被忽略）、loadflags 的 KASLR_FLAG 位、cmd_line_ptr、
         * ramdisk_*、hardware_subarch、setup_data；boot_flag、header、
         * kernel_alignment、initrd_addr_max 只是让这份 boot_params 在形式上
         * 完整，方便工具检查。 */
        bp->hdr.boot_flag      = 0xAA55;
        bp->hdr.header         = 0x53726448;    /* "HdrS" */
        bp->hdr.version        = 0x020f;
        bp->hdr.loadflags      = LOADED_HIGH;   /* 内核在 1MiB 以上 */
        bp->hdr.kernel_alignment = 0x1000000;
        bp->hdr.initrd_addr_max  = (uint32_t)ZP_INITRD_ADDR_MAX;
    }
    bp->hdr.type_of_loader = 0xff;              /* 未注册的 bootloader */

    /* boot_params 偏移 0x070 的 acpi_rsdp_addr。它不在 setup header 里，
     * 也不对应任何协议版本号（2.14 是被作废的版本，见 boot.rst）；
     * 内核在 acpi_os_get_root_pointer() 里先看这里，不必扫 BIOS 区。 */
    bp->acpi_rsdp_addr = zc->acpi_rsdp;

    /* cmdline */
    cl_len = strlen(zc->cmdline);
    if (cl_len + 1 > ZP_CMDLINE_MAX) {
        vmm_err("kernel cmdline is too long (%zu bytes, max %d)",
                cl_len, ZP_CMDLINE_MAX - 1);
        return VMM_ERR_INVAL;
    }
    /* bzImage 在 cmdline_size 里声明了自己能接受的长度（协议 2.06 起） */
    if (zc->kernel_hdr && bp->hdr.version >= 0x0206 &&
        cl_len > bp->hdr.cmdline_size) {
        vmm_err("kernel cmdline is %zu bytes, the kernel accepts at most %u",
                cl_len, bp->hdr.cmdline_size);
        return VMM_ERR_INVAL;
    }
    if (mem_write(vm, ZP_CMDLINE_GPA, zc->cmdline, cl_len + 1) != VMM_OK)
        return VMM_ERR_INVAL;
    bp->hdr.cmd_line_ptr = (uint32_t)ZP_CMDLINE_GPA;
    bp->ext_cmd_line_ptr = 0;
    /* 不写 hdr.cmdline_size：它是 boot.rst 里的 "read" 类字段，方向是
     * 内核告诉 loader 自己能接受多长的命令行（bzImage 里是 2047），
     * loader 写它没有意义。内核本体也不读它，上限见 ZP_CMDLINE_MAX。 */
    vmm_info("cmdline @ GPA 0x%llx: \"%s\"",
             (unsigned long long)ZP_CMDLINE_GPA, zc->cmdline);

    /* E820：没有 BIOS，内存图完全由我们说了算，逐个 RAM slot 生成，
     * 这样 E820 和 memslot 不可能各说各的。
     * 低 640K 报 RAM，是因为内核会在 1MiB 以下找地方放 trampoline
     * （SMP 唤醒 AP 用），完全不给低端内存的话会 panic。
     * slot 之间的 hole 不写条目，原因见 memory.h 的布局说明。 */
    for (; s; s = mem_next_ram_slot(vm, s->gpa + s->size)) {
        if (s->gpa == 0) {
            e820_add(bp, 0, ZP_EBDA_START, E820_TYPE_RAM);
            e820_add(bp, ZP_EBDA_START, ZP_HIGH_MEM_START - ZP_EBDA_START,
                     E820_TYPE_RESERVED);
            e820_add(bp, ZP_HIGH_MEM_START, s->size - ZP_HIGH_MEM_START,
                     E820_TYPE_RAM);
        } else {
            e820_add(bp, s->gpa, s->size, E820_TYPE_RAM);
        }
    }

    if (zc->initrd_path)
        return load_initrd(vm, bp, zc);
    return VMM_OK;
}
