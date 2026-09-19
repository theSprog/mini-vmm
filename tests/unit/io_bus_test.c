/* tests/unit/io_bus_test.c — IO 总线的注册、查找与分发语义  [P1c]
 *
 * 为什么要单独测这一层：PIO/MMIO 分发是所有设备共用的唯一入口，
 * Step 3 起的每一个 VirtIO transport 都要挂在上面。现在它只被四个
 * 传统设备间接覆盖，而那四个设备恰好都“按 offset 分派、忽略 size”，
 * 把这一层自身的边界行为完全遮住了。
 *
 * 取巧之处（有意为之）：handle_io() / handle_mmio() 在 src/vm.c 里是
 * static，本文件直接 #include 整个 vm.c，从而在不改动生产代码的前提下
 * 调到真实的分发路径。代价是要把 vm.c 引用的外部符号全部桩掉，见下。
 * 换成“把分发逻辑提取成公开函数”会更干净，但那是生产代码改动，
 * 需要单独批准，见 TEST_DESIGN.md 的 open question O2。
 *
 * 不碰 /dev/kvm：所有 kvm_* 都是桩，kvm_run 页由本文件伪造。
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmm.h"
#include "memory.h"
#include "kvm_wrappers.h"
#include "arch/arch.h"
#include "devices/irqchip.h"
#include "harness.h"

/* ------------------------------------------------------------------ */
/* 桩：vm.c 引用到的所有外部符号                                        */
/* ------------------------------------------------------------------ */

int vmm_log_level = -1;

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

int  mem_init(struct vmm_vm *vm, uint64_t sz)   { (void)vm; (void)sz; return VMM_OK; }
void mem_destroy(struct vmm_vm *vm)             { (void)vm; }

int  kvm_open(void)                             { return -1; }
int  kvm_get_api_version(int f)                 { (void)f; return 12; }
int  kvm_check_extension(int f, int c)          { (void)f; (void)c; return 1; }
int  kvm_get_vcpu_mmap_size(int f)              { (void)f; return 4096; }
int  kvm_create_vm(int f)                       { (void)f; return -1; }
int  kvm_create_vcpu(int f, int id)             { (void)f; (void)id; return -1; }
struct kvm_run *kvm_mmap_run(int f, size_t s)   { (void)f; (void)s; return NULL; }
int  kvm_get_regs(int f, struct kvm_regs *r)    { (void)f; (void)r; return VMM_ERR_SYS; }
int  kvm_set_regs(int f, const struct kvm_regs *r)   { (void)f; (void)r; return VMM_OK; }
int  kvm_get_sregs(int f, struct kvm_sregs *s)  { (void)f; (void)s; return VMM_ERR_SYS; }
int  kvm_set_sregs(int f, const struct kvm_sregs *s) { (void)f; (void)s; return VMM_OK; }
int  kvm_run(int f)                             { (void)f; return VMM_ERR_SYS; }
int  kvm_get_mp_state(int f, uint32_t *st)      { (void)f; (void)st; return VMM_ERR_SYS; }
const char *kvm_mp_state_str(uint32_t st)       { (void)st; return "?"; }
const struct arch_cpu_ops *arch_probe(void)     { return NULL; }
int  irqchip_create(struct vmm_vm *vm)          { (void)vm; return VMM_OK; }

#include "mutants.h"

/* 被测函数走一层指针：默认是 src/vm.c 里真实的 io_bus_dispatch_*，
 * --mutant N 时换成 tests/unit/io_bus_mutants.c 里故意写错的那一份。 */
static pio_dispatch_fn  g_pio  = io_bus_dispatch_pio;
static mmio_dispatch_fn g_mmio = io_bus_dispatch_mmio;

/* 下面两个薄封装让用例保持“摆好 kvm_run 再触发一次 exit”的写法不变，
 * 同时又是通过公开接口调进去的。KVM_EXIT 路径上从 kvm_run 取字段那几行
 * （尤其是必须按 io.data_offset 定位缓冲区）不在这里覆盖，由 e2e 覆盖，
 * 见 TEST_DESIGN.md。 */
static int handle_io(struct vmm_vcpu *v)
{
    struct kvm_run *run = v->run;
    uint8_t *data = (uint8_t *)run + run->io.data_offset;

    return g_pio(v, (uint16_t)run->io.port,
                 run->io.direction == KVM_EXIT_IO_OUT,
                 run->io.size, run->io.count, data);
}

static int handle_mmio(struct vmm_vcpu *v)
{
    struct kvm_run *run = v->run;

    return g_mmio(v, run->mmio.phys_addr, run->mmio.is_write != 0,
                  run->mmio.len, run->mmio.data);
}

/* ------------------------------------------------------------------ */
/* 伪造的 kvm_run 页与 mock 设备                                        */
/* ------------------------------------------------------------------ */

#define RUN_PAGES     2
#define DATA_OFF      1024              /* io.data 在 kvm_run 页内的偏移 */
#define GUARD         0x5A              /* 数据区前后的护栏字节 */
#define GUARD_LEN     64

static struct vmm_vm    g_vm;
static struct vmm_vcpu  g_vcpu;
static uint8_t         *g_page;         /* 伪造的 kvm_run 共享页 */

/* mock 设备把每一次回调原样记下来，用来断言 offset/size/顺序 */
struct call {
    int      is_write;
    uint64_t off;
    uint32_t size;
    uint64_t val;
};

#define MAX_CALLS 32
struct mockdev {
    const char   *name;
    struct call   calls[MAX_CALLS];
    int           n_calls;
    int           ret;          /* 回调返回值 */
    int           ret_at;       /* 第几次调用开始返回 ret（0 = 从不） */
    uint8_t       fill;         /* 读回调往 data 里填的字节 */
};

static struct mockdev g_dev_a, g_dev_b;

static void mock_reset(struct mockdev *d, const char *name)
{
    memset(d, 0, sizeof(*d));
    d->name = name;
    d->ret  = VMM_OK;
    d->fill = 0x11;
}

static int mock_record(struct mockdev *d, int is_write, uint64_t off,
                       uint32_t size, const void *data)
{
    struct call *c;
    uint32_t i;

    if (d->n_calls < MAX_CALLS) {
        c = &d->calls[d->n_calls];
        c->is_write = is_write;
        c->off      = off;
        c->size     = size;
        c->val      = 0;
        if (data)
            for (i = 0; i < size && i < 8; i++)
                c->val |= (uint64_t)((const uint8_t *)data)[i] << (8 * i);
    }
    d->n_calls++;

    if (d->ret_at && d->n_calls >= d->ret_at)
        return d->ret;
    return VMM_OK;
}

static int mock_write(void *opaque, uint64_t off, uint32_t size,
                      const void *data)
{
    return mock_record(opaque, 1, off, size, data);
}

static int mock_read(void *opaque, uint64_t off, uint32_t size, void *data)
{
    struct mockdev *d = opaque;
    uint32_t i;

    /* 先填数据再记录：填的图案带上 offset，便于断言“哪一次调用写的” */
    for (i = 0; i < size; i++)
        ((uint8_t *)data)[i] = (uint8_t)(d->fill + off + i);
    return mock_record(d, 0, off, size, data);
}

static struct vmm_io_dev mkdev(const char *name, uint64_t base, uint64_t len,
                               struct mockdev *m)
{
    struct vmm_io_dev d;

    memset(&d, 0, sizeof(d));
    d.name   = name;
    d.base   = base;
    d.len    = len;
    d.opaque = m;
    d.read   = mock_read;
    d.write  = mock_write;
    return d;
}

static void vm_reset(void)
{
    memset(&g_vm, 0, sizeof(g_vm));
    g_vm.kvm_fd = g_vm.vm_fd = -1;
    pthread_mutex_init(&g_vm.io_lock, NULL);
    memset(&g_vcpu, 0, sizeof(g_vcpu));
    g_vcpu.vm  = &g_vm;
    g_vcpu.fd  = -1;
    g_vcpu.run = (struct kvm_run *)g_page;
    mock_reset(&g_dev_a, "mock-a");
    mock_reset(&g_dev_b, "mock-b");
}

/* 摆好一次 KVM_EXIT_IO，data 区前后压护栏 */
static uint8_t *io_setup(uint16_t port, int is_out, uint32_t size,
                         uint32_t count)
{
    struct kvm_run *run = g_vcpu.run;
    uint8_t *data = g_page + DATA_OFF;

    memset(g_page, 0, RUN_PAGES * 4096);
    memset(data - GUARD_LEN, GUARD, GUARD_LEN);
    memset(data + (size_t)size * count, GUARD, GUARD_LEN);

    run->exit_reason     = KVM_EXIT_IO;
    run->io.direction    = is_out ? KVM_EXIT_IO_OUT : KVM_EXIT_IO_IN;
    run->io.size         = size;
    run->io.port         = port;
    run->io.count        = count;
    run->io.data_offset  = DATA_OFF;
    return data;
}

static void check_guards(uint32_t size, uint32_t count, const char *what)
{
    uint8_t *data = g_page + DATA_OFF;
    size_t i;

    for (i = 0; i < GUARD_LEN; i++) {
        CHECK_U64(data[-(ptrdiff_t)GUARD_LEN + (ptrdiff_t)i], GUARD,
                  "%s: 数据区之前的护栏被改写（下标 -%zu）", what,
                  GUARD_LEN - i);
        CHECK_U64(data[(size_t)size * count + i], GUARD,
                  "%s: 数据区之后的护栏被改写（下标 +%zu）", what,
                  (size_t)size * count + i);
    }
}

/* ------------------------------------------------------------------ */
/* 注册与查找                                                          */
/* ------------------------------------------------------------------ */

static void case_register(void)
{
    struct vmm_io_dev d;
    int r, i;

    T_CASE("vmm_register_pio：重叠检测与紧邻允许");
    vm_reset();

    d = mkdev("a", 0x3f8, 8, &g_dev_a);
    CHECK_U64(vmm_register_pio(&g_vm, &d), VMM_OK, "首次注册应当成功");

    d = mkdev("dup", 0x3f8, 8, &g_dev_b);
    CHECK_U64(vmm_register_pio(&g_vm, &d), VMM_ERR_INVAL, "完全相同必须拒绝");
    d = mkdev("front", 0x3f4, 8, &g_dev_b);
    CHECK_U64(vmm_register_pio(&g_vm, &d), VMM_ERR_INVAL, "前半重叠必须拒绝");
    d = mkdev("back", 0x3fc, 8, &g_dev_b);
    CHECK_U64(vmm_register_pio(&g_vm, &d), VMM_ERR_INVAL, "后半重叠必须拒绝");
    d = mkdev("inner", 0x3fa, 2, &g_dev_b);
    CHECK_U64(vmm_register_pio(&g_vm, &d), VMM_ERR_INVAL, "被包含必须拒绝");
    d = mkdev("outer", 0x3f0, 32, &g_dev_b);
    CHECK_U64(vmm_register_pio(&g_vm, &d), VMM_ERR_INVAL, "完全覆盖必须拒绝");
    CHECK_U64(g_vm.nr_pio_devs, 1, "被拒的注册不得进表");

    d = mkdev("left", 0x3f0, 8, &g_dev_b);
    CHECK_U64(vmm_register_pio(&g_vm, &d), VMM_OK, "紧贴左侧应当允许");
    d = mkdev("right", 0x400, 8, &g_dev_b);
    CHECK_U64(vmm_register_pio(&g_vm, &d), VMM_OK, "紧贴右侧应当允许");
    CHECK_U64(g_vm.nr_pio_devs, 3, "现在应当有三个 PIO 设备");

    T_CASE("PIO/MMIO 设备表满时拒绝注册");
    vm_reset();
    for (i = 0; i < VMM_MAX_PIO_DEVS; i++) {
        d = mkdev("fill", 0x1000 + (uint64_t)i * 16, 16, &g_dev_a);
        r = vmm_register_pio(&g_vm, &d);
        if (r != VMM_OK)
            break;
    }
    CHECK_U64(i, VMM_MAX_PIO_DEVS, "应当正好装满 VMM_MAX_PIO_DEVS 个");
    d = mkdev("overflow", 0x9000, 16, &g_dev_a);
    CHECK_U64(vmm_register_pio(&g_vm, &d), VMM_ERR_NOMEM, "满表必须返回 NOMEM");

    for (i = 0; i < VMM_MAX_MMIO_DEVS; i++) {
        d = mkdev("fill", 0xd0000000ULL + (uint64_t)i * 0x1000, 0x1000, &g_dev_a);
        r = vmm_register_mmio(&g_vm, &d);
        if (r != VMM_OK)
            break;
    }
    CHECK_U64(i, VMM_MAX_MMIO_DEVS, "MMIO 表应当装满 VMM_MAX_MMIO_DEVS 个");
    d = mkdev("overflow", 0xe0000000ULL, 0x1000, &g_dev_a);
    CHECK_U64(vmm_register_mmio(&g_vm, &d), VMM_ERR_NOMEM,
              "MMIO 满表必须返回 NOMEM");
}

static void case_lookup(void)
{
    struct vmm_io_dev d;

    T_CASE("vmm_lookup_pio / vmm_lookup_mmio 的区间边界");
    vm_reset();

    d = mkdev("a", 0x3f8, 8, &g_dev_a);
    CHECK_U64(vmm_register_pio(&g_vm, &d), VMM_OK, "注册 a");
    d = mkdev("b", 0x60, 1, &g_dev_b);
    CHECK_U64(vmm_register_pio(&g_vm, &d), VMM_OK, "注册 b");

    CHECK(vmm_lookup_pio(&g_vm, 0x3f8) != NULL, "base 应当命中");
    CHECK(vmm_lookup_pio(&g_vm, 0x3ff) != NULL, "base+len-1 应当命中");
    CHECK_PTR(vmm_lookup_pio(&g_vm, 0x3f7), NULL, "base-1 不得命中");
    CHECK_PTR(vmm_lookup_pio(&g_vm, 0x400), NULL, "base+len 不得命中");
    CHECK(vmm_lookup_pio(&g_vm, 0x60) != NULL, "长度为 1 的设备应当命中");
    CHECK_PTR(vmm_lookup_pio(&g_vm, 0x61), NULL, "长度为 1 的设备只占一个端口");
    CHECK_PTR(vmm_lookup_pio(&g_vm, 0), NULL, "端口 0 未注册");

    /* 查到的必须是正确的那一个，而不是“碰巧第一个” */
    CHECK_PTR(vmm_lookup_pio(&g_vm, 0x3fa)->opaque, &g_dev_a, "0x3fa 属于 a");
    CHECK_PTR(vmm_lookup_pio(&g_vm, 0x60)->opaque, &g_dev_b, "0x60 属于 b");

    d = mkdev("m", 0xd0000000ULL, 0x200, &g_dev_a);
    CHECK_U64(vmm_register_mmio(&g_vm, &d), VMM_OK, "注册 MMIO 设备");
    CHECK(vmm_lookup_mmio(&g_vm, 0xd00001ffULL) != NULL, "MMIO 末字节应当命中");
    CHECK_PTR(vmm_lookup_mmio(&g_vm, 0xd0000200ULL), NULL,
              "MMIO base+len 不得命中");
}

/* ------------------------------------------------------------------ */
/* 分发                                                                */
/* ------------------------------------------------------------------ */

static void case_unhandled_pio(void)
{
    uint8_t *data;
    uint32_t i;
    int r;

    T_CASE("未注册端口：读回全 0xFF，写被丢弃");
    vm_reset();

    /* 判据来自 arch/x86/kernel/i8237.c 的 i8237A_init_ops()：
     *   if (dma_inb(DMA_PAGE_0) == 0xFF) return -ENODEV;
     * 注释原文 "All removed ports must return 0xff for a inb() request."
     * 读回 0x00 会让 Linux 凭空多出一个 8237 DMA 子系统。 */
    data = io_setup(0x87, 0, 1, 1);
    r = handle_io(&g_vcpu);
    CHECK_U64(r, VMM_OK, "未注册端口的读不得报错");
    CHECK_U64(data[0], 0xFF, "1 字节读必须返回 0xFF");
    check_guards(1, 1, "unhandled IN size=1");

    data = io_setup(0xcf8, 0, 4, 1);
    r = handle_io(&g_vcpu);
    CHECK_U64(r, VMM_OK, "4 字节读不得报错");
    for (i = 0; i < 4; i++)
        CHECK_U64(data[i], 0xFF, "4 字节读的第 %u 字节必须是 0xFF", i);
    check_guards(4, 1, "unhandled IN size=4");

    /* 串操作：size*count 个字节都要填满，且一个字节都不许多写 */
    data = io_setup(0x1234, 0, 2, 8);
    r = handle_io(&g_vcpu);
    CHECK_U64(r, VMM_OK, "串读不得报错");
    for (i = 0; i < 16; i++)
        CHECK_U64(data[i], 0xFF, "串读的第 %u 字节必须是 0xFF", i);
    check_guards(2, 8, "unhandled IN size=2 count=8");

    /* 写：丢弃，且不得改动 data */
    data = io_setup(0x80, 1, 1, 1);
    data[0] = 0x42;
    r = handle_io(&g_vcpu);
    CHECK_U64(r, VMM_OK, "未注册端口的写不得报错");
    CHECK_U64(data[0], 0x42, "写被丢弃时不得改动 data 缓冲区");
    check_guards(1, 1, "unhandled OUT");
}

static void case_dispatch_offset(void)
{
    struct vmm_io_dev d;
    uint8_t *data;
    int r;

    T_CASE("分发给设备时 offset 相对 base，size 原样传递");
    vm_reset();
    d = mkdev("a", 0x3f8, 8, &g_dev_a);
    vmm_register_pio(&g_vm, &d);

    data = io_setup(0x3fd, 1, 1, 1);      /* LSR 的位置 */
    data[0] = 0x5A;
    r = handle_io(&g_vcpu);
    CHECK_U64(r, VMM_OK, "写应当成功");
    CHECK_U64(g_dev_a.n_calls, 1, "应当只调一次回调");
    CHECK_U64(g_dev_a.calls[0].is_write, 1, "方向应当是写");
    CHECK_U64(g_dev_a.calls[0].off, 5, "offset 应当是 0x3fd - 0x3f8 = 5");
    CHECK_U64(g_dev_a.calls[0].size, 1, "size 应当原样传递");
    CHECK_U64(g_dev_a.calls[0].val, 0x5A, "写入的值应当原样传递");

    /* 读方向：回调填的数据必须落回 data 缓冲区 */
    mock_reset(&g_dev_a, "a");
    data = io_setup(0x3f8, 0, 1, 1);
    r = handle_io(&g_vcpu);
    CHECK_U64(r, VMM_OK, "读应当成功");
    CHECK_U64(g_dev_a.calls[0].off, 0, "offset 应当是 0");
    CHECK_U64(data[0], 0x11, "回调写的数据必须落回 data 缓冲区");
    check_guards(1, 1, "dispatch IN");

    /* base 为 0 的设备：offset 就是端口号本身 */
    vm_reset();
    d = mkdev("z", 0, 4, &g_dev_a);
    vmm_register_pio(&g_vm, &d);
    io_setup(3, 1, 1, 1);
    r = handle_io(&g_vcpu);
    CHECK_U64(r, VMM_OK, "base=0 的设备应当能分发");
    CHECK_U64(g_dev_a.calls[0].off, 3, "base=0 时 offset 等于端口号");
}

static void case_dispatch_string(void)
{
    struct vmm_io_dev d;
    uint8_t *data;
    int i, r;

    T_CASE("串操作 count>1：逐次回调，每次前进 size 字节");
    vm_reset();
    d = mkdev("a", 0x3f8, 8, &g_dev_a);
    vmm_register_pio(&g_vm, &d);

    /* outsb 批量写 4 个字节到 THR：4 次回调，offset 恒为 0，
     * 每次取 data 里的下一个字节。串口就是靠这条路径批量输出的。 */
    data = io_setup(0x3f8, 1, 1, 4);
    for (i = 0; i < 4; i++)
        data[i] = (uint8_t)('A' + i);
    r = handle_io(&g_vcpu);
    CHECK_U64(r, VMM_OK, "串写应当成功");
    CHECK_U64(g_dev_a.n_calls, 4, "count=4 应当产生 4 次回调");
    for (i = 0; i < 4 && i < g_dev_a.n_calls; i++) {
        CHECK_U64(g_dev_a.calls[i].off, 0, "第 %d 次 offset 应当恒为 0", i);
        CHECK_U64(g_dev_a.calls[i].size, 1, "第 %d 次 size 应当是 1", i);
        CHECK_U64(g_dev_a.calls[i].val, (uint64_t)('A' + i),
                  "第 %d 次应当取 data 里的下一个字节", i);
    }
    check_guards(1, 4, "string OUT");

    /* size=2 count=3：步长必须是 2 而不是 1 */
    mock_reset(&g_dev_a, "a");
    data = io_setup(0x3f8, 1, 2, 3);
    for (i = 0; i < 6; i++)
        data[i] = (uint8_t)(0x10 + i);
    r = handle_io(&g_vcpu);
    CHECK_U64(r, VMM_OK, "size=2 的串写应当成功");
    CHECK_U64(g_dev_a.n_calls, 3, "count=3 应当产生 3 次回调");
    CHECK_U64(g_dev_a.calls[0].val, 0x1110, "第 0 次应当是 data[0..1]");
    CHECK_U64(g_dev_a.calls[1].val, 0x1312, "第 1 次应当是 data[2..3]");
    CHECK_U64(g_dev_a.calls[2].val, 0x1514, "第 2 次应当是 data[4..5]");
    check_guards(2, 3, "string OUT size=2");

    /* 读方向的串操作：每次回调写进 data 的不同位置 */
    mock_reset(&g_dev_a, "a");
    data = io_setup(0x3fa, 0, 1, 3);
    r = handle_io(&g_vcpu);
    CHECK_U64(r, VMM_OK, "串读应当成功");
    CHECK_U64(g_dev_a.n_calls, 3, "串读应当产生 3 次回调");
    CHECK_U64(data[0], 0x11 + 2, "第 0 字节由第 0 次回调写入");
    CHECK_U64(data[1], 0x11 + 2, "第 1 字节由第 1 次回调写入");
    CHECK_U64(data[2], 0x11 + 2, "第 2 字节由第 2 次回调写入");
    check_guards(1, 3, "string IN");

    /* count=0：一次回调都不该有 */
    mock_reset(&g_dev_a, "a");
    io_setup(0x3f8, 1, 1, 0);
    r = handle_io(&g_vcpu);
    CHECK_U64(r, VMM_OK, "count=0 不得报错");
    CHECK_U64(g_dev_a.n_calls, 0, "count=0 不得产生回调");
}

static void case_callback_return(void)
{
    struct vmm_io_dev d;
    int r;

    T_CASE("回调返回值：VMM_ERR_EXIT 立即中止串操作并原样上报");
    vm_reset();
    d = mkdev("a", 0x3f8, 8, &g_dev_a);
    vmm_register_pio(&g_vm, &d);

    /* 契约（HANDOFF 第 8 节）：VMM_ERR_EXIT 是约定的正常退出信号，
     * guest 请求关机/复位走这条路。它必须立刻停止后续访问——
     * i8042 的 0xFE 脉冲之后再执行剩下的串操作是没有意义的。 */
    g_dev_a.ret    = VMM_ERR_EXIT;
    g_dev_a.ret_at = 2;
    io_setup(0x3f8, 1, 1, 5);
    r = handle_io(&g_vcpu);
    CHECK_U64((uint64_t)(int64_t)r, (uint64_t)(int64_t)VMM_ERR_EXIT,
              "回调的 VMM_ERR_EXIT 必须原样上报");
    CHECK_U64(g_dev_a.n_calls, 2, "第 2 次回调返回退出后不得继续第 3 次");

    /* 其它错误码同样中止 */
    mock_reset(&g_dev_a, "a");
    g_dev_a.ret    = VMM_ERR_GUEST;
    g_dev_a.ret_at = 1;
    io_setup(0x3f8, 1, 1, 5);
    r = handle_io(&g_vcpu);
    CHECK_U64((uint64_t)(int64_t)r, (uint64_t)(int64_t)VMM_ERR_GUEST,
              "回调的错误码必须原样上报");
    CHECK_U64(g_dev_a.n_calls, 1, "出错后不得继续后续访问");

    T_CASE("read/write 回调为 NULL 时不得崩溃");
    vm_reset();
    d = mkdev("nocb", 0x3f8, 8, &g_dev_a);
    d.read = NULL;
    d.write = NULL;
    vmm_register_pio(&g_vm, &d);
    io_setup(0x3f8, 1, 1, 1);
    CHECK_U64(handle_io(&g_vcpu), VMM_OK, "write 为 NULL 时应当返回 VMM_OK");
    io_setup(0x3f8, 0, 1, 1);
    CHECK_U64(handle_io(&g_vcpu), VMM_OK, "read 为 NULL 时应当返回 VMM_OK");
}

static void case_mmio(void)
{
    struct vmm_io_dev d;
    struct kvm_run *run = (struct kvm_run *)g_page;
    int i, r;

    T_CASE("MMIO 分发：offset、len 与未注册地址的返回值");
    vm_reset();
    d = mkdev("m", 0xd0000000ULL, 0x200, &g_dev_a);
    vmm_register_mmio(&g_vm, &d);

    memset(g_page, 0, RUN_PAGES * 4096);
    run->exit_reason    = KVM_EXIT_MMIO;
    run->mmio.phys_addr = 0xd0000010ULL;
    run->mmio.len       = 4;
    run->mmio.is_write  = 1;
    run->mmio.data[0]   = 0x78;
    run->mmio.data[1]   = 0x56;
    run->mmio.data[2]   = 0x34;
    run->mmio.data[3]   = 0x12;
    r = handle_mmio(&g_vcpu);
    CHECK_U64(r, VMM_OK, "MMIO 写应当成功");
    CHECK_U64(g_dev_a.n_calls, 1, "应当只调一次回调");
    CHECK_U64(g_dev_a.calls[0].off, 0x10, "MMIO offset 应当相对 base");
    CHECK_U64(g_dev_a.calls[0].size, 4, "MMIO len 应当原样传递");
    CHECK_U64(g_dev_a.calls[0].val, 0x12345678ULL, "MMIO 数据应当原样传递");

    /* 未注册的 MMIO 地址：读回全 1 */
    mock_reset(&g_dev_a, "m");
    memset(g_page, 0, RUN_PAGES * 4096);
    run->mmio.phys_addr = 0xe0000000ULL;
    run->mmio.len       = 4;
    run->mmio.is_write  = 0;
    r = handle_mmio(&g_vcpu);
    CHECK_U64(r, VMM_OK, "未注册 MMIO 读不得报错");
    CHECK_U64(g_dev_a.n_calls, 0, "未注册地址不得调到设备");
    for (i = 0; i < 4; i++)
        CHECK_U64(run->mmio.data[i], 0xFF,
                  "未注册 MMIO 读的第 %d 字节必须是 0xFF", i);
}

/* ------------------------------------------------------------------ */
/* CHARACTERIZATION：尚未决定的语义，只记录当前行为                     */
/* ------------------------------------------------------------------ */

static void case_characterize_overrun(void)
{
    struct vmm_io_dev d;
    struct kvm_run *run = (struct kvm_run *)g_page;
    int r;

    T_CASE("[CHARACTERIZATION] 跨越设备末端的访问：当前不做校验");

    /* handle_io()/handle_mmio() 算出 off = addr - dev->base 之后，
     * 没有校验 off + size <= dev->len，直接把访问交给设备回调。
     * 当前四个传统设备都按 off 分派、忽略 size，所以看不出影响；
     * Step 3 的 virtio-mmio 配置空间按宽度访问，一定会碰到。
     *
     * 这一条不是契约断言，是把现状钉下来：语义该怎么定见
     * TEST_DESIGN.md 的 open question O1，定了之后本用例要改写。 */
    vm_reset();
    d = mkdev("a", 0x3f8, 8, &g_dev_a);
    vmm_register_pio(&g_vm, &d);

    io_setup(0x3ff, 1, 4, 1);            /* 末端端口 + 4 字节宽 */
    r = handle_io(&g_vcpu);
    CHECK_U64(r, VMM_OK, "当前实现放行");
    CHECK_U64(g_dev_a.n_calls, 1, "当前实现照常调用设备回调");
    CHECK_U64(g_dev_a.calls[0].off, 7, "回调拿到 off=7");
    CHECK_U64(g_dev_a.calls[0].size, 4, "回调拿到 size=4，off+size=11 > len=8");

    mock_reset(&g_dev_a, "a");
    memset(g_page, 0, RUN_PAGES * 4096);
    run->exit_reason    = KVM_EXIT_MMIO;
    run->mmio.phys_addr = 0xd00001feULL;  /* 设备末端前 2 字节 */
    run->mmio.len       = 8;
    run->mmio.is_write  = 0;
    vm_reset();
    d = mkdev("m", 0xd0000000ULL, 0x200, &g_dev_a);
    vmm_register_mmio(&g_vm, &d);
    run->exit_reason    = KVM_EXIT_MMIO;
    run->mmio.phys_addr = 0xd00001feULL;
    run->mmio.len       = 8;
    run->mmio.is_write  = 0;
    r = handle_mmio(&g_vcpu);
    CHECK_U64(r, VMM_OK, "MMIO 同样放行");
    CHECK_U64(g_dev_a.calls[0].off, 0x1fe, "回调拿到 off=0x1fe");
    CHECK_U64(g_dev_a.calls[0].size, 8, "回调拿到 len=8，越过设备末端 6 字节");

    printf("       注意：以上是当前行为的记录，不是契约。见 TEST_DESIGN.md O1\n");
}

int main(int argc, char **argv)
{
    int i;

    T_MAIN_BEGIN();
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) {
            vmm_log_level = 3;
        } else if (!strcmp(argv[i], "--list-mutants")) {
            int m;
            printf("%d\n", io_mutants_count);
            for (m = 0; m < io_mutants_count; m++)
                printf("%d %s\t%s\n", m, io_mutants[m].name,
                       io_mutants[m].breaks);
            return 0;
        } else if (!strcmp(argv[i], "--mutant") && i + 1 < argc) {
            int m = atoi(argv[++i]);
            if (m < 0 || m >= io_mutants_count) {
                fprintf(stderr, "mutant %d out of range (0..%d)\n",
                        m, io_mutants_count - 1);
                return 2;
            }
            if (io_mutants[m].pio)
                g_pio = io_mutants[m].pio;
            if (io_mutants[m].mmio)
                g_mmio = io_mutants[m].mmio;
            printf("MUTANT %d %s — 违反：%s\n",
                   m, io_mutants[m].name, io_mutants[m].breaks);
        }
    }

    g_page = calloc(RUN_PAGES, 4096);
    if (!g_page)
        return 2;

    case_register();
    case_lookup();
    case_unhandled_pio();
    case_dispatch_offset();
    case_dispatch_string();
    case_callback_return();
    case_mmio();
    case_characterize_overrun();

    free(g_page);
    return T_SUMMARY();
}
