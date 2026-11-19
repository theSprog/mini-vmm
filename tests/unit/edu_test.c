/* tests/unit/edu_test.c — edu 教学设备的设备模型
 *
 * 链接真实的 src/devices/edu.c，PCI 核心和事件循环换成桩：DMA 落在一块假
 * 的 guest 内存上，INTx 电平和 MSI 消息被记录下来，事件循环只记下设备登记
 * 的回调，由用例在想要的时刻调用它，模拟 I/O 线程开始执行。
 *
 * 判据全部来自 include/devices/edu_regs.h 写下的规范：寄存器的访问语义、
 * DMA 的三种结果和完成时的状态变化、INTx 电平等于 INT_STATUS != 0、MSI 在
 * INT_STATUS 每多出一个原来为 0 的位时发一条、复位丢弃执行中的命令。
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "vmm.h"
#include "opts.h"
#include "io/event_loop.h"
#include "devices/edu.h"
#include "harness.h"

/* ------------------------------------------------------------------ */
/* 桩                                                                  */
/* ------------------------------------------------------------------ */

int vmm_log_level = -1;
static char g_last_log[512];

void vmm_log(int level, const char *file, int line, const char *fmt, ...)
{
    va_list ap;

    (void)file; (void)line;
    va_start(ap, fmt);
    vsnprintf(g_last_log, sizeof(g_last_log), fmt, ap);
    va_end(ap);
    if (level <= vmm_log_level)
        fprintf(stderr, "       [vmm] %s\n", g_last_log);
}

#define RAM_SIZE  (64u << 10)
static uint8_t  g_ram[RAM_SIZE];
static bool     g_master = true;
static uint64_t g_last_gpa;
static unsigned g_dma_calls;

static struct edu *E;

/* 设备交给 DMA 层的 host 缓冲区必须整段落在它自己的 4 KiB 缓冲区里。
 * 范围检查写错时（例如偏移加长度在 32 位里回绕），这里先于 memcpy 报出
 * 来，而不是让 memcpy 越界写 host 内存 */
static bool host_buf_ok(const void *p, size_t len)
{
    uintptr_t lo = (uintptr_t)E->buf, hi = lo + EDU_BUF_SIZE, a = (uintptr_t)p;

    CHECK(a >= lo && a <= hi && len <= hi - a,
          "device passed host buffer %p+0x%zx outside its buffer %p+0x%x",
          p, len, (void *)E->buf, EDU_BUF_SIZE);
    return a >= lo && a <= hi && len <= hi - a;
}

static int fake_dma(uint64_t gpa, size_t len)
{
    g_dma_calls++;
    g_last_gpa = gpa;
    if (!g_master || gpa > RAM_SIZE || len > RAM_SIZE - gpa)
        return VMM_ERR_GUEST;
    return VMM_OK;
}

int pci_dma_read(struct pci_device *dev, uint64_t gpa, void *dst, size_t len)
{
    (void)dev;
    if (!host_buf_ok(dst, len) || fake_dma(gpa, len) != VMM_OK)
        return VMM_ERR_GUEST;
    memcpy(dst, g_ram + gpa, len);
    return VMM_OK;
}

int pci_dma_write(struct pci_device *dev, uint64_t gpa, const void *src, size_t len)
{
    (void)dev;
    if (!host_buf_ok(src, len) || fake_dma(gpa, len) != VMM_OK)
        return VMM_ERR_GUEST;
    memcpy(g_ram + gpa, src, len);
    return VMM_OK;
}

static bool     g_irq;
static unsigned g_irq_edges;

void pci_set_irq(struct pci_device *dev, bool level)
{
    (void)dev;
    if (level && !g_irq)
        g_irq_edges++;
    g_irq = level;
}

static bool     g_msi_on;
static unsigned g_msi_sent;

bool pci_msi_enabled(const struct pci_device *dev)
{
    (void)dev;
    return g_msi_on;
}

void pci_msi_notify(struct pci_device *dev)
{
    (void)dev;
    if (!g_msi_on) {
        printf("FAIL: pci_msi_notify called with MSI disabled\n");
        exit(1);
    }
    g_msi_sent++;
}

static struct pci_device *g_registered;

int pci_register_device(struct pci_bus *bus, struct pci_device *dev)
{
    (void)bus;
    g_registered = dev;
    return VMM_OK;
}

static evl_handler_fn g_wake_fn;
static void          *g_wake_opaque;
static int            g_wake_fd = -1;

int evl_add(struct event_loop *l, int fd, uint32_t events, evl_handler_fn fn, void *opaque)
{
    (void)l; (void)events;
    g_wake_fn = fn;
    g_wake_opaque = opaque;
    g_wake_fd = fd;
    return VMM_OK;
}

/* ------------------------------------------------------------------ */
/* 夹具                                                                */
/* ------------------------------------------------------------------ */

static struct vmm_vm g_vm;

/* 每个用例一台新设备，经 --device 的同一条路径创建和挂载 */
static void fresh(void)
{
    struct opts o;
    struct vmm_device d = { 0 };
    void *dev = NULL;

    if (E && E->wake_fd >= 0)
        close(E->wake_fd);
    free(E);
    memset(g_ram, 0, sizeof(g_ram));
    g_master = true;
    g_irq = false;
    g_irq_edges = 0;
    g_msi_on = false;
    g_msi_sent = 0;
    g_dma_calls = 0;
    g_registered = NULL;
    g_wake_fn = NULL;
    if (opts_parse(&o, "--device", "edu", "driver") != VMM_OK ||
        edu_device_type.create(&o, &dev) != VMM_OK) {
        printf("fixture: create failed: %s\n", g_last_log);
        exit(1);
    }
    opts_free(&o);
    d.type = &edu_device_type;
    d.dev = dev;
    if (edu_device_type.attach(&d, &g_vm, NULL, NULL) != VMM_OK || !g_wake_fn) {
        printf("fixture: attach failed: %s\n", g_last_log);
        exit(1);
    }
    E = dev;
}

static struct pci_bar *bar0(void)
{
    return &g_registered->bars[0];
}

static uint32_t rd(uint32_t off)
{
    uint32_t v = 0xDEADBEEF;

    bar0()->read(bar0()->opaque, off, 4, &v);
    return v;
}

static void wr(uint32_t off, uint32_t v)
{
    bar0()->write(bar0()->opaque, off, 4, &v);
}

/* 模拟 I/O 线程被 eventfd 唤醒 */
static void run_io(void)
{
    g_wake_fn(g_wake_opaque, g_wake_fd, EPOLLIN);
}

static void dma(uint64_t addr, uint32_t off, uint32_t len, uint32_t cmd)
{
    wr(EDU_REG_DMA_ADDR_LO, (uint32_t)addr);
    wr(EDU_REG_DMA_ADDR_HI, (uint32_t)(addr >> 32));
    wr(EDU_REG_DMA_OFF, off);
    wr(EDU_REG_DMA_LEN, len);
    wr(EDU_REG_DMA_CMD, cmd | EDU_DMA_CMD_START);
}

/* ------------------------------------------------------------------ */
/* 用例                                                                */
/* ------------------------------------------------------------------ */

static void test_identity(void)
{
    T_CASE("PCI 身份与 BAR0");
    fresh();
    CHECK_U64(g_registered->vendor_id, 0x1234, "vendor");
    CHECK_U64(g_registered->device_id, 0x0003, "device");
    CHECK_U64(g_registered->class_code, 0xFF0000, "class");
    CHECK_U64(g_registered->intx_pin, 1, "INTA");
    CHECK(g_registered->msi, "MSI capability");
    CHECK(g_registered->bus_master, "Bus Master writable");
    CHECK_U64(bar0()->type, PCI_BAR_MEM32, "BAR0 type");
    CHECK_U64(bar0()->size, 0x2000, "BAR0 size");
}

static void test_registers(void)
{
    uint8_t b[8];
    uint64_t q = 0;
    uint16_t h = 0;

    T_CASE("ID、SCRATCH、未定义寄存器与访问宽度");
    fresh();
    CHECK_U64(rd(EDU_REG_ID), EDU_ID_VALUE, "ID");
    memcpy(b, (uint32_t[]){ EDU_ID_VALUE }, 4);
    CHECK(!memcmp(b, "EDU1", 4), "ID bytes read as EDU1");
    CHECK_U64(rd(EDU_REG_SCRATCH), 0, "scratch resets to 0");
    wr(EDU_REG_SCRATCH, 0xA5A5F00D);
    CHECK_U64(rd(EDU_REG_SCRATCH), 0xA5A5F00D, "scratch reads back");
    wr(EDU_REG_ID, 0);
    CHECK_U64(rd(EDU_REG_ID), EDU_ID_VALUE, "ID is read-only");
    CHECK_U64(rd(0x100), 0, "undefined register reads 0");
    CHECK_U64(rd(EDU_REG_INT_RAISE), 0, "INT_RAISE reads 0");
    CHECK_U64(rd(EDU_REG_INT_ACK), 0, "INT_ACK reads 0");

    bar0()->read(bar0()->opaque, EDU_REG_SCRATCH, 2, &h);
    CHECK_U64(h, 0xFFFF, "2-byte register read returns all ones");
    bar0()->read(bar0()->opaque, EDU_REG_SCRATCH + 1, 4, &q);
    CHECK_U64(q & 0xFFFFFFFF, 0xFFFFFFFF, "unaligned register read returns all ones");
    h = 0x1234;
    bar0()->write(bar0()->opaque, EDU_REG_SCRATCH, 2, &h);
    CHECK_U64(rd(EDU_REG_SCRATCH), 0xA5A5F00D, "2-byte register write ignored");
}

static void test_buffer_mmio(void)
{
    uint64_t q = 0x1122334455667788ULL, got = 0;
    uint8_t c = 0;

    T_CASE("缓冲区经 MMIO 任意宽度、任意偏移读写");
    fresh();
    bar0()->write(bar0()->opaque, EDU_BUF_OFFSET + 3, 8, &q);
    CHECK(!memcmp(E->buf + 3, &q, 8), "8-byte unaligned write lands in the buffer");
    bar0()->read(bar0()->opaque, EDU_BUF_OFFSET + 3, 8, &got);
    CHECK_U64(got, q, "8-byte unaligned read");
    bar0()->read(bar0()->opaque, EDU_BUF_OFFSET + 4, 1, &c);
    CHECK_U64(c, 0x77, "single byte read");
    c = 0xEE;
    bar0()->write(bar0()->opaque, EDU_BUF_OFFSET + EDU_BUF_SIZE - 1, 1, &c);
    CHECK_U64(E->buf[EDU_BUF_SIZE - 1], 0xEE, "last byte of the buffer");

    /* MMIO 分发只看起始地址，越过 BAR 结尾的那部分要由设备挡住 */
    q = 0x0102030405060708ULL;
    bar0()->write(bar0()->opaque, EDU_BAR0_SIZE - 4, 8, &q);
    CHECK_U64(E->buf[EDU_BUF_SIZE - 4], 0, "write past the end of BAR0 is dropped");
    got = 0;
    bar0()->read(bar0()->opaque, EDU_BAR0_SIZE - 4, 8, &got);
    CHECK_U64(got, ~0ULL, "read past the end of BAR0 returns all ones");
}

static void test_intx(void)
{
    T_CASE("INT_RAISE/INT_ACK 与 INTx 电平");
    fresh();
    wr(EDU_REG_INT_RAISE, 0x00000F01);
    CHECK_U64(rd(EDU_REG_INT_STATUS), 0x00000F00, "RAISE only sets soft bits");
    CHECK(g_irq, "INTx high while INT_STATUS != 0");
    wr(EDU_REG_INT_ACK, 0x00000100);
    CHECK_U64(rd(EDU_REG_INT_STATUS), 0x00000E00, "ACK clears only the written bits");
    CHECK(g_irq, "INTx stays high with bits left");
    wr(EDU_REG_INT_ACK, 0xFFFFFFFF);
    CHECK_U64(rd(EDU_REG_INT_STATUS), 0, "ACK all");
    CHECK(!g_irq, "INTx low once INT_STATUS is 0");
    wr(EDU_REG_INT_RAISE, 0x000000FE);
    CHECK_U64(rd(EDU_REG_INT_STATUS), 0, "reserved bits 1-7 cannot be raised");
    CHECK(!g_irq, "no INTx for reserved bits");
    CHECK_U64(g_msi_sent, 0, "no MSI while MSI is off");
}

static void test_msi(void)
{
    T_CASE("MSI：每多出一个原来为 0 的位发一条");
    fresh();
    g_msi_on = true;
    wr(EDU_REG_INT_RAISE, 0x100);
    CHECK_U64(g_msi_sent, 1, "first cause");
    wr(EDU_REG_INT_RAISE, 0x100);
    CHECK_U64(g_msi_sent, 1, "same pending cause again: no message");
    wr(EDU_REG_INT_RAISE, 0x300);
    CHECK_U64(g_msi_sent, 2, "one new bit among the written ones: one message");
    wr(EDU_REG_INT_ACK, 0x100);
    CHECK_U64(g_msi_sent, 2, "ACK sends nothing");
    wr(EDU_REG_INT_RAISE, 0x100);
    CHECK_U64(g_msi_sent, 3, "acked cause raised again");
    wr(EDU_REG_INT_ACK, 0xFFFFFFFF);
    dma(0, 0, 16, EDU_DMA_CMD_IRQ);
    run_io();
    CHECK_U64(g_msi_sent, 4, "DMA completion with IRQ sends one message");
}

static void test_dma_async(void)
{
    unsigned i;

    T_CASE("DMA 从 guest 内存到缓冲区：异步完成");
    fresh();
    for (i = 0; i < EDU_BUF_SIZE; i++)
        g_ram[0x2000 + i] = (uint8_t)(i * 7 + 1);
    dma(0x2000, 0, EDU_BUF_SIZE, EDU_DMA_CMD_IRQ);
    CHECK_U64(rd(EDU_REG_DMA_CMD) & EDU_DMA_CMD_START, EDU_DMA_CMD_START,
              "START reads 1 until the command completes");
    CHECK_U64(g_dma_calls, 0, "nothing moved on the vCPU thread");
    CHECK(!g_irq, "no interrupt before completion");
    CHECK_U64(rd(EDU_REG_DMA_DONE_COUNT), 0, "not counted yet");
    run_io();
    CHECK(!memcmp(E->buf, g_ram + 0x2000, EDU_BUF_SIZE), "buffer equals guest memory");
    CHECK_U64(rd(EDU_REG_DMA_CMD) & EDU_DMA_CMD_START, 0, "START cleared");
    CHECK_U64(rd(EDU_REG_DMA_RESULT), EDU_DMA_RES_OK, "result OK");
    CHECK_U64(rd(EDU_REG_DMA_DONE_COUNT), 1, "done count");
    CHECK_U64(rd(EDU_REG_INT_STATUS), EDU_INT_DMA, "DMA cause set");
    CHECK(g_irq, "INTx raised on completion");
    run_io();
    CHECK_U64(rd(EDU_REG_DMA_DONE_COUNT), 1, "a spurious wakeup runs nothing");
}

static void test_dma_to_ram(void)
{
    T_CASE("DMA 从缓冲区到 guest 内存，不带 IRQ 时不置中断");
    fresh();
    memset(E->buf + 100, 0x5A, 50);
    dma(0x3000, 100, 50, EDU_DMA_CMD_TO_RAM);
    run_io();
    CHECK_U64(g_ram[0x3000], 0x5A, "first byte");
    CHECK_U64(g_ram[0x3000 + 49], 0x5A, "last byte");
    CHECK_U64(g_ram[0x3000 + 50], 0, "nothing past len");
    CHECK_U64(rd(EDU_REG_INT_STATUS), 0, "no cause without IRQ bit");
    CHECK(!g_irq, "no INTx");
    CHECK_U64(rd(EDU_REG_DMA_DONE_COUNT), 1, "still counted");
}

static void test_dma_snapshot(void)
{
    T_CASE("执行中写 DMA 寄存器被丢弃，命令按 START 时的参数执行");
    fresh();
    memset(g_ram + 0x1000, 0x11, 64);
    dma(0x1000, 0, 32, 0);
    wr(EDU_REG_DMA_LEN, 64);
    wr(EDU_REG_DMA_OFF, 8);
    wr(EDU_REG_DMA_CMD, EDU_DMA_CMD_START | EDU_DMA_CMD_TO_RAM);
    CHECK_U64(rd(EDU_REG_DMA_LEN), 32, "LEN write ignored while busy");
    CHECK_U64(rd(EDU_REG_DMA_OFF), 0, "OFF write ignored while busy");
    CHECK(strstr(g_last_log, "ignored while a command is running") != NULL,
          "busy write logged: %s", g_last_log);
    run_io();
    CHECK_U64(E->buf[31], 0x11, "byte 31 copied");
    CHECK_U64(E->buf[32], 0, "byte 32 not copied");
    CHECK_U64(rd(EDU_REG_DMA_DONE_COUNT), 1, "the second START did not queue a command");
}

static void test_dma_64bit(void)
{
    T_CASE("DMA 地址取 ADDR_HI:ADDR_LO 的 64 位值");
    fresh();
    dma(0x100002000ULL, 0, 16, 0);
    run_io();
    CHECK_U64(g_last_gpa, 0x100002000ULL, "64-bit bus address reaches the DMA layer");
    CHECK_U64(rd(EDU_REG_DMA_RESULT), EDU_DMA_RES_BUS_ERROR, "outside fake RAM");
}

static void test_dma_errors(void)
{
    static const struct { uint32_t off, len; const char *what; } bad[] = {
        { 0, 0, "zero length" },
        { 0, EDU_BUF_SIZE + 1, "longer than the buffer" },
        { EDU_BUF_SIZE - 8, 16, "runs past the end" },
        { 0xFFFFFFFF, 2, "offset + length wraps around" },
        { EDU_BUF_SIZE, 1, "starts at the end" },
    };
    unsigned i;

    T_CASE("DMA 的 BAD_RANGE 与 BUS_ERROR：一个字节都不搬，照常完成");
    fresh();
    memset(g_ram, 0x77, 64);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        dma(0, bad[i].off, bad[i].len, EDU_DMA_CMD_IRQ);
        run_io();
        CHECK_U64(rd(EDU_REG_DMA_RESULT), EDU_DMA_RES_BAD_RANGE, "%s", bad[i].what);
        CHECK_U64(rd(EDU_REG_DMA_DONE_COUNT), i + 1, "%s: counted", bad[i].what);
        CHECK_U64(rd(EDU_REG_INT_STATUS), EDU_INT_DMA, "%s: completion interrupt", bad[i].what);
        wr(EDU_REG_INT_ACK, EDU_INT_DMA);
    }
    CHECK_U64(g_dma_calls, 0, "BAD_RANGE never reaches the DMA layer");

    g_master = false;
    dma(0, 0, 64, EDU_DMA_CMD_IRQ);
    run_io();
    CHECK_U64(rd(EDU_REG_DMA_RESULT), EDU_DMA_RES_BUS_ERROR, "bus master off");
    CHECK_U64(E->buf[0], 0, "nothing copied");
    g_master = true;
    dma(RAM_SIZE - 8, 0, 64, 0);
    run_io();
    CHECK_U64(rd(EDU_REG_DMA_RESULT), EDU_DMA_RES_BUS_ERROR, "range leaves RAM");
    dma(0, 0, 64, 0);
    run_io();
    CHECK_U64(rd(EDU_REG_DMA_RESULT), EDU_DMA_RES_OK, "a good command after errors");
    CHECK_U64(E->buf[63], 0x77, "copied");
}

static void test_reset(void)
{
    T_CASE("复位：寄存器和缓冲区清 0，执行中的命令被丢弃");
    fresh();
    wr(EDU_REG_SCRATCH, 1);
    wr(EDU_REG_INT_RAISE, 0x100);
    memset(E->buf, 0xCC, EDU_BUF_SIZE);
    memset(g_ram, 0x99, 64);
    dma(0, 0, 64, EDU_DMA_CMD_IRQ);
    edu_reset(E);
    CHECK(!g_irq, "INTx low after reset");
    CHECK_U64(rd(EDU_REG_SCRATCH), 0, "scratch");
    CHECK_U64(rd(EDU_REG_INT_STATUS), 0, "INT_STATUS");
    CHECK_U64(rd(EDU_REG_DMA_CMD), 0, "DMA_CMD");
    CHECK_U64(E->buf[0], 0, "buffer cleared");
    run_io();
    CHECK_U64(g_dma_calls, 0, "discarded command does not run");
    CHECK_U64(rd(EDU_REG_DMA_DONE_COUNT), 0, "not counted");
    CHECK(!g_irq, "no interrupt from the discarded command");
    dma(0, 0, 64, 0);
    run_io();
    CHECK_U64(E->buf[0], 0x99, "device works after reset");
}

int main(int argc, char **argv)
{
    T_MAIN_BEGIN();
    pthread_mutex_init(&g_vm.io_lock, NULL);
    test_identity();
    test_registers();
    test_buffer_mmio();
    test_intx();
    test_msi();
    test_dma_async();
    test_dma_to_ram();
    test_dma_snapshot();
    test_dma_64bit();
    test_dma_errors();
    test_reset();
    if (E && E->wake_fd >= 0)
        close(E->wake_fd);
    free(E);
    return T_SUMMARY();
}
