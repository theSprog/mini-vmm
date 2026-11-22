/* include/kvm_stats.h — 读 KVM 的二进制统计（KVM_GET_STATS_FD）
 *
 * VMM 自己只看得到回到用户态的 exit。被 KVM 在内核里就地处理掉的 exit
 * （NPT 缺页、MSR、LAPIC 定时器、内核里模拟的 PIT/PIC 端口……）要靠 KVM
 * 自己的计数器，两边一对照，才知道一次 exit 到底有没有付出“回用户态”
 * 的那一段开销。
 *
 * 文件格式见 Documentation/virt/kvm/api.rst 4.133 节（KVM_GET_STATS_FD）：
 *
 *   offset 0           struct kvm_stats_header
 *   id_offset          id 字符串，name_size 字节
 *   desc_offset        num_desc 个 { struct kvm_stats_desc, name[name_size] }
 *   data_offset        数据块，每个 desc 的值在 data_offset + desc.offset 处，
 *                      desc.size 个 u64
 *
 * 文档明确说 header 之外的三块不保证相邻、不保证按这个顺序，所以一律按
 * header 里的 offset 用 pread 定位，不能假设紧跟在 header 后面。
 */
#ifndef KVM_STATS_H
#define KVM_STATS_H

#include <stdint.h>

/* 一项单值统计（desc.size == 1）。flags 原样保留 KVM_STATS_TYPE_*、
 * KVM_STATS_UNIT_*、KVM_STATS_BASE_* 三个字段。 */
struct kvm_stat {
    const char *name;           /* 指向 kvm_stats.names 里的字符串 */
    uint32_t    flags;
    int16_t     exponent;
    uint64_t    value;
};

struct kvm_stats {
    struct kvm_stat *v;
    unsigned         n;
    /* 跳过的多值统计（直方图）个数。它们记录的是 halt-polling 时长这类
     * 分布，要按 bucket 解释才有意义，exit 计数用不到。 */
    unsigned         skipped;
    char            *names;     /* 所有 name 的存储，kvm_stats_free 释放 */
};

/* 从 KVM_GET_STATS_FD 返回的 fd 读出全部单值统计。
 * 返回 VMM_OK；文件结构不合法（offset 越界、name 没有结尾 '\0'、
 * 数量离谱）返回 VMM_ERR_INVAL；pread 失败返回 VMM_ERR_SYS。
 * 失败时 *st 被清零，不需要 free。 */
int  kvm_stats_read(int fd, struct kvm_stats *st);
void kvm_stats_free(struct kvm_stats *st);

/* 按名字找一项，没有返回 NULL */
const struct kvm_stat *kvm_stats_find(const struct kvm_stats *st,
                                      const char *name);

#endif /* KVM_STATS_H */
