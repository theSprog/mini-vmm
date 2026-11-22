/* src/boot/bzimage.c — 按 64 位启动协议加载 bzImage
 *
 * bzImage 的文件结构（arch/x86/boot/header.S；拼接方式见 arch/x86/boot/Makefile
 * 的 cmd_image：setup.bin 补齐到 4K 边界，后面接 vmlinux.bin）：
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
#include <stddef.h>
#include <string.h>

#include "vmm.h"
#include "memory.h"
#include "boot/image_file.h"
#include "boot/kernel_loader.h"
#include "boot/zeropage.h"

#define BZ_HDR_OFF          0x1f1       /* setup header 在文件与 boot_params 中的偏移 */
#define BZ_JUMP_OFF         0x200       /* 2 字节短跳转，第二个字节决定 header 长度 */
#define BZ_SECTOR           512
#define BZ_DEFAULT_SECTS    4           /* setup_sects 为 0 时按 4 处理 */
#define BZ_ENTRY64_OFF      0x200       /* 64 位入口相对保护模式部分起点的偏移 */
#define BZ_MIN_PROTOCOL     0x020c      /* 2.12：有 xloadflags，能声明 64 位入口 */
#define BZ_DEFAULT_LOAD     0x100000ULL /* 协议 2.10 之前没有 pref_address */

/* 只接受能从 64 位入口启动、装在 1MiB 以上的 bzImage */
static int check_header(const struct setup_header *hdr, const char *path)
{
    if (hdr->version < BZ_MIN_PROTOCOL) {
        vmm_err("%s: boot protocol %u.%02u is too old, need >= 2.12",
                path, hdr->version >> 8, hdr->version & 0xff);
        return VMM_ERR_INVAL;
    }
    if (!(hdr->xloadflags & XLF_KERNEL_64)) {
        vmm_err("%s: bzImage has no 64-bit entry point (XLF_KERNEL_64 clear), "
                "is it a 32-bit kernel?", path);
        return VMM_ERR_INVAL;
    }
    if (!(hdr->loadflags & LOADED_HIGH)) {
        vmm_err("%s: zImage (loaded below 1MiB) is not supported", path);
        return VMM_ERR_INVAL;
    }
    return VMM_OK;
}

/* 保护模式部分在文件里的位置：从 (setup_sects + 1) 个扇区之后到文件末尾 */
static int locate_pm_part(const struct setup_header *hdr, uint64_t file_size,
                          const char *path, uint64_t *off, uint64_t *len)
{
    *off = (uint64_t)((hdr->setup_sects ? hdr->setup_sects : BZ_DEFAULT_SECTS) + 1)
           * BZ_SECTOR;
    if (*off >= file_size) {
        vmm_err("%s: setup_sects=%u points beyond the end of the file",
                path, hdr->setup_sects);
        return VMM_ERR_INVAL;
    }
    *len = file_size - *off;
    /* syssize 以 16 字节为单位（协议 2.04 起是 4 字节字段），只做健全性检查 */
    if (hdr->syssize && (uint64_t)hdr->syssize * 16 > *len) {
        vmm_err("%s: truncated, syssize says 0x%llx bytes but only 0x%llx present",
                path, (unsigned long long)hdr->syssize * 16,
                (unsigned long long)*len);
        return VMM_ERR_INVAL;
    }
    return VMM_OK;
}

/* header 的实际长度 = 0x202 + 0x201 处那个字节（短跳转的偏移）- 0x1f1，
 * 更老的内核 header 更短；超过本结构体的部分我们也不认识，截掉 */
static size_t header_len(const uint8_t *file)
{
    uint64_t end = BZ_JUMP_OFF + 2 + file[BZ_JUMP_OFF + 1];

    if (end > BZ_HDR_OFF + sizeof(struct setup_header))
        end = BZ_HDR_OFF + sizeof(struct setup_header);
    return end - BZ_HDR_OFF;
}

/* 装载地址：优先 pref_address（2.10 起有），它就是链接地址，
 * 解压器不必再挪一次。relocatable 内核要求按 kernel_alignment 对齐。 */
static int choose_load_addr(const struct setup_header *hdr, const char *path,
                            uint64_t *load)
{
    *load = hdr->pref_address ? hdr->pref_address : BZ_DEFAULT_LOAD;
    if (hdr->relocatable_kernel && hdr->kernel_alignment &&
        (*load & (hdr->kernel_alignment - 1))) {
        vmm_err("%s: pref_address 0x%llx is not aligned to kernel_alignment 0x%x",
                path, (unsigned long long)*load, hdr->kernel_alignment);
        return VMM_ERR_INVAL;
    }
    if (*load < ZP_HIGH_MEM_START) {
        vmm_err("%s: load address 0x%llx is below 1MiB",
                path, (unsigned long long)*load);
        return VMM_ERR_INVAL;
    }
    return VMM_OK;
}

/* 把保护模式部分拷到 load。init_size 是解压、重定位期间从装载地址起需要
 * 的线性内存，比文件大得多（解压后的内核加上解压器的工作区），必须整段
 * 是 RAM。*need 返回占用的长度 */
static int copy_pm_part(struct vmm_vm *vm, const struct setup_header *hdr,
                        const uint8_t *src, uint64_t len, uint64_t load,
                        const char *path, uint64_t *need)
{
    enum gpa_status gs;
    uint8_t *dst;

    *need = hdr->init_size > len ? hdr->init_size : len;
    gs = gpa_to_hva(vm, load, *need, (void **)&dst);
    if (gs != GPA_OK) {
        vmm_err("%s: needs RAM [0x%llx, 0x%llx) (init_size): %s, "
                "increase --mem", path, (unsigned long long)load,
                (unsigned long long)(load + *need), gpa_status_str(gs));
        return VMM_ERR_INVAL;
    }
    memcpy(dst, src, len);
    /* guest RAM 是匿名映射，本来就是 0；显式清零让结果不依赖这一点 */
    memset(dst + len, 0, *need - len);
    return VMM_OK;
}

/* kernel_version 是版本字符串相对 0x200 的偏移 */
static void log_kernel_version(const struct setup_header *hdr,
                               const struct image_file *f)
{
    uint64_t off = (uint64_t)hdr->kernel_version + BZ_JUMP_OFF;
    int max;

    if (!hdr->kernel_version || off >= f->size)
        return;
    max = (int)(f->size - off < 80 ? f->size - off : 80);
    vmm_info("bzImage: %.*s", max, (const char *)f->data + off);
}

int bzimage_load(struct vmm_vm *vm, const char *path, struct kernel_image *img)
{
    struct setup_header hdr;
    struct image_file f;
    uint64_t load, need, pm_off, pm_len;
    int r;

    r = image_file_map(path, BZ_HDR_OFF + sizeof(hdr),
                       "a bzImage setup header", &f);
    if (r != VMM_OK)
        return r;

    /* header 在文件里没有对齐保证，先整体拷出来再看 */
    memcpy(&hdr, f.data + BZ_HDR_OFF, sizeof(hdr));

    r = check_header(&hdr, path);
    if (r == VMM_OK)
        r = locate_pm_part(&hdr, f.size, path, &pm_off, &pm_len);
    if (r == VMM_OK)
        r = choose_load_addr(&hdr, path, &load);
    if (r == VMM_OK)
        r = copy_pm_part(vm, &hdr, f.data + pm_off, pm_len, load, path, &need);
    if (r != VMM_OK)
        goto out;

    /* 协议要求：relocatable 内核装好后 loader 要把 code32_start 改成实际
     * 装载地址。64 位入口不经过它，但照做无害，工具也能据此看到装在哪 */
    hdr.code32_start = (uint32_t)load;

    img->format    = KERNEL_FMT_BZIMAGE;
    img->entry_gpa = load + BZ_ENTRY64_OFF;
    img->lo_gpa    = load;
    img->hi_gpa    = load + need;
    img->hdr       = hdr;
    img->hdr_len   = header_len(f.data);

    log_kernel_version(&hdr, &f);
    vmm_info("bzImage: protocol %u.%02u, setup %llu bytes, payload 0x%llx bytes "
             "-> GPA 0x%llx, init_size 0x%x, entry 0x%llx",
             hdr.version >> 8, hdr.version & 0xff,
             (unsigned long long)pm_off, (unsigned long long)pm_len,
             (unsigned long long)load, hdr.init_size,
             (unsigned long long)img->entry_gpa);
out:
    image_file_unmap(&f);
    return r;
}
