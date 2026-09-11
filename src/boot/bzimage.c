/* src/boot/bzimage.c — 按 64 位启动协议加载 bzImage
 *
 * bzImage 的文件结构（arch/x86/boot/header.S、tools/build.c）：
 *
 *   [0, 512)                 传统引导扇区；0x1f1 起是 setup header
 *   [512, (setup_sects+1)*512) 16 位 setup 代码（调 BIOS 用，这里不执行）
 *   [(setup_sects+1)*512, EOF) 保护模式部分：解压器 + 压缩过的 vmlinux
 *
 * 64 位协议下 loader 要做的事（boot.rst "64-bit Boot Protocol"）：
 *   1. 把 setup header 拷进清零过的 boot_params，填 loader 负责的字段
 *      （由 zeropage.c 完成，这里只把 header 交出去）；
 *   2. 把保护模式部分装到合适的物理地址；
 *   3. 长模式、恒等映射覆盖 [装载地址, +init_size)、zero page 与命令行，
 *      从“装载地址 + 0x200”进入（解压器的 startup_64）。
 *
 * 之后的事全归解压器：自己建页表，按 E820 挑 KASLR 地址，解压，处理
 * 重定位，最后按同样的约定跳进 vmlinux 的 startup_64，并在 loadflags
 * 里置上 KASLR_FLAG。所以 VMM 这边比直启 vmlinux 还省事：不用解析
 * ELF，也不用伪造 setup header。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "vmm.h"
#include "memory.h"
#include "boot/kernel_loader.h"
#include "boot/zeropage.h"

#define BZ_HDR_OFF          0x1f1       /* setup header 在文件与 boot_params 中的偏移 */
#define BZ_JUMP_OFF         0x200       /* 2 字节短跳转，第二个字节决定 header 长度 */
#define BZ_SECTOR           512
#define BZ_DEFAULT_SECTS    4           /* setup_sects 为 0 时按 4 处理 */
#define BZ_ENTRY64_OFF      0x200       /* 64 位入口相对保护模式部分起点的偏移 */
#define BZ_MIN_PROTOCOL     0x020c      /* 2.12：有 xloadflags，能声明 64 位入口 */
#define BZ_DEFAULT_LOAD     0x100000ULL /* 协议 2.10 之前没有 pref_address */

int bzimage_load(struct vmm_vm *vm, const char *path, struct kernel_image *img)
{
    struct setup_header hdr;
    const uint8_t *file;
    struct stat st;
    uint64_t load, need, pm_off, pm_len, hdr_end;
    uint8_t *dst;
    int fd, r = VMM_ERR_INVAL;

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
    if ((uint64_t)st.st_size < BZ_HDR_OFF + sizeof(hdr)) {
        vmm_err("%s: too small for a bzImage setup header", path);
        close(fd);
        return VMM_ERR_INVAL;
    }
    file = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED) {
        vmm_err("mmap %s: %s", path, strerror(errno));
        return VMM_ERR_SYS;
    }

    /* header 在文件里没有对齐保证，先整体拷出来再看 */
    memcpy(&hdr, file + BZ_HDR_OFF, sizeof(hdr));

    if (hdr.version < BZ_MIN_PROTOCOL) {
        vmm_err("%s: boot protocol %u.%02u is too old, need >= 2.12",
                path, hdr.version >> 8, hdr.version & 0xff);
        goto out;
    }
    if (!(hdr.xloadflags & XLF_KERNEL_64)) {
        vmm_err("%s: bzImage has no 64-bit entry point (XLF_KERNEL_64 clear), "
                "is it a 32-bit kernel?", path);
        goto out;
    }
    if (!(hdr.loadflags & LOADED_HIGH)) {
        vmm_err("%s: zImage (loaded below 1MiB) is not supported", path);
        goto out;
    }

    /* 保护模式部分从 (setup_sects + 1) 个扇区之后开始 */
    pm_off = (uint64_t)((hdr.setup_sects ? hdr.setup_sects : BZ_DEFAULT_SECTS) + 1)
             * BZ_SECTOR;
    if (pm_off >= (uint64_t)st.st_size) {
        vmm_err("%s: setup_sects=%u points beyond the end of the file",
                path, hdr.setup_sects);
        goto out;
    }
    pm_len = (uint64_t)st.st_size - pm_off;
    /* syssize 以 16 字节为单位（协议 2.04 起是 4 字节字段），只做健全性检查 */
    if (hdr.syssize && (uint64_t)hdr.syssize * 16 > pm_len) {
        vmm_err("%s: truncated, syssize says 0x%llx bytes but only 0x%llx present",
                path, (unsigned long long)hdr.syssize * 16,
                (unsigned long long)pm_len);
        goto out;
    }

    /* header 的实际长度 = 0x202 + 0x201 处那个字节（短跳转的偏移），
     * 更老的内核 header 更短；超过本结构体的部分我们也不认识，截掉 */
    hdr_end = BZ_JUMP_OFF + 2 + file[BZ_JUMP_OFF + 1];
    if (hdr_end > BZ_HDR_OFF + sizeof(hdr))
        hdr_end = BZ_HDR_OFF + sizeof(hdr);

    /* 装载地址：优先 pref_address（2.10 起有），它就是链接地址，
     * 解压器不必再挪一次。relocatable 内核要求按 kernel_alignment 对齐。 */
    load = hdr.pref_address ? hdr.pref_address : BZ_DEFAULT_LOAD;
    if (hdr.relocatable_kernel && hdr.kernel_alignment &&
        (load & (hdr.kernel_alignment - 1))) {
        vmm_err("%s: pref_address 0x%llx is not aligned to kernel_alignment 0x%x",
                path, (unsigned long long)load, hdr.kernel_alignment);
        goto out;
    }
    if (load < ZP_HIGH_MEM_START) {
        vmm_err("%s: load address 0x%llx is below 1MiB",
                path, (unsigned long long)load);
        goto out;
    }

    /* init_size 是解压、重定位期间从装载地址起需要的线性内存，
     * 比文件大得多（6.6 约 28MiB，文件约 7.6MiB），必须整段是 RAM */
    need = hdr.init_size > pm_len ? hdr.init_size : pm_len;
    dst = gpa_to_hva(vm, load, need);
    if (!dst) {
        vmm_err("%s: needs RAM [0x%llx, 0x%llx) (init_size), increase --mem",
                path, (unsigned long long)load,
                (unsigned long long)(load + need));
        goto out;
    }
    memcpy(dst, file + pm_off, pm_len);
    /* guest RAM 是匿名映射，本来就是 0；显式清零让结果不依赖这一点 */
    memset(dst + pm_len, 0, need - pm_len);

    /* 协议要求：relocatable 内核装好后 loader 要把 code32_start 改成实际
     * 装载地址。64 位入口不经过它，但照做无害，工具也能据此看到装在哪 */
    hdr.code32_start = (uint32_t)load;

    img->format    = KERNEL_FMT_BZIMAGE;
    img->entry_gpa = load + BZ_ENTRY64_OFF;
    img->lo_gpa    = load;
    img->hi_gpa    = load + need;
    img->hdr       = hdr;
    img->hdr_len   = hdr_end - BZ_HDR_OFF;

    /* kernel_version 是版本字符串相对 0x200 的偏移 */
    if (hdr.kernel_version &&
        (uint64_t)hdr.kernel_version + BZ_JUMP_OFF < (uint64_t)st.st_size) {
        uint64_t off = (uint64_t)hdr.kernel_version + BZ_JUMP_OFF;
        int max = (int)((uint64_t)st.st_size - off < 80 ?
                        (uint64_t)st.st_size - off : 80);

        vmm_info("bzImage: %.*s", max, (const char *)file + off);
    }
    vmm_info("bzImage: protocol %u.%02u, setup %llu bytes, payload 0x%llx bytes "
             "-> GPA 0x%llx, init_size 0x%x, entry 0x%llx",
             hdr.version >> 8, hdr.version & 0xff,
             (unsigned long long)pm_off, (unsigned long long)pm_len,
             (unsigned long long)load, hdr.init_size,
             (unsigned long long)img->entry_gpa);
    r = VMM_OK;
out:
    munmap((void *)file, (size_t)st.st_size);
    return r;
}
