/* src/mem.c — guest 物理内存管理 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "vmm.h"
#include "memory.h"
#include "kvm_wrappers.h"

#define PAGE_SIZE 4096ULL
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))

int mem_add_region(struct vmm_vm *vm, uint64_t gpa, uint64_t size,
                   void *hva, uint32_t flags, bool mmap_owned)
{
    struct vmm_mem *m = &vm->mem;
    struct vmm_mem_slot *s;
    uint32_t i;

    if (!size || (gpa & (PAGE_SIZE - 1)) || (size & (PAGE_SIZE - 1))) {
        vmm_err("mem_add_region: gpa/size must be page aligned (gpa=0x%llx size=0x%llx)",
                (unsigned long long)gpa, (unsigned long long)size);
        return VMM_ERR_INVAL;
    }
    if ((uintptr_t)hva & (PAGE_SIZE - 1)) {
        vmm_err("mem_add_region: hva is not page aligned");
        return VMM_ERR_INVAL;
    }

    /* 与已有 slot 重叠检查：KVM 自己也会拒绝，但它的报错是 EINVAL，
     * 我们在这里拦下来能给出更有用的信息。 */
    for (i = 0; i < VMM_MEM_MAX_SLOTS; i++) {
        s = &m->slots[i];
        if (!s->used)
            continue;
        if (gpa < s->gpa + s->size && s->gpa < gpa + size) {
            vmm_err("mem_add_region: GPA range [0x%llx,0x%llx) "
                    "overlaps slot %u [0x%llx,0x%llx)",
                    (unsigned long long)gpa,
                    (unsigned long long)(gpa + size),
                    s->slot, (unsigned long long)s->gpa,
                    (unsigned long long)(s->gpa + s->size));
            return VMM_ERR_INVAL;
        }
    }

    for (i = 0; i < VMM_MEM_MAX_SLOTS; i++)
        if (!m->slots[i].used)
            break;
    if (i == VMM_MEM_MAX_SLOTS) {
        vmm_err("mem_add_region: out of memory slots");
        return VMM_ERR_NOMEM;
    }

    s = &m->slots[i];
    s->slot       = i;
    s->gpa        = gpa;
    s->size       = size;
    s->hva        = hva;
    s->flags      = flags;
    s->mmap_owned = mmap_owned;
    s->used       = true;

    if (kvm_set_user_memory_region(vm->vm_fd, s->slot, flags,
                                   gpa, size, (uint64_t)(uintptr_t)hva)
        != VMM_OK) {
        s->used = false;
        return VMM_ERR_SYS;
    }

    m->nr_slots++;
    vmm_info("memslot %u: GPA [0x%llx, 0x%llx) -> HVA %p (%llu MiB)",
             s->slot, (unsigned long long)gpa,
             (unsigned long long)(gpa + size), hva,
             (unsigned long long)(size >> 20));
    return VMM_OK;
}

int mem_init(struct vmm_vm *vm, uint64_t ram_size)
{
    void *p;
    int r;

    ram_size = ALIGN_UP(ram_size, PAGE_SIZE);

    /* MAP_NORESERVE：guest 大部分页在启动后很久都不会被碰到，
     * 不预留 swap 让 128MiB 甚至几 GiB 的 VM 启动瞬间完成。
     * 真正的物理页在 guest 第一次访问触发 EPT/NPT violation 时，
     * 由 host 的缺页路径按需分配。 */
    p = mmap(NULL, ram_size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        vmm_err("mmap guest RAM (%llu MiB): %s",
                (unsigned long long)(ram_size >> 20), strerror(errno));
        return VMM_ERR_NOMEM;
    }

    /* 尽量让 host 用 2MiB 透明大页承载 guest RAM，减少 NPT 的层级和
     * TLB miss。失败不致命（比如 THP 被关掉了）。 */
    if (madvise(p, ram_size, MADV_HUGEPAGE) < 0)
        vmm_dbg("madvise(MADV_HUGEPAGE) failed: %s (harmless)",
                strerror(errno));

    r = mem_add_region(vm, 0, ram_size, p, 0, true);
    if (r != VMM_OK) {
        munmap(p, ram_size);
        return r;
    }

    vm->mem.ram_size = ram_size;
    return VMM_OK;
}

void mem_destroy(struct vmm_vm *vm)
{
    struct vmm_mem *m = &vm->mem;
    uint32_t i;

    for (i = 0; i < VMM_MEM_MAX_SLOTS; i++) {
        struct vmm_mem_slot *s = &m->slots[i];
        if (!s->used)
            continue;
        /* 先把 slot 从 KVM 摘掉（memory_size = 0 即注销），
         * 再 munmap；顺序反了内核可能还持有页引用。 */
        if (vm->vm_fd >= 0)
            kvm_set_user_memory_region(vm->vm_fd, s->slot, 0,
                                       s->gpa, 0, 0);
        if (s->mmap_owned && s->hva)
            munmap(s->hva, s->size);
        s->used = false;
    }
    m->nr_slots = 0;
}

struct vmm_mem_slot *mem_find_slot(struct vmm_vm *vm, uint64_t gpa)
{
    uint32_t i;

    for (i = 0; i < VMM_MEM_MAX_SLOTS; i++) {
        struct vmm_mem_slot *s = &vm->mem.slots[i];
        if (s->used && gpa >= s->gpa && gpa < s->gpa + s->size)
            return s;
    }
    return NULL;
}

void *gpa_to_hva(struct vmm_vm *vm, uint64_t gpa, uint64_t len)
{
    struct vmm_mem_slot *s;

    if (len == 0)
        return NULL;
    /* 溢出检查必须在范围检查之前：guest 完全可以填一个
     * gpa = 0xffffffffffffff00 的描述符，让 gpa+len 回绕到小数值，
     * 从而绕过下面的上界比较。 */
    if (gpa + len < gpa)
        return NULL;

    s = mem_find_slot(vm, gpa);
    if (!s)
        return NULL;
    if (gpa + len > s->gpa + s->size)
        return NULL;   /* 跨 slot 或越界，一律拒绝 */

    return (uint8_t *)s->hva + (gpa - s->gpa);
}

int mem_read(struct vmm_vm *vm, uint64_t gpa, void *dst, size_t len)
{
    void *src = gpa_to_hva(vm, gpa, len);

    if (!src) {
        vmm_err("mem_read: GPA [0x%llx,+0x%zx) out of bounds",
                (unsigned long long)gpa, len);
        return VMM_ERR_GUEST;
    }
    memcpy(dst, src, len);
    return VMM_OK;
}

int mem_write(struct vmm_vm *vm, uint64_t gpa, const void *src, size_t len)
{
    void *dst = gpa_to_hva(vm, gpa, len);

    if (!dst) {
        vmm_err("mem_write: GPA [0x%llx,+0x%zx) out of bounds",
                (unsigned long long)gpa, len);
        return VMM_ERR_GUEST;
    }
    memcpy(dst, src, len);
    return VMM_OK;
}

int mem_load_file(struct vmm_vm *vm, uint64_t gpa, const char *path,
                  size_t *out_len)
{
    struct stat st;
    uint8_t *dst;
    ssize_t n;
    size_t done = 0;
    int fd, r = VMM_OK;

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
    if (st.st_size == 0) {
        vmm_err("%s is an empty file", path);
        close(fd);
        return VMM_ERR_INVAL;
    }

    dst = gpa_to_hva(vm, gpa, (uint64_t)st.st_size);
    if (!dst) {
        vmm_err("failed to load %s: GPA 0x%llx + %lld bytes exceeds "
                "guest RAM (try a larger --mem)",
                path, (unsigned long long)gpa, (long long)st.st_size);
        close(fd);
        return VMM_ERR_INVAL;
    }

    while (done < (size_t)st.st_size) {
        n = read(fd, dst + done, (size_t)st.st_size - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            vmm_err("read %s: %s", path, strerror(errno));
            r = VMM_ERR_SYS;
            break;
        }
        if (n == 0)
            break;
        done += (size_t)n;
    }

    close(fd);
    if (r == VMM_OK) {
        vmm_info("loaded %s: %zu bytes -> GPA 0x%llx",
                 path, done, (unsigned long long)gpa);
        if (out_len)
            *out_len = done;
    }
    return r;
}

void mem_hexdump(struct vmm_vm *vm, uint64_t gpa, size_t len)
{
    const uint8_t *p = gpa_to_hva(vm, gpa, len);
    size_t i, j;

    if (!p) {
        fprintf(stderr, "hexdump: GPA 0x%llx is not accessible\n",
                (unsigned long long)gpa);
        return;
    }

    for (i = 0; i < len; i += 16) {
        fprintf(stderr, "  %012llx  ", (unsigned long long)(gpa + i));
        for (j = 0; j < 16; j++) {
            if (i + j < len)
                fprintf(stderr, "%02x ", p[i + j]);
            else
                fprintf(stderr, "   ");
            if (j == 7)
                fputc(' ', stderr);
        }
        fprintf(stderr, " |");
        for (j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = p[i + j];
            fputc((c >= 0x20 && c < 0x7f) ? c : '.', stderr);
        }
        fprintf(stderr, "|\n");
    }
}
