/* src/boot/elf_loader.c — 解析 vmlinux ELF64                   [Step 2]
 *
 * vmlinux 的程序头里同时有 p_vaddr（0xffffffff81000000 这类内核虚拟地址）
 * 和 p_paddr（0x1000000 这类物理地址）。我们还没有进 guest，只能按
 * 物理地址摆放，所以一律用 p_paddr。
 *
 * e_entry 在 x86_64 vmlinux 里链接脚本写的是 ENTRY(phys_startup_64)，
 * 已经是物理地址；但为了兼容别的构建方式，如果它落在内核虚拟地址区，
 * 就用所在段的 (p_vaddr - p_paddr) 把它换算回物理地址。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <elf.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "vmm.h"
#include "memory.h"
#include "boot/elf_loader.h"

static int check_ehdr(const Elf64_Ehdr *eh, size_t file_size, const char *path)
{
    uint64_t ph_bytes;

    if (file_size < sizeof(*eh) || memcmp(eh->e_ident, ELFMAG, SELFMAG)) {
        vmm_err("%s is not an ELF file", path);
        return VMM_ERR_INVAL;
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_ident[EI_DATA] != ELFDATA2LSB ||
        eh->e_machine != EM_X86_64) {
        vmm_err("%s is not an x86_64 little-endian ELF64", path);
        return VMM_ERR_INVAL;
    }
    if (eh->e_phentsize != sizeof(Elf64_Phdr)) {
        vmm_err("%s: unexpected e_phentsize %u", path, eh->e_phentsize);
        return VMM_ERR_INVAL;
    }
    /* e_phoff 和 e_phnum 都直接来自文件，写成 `e_phoff + n * 56 > file_size`
     * 会在 64 位下回绕：e_phoff = 0xffffffffffffffc0、e_phnum = 2 时和是
     * 0x30，比较通过，随后 img + e_phoff 把指针推到映射区之前 64 字节。
     * 所以全程不做加法——e_phnum 是 16 位，乘 56 最多 0x37ffc8，不会溢出；
     * 再用减法把上界移到右边比较。同样的写法见 src/mem.c 的 gpa_to_hva()。 */
    ph_bytes = (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr);
    if (eh->e_phoff > file_size || ph_bytes > file_size - eh->e_phoff) {
        vmm_err("%s: program header table is truncated", path);
        return VMM_ERR_INVAL;
    }
    return VMM_OK;
}

int elf_load_vmlinux(struct vmm_vm *vm, const char *path,
                     struct elf_load_info *info)
{
    struct stat st;
    const uint8_t *img;
    const Elf64_Ehdr *eh;
    const uint8_t *ph_tab;
    uint64_t entry;
    int fd, i, r = VMM_OK, nr_load = 0;
    bool entry_found = false;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        vmm_err("open %s: %s", path, strerror(errno));
        return VMM_ERR_SYS;
    }
    if (fstat(fd, &st) < 0) {
        vmm_err("fstat %s: %s", path, strerror(errno));
        close(fd);
        return VMM_ERR_SYS;
    }
    img = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (img == MAP_FAILED) {
        vmm_err("mmap %s: %s", path, strerror(errno));
        return VMM_ERR_SYS;
    }

    eh = (const Elf64_Ehdr *)img;
    r = check_ehdr(eh, (size_t)st.st_size, path);
    if (r != VMM_OK)
        goto out;

    memset(info, 0, sizeof(*info));
    info->lo_gpa = UINT64_MAX;
    entry = eh->e_entry;
    /* e_phoff 没有任何对齐保证，直接当 Elf64_Phdr* 解引用是未定义行为
     * （x86 上跑得通，UBSan 会报 misaligned member access）。逐个拷进
     * 对齐的局部变量再用，和 bzimage.c 处理 setup header 的做法一致。 */
    ph_tab = img + eh->e_phoff;

    for (i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr p;
        uint8_t *dst;

        memcpy(&p, ph_tab + (size_t)i * sizeof(p), sizeof(p));

        if (p.p_type != PT_LOAD || p.p_memsz == 0)
            continue;
        /* p_offset 与 p_filesz 同样是文件说了算，相加一样会回绕 */
        if (p.p_filesz > p.p_memsz ||
            p.p_offset > (uint64_t)st.st_size ||
            p.p_filesz > (uint64_t)st.st_size - p.p_offset) {
            vmm_err("%s: PT_LOAD #%d is malformed", path, i);
            r = VMM_ERR_INVAL;
            goto out;
        }

        dst = gpa_to_hva(vm, p.p_paddr, p.p_memsz);
        if (!dst) {
            vmm_err("%s: PT_LOAD #%d [0x%llx, 0x%llx) exceeds guest RAM "
                    "(try a larger --mem)", path, i,
                    (unsigned long long)p.p_paddr,
                    (unsigned long long)(p.p_paddr + p.p_memsz));
            r = VMM_ERR_INVAL;
            goto out;
        }
        memcpy(dst, img + p.p_offset, p.p_filesz);
        memset(dst + p.p_filesz, 0, p.p_memsz - p.p_filesz);

        vmm_info("vmlinux PT_LOAD #%d: GPA [0x%llx, 0x%llx) file 0x%llx mem 0x%llx",
                 i, (unsigned long long)p.p_paddr,
                 (unsigned long long)(p.p_paddr + p.p_memsz),
                 (unsigned long long)p.p_filesz,
                 (unsigned long long)p.p_memsz);

        if (p.p_paddr < info->lo_gpa)
            info->lo_gpa = p.p_paddr;
        if (p.p_paddr + p.p_memsz > info->hi_gpa)
            info->hi_gpa = p.p_paddr + p.p_memsz;

        /* 入口换算：e_entry 是物理地址时落在 [p_paddr, +memsz)，
         * 是虚拟地址时落在 [p_vaddr, +memsz) */
        if (!entry_found) {
            if (entry >= p.p_paddr && entry < p.p_paddr + p.p_memsz) {
                entry_found = true;
            } else if (entry >= p.p_vaddr &&
                       entry < p.p_vaddr + p.p_memsz) {
                entry = entry - p.p_vaddr + p.p_paddr;
                entry_found = true;
            }
        }
        nr_load++;
    }

    if (!nr_load) {
        vmm_err("%s has no PT_LOAD segment", path);
        r = VMM_ERR_INVAL;
        goto out;
    }
    if (!entry_found) {
        vmm_err("%s: e_entry 0x%llx is not inside any PT_LOAD segment",
                path, (unsigned long long)eh->e_entry);
        r = VMM_ERR_INVAL;
        goto out;
    }

    info->entry_gpa = entry;
    vmm_info("vmlinux loaded: GPA [0x%llx, 0x%llx), entry 0x%llx",
             (unsigned long long)info->lo_gpa,
             (unsigned long long)info->hi_gpa,
             (unsigned long long)info->entry_gpa);
out:
    munmap((void *)img, (size_t)st.st_size);
    return r;
}
