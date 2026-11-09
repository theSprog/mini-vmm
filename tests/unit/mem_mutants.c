/* tests/unit/mem_mutants.c — gpa_to_hva() 的故意错误实现
 *
 * 每一个都对应 include/memory.h 里写明的一条契约。名字是“坏在哪”，
 * 不是“怎么写的”，这样 negctl 的输出直接就能读成一句话：
 *   “如果有人把溢出检查删了，mem_test 会红。”
 *
 * 全部照抄真实实现的结构，只改动目标那一处——这样测试红了就能确定
 * 是那一处导致的，而不是因为 mutant 整体写崩了。
 */
#include "mutants.h"

/* 真实实现（对照用）：len>0、无回绕、[gpa,gpa+len) 完整落在同一 slot 内，
 * 拒绝时按原因分类，且不碰 *hva */
static struct vmm_mem_slot *find(struct vmm_vm *vm, uint64_t gpa)
{
    uint32_t i;

    for (i = 0; i < VMM_MEM_MAX_SLOTS; i++) {
        struct vmm_mem_slot *s = &vm->mem.slots[i];
        if (s->used && gpa >= s->gpa && gpa < s->gpa + s->size)
            return s;
    }
    return NULL;
}

static enum gpa_status past(struct vmm_vm *vm, struct vmm_mem_slot *s)
{
    return find(vm, s->gpa + s->size) ? GPA_ERR_CROSSES_SLOT : GPA_ERR_PAST_END;
}

static enum gpa_status ok(struct vmm_mem_slot *s, uint64_t gpa, void **hva)
{
    *hva = (uint8_t *)s->hva + (gpa - s->gpa);
    return GPA_OK;
}

/* M1：漏掉整数回绕检查。elf_loader.c 和 bzimage.c 里对文件给出的偏移
 * 做加法时也是同一类错误，见 tests/fuzz。 */
static enum gpa_status m_no_overflow_check(struct vmm_vm *vm, uint64_t gpa,
                                           uint64_t len, void **hva)
{
    struct vmm_mem_slot *s;

    if (len == 0)
        return GPA_ERR_ZERO_LEN;
    s = find(vm, gpa);
    if (!s)
        return GPA_ERR_UNMAPPED;
    if (gpa + len > s->gpa + s->size)
        return past(vm, s);
    return ok(s, gpa, hva);
}

/* M2：上界差一，多放行一个字节 */
static enum gpa_status m_off_by_one_end(struct vmm_vm *vm, uint64_t gpa,
                                        uint64_t len, void **hva)
{
    struct vmm_mem_slot *s;

    if (len == 0)
        return GPA_ERR_ZERO_LEN;
    if (gpa + len < gpa)
        return GPA_ERR_WRAP;
    s = find(vm, gpa);
    if (!s)
        return GPA_ERR_UNMAPPED;
    if (gpa + len > s->gpa + s->size + 1)
        return past(vm, s);
    return ok(s, gpa, hva);
}

/* M3：接受 len == 0。契约明写“len 必须 > 0”。 */
static enum gpa_status m_accepts_zero_len(struct vmm_vm *vm, uint64_t gpa,
                                          uint64_t len, void **hva)
{
    struct vmm_mem_slot *s;

    if (gpa + len < gpa)
        return GPA_ERR_WRAP;
    s = find(vm, gpa);
    if (!s)
        return GPA_ERR_UNMAPPED;
    if (gpa + len > s->gpa + s->size)
        return past(vm, s);
    return ok(s, gpa, hva);
}

/* M4：只按起始地址查 slot，完全不看长度。
 * 这是 AGENTS.md「长期 invariant」点名要防的那种写法，也是 virtqueue 最容易
 * 踩的坑：descriptor 的 addr 落在 RAM 里，len 却把范围拖出去。 */
static enum gpa_status m_ignores_length(struct vmm_vm *vm, uint64_t gpa,
                                        uint64_t len, void **hva)
{
    struct vmm_mem_slot *s;

    if (len == 0)
        return GPA_ERR_ZERO_LEN;
    s = find(vm, gpa);
    if (!s)
        return GPA_ERR_UNMAPPED;
    return ok(s, gpa, hva);
}

/* M5：偏移算错，返回 slot 起点而不是 slot 起点加偏移 */
static enum gpa_status m_wrong_offset(struct vmm_vm *vm, uint64_t gpa,
                                      uint64_t len, void **hva)
{
    struct vmm_mem_slot *s;

    if (len == 0)
        return GPA_ERR_ZERO_LEN;
    if (gpa + len < gpa)
        return GPA_ERR_WRAP;
    s = find(vm, gpa);
    if (!s)
        return GPA_ERR_UNMAPPED;
    if (gpa + len > s->gpa + s->size)
        return past(vm, s);
    *hva = s->hva;
    return GPA_OK;
}

/* M6：长度被截成 32 位。大 len 会被截成小值从而通过上界检查。 */
static enum gpa_status m_len_truncated_32(struct vmm_vm *vm, uint64_t gpa,
                                          uint64_t len, void **hva)
{
    struct vmm_mem_slot *s;
    uint64_t l = (uint32_t)len;

    if (l == 0)
        return GPA_ERR_ZERO_LEN;
    if (gpa + l < gpa)
        return GPA_ERR_WRAP;
    s = find(vm, gpa);
    if (!s)
        return GPA_ERR_UNMAPPED;
    if (gpa + l > s->gpa + s->size)
        return past(vm, s);
    return ok(s, gpa, hva);
}

/* M7：相邻 slot 被当成连续内存，允许跨界。
 * HVA 上两个 slot 毫无关系，跨界返回的指针后半段指向别人的内存。
 * mem_init() 的低段和高段恰好 HVA 连续，但 GPA 不相接，所以这种写法在
 * 真实布局上暂时不出事，一旦出现 GPA 相接的 slot 就是越界读写。 */
static enum gpa_status m_merges_adjacent(struct vmm_vm *vm, uint64_t gpa,
                                         uint64_t len, void **hva)
{
    struct vmm_mem_slot *s;
    uint64_t end;

    if (len == 0)
        return GPA_ERR_ZERO_LEN;
    if (gpa + len < gpa)
        return GPA_ERR_WRAP;
    s = find(vm, gpa);
    if (!s)
        return GPA_ERR_UNMAPPED;
    end = s->gpa + s->size;
    /* 把紧邻的后继 slot 也算进可用范围 */
    for (;;) {
        struct vmm_mem_slot *n = find(vm, end);
        if (!n)
            break;
        end = n->gpa + n->size;
    }
    if (gpa + len > end)
        return GPA_ERR_PAST_END;
    return ok(s, gpa, hva);
}

/* M8：拒绝是对的，原因报错了：跨进相邻 slot 也报成 PAST_END。
 * 日志会把“调用方忘了拆分”说成“长度算错”，排查方向就偏了。 */
static enum gpa_status m_wrong_reason(struct vmm_vm *vm, uint64_t gpa,
                                      uint64_t len, void **hva)
{
    struct vmm_mem_slot *s;

    if (len == 0)
        return GPA_ERR_ZERO_LEN;
    if (gpa + len < gpa)
        return GPA_ERR_WRAP;
    s = find(vm, gpa);
    if (!s)
        return GPA_ERR_UNMAPPED;
    if (gpa + len > s->gpa + s->size)
        return GPA_ERR_PAST_END;
    return ok(s, gpa, hva);
}

/* M9：拒绝时把 *hva 清成 NULL。契约写明拒绝时 *hva 不动，调用方可以
 * 依赖这一点保留原值；更要紧的是，写出参数的实现往往也会在别的分支里
 * 写出一个没检查完的指针。 */
static enum gpa_status m_clobbers_out(struct vmm_vm *vm, uint64_t gpa,
                                      uint64_t len, void **hva)
{
    struct vmm_mem_slot *s;

    *hva = NULL;
    if (len == 0)
        return GPA_ERR_ZERO_LEN;
    if (gpa + len < gpa)
        return GPA_ERR_WRAP;
    s = find(vm, gpa);
    if (!s)
        return GPA_ERR_UNMAPPED;
    if (gpa + len > s->gpa + s->size)
        return past(vm, s);
    return ok(s, gpa, hva);
}

const struct mem_mutant mem_mutants[] = {
    { "no-overflow-check", "gpa + len 回绕时必须拒绝",           m_no_overflow_check },
    { "off-by-one-end",    "[gpa,gpa+len) 必须完整落在 slot 内", m_off_by_one_end    },
    { "accepts-zero-len",  "len 必须大于 0",                     m_accepts_zero_len  },
    { "ignores-length",    "长度必须参与边界判定",               m_ignores_length    },
    { "wrong-offset",      "返回值必须是 slot 起点加偏移",       m_wrong_offset      },
    { "len-truncated-32",  "长度必须按 64 位处理",               m_len_truncated_32  },
    { "merges-adjacent",   "跨 slot 的访问一律非法",             m_merges_adjacent   },
    { "wrong-reason",      "跨进相邻 slot 必须报 CROSSES_SLOT",  m_wrong_reason      },
    { "clobbers-out",      "拒绝时不得改动 *hva",                m_clobbers_out      },
};

const int mem_mutants_count = (int)(sizeof(mem_mutants) / sizeof(mem_mutants[0]));
