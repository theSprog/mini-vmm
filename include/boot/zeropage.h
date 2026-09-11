/* include/boot/zeropage.h — boot_params（zero page）构造
 *
 * Linux x86 64 位启动协议（Documentation/arch/x86/boot.rst）规定：
 *   - 进入 startup_64 时处于长模式、分页开启、恒等映射覆盖
 *     内核、zero page、cmdline、initrd
 *   - %rsi = struct boot_params 的物理地址
 *   - __BOOT_CS = 0x10 可执行，__BOOT_DS = 0x18 可读写
 *   - 中断关闭
 * struct boot_params 直接用 <asm/bootparam.h>，不自己抄。
 */
#ifndef BOOT_ZEROPAGE_H
#define BOOT_ZEROPAGE_H

#include <stddef.h>
#include <stdint.h>

struct vmm_vm;

/* 低端内存布局，避开 GDT/页表区 [0x1000, 0x5000) */
#define ZP_BOOT_PARAMS_GPA  0x7000ULL
#define ZP_CMDLINE_GPA      0x20000ULL
/* 命令行缓冲区上限，含结尾 NUL。必须等于 x86 内核的 COMMAND_LINE_SIZE
 * （arch/x86/include/asm/setup.h，2048）：copy_bootdata() 从 cmd_line_ptr
 * 固定复制 2048 字节，再用 strscpy 截断，第 2047 个字符之后的内容会被
 * guest 静默丢掉。VMM 这边按同一个上限检查，超长直接报错，而不是让
 * 尾部参数悄悄失效。 */
#define ZP_CMDLINE_MAX      2048

/* 0x9fc00 起到 1MiB 按 PC 惯例报为保留（EBDA + VGA + BIOS ROM 区） */
#define ZP_EBDA_START       0x9fc00ULL
#define ZP_HIGH_MEM_START   0x100000ULL

/* vmlinux 没有 setup header，initrd_addr_max 取协议 2.02 及以前的默认值；
 * bzImage 用内核自己声明的值（6.6 是 0x7fffffff） */
#define ZP_INITRD_ADDR_MAX  0x37ffffffULL

struct setup_header;

struct zp_config {
    const char *cmdline;
    const char *initrd_path;   /* 可为 NULL */
    uint64_t    kernel_end;    /* 内核最高物理地址，initrd 不能压上去 */
    uint64_t    acpi_rsdp;     /* 0 = 没有 ACPI 表 */
    /* bzImage 自带的 setup header（boot_params 偏移 0x1f1 起的
     * kernel_hdr_len 个字节）。NULL 表示 vmlinux，由 zeropage.c 伪造。 */
    const struct setup_header *kernel_hdr;
    size_t      kernel_hdr_len;
    uint64_t    initrd_addr_max;   /* initrd 最高可用字节地址，0 = ZP_INITRD_ADDR_MAX */
};

/* 构造 boot_params，写 cmdline，载入 initrd（如有）。 */
int zeropage_setup(struct vmm_vm *vm, const struct zp_config *zc);

#endif /* BOOT_ZEROPAGE_H */
