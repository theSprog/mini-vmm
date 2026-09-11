/* include/boot/elf_loader.h — 未压缩 vmlinux（ELF64）加载 */
#ifndef BOOT_ELF_LOADER_H
#define BOOT_ELF_LOADER_H

#include <stdint.h>

struct vmm_vm;

struct elf_load_info {
    uint64_t entry_gpa;   /* 64 位入口的物理地址（startup_64） */
    uint64_t lo_gpa;      /* 所有 PT_LOAD 段覆盖的最低物理地址 */
    uint64_t hi_gpa;      /* 最高物理地址（开区间），用于和 initrd 查重叠 */
};

/* 按 PT_LOAD 的 p_paddr 把段拷进 guest 物理内存，bss 部分清零。 */
int elf_load_vmlinux(struct vmm_vm *vm, const char *path,
                     struct elf_load_info *info);

#endif /* BOOT_ELF_LOADER_H */
