/* include/boot/kernel_loader.h — 内核镜像加载：vmlinux（ELF）与 bzImage 自动识别
 *
 * 两种格式走的都是 64 位启动协议（Documentation/arch/x86/boot.rst），
 * vCPU 进入时的状态完全相同（长模式、__BOOT_CS/__BOOT_DS、RSI 指向
 * boot_params），区别只在“跳到哪里”和“boot_params 里的 setup header
 * 由谁提供”：
 *
 *   vmlinux  未压缩的内核本体。按 PT_LOAD 的 p_paddr 摆放，入口是 ELF 的
 *            e_entry（startup_64）。没有 setup header，由 zeropage.c 伪造
 *            内核本体会读的那几个字段。没有解压器，也就没有 KASLR。
 *   bzImage  setup 代码 + 解压器 + 压缩过的 vmlinux。保护模式部分装到
 *            pref_address，从“装载地址 + 0x200”进入解压器的 startup_64；
 *            setup header 由内核自己提供，VMM 原样拷进 boot_params 后只填
 *            loader 负责的字段。解压、重定位、KASLR 都由解压器完成。
 */
#ifndef BOOT_KERNEL_LOADER_H
#define BOOT_KERNEL_LOADER_H

#include <stddef.h>
#include <stdint.h>
#include <asm/bootparam.h>

struct vmm_vm;

enum kernel_format {
    KERNEL_FMT_VMLINUX,
    KERNEL_FMT_BZIMAGE,
};

struct kernel_image {
    enum kernel_format format;
    uint64_t entry_gpa;     /* vCPU 的第一条指令 */
    uint64_t lo_gpa;        /* 内核在 guest 物理内存里占用的区间 [lo, hi) */
    uint64_t hi_gpa;        /* bzImage 是装载地址 + init_size，initrd 不能压上来 */

    /* 只有 bzImage 有：内核自带的 setup header（文件偏移 0x1f1 起），
     * 已经填好了 code32_start。hdr_len == 0 表示没有（vmlinux）。 */
    struct setup_header hdr;
    size_t   hdr_len;
};

/* 看文件头决定格式并加载：ELF 魔数 -> vmlinux；0x1fe 处 0xAA55 且
 * 0x202 处 "HdrS" -> bzImage；都不是则报错。 */
int kernel_load(struct vmm_vm *vm, const char *path, struct kernel_image *img);

/* 由 kernel_load() 调用，见 src/boot/bzimage.c */
int bzimage_load(struct vmm_vm *vm, const char *path, struct kernel_image *img);

const char *kernel_format_name(enum kernel_format fmt);

#endif /* BOOT_KERNEL_LOADER_H */
