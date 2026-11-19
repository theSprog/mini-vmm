/* src/boot/linux_boot.c — Linux 装载器，约定见 include/boot/linux_boot.h */
#include <string.h>

#include "vmm.h"
#include "boot/gdt_pgtable.h"
#include "boot/kernel_loader.h"
#include "boot/zeropage.h"
#include "boot/mptable.h"
#include "boot/acpi.h"
#include "boot/linux_boot.h"

int linux_boot_load(struct vmm_vm *vm, const struct pci_bus *pci,
                    const struct acpi_devices *acpi)
{
    const struct vmm_config *cfg = &vm->cfg;
    struct boot_mem_layout layout;
    struct kernel_image kimg;
    struct zp_config zc;
    uint64_t rsdp;
    int r;

    /* vmlinux 还是 bzImage 由文件头决定，见 boot/kernel_loader.h */
    r = kernel_load(vm, cfg->payload_path, &kimg);
    if (r != VMM_OK)
        return r;
    vmm_info("kernel format: %s", kernel_format_name(kimg.format));

    /* 内核必须完全落在 1MiB 以上，不能碰到低端的 GDT/页表/zero page */
    if (kimg.lo_gpa < ZP_HIGH_MEM_START) {
        vmm_err("kernel starts at 0x%llx, below 1MiB",
                (unsigned long long)kimg.lo_gpa);
        return VMM_ERR_INVAL;
    }

    /* 中断拓扑同时用两种格式描述，guest 内核开了哪个就用哪个 */
    r = mptable_setup(vm, vm->nr_vcpus, pci);
    if (r != VMM_OK)
        return r;
    r = acpi_setup(vm, vm->nr_vcpus, acpi, &rsdp);
    if (r != VMM_OK)
        return r;

    memset(&zc, 0, sizeof(zc));
    zc.acpi_rsdp   = rsdp;
    zc.cmdline     = cfg->cmdline;
    zc.initrd_path = cfg->initrd_path;
    zc.kernel_end  = kimg.hi_gpa;
    if (kimg.hdr_len) {
        zc.kernel_hdr     = &kimg.hdr;
        zc.kernel_hdr_len = kimg.hdr_len;
        zc.initrd_addr_max = kimg.hdr.initrd_addr_max;
    }
    /* 协议不要求 initrd 恒等映射，但把它留在 VMM 页表覆盖的范围内不花
     * 任何代价，出了问题也好用 --walk 查 */
    if (zc.initrd_addr_max > BOOT_IDENTITY_MAP_SIZE - 1)
        zc.initrd_addr_max = BOOT_IDENTITY_MAP_SIZE - 1;
    r = zeropage_setup(vm, &zc);
    if (r != VMM_OK)
        return r;

    /* 恒等映射 1GiB 就够：内核在 16MiB 附近（bzImage 的 init_size 区间
     * 也在 16MiB 起的几十 MiB 内），zero page/cmdline 在低端，initrd 被
     * 限制在 1GiB 以下。vmlinux 的 startup_64、bzImage 的解压器都会很快
     * 换上自己的页表，这张表只用于最初那一小段。 */
    boot_layout_default(&layout);
    r = boot_layout_validate(vm, &layout);
    if (r != VMM_OK)
        return r;
    r = boot_build_page_table(vm, &layout);
    if (r != VMM_OK)
        return r;

    return boot_setup_linux64(&vm->vcpus[0], &layout, kimg.entry_gpa,
                              ZP_BOOT_PARAMS_GPA);
}
