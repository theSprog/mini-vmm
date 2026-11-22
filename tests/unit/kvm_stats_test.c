/* tests/unit/kvm_stats_test.c — KVM_GET_STATS_FD 文件解析（src/kvm_stats.c）
 *
 * 判据来自 Documentation/virt/kvm/api.rst 4.133 节对文件格式的规定：
 *   - header 在 offset 0，其余三块的位置只由 header 里的 offset 决定，
 *     “不保证相邻，也不保证按 header/id/desc/data 的顺序”；
 *   - 每个 desc 后面紧跟 name_size 字节的 name；
 *   - 值在 data_offset + desc.offset 处，desc.size 个 u64。
 * 真实内核给出的文件恰好是按顺序紧挨着的，只用真 fd 测不出“按位置猜”
 * 的错误实现，所以这里用 memfd 按规定允许的其他布局造文件：块的顺序
 * 倒过来、块之间留空、name_size 不是 8 的倍数（desc 在内存里不对齐）。
 *
 * 损坏的文件（offset 越界、name 不以 '\0' 结尾、数量离谱）必须被拒绝，
 * 而且 ASan 下不能有越界读。真实 fd 的读取由 e2e exit-stats 覆盖。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/kvm.h>

#include "vmm.h"
#include "kvm_stats.h"
#include "harness.h"

/* 单次分配超过 64 MiB 就让 malloc 返回 NULL。损坏的 desc.offset 能让
 * 数据块长度接近 4 GiB；没有上限检查的实现会先分配这么大一块，再因为
 * 文件不够长返回 INVAL，结果码一样，测不出来。限住分配之后它会变成
 * NOMEM，和期望的 INVAL 不符。环境变量 ASAN_OPTIONS 里没写这两项时
 * 以这里为准。 */
const char *__asan_default_options(void);
const char *__asan_default_options(void)
{
    return "max_allocation_size_mb=64:allocator_may_return_null=1";
}

int vmm_log_level = -1;

void vmm_log(int level, const char *file, int line, const char *fmt, ...)
{
    (void)level; (void)file; (void)line; (void)fmt;
}

struct ent {
    const char *name;
    uint32_t    flags;
    uint16_t    size;
    uint32_t    offset;     /* 数据块内的偏移 */
    uint64_t    value;      /* size 个值都填这个 */
    bool        nodata;     /* 不写值：desc 指向文件之外 */
};

struct layout {
    uint32_t name_size;
    uint32_t id_off, desc_off, data_off;
    uint32_t num_desc;      /* 0 表示用 n_ents */
    const struct ent *ents;
    unsigned n_ents;
    bool     unterminated;  /* 第一个 name 填满 name_size，不留 '\0' */
};

static void pwrite_at(int fd, const void *p, size_t n, uint64_t off)
{
    if (pwrite(fd, p, n, (off_t)off) != (ssize_t)n) {
        perror("pwrite");
        _exit(2);
    }
}

/* 按 layout 造一个统计文件，返回 fd */
static int build(const struct layout *l)
{
    struct kvm_stats_header h = {
        .flags = 0, .name_size = l->name_size,
        .num_desc = l->num_desc ? l->num_desc : l->n_ents,
        .id_offset = l->id_off, .desc_offset = l->desc_off,
        .data_offset = l->data_off,
    };
    size_t stride = sizeof(struct kvm_stats_desc) + l->name_size;
    char id[256];
    unsigned i, k;
    int fd = memfd_create("kvm-stats-test", MFD_CLOEXEC);

    if (fd < 0) {
        perror("memfd_create");
        _exit(2);
    }
    pwrite_at(fd, &h, sizeof(h), 0);
    memset(id, 0, sizeof(id));
    snprintf(id, sizeof(id), "kvm-test/vcpu-0");
    pwrite_at(fd, id, l->name_size, l->id_off);

    for (i = 0; i < l->n_ents; i++) {
        const struct ent *e = &l->ents[i];
        struct kvm_stats_desc d = {
            .flags = e->flags, .exponent = 0, .size = e->size,
            .offset = e->offset, .bucket_size = 0,
        };
        char name[256];

        memset(name, 0, sizeof(name));
        if (i == 0 && l->unterminated)
            memset(name, 'x', l->name_size);
        else
            snprintf(name, sizeof(name), "%s", e->name);
        pwrite_at(fd, &d, sizeof(d), l->desc_off + stride * i);
        pwrite_at(fd, name, l->name_size,
                  l->desc_off + stride * i + sizeof(d));
        for (k = 0; k < e->size && !e->nodata; k++)
            pwrite_at(fd, &e->value, 8,
                      (uint64_t)l->data_off + e->offset + 8 * k);
    }
    return fd;
}

#define HIST (KVM_STATS_TYPE_LOG_HIST | KVM_STATS_UNIT_SECONDS)
#define CUM  (KVM_STATS_TYPE_CUMULATIVE | KVM_STATS_UNIT_NONE)
#define INST (KVM_STATS_TYPE_INSTANT | KVM_STATS_UNIT_NONE)

static const struct ent g_ents[] = {
    { "pages_4k",       INST, 1,  0, 123, false },
    { "io_exits",       CUM,  1,  8, 3,   false },
    { "halt_wait_hist", HIST, 4, 24, 9,   false },
    { "exits",          CUM,  1, 16, 7,   false },
};
#define N_ENTS (sizeof(g_ents) / sizeof(g_ents[0]))

static void check_parsed(const struct kvm_stats *st)
{
    const struct kvm_stat *s;

    CHECK_U64(st->n, 3, "single-value stats kept");
    CHECK_U64(st->skipped, 1, "histogram skipped");
    if (st->n != 3)
        return;
    /* 顺序跟 desc 块一致 */
    CHECK(!strcmp(st->v[0].name, "pages_4k"), "v[0] is %s", st->v[0].name);
    CHECK(!strcmp(st->v[1].name, "io_exits"), "v[1] is %s", st->v[1].name);
    CHECK(!strcmp(st->v[2].name, "exits"), "v[2] is %s", st->v[2].name);
    s = kvm_stats_find(st, "exits");
    CHECK(s != NULL, "exits found");
    if (s) {
        CHECK_U64(s->value, 7, "exits value read at data_offset + 16");
        CHECK_U64(s->flags, CUM, "flags preserved");
    }
    s = kvm_stats_find(st, "pages_4k");
    CHECK(s && s->value == 123, "pages_4k value read at data_offset + 0");
    s = kvm_stats_find(st, "io_exits");
    CHECK(s && s->value == 3, "io_exits value read at data_offset + 8");
    CHECK_PTR(kvm_stats_find(st, "halt_wait_hist"), NULL,
              "histogram is not reported as a single value");
}

/* 和真实内核一样的布局：header、id、desc、data 依次紧挨 */
static void case_kernel_order(void)
{
    struct layout l = { .name_size = 48, .ents = g_ents, .n_ents = N_ENTS };
    struct kvm_stats st;
    int fd;

    T_CASE("blocks in header/id/desc/data order");
    l.id_off   = sizeof(struct kvm_stats_header);
    l.desc_off = l.id_off + l.name_size;
    l.data_off = l.desc_off + (sizeof(struct kvm_stats_desc) + 48) * N_ENTS;
    fd = build(&l);
    CHECK_U64(kvm_stats_read(fd, &st), VMM_OK, "read");
    check_parsed(&st);
    kvm_stats_free(&st);
    close(fd);
}

/* api.rst：块不保证相邻、不保证顺序。数据块放最前，desc 在中间，id 在
 * 最后，块之间留空。按“紧跟在前一块后面”去猜位置的实现在这里读错。 */
static void case_reordered(void)
{
    struct layout l = { .name_size = 48, .ents = g_ents, .n_ents = N_ENTS,
                        .data_off = 0x40, .desc_off = 0x200, .id_off = 0x800 };
    struct kvm_stats st;
    int fd;

    T_CASE("blocks reordered with gaps");
    fd = build(&l);
    CHECK_U64(kvm_stats_read(fd, &st), VMM_OK, "read");
    check_parsed(&st);
    kvm_stats_free(&st);
    close(fd);
}

/* name_size = 21：desc 的步长 37，不是 8 的倍数，后面的 desc 在缓冲区
 * 里不对齐。直接把指针强转成 struct kvm_stats_desc * 会被 UBSan 拦下。 */
static void case_odd_name_size(void)
{
    struct layout l = { .name_size = 21, .ents = g_ents, .n_ents = N_ENTS,
                        .data_off = 0x100, .desc_off = 0x18, .id_off = 0x400 };
    struct kvm_stats st;
    int fd;

    T_CASE("name_size not a multiple of 8");
    fd = build(&l);
    CHECK_U64(kvm_stats_read(fd, &st), VMM_OK, "read");
    check_parsed(&st);
    kvm_stats_free(&st);
    close(fd);
}

static void expect_reject(const char *what, const struct layout *l, int want)
{
    struct kvm_stats st;
    int fd = build(l);

    memset(&st, 0xA5, sizeof(st));
    CHECK_U64(kvm_stats_read(fd, &st), (uint64_t)(int64_t)want,
              "%s is rejected", what);
    CHECK(st.v == NULL && st.names == NULL && st.n == 0,
          "%s: result is zeroed on failure", what);
    close(fd);
}

static void case_malformed(void)
{
    struct layout base = { .name_size = 48, .ents = g_ents, .n_ents = N_ENTS,
                           .data_off = 0x40, .desc_off = 0x200,
                           .id_off = 0x800 };
    struct ent bad[2] = { g_ents[0], g_ents[1] };
    struct layout l;

    T_CASE("malformed files are rejected");

    l = base; l.unterminated = true;
    expect_reject("name without '\\0'", &l, VMM_ERR_INVAL);

    l = base; l.name_size = 0; l.n_ents = 0; l.num_desc = 1;
    expect_reject("name_size 0", &l, VMM_ERR_INVAL);

    l = base; l.num_desc = 100000;
    expect_reject("num_desc 100000", &l, VMM_ERR_INVAL);

    /* 声明 10 个 desc，文件里只有 4 个：desc 块读到文件尾 */
    l = base; l.desc_off = 0x900; l.num_desc = 10; l.id_off = 0x40;
    l.data_off = 0x80;
    expect_reject("desc block past end of file", &l, VMM_ERR_INVAL);

    bad[1].nodata = true;
    bad[1].offset = 0x8000;      /* 在上限之内，但文件没那么长 */
    l = base; l.ents = bad; l.n_ents = 2;
    expect_reject("data past end of file", &l, VMM_ERR_INVAL);

    bad[1].offset = 0xFFFFFFF0u; /* 超过数据块上限，不能先去 malloc */
    expect_reject("data offset near 4 GiB", &l, VMM_ERR_INVAL);

    bad[1].offset = 8;
    bad[1].size = 0;
    bad[1].nodata = false;
    expect_reject("size 0", &l, VMM_ERR_INVAL);
}

static void case_bad_fd(void)
{
    struct kvm_stats st;

    T_CASE("pread failure is a system error");
    CHECK_U64(kvm_stats_read(-1, &st), (uint64_t)(int64_t)VMM_ERR_SYS,
              "fd -1");
    CHECK(st.v == NULL && st.n == 0, "result is zeroed");
}

int main(int argc, char **argv)
{
    T_MAIN_BEGIN();
    case_kernel_order();
    case_reordered();
    case_odd_name_size();
    case_malformed();
    case_bad_fd();
    return T_SUMMARY();
}
