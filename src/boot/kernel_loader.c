/* src/boot/kernel_loader.c — 按文件头识别内核格式并分派加载 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <elf.h>

#include "vmm.h"
#include "boot/elf_loader.h"
#include "boot/kernel_loader.h"

/* setup header 的魔数位置（boot.rst）：0x1fe 是 boot_flag，0x202 是 "HdrS" */
#define BZ_BOOT_FLAG_OFF  0x1fe
#define BZ_HEADER_OFF     0x202

const char *kernel_format_name(enum kernel_format fmt)
{
    switch (fmt) {
    case KERNEL_FMT_VMLINUX: return "vmlinux (ELF)";
    case KERNEL_FMT_BZIMAGE: return "bzImage";
    }
    return "unknown";
}

int kernel_load(struct vmm_vm *vm, const char *path, struct kernel_image *img)
{
    uint8_t head[BZ_HEADER_OFF + 4];
    ssize_t n;
    int fd;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        vmm_err("open %s: %s", path, strerror(errno));
        return VMM_ERR_SYS;
    }
    n = pread(fd, head, sizeof(head), 0);
    close(fd);
    if (n < 0) {
        vmm_err("read %s: %s", path, strerror(errno));
        return VMM_ERR_SYS;
    }

    memset(img, 0, sizeof(*img));

    if (n >= SELFMAG && !memcmp(head, ELFMAG, SELFMAG)) {
        struct elf_load_info elf;
        int r = elf_load_vmlinux(vm, path, &elf);

        if (r != VMM_OK)
            return r;
        img->format    = KERNEL_FMT_VMLINUX;
        img->entry_gpa = elf.entry_gpa;
        img->lo_gpa    = elf.lo_gpa;
        img->hi_gpa    = elf.hi_gpa;
        return VMM_OK;
    }

    if (n == (ssize_t)sizeof(head) &&
        head[BZ_BOOT_FLAG_OFF] == 0x55 && head[BZ_BOOT_FLAG_OFF + 1] == 0xaa &&
        !memcmp(head + BZ_HEADER_OFF, "HdrS", 4))
        return bzimage_load(vm, path, img);

    vmm_err("%s is neither an ELF vmlinux nor a bzImage "
            "(no ELF magic, no \"HdrS\" at offset 0x%x)", path, BZ_HEADER_OFF);
    return VMM_ERR_INVAL;
}
