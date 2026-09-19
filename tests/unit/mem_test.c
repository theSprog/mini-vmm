/* tests/unit/mem_test.c — guest 内存模型与 GPA→HVA 边界契约  [P1b]
 *
 * 验的是 AGENT_GUIDE 第 4 节的头号 invariant：
 *   “任何 Guest 提供的地址范围都必须经过带检查的 GPA→HVA helper，
 *    并验证 integer overflow / 是否完整落在合法 region / length 是否有效。”
 * Step 3 的 virtqueue 每一个 descriptor segment 都要过 gpa_to_hva()，
 * 这里先把它的语义钉死。
 *
 * oracle 不是“当前实现返回什么”，而是 include/memory.h 写明的契约，
 * 由本文件里一个独立写出的模型 model_lookup() 表达：
 *   - 用 unsigned __int128 做区间运算，结构上不需要溢出保护，
 *     因此不会和被测实现共享同一个错误假设；
 *   - 判定方式也不同：实现是“先按 gpa 找 slot，再查上界”，
 *     模型是“对每个 slot 直接做一次完整包含判定”。
 * 两者在同一批输入上必须逐点一致。
 *
 * 不碰 /dev/kvm：kvm_set_user_memory_region() 在本文件里被桩掉。
 *
 * 用法：build/tests/mem_test [iterations]   默认随机轮数 200000
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "vmm.h"
#include "memory.h"
#include "kvm_wrappers.h"
#include "harness.h"
#include "mutants.h"

/* ------------------------------------------------------------------ */
/* 桩                                                                  */
/* ------------------------------------------------------------------ */

int vmm_log_level = -1;          /* 默认全部静音，-v 时打开 */

void vmm_log(int level, const char *file, int line, const char *fmt, ...)
{
    va_list ap;

    (void)file; (void)line;
    if (level > vmm_log_level)
        return;
    va_start(ap, fmt);
    fprintf(stderr, "       [vmm] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* KVM_SET_USER_MEMORY_REGION 的桩：记录调用，可注入失败 */
static int      g_kvm_calls;
static int      g_kvm_fail_next;   /* >0 时下一次调用失败 */
static uint32_t g_kvm_last_slot;
static uint64_t g_kvm_last_gpa, g_kvm_last_size;

int kvm_set_user_memory_region(int vm_fd, uint32_t slot, uint32_t flags,
                               uint64_t gpa, uint64_t size, uint64_t hva)
{
    (void)vm_fd; (void)flags; (void)hva;
    g_kvm_calls++;
    g_kvm_last_slot = slot;
    g_kvm_last_gpa  = gpa;
    g_kvm_last_size = size;
    if (g_kvm_fail_next) {
        g_kvm_fail_next--;
        return VMM_ERR_SYS;
    }
    return VMM_OK;
}

/* 被测函数走一层指针：默认是真实实现，--mutant N 时换成
 * tests/unit/mem_mutants.c 里故意写错的那一份。negative control 用，
 * 见 mutants.h 开头的说明。 */
static gpa_to_hva_fn g_uut = gpa_to_hva;

/* ------------------------------------------------------------------ */
/* 独立模型                                                            */
/* ------------------------------------------------------------------ */

struct model_slot {
    uint64_t gpa;
    uint64_t size;
    uint8_t *hva;
};

#define MODEL_MAX 8
static struct model_slot g_model[MODEL_MAX];
static int               g_model_n;

static void model_add(uint64_t gpa, uint64_t size, void *hva)
{
    g_model[g_model_n].gpa  = gpa;
    g_model[g_model_n].size = size;
    g_model[g_model_n].hva  = hva;
    g_model_n++;
}

/* 契约的直接翻译：len > 0，且 [gpa, gpa+len) 完整落在某一个 slot 内。
 * 128 位区间运算，不需要任何溢出特判——这正是它能当 oracle 的原因。 */
static uint8_t *model_lookup(uint64_t gpa, uint64_t len)
{
    __uint128_t beg = gpa, end = (__uint128_t)gpa + (__uint128_t)len;
    int i;

    if (len == 0)
        return NULL;
    for (i = 0; i < g_model_n; i++) {
        __uint128_t sbeg = g_model[i].gpa;
        __uint128_t send = (__uint128_t)g_model[i].gpa + g_model[i].size;

        if (beg >= sbeg && end <= send)
            return g_model[i].hva + (gpa - g_model[i].gpa);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 被测对象                                                            */
/* ------------------------------------------------------------------ */

static struct vmm_vm g_vm;

/* 布局刻意包含四种关系：
 *   S0/S1 紧邻（end == base）—— 跨界必须拒绝，哪怕 GPA 连续
 *   S1/S2 之间有空洞
 *   S2    大 slot
 *   S3    高地址，走 64 位算术路径
 * 不放“结束于 2^64”的 slot：那种布局现实中不存在，而实现里
 * s->gpa + s->size 会回绕成 0，见 TEST_DESIGN.md 的 residual risk。 */
#define S0_GPA  0x000000000ULL
#define S0_SIZE 0x000100000ULL          /* 1 MiB */
#define S1_GPA  0x000100000ULL          /* 紧邻 S0 */
#define S1_SIZE 0x000100000ULL
#define S2_GPA  0x040000000ULL          /* 1 GiB，中间有空洞 */
#define S2_SIZE 0x000200000ULL
#define S3_GPA  0xFF00000000ULL         /* 1 TiB 附近 */
#define S3_SIZE 0x000010000ULL

static void *anon_map(size_t size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mmap %zu failed\n", size);
        exit(2);
    }
    return p;
}

static void setup_slots(void)
{
    void *p;
    int r;

    memset(&g_vm, 0, sizeof(g_vm));
    g_vm.vm_fd = -1;
    g_model_n = 0;

    /* S0 走 mem_init（它自己 mmap），其余走 mem_add_region */
    r = mem_init(&g_vm, S0_SIZE);
    CHECK_U64(r, VMM_OK, "mem_init should succeed");
    model_add(S0_GPA, S0_SIZE, g_vm.mem.slots[0].hva);

    p = anon_map(S1_SIZE);
    r = mem_add_region(&g_vm, S1_GPA, S1_SIZE, p, 0, false);
    CHECK_U64(r, VMM_OK, "S1 (adjacent to S0) should register");
    model_add(S1_GPA, S1_SIZE, p);

    p = anon_map(S2_SIZE);
    r = mem_add_region(&g_vm, S2_GPA, S2_SIZE, p, 0, false);
    CHECK_U64(r, VMM_OK, "S2 should register");
    model_add(S2_GPA, S2_SIZE, p);

    p = anon_map(S3_SIZE);
    r = mem_add_region(&g_vm, S3_GPA, S3_SIZE, p, 0, false);
    CHECK_U64(r, VMM_OK, "S3 (high GPA) should register");
    model_add(S3_GPA, S3_SIZE, p);

    CHECK_U64(g_vm.mem.nr_slots, 4, "four slots registered");
}

/* 逐点比对：实现必须和模型给出同一个答案（NULL 与否，以及具体指针） */
static void cmp_one(uint64_t gpa, uint64_t len)
{
    void    *got  = g_uut(&g_vm, gpa, len);
    uint8_t *want = model_lookup(gpa, len);

    t_checks++;
    if (got != want) {
        t_fails++; t_case_fails++;
        printf("  FAIL %s\n       gpa=0x%llx len=0x%llx\n"
               "       impl  %p\n       model %p\n",
               t_case_name, (unsigned long long)gpa,
               (unsigned long long)len, got, (void *)want);
    }
}

/* ------------------------------------------------------------------ */

static const uint64_t g_lens[] = {
    0, 1, 2, 3, 4, 7, 8, 16, 64, 4095, 4096, 4097,
    0x10000, 0x100000, 0x100001, 0x200000,
    /* 低 32 位很小、高 32 位非零：任何把 len 截成 32 位的实现都会
     * 把这些当成合法的小长度放行。这一类是 negctl 的 len-truncated-32
     * mutant 逼出来的——原来的长度表里一个都没有，它当时溜过去了。 */
    0x100000000ULL, 0x100000001ULL, 0x100000008ULL, 0x100001000ULL,
    0x1FFFFF000ULL, 0x8000000000001000ULL, 0xFFFFFFFF00001000ULL,
    0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL,
    0xFFFFFFFFFFFFFFFEULL, 0xFFFFFFFFFFFFFFFFULL,
};
#define N_LENS (sizeof(g_lens) / sizeof(g_lens[0]))

static void case_boundary_grid(void)
{
    static const uint64_t bases[] = { S0_GPA, S1_GPA, S2_GPA, S3_GPA };
    static const uint64_t sizes[] = { S0_SIZE, S1_SIZE, S2_SIZE, S3_SIZE };
    static const int64_t  deltas[] = {
        -4097, -4096, -1, 0, 1, 4095, 4096,
    };
    size_t b, d, l;

    T_CASE("slot 边界网格：起点/终点各 +-1 页与 20 种长度");
    for (b = 0; b < 4; b++) {
        uint64_t beg = bases[b], end = bases[b] + sizes[b];

        for (d = 0; d < sizeof(deltas) / sizeof(deltas[0]); d++) {
            for (l = 0; l < N_LENS; l++) {
                cmp_one(beg + (uint64_t)deltas[d], g_lens[l]);
                cmp_one(end + (uint64_t)deltas[d], g_lens[l]);
            }
        }
    }
}

static void case_overflow(void)
{
    T_CASE("整数回绕：gpa + len 溢出必须拒绝而不是绕过上界检查");

    /* 这几条是契约里点名的场景：guest 填一个接近 2^64 的 GPA，
     * 让 gpa + len 回绕成小数值，从而“看起来”落在低地址 slot 里。 */
    cmp_one(0xFFFFFFFFFFFFFFFFULL, 1);
    cmp_one(0xFFFFFFFFFFFFFFFFULL, 2);
    cmp_one(0xFFFFFFFFFFFFFF00ULL, 0x200);
    cmp_one(0xFFFFFFFFFFF00000ULL, 0x100001);
    cmp_one(0xFFFFFFFF00000000ULL, 0xFFFFFFFF00000001ULL);
    /* 回绕到正好落在 S0 里 */
    cmp_one(0xFFFFFFFFFFFFF000ULL, 0x1000 + S0_SIZE / 2);
    /* len 本身巨大 */
    cmp_one(S0_GPA, 0xFFFFFFFFFFFFFFFFULL);
    cmp_one(S2_GPA, 0xFFFFFFFFFFFFFFFFULL);
    /* gpa=0 len=0 */
    cmp_one(0, 0);

    /* 显式再断言一次结果，避免“模型和实现一起错”这种情况下静默通过 */
    CHECK_PTR(g_uut(&g_vm, 0xFFFFFFFFFFFFFFFFULL, 2), NULL,
              "gpa=2^64-1 len=2 must be rejected");
    CHECK_PTR(g_uut(&g_vm, S0_GPA, 0), NULL, "len=0 must be rejected");
    CHECK_PTR(g_uut(&g_vm, S0_GPA, 0xFFFFFFFFFFFFFFFFULL), NULL,
              "huge len must be rejected");
}

static void case_cross_slot(void)
{
    uint8_t *h0 = g_model[0].hva;

    T_CASE("相邻 slot 之间不得合并：跨界访问一律拒绝");

    /* S0 与 S1 在 GPA 上首尾相接，但它们是两次独立的
     * KVM_SET_USER_MEMORY_REGION，HVA 上毫无关系。
     * 契约写明“跨 slot 的访问一律视为非法（由调用方拆分）”。 */
    CHECK_PTR(g_uut(&g_vm, S0_GPA + S0_SIZE - 8, 8), h0 + S0_SIZE - 8,
              "刚好贴住 S0 末尾的 8 字节访问应当合法");
    CHECK_PTR(g_uut(&g_vm, S0_GPA + S0_SIZE - 8, 9), NULL,
              "跨过 S0/S1 边界 1 字节就必须拒绝");
    CHECK_PTR(g_uut(&g_vm, S0_GPA + S0_SIZE - 1, 2), NULL,
              "跨 S0/S1 边界的 2 字节访问必须拒绝");
    CHECK_PTR(g_uut(&g_vm, S0_GPA, S0_SIZE + 1), NULL,
              "从 S0 起点跨到 S1 必须拒绝");
    CHECK_PTR(g_uut(&g_vm, S1_GPA, S1_SIZE), g_model[1].hva,
              "整个 S1 应当合法");

    /* 空洞：S1 末尾到 S2 起点之间没有内存 */
    CHECK_PTR(g_uut(&g_vm, S1_GPA + S1_SIZE, 1), NULL,
              "空洞里的地址必须拒绝");
    CHECK_PTR(g_uut(&g_vm, S2_GPA - 1, 1), NULL,
              "S2 起点前一字节必须拒绝");
}

/* xorshift64*：固定种子，失败可原样复现 */
static uint64_t g_seed = 0x2545F4914F6CDD1DULL;

static uint64_t rnd(void)
{
    g_seed ^= g_seed >> 12;
    g_seed ^= g_seed << 25;
    g_seed ^= g_seed >> 27;
    return g_seed * 0x2545F4914F6CDD1DULL;
}

static void case_random(unsigned long iters)
{
    static const uint64_t anchors[] = {
        S0_GPA, S0_GPA + S0_SIZE, S1_GPA, S1_GPA + S1_SIZE,
        S2_GPA, S2_GPA + S2_SIZE, S3_GPA, S3_GPA + S3_SIZE,
        0, 0xFFFFFFFFFFFFFFFFULL,
    };
    unsigned long i;

    T_CASE("随机输入与模型逐点一致");
    printf("       seed=0x%016llx iters=%lu\n",
           (unsigned long long)0x2545F4914F6CDD1DULL, iters);

    for (i = 0; i < iters; i++) {
        uint64_t gpa, len;
        uint64_t r = rnd();

        /* 三种分布混合：纯随机、锚点附近的小扰动、页对齐 */
        switch (r & 3) {
        case 0:
            gpa = rnd();
            break;
        case 1:
            gpa = anchors[rnd() % (sizeof(anchors) / sizeof(anchors[0]))]
                  + (uint64_t)(int64_t)((int32_t)(rnd() & 0xFFFF) - 0x8000);
            break;
        case 2:
            gpa = anchors[rnd() % (sizeof(anchors) / sizeof(anchors[0]))]
                  + ((rnd() & 0xFF) * 4096);
            break;
        default:
            gpa = rnd() & 0xFFFFFFFFULL;
            break;
        }

        switch (rnd() & 7) {
        case 0:  len = rnd(); break;
        case 1:  len = rnd() & 0xFFFF; break;
        case 2:  len = g_lens[rnd() % N_LENS]; break;
        /* 高 32 位随机非零、低 32 位小：专打 32 位截断 */
        case 3:  len = ((rnd() | 1) << 32) | (rnd() & 0xFFFF); break;
        default: len = (rnd() & 0xF) + 1; break;
        }

        cmp_one(gpa, len);
    }
}

static void case_read_write(void)
{
    uint8_t  src[64], dst[64 + 16];
    uint64_t gpa = S2_GPA + 0x1000;
    int      r;
    size_t   i;

    T_CASE("mem_read / mem_write 的越界行为");

    for (i = 0; i < sizeof(src); i++)
        src[i] = (uint8_t)(i * 7 + 1);

    r = mem_write(&g_vm, gpa, src, sizeof(src));
    CHECK_U64(r, VMM_OK, "合法写应当成功");
    memset(dst, 0xAA, sizeof(dst));
    r = mem_read(&g_vm, gpa, dst, sizeof(src));
    CHECK_U64(r, VMM_OK, "合法读应当成功");
    CHECK(memcmp(src, dst, sizeof(src)) == 0, "写进去再读出来必须一致");
    for (i = sizeof(src); i < sizeof(dst); i++)
        CHECK_U64(dst[i], 0xAA, "mem_read 不得写出目标缓冲区之外");

    /* 越界：必须返回 VMM_ERR_GUEST，并且一个字节都不许拷 */
    memset(dst, 0xAA, sizeof(dst));
    r = mem_read(&g_vm, S2_GPA + S2_SIZE - 4, dst, 64);
    CHECK_U64(r, VMM_ERR_GUEST, "跨过 slot 末尾的读必须被拒绝");
    for (i = 0; i < sizeof(dst); i++)
        CHECK_U64(dst[i], 0xAA, "被拒绝的 mem_read 不得改动目标缓冲区");

    r = mem_write(&g_vm, S2_GPA + S2_SIZE - 4, src, 64);
    CHECK_U64(r, VMM_ERR_GUEST, "跨过 slot 末尾的写必须被拒绝");

    r = mem_read(&g_vm, 0xFFFFFFFFFFFFFFF0ULL, dst, 64);
    CHECK_U64(r, VMM_ERR_GUEST, "回绕地址的读必须被拒绝");

    r = mem_write(&g_vm, S1_GPA + S1_SIZE - 1, src, 2);
    CHECK_U64(r, VMM_ERR_GUEST, "跨 slot 的写必须被拒绝");
}

static void case_find_slot(void)
{
    T_CASE("mem_find_slot 与 slot 表一致");

    CHECK(mem_find_slot(&g_vm, S0_GPA) != NULL, "S0 起点应当能找到 slot");
    CHECK(mem_find_slot(&g_vm, S0_GPA + S0_SIZE - 1) != NULL,
          "S0 末字节应当能找到 slot");
    CHECK(mem_find_slot(&g_vm, S1_GPA + S1_SIZE) == NULL,
          "空洞里不应当找到 slot");
    CHECK(mem_find_slot(&g_vm, S2_GPA + S2_SIZE) == NULL,
          "S2 末尾之后不应当找到 slot");
    CHECK(mem_find_slot(&g_vm, 0xFFFFFFFFFFFFFFFFULL) == NULL,
          "顶端地址不应当找到 slot");
}

/* 注册路径的参数校验，用一个干净的 vm，避免污染上面的布局 */
static void case_add_region_validation(void)
{
    static struct vmm_vm vm;
    uint8_t *p = anon_map(0x200000);
    int r, i;

    T_CASE("mem_add_region 的参数校验与重叠检测");

    memset(&vm, 0, sizeof(vm));
    vm.vm_fd = -1;

    r = mem_add_region(&vm, 0, 0, p, 0, false);
    CHECK_U64(r, VMM_ERR_INVAL, "size = 0 必须拒绝");
    r = mem_add_region(&vm, 0x1000, 0x1001, p, 0, false);
    CHECK_U64(r, VMM_ERR_INVAL, "size 未页对齐必须拒绝");
    r = mem_add_region(&vm, 0x1001, 0x1000, p, 0, false);
    CHECK_U64(r, VMM_ERR_INVAL, "gpa 未页对齐必须拒绝");
    r = mem_add_region(&vm, 0x1000, 0x1000, p + 1, 0, false);
    CHECK_U64(r, VMM_ERR_INVAL, "hva 未页对齐必须拒绝");
    CHECK_U64(vm.mem.nr_slots, 0, "被拒绝的注册不得占用 slot");

    r = mem_add_region(&vm, 0x100000, 0x10000, p, 0, false);
    CHECK_U64(r, VMM_OK, "合法注册应当成功");

    /* 四种重叠关系都必须被拦下 */
    r = mem_add_region(&vm, 0x100000, 0x10000, p, 0, false);
    CHECK_U64(r, VMM_ERR_INVAL, "完全相同的区间必须拒绝");
    r = mem_add_region(&vm, 0x0F8000, 0x10000, p, 0, false);
    CHECK_U64(r, VMM_ERR_INVAL, "前半重叠必须拒绝");
    r = mem_add_region(&vm, 0x108000, 0x10000, p, 0, false);
    CHECK_U64(r, VMM_ERR_INVAL, "后半重叠必须拒绝");
    r = mem_add_region(&vm, 0x104000, 0x1000, p, 0, false);
    CHECK_U64(r, VMM_ERR_INVAL, "被包含的区间必须拒绝");
    CHECK_U64(vm.mem.nr_slots, 1, "重叠被拒后 slot 数不变");

    /* 首尾相接不算重叠 */
    r = mem_add_region(&vm, 0x110000, 0x10000, p, 0, false);
    CHECK_U64(r, VMM_OK, "紧邻但不重叠的区间应当允许");
    r = mem_add_region(&vm, 0x0F0000, 0x10000, p, 0, false);
    CHECK_U64(r, VMM_OK, "另一侧紧邻的区间也应当允许");
    CHECK_U64(vm.mem.nr_slots, 3, "现在应当有三个 slot");

    /* slot 表耗尽 */
    for (i = 0; i < VMM_MEM_MAX_SLOTS; i++) {
        r = mem_add_region(&vm, 0x1000000ULL + (uint64_t)i * 0x10000,
                           0x10000, p, 0, false);
        if (r != VMM_OK)
            break;
    }
    CHECK_U64(r, VMM_ERR_NOMEM, "slot 表满时必须返回 VMM_ERR_NOMEM");
    CHECK_U64(vm.mem.nr_slots, VMM_MEM_MAX_SLOTS, "满表时槽位应当用满");

    munmap(p, 0x200000);
}

static void case_kvm_failure_rollback(void)
{
    static struct vmm_vm vm;
    uint8_t *p = anon_map(0x10000);
    int r;

    T_CASE("KVM 注册失败时不得留下半成品 slot");

    memset(&vm, 0, sizeof(vm));
    vm.vm_fd = -1;

    g_kvm_fail_next = 1;
    r = mem_add_region(&vm, 0x100000, 0x10000, p, 0, false);
    CHECK_U64(r, VMM_ERR_SYS, "KVM 失败时必须返回 VMM_ERR_SYS");
    CHECK_U64(vm.mem.nr_slots, 0, "失败的注册不得计入 nr_slots");
    CHECK_PTR(mem_find_slot(&vm, 0x100000), NULL,
              "失败的注册不得留下可查到的 slot");
    CHECK_PTR(g_uut(&vm, 0x100000, 1), NULL,
              "失败的注册不得让 gpa_to_hva 返回悬空指针");

    /* 同一个槽位应当能被重新使用 */
    g_kvm_fail_next = 0;
    r = mem_add_region(&vm, 0x100000, 0x10000, p, 0, false);
    CHECK_U64(r, VMM_OK, "失败之后同一区间应当能重新注册");
    CHECK_U64(vm.mem.nr_slots, 1, "重新注册后应当有一个 slot");

    munmap(p, 0x10000);
}

int main(int argc, char **argv)
{
    unsigned long iters = 200000;
    int i;

    T_MAIN_BEGIN();
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) {
            vmm_log_level = 3;
        } else if (!strcmp(argv[i], "--list-mutants")) {
            int m;
            printf("%d\n", mem_mutants_count);
            for (m = 0; m < mem_mutants_count; m++)
                printf("%d %s\t%s\n", m, mem_mutants[m].name,
                       mem_mutants[m].breaks);
            return 0;
        } else if (!strcmp(argv[i], "--mutant") && i + 1 < argc) {
            int m = atoi(argv[++i]);
            if (m < 0 || m >= mem_mutants_count) {
                fprintf(stderr, "mutant %d out of range (0..%d)\n",
                        m, mem_mutants_count - 1);
                return 2;
            }
            g_uut = mem_mutants[m].fn;
            iters = 20000;      /* negative control 不需要跑满 20 万轮 */
            printf("MUTANT %d %s — 违反：%s\n",
                   m, mem_mutants[m].name, mem_mutants[m].breaks);
        } else {
            iters = strtoul(argv[i], NULL, 0);
        }
    }

    setup_slots();
    case_boundary_grid();
    case_overflow();
    case_cross_slot();
    case_random(iters);
    case_read_write();
    case_find_slot();
    case_add_region_validation();
    case_kvm_failure_rollback();

    (void)g_kvm_calls; (void)g_kvm_last_slot;
    (void)g_kvm_last_gpa; (void)g_kvm_last_size;
    return T_SUMMARY();
}
