/* include/memory.h — guest 物理内存管理与 GPA→HVA 转换
 *
 * 这是整个 VMM 里最容易埋雷的模块之一：Step 3 的 virtqueue 里所有
 * 描述符地址都是 guest 自己填的 GPA，如果转换函数不做长度校验，
 * 一个恶意/错乱的 guest 就能让 VMM 进程段错误。所以从 Step 1 开始
 * 就把边界检查做进接口签名里（gpa_to_hva 强制要求传 len）。
 */
#ifndef VMM_MEMORY_H
#define VMM_MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct vmm_vm;

#define VMM_MEM_MAX_SLOTS 8

/* 一个 slot 对应一次 KVM_SET_USER_MEMORY_REGION。
 * Step 1 只有一个 slot（GPA 0 起的整块 RAM）；
 * Step 2 起会因为要给 MMIO 空间/ROM 挖洞而出现多个 slot。 */
struct vmm_mem_slot {
    uint32_t slot;      /* KVM slot 号，全 VM 唯一 */
    uint64_t gpa;       /* guest 物理起始地址 */
    uint64_t size;      /* 字节，须页对齐 */
    void    *hva;       /* host 侧 mmap 得到的用户态虚拟地址 */
    uint32_t flags;     /* KVM_MEM_LOG_DIRTY_PAGES / KVM_MEM_READONLY */
    bool     mmap_owned;/* 是否由本模块 mmap，决定 destroy 时要不要 munmap */
    bool     used;
};

struct vmm_mem {
    struct vmm_mem_slot slots[VMM_MEM_MAX_SLOTS];
    uint32_t            nr_slots;
    uint64_t            ram_size;   /* 所有 RAM slot 之和 */
};

/* 分配 ram_size 字节匿名内存并注册为 slot 0（GPA 0 起）。 */
int  mem_init(struct vmm_vm *vm, uint64_t ram_size);
void mem_destroy(struct vmm_vm *vm);

/* 注册一块已有的 host 内存为 guest 物理内存。hva 须页对齐。
 * mmap_owned=false 表示内存由调用方管理（如后面 VFIO 的 BAR 映射）。 */
int  mem_add_region(struct vmm_vm *vm, uint64_t gpa, uint64_t size,
                    void *hva, uint32_t flags, bool mmap_owned);

/* 查 slot；找不到返回 NULL */
struct vmm_mem_slot *mem_find_slot(struct vmm_vm *vm, uint64_t gpa);

/* 核心转换函数。
 * 语义：保证 [gpa, gpa+len) 完整落在同一个 slot 内，否则返回 NULL。
 * len 必须 > 0。跨 slot 的访问一律视为非法（由调用方拆分）。 */
void *gpa_to_hva(struct vmm_vm *vm, uint64_t gpa, uint64_t len);

/* 带边界检查的读写，内部走 gpa_to_hva */
int  mem_read(struct vmm_vm *vm, uint64_t gpa, void *dst, size_t len);
int  mem_write(struct vmm_vm *vm, uint64_t gpa, const void *src, size_t len);

/* 把文件整个读进 GPA 处；out_len 可为 NULL。Step 1 用来载入 payload.bin */
int  mem_load_file(struct vmm_vm *vm, uint64_t gpa, const char *path,
                   size_t *out_len);

/* 调试：hexdump 一段 guest 物理内存到 stderr。Step 1.2 验证 0x500000 用 */
void mem_hexdump(struct vmm_vm *vm, uint64_t gpa, size_t len);

#endif /* VMM_MEMORY_H */
