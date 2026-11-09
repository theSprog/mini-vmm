/* include/memory.h — guest 物理内存管理与 GPA→HVA 转换
 *
 * 这是整个 VMM 里最容易埋雷的模块之一：DMA 描述符、virtqueue 里所有
 * 地址都是 guest 自己填的 GPA，如果转换函数不做长度校验，
 * 一个恶意/错乱的 guest 就能让 VMM 进程段错误。所以从一开始
 * 就把边界检查做进接口签名里（gpa_to_hva 强制要求传 len）。
 */
#ifndef VMM_MEMORY_H
#define VMM_MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct vmm_vm;

#define VMM_MEM_MAX_SLOTS 8

/* guest 物理地址布局。
 *
 *   [0, low)                 RAM，slot 0，low = min(ram_size, 3GiB)
 *   [3GiB, 4GiB)             32 位 MMIO hole：IOAPIC 0xfec00000、LAPIC 0xfee00000、
 *                            以后的 32 位 PCI BAR 都在这里
 *   [4GiB, 4GiB + 剩余部分)  RAM，slot 1，只在 ram_size > 3GiB 时存在
 *
 * hole 必须在 4GiB 以下，因为只有 32 位地址能力的设备 BAR 和固定的平台设备
 * 地址都在这一段；留 1GiB 是为了给 pci-bus 的 BAR 和 pcie-ecam 的 256MiB
 * 配置空间窗口都留出余量。hole 在 E820 里不出现：Linux 的
 * e820_search_gap() 把 4GiB 以下最大的未描述空隙交给 PCI 分配 BAR，
 * 标成 reserved 反而会让它找不到这段空间。 */
#define VMM_MEM_HOLE_START  0xC0000000ULL
#define VMM_MEM_HIGH_START  0x100000000ULL

/* 一个 slot 对应一次 KVM_SET_USER_MEMORY_REGION。 */
struct vmm_mem_slot {
    uint32_t slot;      /* KVM slot 号，全 VM 唯一 */
    uint64_t gpa;       /* guest 物理起始地址 */
    uint64_t size;      /* 字节，须页对齐 */
    void    *hva;       /* host 侧 mmap 得到的用户态虚拟地址 */
    uint32_t flags;     /* KVM_MEM_LOG_DIRTY_PAGES / KVM_MEM_READONLY */
    bool     mmap_owned;/* 是否由本模块 mmap，决定 destroy 时要不要 munmap */
    bool     ram;       /* guest RAM（进 E820）；false 是设备内存，如以后的 VFIO BAR */
    bool     used;
};

struct vmm_mem {
    struct vmm_mem_slot slots[VMM_MEM_MAX_SLOTS];
    uint32_t            nr_slots;
    uint64_t            ram_size;   /* 所有 RAM slot 之和 */
};

/* 分配 ram_size 字节匿名内存，按上面的布局注册成一个或两个 RAM slot。
 * 两个 slot 共用同一次 mmap，HVA 上连续，GPA 上被 hole 隔开。 */
int  mem_init(struct vmm_vm *vm, uint64_t ram_size);
void mem_destroy(struct vmm_vm *vm);

/* 注册一块已有的 host 内存为 guest 物理内存。hva 须页对齐。
 * mmap_owned=false 表示内存由调用方管理（如后面 VFIO 的 BAR 映射）。 */
int  mem_add_region(struct vmm_vm *vm, uint64_t gpa, uint64_t size,
                    void *hva, uint32_t flags, bool mmap_owned);

/* 查 slot；找不到返回 NULL */
struct vmm_mem_slot *mem_find_slot(struct vmm_vm *vm, uint64_t gpa);

/* 起始 GPA >= gpa 的 RAM slot 里起点最低的一个，没有返回 NULL。
 * 按 GPA 升序遍历所有 RAM：
 *   for (s = mem_next_ram_slot(vm, 0); s; s = mem_next_ram_slot(vm, s->gpa + s->size)) */
const struct vmm_mem_slot *mem_next_ram_slot(struct vmm_vm *vm, uint64_t gpa);

/* gpa_to_hva 的结果。拒绝的原因分开报，是因为它们指向不同的 bug：
 * WRAP 几乎总是 guest 填了垃圾地址，CROSSES_SLOT 是调用方该拆分却没拆，
 * PAST_END 是长度算错或者访问了 hole。 */
enum gpa_status {
    GPA_OK = 0,
    GPA_ERR_ZERO_LEN,       /* len == 0 */
    GPA_ERR_WRAP,           /* 结束地址 gpa + len >= 2^64，64 位表示不了 */
    GPA_ERR_UNMAPPED,       /* gpa 不在任何 slot 里 */
    GPA_ERR_CROSSES_SLOT,   /* 从一个 slot 延伸进紧邻的另一个 slot */
    GPA_ERR_PAST_END,       /* 从 slot 延伸到没有 slot 的地址 */
};

const char *gpa_status_str(enum gpa_status st);

/* 核心转换函数。
 * 保证 [gpa, gpa+len) 完整落在同一个 slot 内才返回 GPA_OK 并写 *hva，
 * 否则返回拒绝原因、*hva 不动。len 必须 > 0。跨 slot 的访问一律非法：
 * 相邻 slot 在 GPA 上相接不代表 HVA 上相接，需要跨 slot 的调用方自己拆分。
 * 返回值不是指针，是为了让调用方拿到失败原因并写进日志。 */
enum gpa_status gpa_to_hva(struct vmm_vm *vm, uint64_t gpa, uint64_t len,
                           void **hva);

/* 带边界检查的读写，内部走 gpa_to_hva，失败时日志里带上原因 */
int  mem_read(struct vmm_vm *vm, uint64_t gpa, void *dst, size_t len);
int  mem_write(struct vmm_vm *vm, uint64_t gpa, const void *src, size_t len);

/* 把文件整个读进 GPA 处；out_len 可为 NULL。裸 payload 模式用来载入 payload.bin */
int  mem_load_file(struct vmm_vm *vm, uint64_t gpa, const char *path,
                   size_t *out_len);

/* 调试：hexdump 一段 guest 物理内存到 stderr。验证长模式 payload 写入的 0x500000 用 */
void mem_hexdump(struct vmm_vm *vm, uint64_t gpa, size_t len);

#endif /* VMM_MEMORY_H */
