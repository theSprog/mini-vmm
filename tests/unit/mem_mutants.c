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

/* 真实实现（对照用）：len>0、无回绕、[gpa,gpa+len) 完整落在同一 slot 内 */
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

/* M1：漏掉整数回绕检查。就是 elf_loader.c 里刚修掉的那一类错误。 */
static void *m_no_overflow_check(struct vmm_vm *vm, uint64_t gpa, uint64_t len)
{
    struct vmm_mem_slot *s;

    if (len == 0)
        return NULL;
    s = find(vm, gpa);
    if (!s)
        return NULL;
    if (gpa + len > s->gpa + s->size)
        return NULL;
    return (uint8_t *)s->hva + (gpa - s->gpa);
}

/* M2：上界差一，多放行一个字节 */
static void *m_off_by_one_end(struct vmm_vm *vm, uint64_t gpa, uint64_t len)
{
    struct vmm_mem_slot *s;

    if (len == 0)
        return NULL;
    if (gpa + len < gpa)
        return NULL;
    s = find(vm, gpa);
    if (!s)
        return NULL;
    if (gpa + len > s->gpa + s->size + 1)
        return NULL;
    return (uint8_t *)s->hva + (gpa - s->gpa);
}

/* M3：接受 len == 0。契约明写“len 必须 > 0”。 */
static void *m_accepts_zero_len(struct vmm_vm *vm, uint64_t gpa, uint64_t len)
{
    struct vmm_mem_slot *s;

    if (gpa + len < gpa)
        return NULL;
    s = find(vm, gpa);
    if (!s)
        return NULL;
    if (gpa + len > s->gpa + s->size)
        return NULL;
    return (uint8_t *)s->hva + (gpa - s->gpa);
}

/* M4：只按起始地址查 slot，完全不看长度。
 * 这是 AGENT_GUIDE 第 4 节点名要防的那种写法，也是 virtqueue 最容易
 * 踩的坑：descriptor 的 addr 落在 RAM 里，len 却把范围拖出去。 */
static void *m_ignores_length(struct vmm_vm *vm, uint64_t gpa, uint64_t len)
{
    struct vmm_mem_slot *s;

    if (len == 0)
        return NULL;
    s = find(vm, gpa);
    if (!s)
        return NULL;
    return (uint8_t *)s->hva + (gpa - s->gpa);
}

/* M5：偏移算错，返回 slot 起点而不是 slot 起点加偏移 */
static void *m_wrong_offset(struct vmm_vm *vm, uint64_t gpa, uint64_t len)
{
    struct vmm_mem_slot *s;

    if (len == 0)
        return NULL;
    if (gpa + len < gpa)
        return NULL;
    s = find(vm, gpa);
    if (!s)
        return NULL;
    if (gpa + len > s->gpa + s->size)
        return NULL;
    return s->hva;
}

/* M6：长度被截成 32 位。大 len 会被截成小值从而通过上界检查。 */
static void *m_len_truncated_32(struct vmm_vm *vm, uint64_t gpa, uint64_t len)
{
    struct vmm_mem_slot *s;
    uint64_t l = (uint32_t)len;

    if (l == 0)
        return NULL;
    if (gpa + l < gpa)
        return NULL;
    s = find(vm, gpa);
    if (!s)
        return NULL;
    if (gpa + l > s->gpa + s->size)
        return NULL;
    return (uint8_t *)s->hva + (gpa - s->gpa);
}

/* M7：相邻 slot 被当成连续内存，允许跨界。
 * HVA 上两个 slot 毫无关系，跨界返回的指针后半段指向别人的内存。 */
static void *m_merges_adjacent(struct vmm_vm *vm, uint64_t gpa, uint64_t len)
{
    struct vmm_mem_slot *s;
    uint64_t end;

    if (len == 0)
        return NULL;
    if (gpa + len < gpa)
        return NULL;
    s = find(vm, gpa);
    if (!s)
        return NULL;
    end = s->gpa + s->size;
    /* 把紧邻的后继 slot 也算进可用范围 */
    for (;;) {
        struct vmm_mem_slot *n = find(vm, end);
        if (!n)
            break;
        end = n->gpa + n->size;
    }
    if (gpa + len > end)
        return NULL;
    return (uint8_t *)s->hva + (gpa - s->gpa);
}

const struct mem_mutant mem_mutants[] = {
    { "no-overflow-check", "gpa + len 回绕时必须拒绝",           m_no_overflow_check },
    { "off-by-one-end",    "[gpa,gpa+len) 必须完整落在 slot 内", m_off_by_one_end    },
    { "accepts-zero-len",  "len 必须大于 0",                     m_accepts_zero_len  },
    { "ignores-length",    "长度必须参与边界判定",               m_ignores_length    },
    { "wrong-offset",      "返回值必须是 slot 起点加偏移",       m_wrong_offset      },
    { "len-truncated-32",  "长度必须按 64 位处理",               m_len_truncated_32  },
    { "merges-adjacent",   "跨 slot 的访问一律非法",             m_merges_adjacent   },
};

const int mem_mutants_count = (int)(sizeof(mem_mutants) / sizeof(mem_mutants[0]));
