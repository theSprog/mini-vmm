/* tests/unit/pci_bus_test.c — PCI 总线：conf1 端口、配置空间语义、BAR decode
 *
 * 所有访问都从 io_bus_dispatch_pio/mmio 进去，也就是 vCPU 陷出后走的同一条
 * 路径：conf1 端口、BAR 表项都是真实注册在 src/vm.c 的设备表里的，BAR 搬家
 * 之后查找结果是否跟着变，测的是 vmm_io_move 和查找的真实组合。
 *
 * 判据来源：
 *   - CONFIG_ADDRESS/CONFIG_DATA 的位定义和“只认 dword 访问”：PCI 3.0
 *     第 3.2.2.3.2 节，Linux arch/x86/pci/direct.c 的 pci_check_type1()
 *   - BAR 探测：Linux drivers/pci/probe.c __pci_read_base() 的访问顺序，
 *     用同一个 pci_size() 算法从读回值推出大小（见 linux_bar_size）
 *   - command 只读位、interrupt line 0xFF：PCI 3.0 第 6.2.2、6.2.4 节
 *   - INTx：Interrupt Disable 与 Interrupt Status 的关系是 PCI 3.0 第 6.2.2、
 *     6.2.3 节；同一根线上多个设备取“或”是 INTx# 开漏线与的电气语义
 *     （第 2.2.6 节），用例里的 KVM_IRQ_LINE 序列就是 IOAPIC 引脚上应当
 *     看到的电平变化
 *   - Bus Master：第 6.2.2 节，位为 0 时设备不得发起总线访问
 *   - MSI：capability 链表是 PCI 3.0 第 6.7 节，MSI capability 的布局和
 *     读写属性是第 6.8.1 节（链表按 Linux pci_find_capability 的方式走），
 *     MSI Enable 打开时不得使用 INTx 是第 6.8.3.3 节，消息内容是第
 *     6.8.3.4 节；哪些消息不合法见 include/devices/irqchip.h（Intel SDM
 *     第 13.11 节），这里记录送进 KVM_SIGNAL_MSI 的每一条消息
 *
 * 不碰 /dev/kvm：src/vm.c 里用到的 kvm_* 全部是桩。
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/pci_regs.h>

#include "vmm.h"
#include "memory.h"
#include "kvm_wrappers.h"
#include "arch/arch.h"
#include "devices/irqchip.h"
#include "devices/pci_test.h"
#include "pci/pci.h"
#include "harness.h"

/* ------------------------------------------------------------------ */
/* 桩：vm.c 引用到的外部符号                                            */
/* ------------------------------------------------------------------ */

int vmm_log_level = -1;

/* 最近一条日志，和“没送达的 MSI”日志的条数：诊断输出本身也是被测行为 */
static char     g_last_log[512];
static unsigned g_drop_logs;

void vmm_log(int level, const char *file, int line, const char *fmt, ...)
{
    va_list ap;

    (void)file; (void)line;
    va_start(ap, fmt);
    vsnprintf(g_last_log, sizeof(g_last_log), fmt, ap);
    va_end(ap);
    if (strstr(g_last_log, "not delivered"))
        g_drop_logs++;
    if (level <= vmm_log_level)
        fprintf(stderr, "       [vmm] %s\n", g_last_log);
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
/* vm.c 的运行循环引用 gdbstub；这里不跑 vCPU，桩函数不会被调用 */
void gdbstub_vcpu_interrupted(struct vmm_vcpu *v) { (void)v; }
int  gdbstub_vcpu_debug_exit(struct vmm_vcpu *v)  { (void)v; return VMM_ERR_GUEST; }

/* KVM_IRQ_LINE 的记录：每次调用追加一条 "gsi=level"，用例比对整串序列，
 * 多发、漏发、发错线都会体现在字符串里 */
static char g_irq_log[512];

int irqchip_set_irq(struct vmm_vm *vm, int gsi, int level)
{
    size_t n = strlen(g_irq_log);

    (void)vm;
    snprintf(g_irq_log + n, sizeof(g_irq_log) - n, "%s%d=%d",
             n ? " " : "", gsi, level);
    return VMM_OK;
}

/* KVM_SIGNAL_MSI 的记录：每条消息追加 "地址:数据"。g_msi_ret 模拟 KVM
 * 没有 vCPU 收下的情形 */
static char        g_msi_log[512];
static int         g_msi_ret = VMM_OK;
static const char *g_msi_why;

int irqchip_signal_msi(struct vmm_vm *vm, uint64_t addr, uint32_t data,
                       const char **why)
{
    size_t n = strlen(g_msi_log);

    (void)vm;
    snprintf(g_msi_log + n, sizeof(g_msi_log) - n, "%s%llx:%x",
             n ? " " : "", (unsigned long long)addr, data);
    if (g_msi_ret != VMM_OK)
        *why = g_msi_why;
    return g_msi_ret;
}

/* guest RAM 只要能区分“读写发生了”和“被拦下了”：[0, sizeof(g_ram)) */
static uint8_t g_ram[4096];

int mem_read(struct vmm_vm *vm, uint64_t gpa, void *dst, size_t len)
{
    (void)vm;
    if (gpa > sizeof(g_ram) || len > sizeof(g_ram) - gpa)
        return VMM_ERR_GUEST;
    memcpy(dst, g_ram + gpa, len);
    return VMM_OK;
}

int mem_write(struct vmm_vm *vm, uint64_t gpa, const void *src, size_t len)
{
    (void)vm;
    if (gpa > sizeof(g_ram) || len > sizeof(g_ram) - gpa)
        return VMM_ERR_GUEST;
    memcpy(g_ram + gpa, src, len);
    return VMM_OK;
}

enum gpa_status gpa_to_hva(struct vmm_vm *vm, uint64_t gpa, uint64_t len, void **hva)
{
    (void)vm;
    if (len == 0)
        return GPA_ERR_ZERO_LEN;
    if (gpa > sizeof(g_ram) || len > sizeof(g_ram) - gpa)
        return GPA_ERR_PAST_END;
    *hva = g_ram + gpa;
    return GPA_OK;
}

const char *gpa_status_str(enum gpa_status st)
{
    return st == GPA_OK ? "ok" : "rejected";
}

/* ------------------------------------------------------------------ */
/* guest 视角的访问：outb/outl/inl 和 MMIO load/store                    */
/* ------------------------------------------------------------------ */

static struct vmm_vm    g_vm;
static struct vmm_vcpu  g_vcpu;
static struct pci_bus   g_bus;
static struct pci_test  g_dev;

static void vm_reset(void)
{
    memset(&g_vm, 0, sizeof(g_vm));
    g_vm.kvm_fd = g_vm.vm_fd = -1;
    memset(&g_vcpu, 0, sizeof(g_vcpu));
    g_vcpu.vm = &g_vm;
    g_vcpu.fd = -1;
}

static void out(uint16_t port, uint32_t size, uint32_t v)
{
    io_bus_dispatch_pio(&g_vcpu, port, true, size, 1, (uint8_t *)&v);
}

static uint32_t in(uint16_t port, uint32_t size)
{
    uint32_t v = 0;

    io_bus_dispatch_pio(&g_vcpu, port, false, size, 1, (uint8_t *)&v);
    return v;
}

static void mmio_w(uint64_t a, uint32_t size, uint32_t v)
{
    io_bus_dispatch_mmio(&g_vcpu, a, true, size, (uint8_t *)&v);
}

static uint32_t mmio_r(uint64_t a, uint32_t size)
{
    uint32_t v = 0;

    io_bus_dispatch_mmio(&g_vcpu, a, false, size, (uint8_t *)&v);
    return v;
}

/* Linux pci_conf1_read/write 的做法：先写地址，再访问 0xCFC + (reg & 3) */
#define CONF1_ADDR(slot, reg)  (0x80000000u | (uint32_t)(slot) << 11 | ((reg) & 0xFC))

static uint32_t cfg_r(int slot, int reg, uint32_t size)
{
    out(0xCF8, 4, CONF1_ADDR(slot, reg));
    return in((uint16_t)(0xCFC + (reg & 3)), size);
}

static void cfg_w(int slot, int reg, uint32_t size, uint32_t v)
{
    out(0xCF8, 4, CONF1_ADDR(slot, reg));
    out((uint16_t)(0xCFC + (reg & 3)), size, v);
}

/* Linux __pci_read_base 的探测序列 + pci_size()：返回推出的 BAR 大小，
 * 0 表示 Linux 会认为这个 BAR 没实现。 */
static uint64_t linux_bar_size(int slot, int bar)
{
    int pos = PCI_BASE_ADDRESS_0 + 4 * bar;
    uint32_t cmd = cfg_r(slot, PCI_COMMAND, 2);
    uint32_t l, sz;
    uint64_t base, maxbase, mask, size;

    cfg_w(slot, PCI_COMMAND, 2, cmd & ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY));
    l = cfg_r(slot, pos, 4);
    cfg_w(slot, pos, 4, ~0u);
    sz = cfg_r(slot, pos, 4);
    cfg_w(slot, pos, 4, l);
    cfg_w(slot, PCI_COMMAND, 2, cmd);

    if (sz == 0xFFFFFFFFu)
        sz = 0;
    if (!sz)
        return 0;
    if (l & PCI_BASE_ADDRESS_SPACE_IO) {
        base    = l & PCI_BASE_ADDRESS_IO_MASK;
        maxbase = sz & PCI_BASE_ADDRESS_IO_MASK;
        mask    = PCI_BASE_ADDRESS_IO_MASK & 0xFFFF;     /* IO_SPACE_LIMIT */
    } else {
        base    = l & PCI_BASE_ADDRESS_MEM_MASK;
        maxbase = sz & PCI_BASE_ADDRESS_MEM_MASK;
        mask    = (uint32_t)PCI_BASE_ADDRESS_MEM_MASK;
    }
    size = mask & maxbase;
    if (!size)
        return 0;
    size &= ~(size - 1);
    if (base == maxbase && ((base | (size - 1)) & mask) != mask)
        return 0;
    return size;
}

static void setup(void)
{
    vm_reset();
    g_irq_log[0] = 0;
    CHECK_U64(pci_bus_init(&g_bus, &g_vm), VMM_OK, "pci_bus_init");
    CHECK_U64(pci_test_attach(&g_dev, &g_bus), VMM_OK, "pci_test_attach");
}

#define TEST_SLOT  1
#define MMIO_BASE  0xC0000000u
#define IO_BASE    0x1000u

/* ------------------------------------------------------------------ */
/* 用例                                                                */
/* ------------------------------------------------------------------ */

static void case_conf1_address(void)
{
    T_CASE("CONFIG_ADDRESS：只认 0xCF8 的 dword 访问，bit 1:0 只读为 0");
    setup();

    /* pci_check_type1() 的原样序列 */
    out(0xCFB, 1, 0x01);
    out(0xCF8, 4, 0x80000000u);
    CHECK_U64(in(0xCF8, 4), 0x80000000u, "dword write must read back");

    out(0xCF8, 4, 0x80000803u);
    CHECK_U64(in(0xCF8, 4), 0x80000800u, "bit 1:0 are read-only zero");

    out(0xCFB, 1, 0x12);
    out(0xCF9, 1, 0x34);
    out(0xCFA, 2, 0x5678);
    CHECK_U64(in(0xCF8, 4), 0x80000800u,
              "byte/word writes to 0xCF8-0xCFB are not CONFIG_ADDRESS");
    CHECK_U64(in(0xCF9, 1), 0xFF, "byte read of 0xCF9 is plain I/O (nothing there)");
    CHECK_U64(in(0xCF8, 2), 0xFFFF, "word read of 0xCF8 is plain I/O");
}

static void case_conf1_data(void)
{
    T_CASE("CONFIG_DATA：字节/字选中 dword 的对应部分，未选中或越界读全 1");
    setup();

    CHECK_U64(cfg_r(0, PCI_VENDOR_ID, 4),
              (uint32_t)PCI_DEVICE_ID_HOST_BRIDGE << 16 | PCI_VENDOR_ID_MINIVMM,
              "host bridge vendor/device at 00:00.0");
    /* pci_sanity_check() 读 PCI_CLASS_DEVICE（0x0A）2 字节 */
    CHECK_U64(cfg_r(0, PCI_CLASS_DEVICE, 2), 0x0600, "host bridge class");
    CHECK_U64(cfg_r(TEST_SLOT, PCI_DEVICE_ID, 2), PCI_DEVICE_ID_TEST, "test device id");
    CHECK_U64(cfg_r(TEST_SLOT, PCI_CLASS_PROG + 2, 1), 0xFF, "test device base class");
    CHECK_U64(cfg_r(TEST_SLOT, PCI_INTERRUPT_PIN, 1), 1, "INTA");

    CHECK_U64(cfg_r(2, PCI_VENDOR_ID, 4), 0xFFFFFFFFu, "empty slot: master abort");
    out(0xCF8, 4, 0x80000000u | 1u << 16);
    CHECK_U64(in(0xCFC, 4), 0xFFFFFFFFu, "bus 1 does not exist");
    out(0xCF8, 4, 0x80000000u | 1u << 8);
    CHECK_U64(in(0xCFC, 4), 0xFFFFFFFFu, "function 1 does not exist");
    out(0xCF8, 4, 0x00000000u);
    CHECK_U64(in(0xCFC, 4), 0xFFFFFFFFu, "enable bit clear: not a config cycle");

    out(0xCF8, 4, 0x80000000u);
    CHECK_U64(in(0xCFE, 4), 0xFFFFFFFFu, "dword at 0xCFE crosses CONFIG_DATA");
}

static void case_ecs_bits(void)
{
    T_CASE("CONFIG_ADDRESS 保留位（AMD ECS 的 bit 27:24）非 0 时不访问低 256 字节");
    setup();

    out(0xCF8, 4, 0x81000000u | (uint32_t)TEST_SLOT << 11 | PCI_COMMAND);
    CHECK_U64(in(0xCFC, 2), 0xFFFF, "extended register reads all ones");
    out(0xCFC, 2, 0);
    CHECK_U64(cfg_r(TEST_SLOT, PCI_COMMAND, 2) & PCI_COMMAND_MEMORY,
              PCI_COMMAND_MEMORY,
              "a write to extended register 0x104 must not alias onto COMMAND");
}

static void case_readonly(void)
{
    T_CASE("只读字段：ID、class、header type、status、pin 写不进去");
    setup();

    cfg_w(TEST_SLOT, PCI_VENDOR_ID, 4, 0xDEADBEEFu);
    cfg_w(TEST_SLOT, PCI_CLASS_REVISION, 4, 0x12345678u);
    cfg_w(TEST_SLOT, PCI_HEADER_TYPE, 1, 0x80);
    cfg_w(TEST_SLOT, PCI_STATUS, 2, 0xFFFF);
    cfg_w(TEST_SLOT, PCI_INTERRUPT_PIN, 1, 4);
    CHECK_U64(cfg_r(TEST_SLOT, PCI_VENDOR_ID, 4),
              (uint32_t)PCI_DEVICE_ID_TEST << 16 | PCI_VENDOR_ID_MINIVMM, "ids");
    CHECK_U64(cfg_r(TEST_SLOT, PCI_CLASS_REVISION, 4), 0xFF000000u, "class/rev");
    CHECK_U64(cfg_r(TEST_SLOT, PCI_HEADER_TYPE, 1), 0, "header type");
    CHECK_U64(cfg_r(TEST_SLOT, PCI_STATUS, 2), 0, "status");
    CHECK_U64(cfg_r(TEST_SLOT, PCI_INTERRUPT_PIN, 1), 1, "interrupt pin");

    CHECK_U64(cfg_r(TEST_SLOT, PCI_INTERRUPT_LINE, 1), 0xFF,
              "interrupt line starts as 0xFF (not wired to the 8259)");
    cfg_w(TEST_SLOT, PCI_INTERRUPT_LINE, 1, 11);
    CHECK_U64(cfg_r(TEST_SLOT, PCI_INTERRUPT_LINE, 1), 11, "interrupt line is RW");
    cfg_w(TEST_SLOT, PCI_CACHE_LINE_SIZE, 1, 16);
    CHECK_U64(cfg_r(TEST_SLOT, PCI_CACHE_LINE_SIZE, 1), 16, "cache line size is RW");
}

static void case_command(void)
{
    T_CASE("command：只有设备具备的功能位可写");
    setup();

    CHECK_U64(cfg_r(TEST_SLOT, PCI_COMMAND, 2),
              PCI_COMMAND_IO | PCI_COMMAND_MEMORY,
              "firmware enabled IO and memory decode");
    cfg_w(TEST_SLOT, PCI_COMMAND, 2, 0xFFFF);
    CHECK_U64(cfg_r(TEST_SLOT, PCI_COMMAND, 2),
              PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_INTX_DISABLE,
              "no bus master (no DMA), no SERR/parity");
    cfg_w(0, PCI_COMMAND, 2, 0xFFFF);
    CHECK_U64(cfg_r(0, PCI_COMMAND, 2), 0, "host bridge has no BARs: all RO 0");
}

static void case_bar_sizing(void)
{
    T_CASE("BAR 探测：按 Linux __pci_read_base 的序列推出的大小与类型");
    setup();

    CHECK_U64(cfg_r(TEST_SLOT, PCI_BASE_ADDRESS_0, 4), MMIO_BASE,
              "BAR0 pre-assigned at the bottom of the MMIO window");
    CHECK_U64(cfg_r(TEST_SLOT, PCI_BASE_ADDRESS_1, 4), IO_BASE | 1,
              "BAR1 pre-assigned at 0x1000, IO space bit set");

    CHECK_U64(linux_bar_size(TEST_SLOT, 0), PCI_TEST_MMIO_SIZE, "BAR0 size");
    CHECK_U64(linux_bar_size(TEST_SLOT, 1), PCI_TEST_IO_SIZE, "BAR1 size");
    CHECK_U64(linux_bar_size(TEST_SLOT, 2), 0, "BAR2 unimplemented");
    CHECK_U64(linux_bar_size(0, 0), 0, "host bridge has no BAR0");

    /* 写全 1 时读回的原始值：地址位只剩 size 以上的，类型位保留 */
    cfg_w(TEST_SLOT, PCI_BASE_ADDRESS_0, 4, ~0u);
    CHECK_U64(cfg_r(TEST_SLOT, PCI_BASE_ADDRESS_0, 4), 0xFFFFF000u, "BAR0 all ones");
    cfg_w(TEST_SLOT, PCI_BASE_ADDRESS_1, 4, ~0u);
    CHECK_U64(cfg_r(TEST_SLOT, PCI_BASE_ADDRESS_1, 4), 0x0000FFF1u,
              "BAR1 all ones: upper 16 bits RO 0 (64K port space), bit 0 = IO");
    cfg_w(TEST_SLOT, PCI_ROM_ADDRESS, 4, ~0u);
    CHECK_U64(cfg_r(TEST_SLOT, PCI_ROM_ADDRESS, 4), 0, "no expansion ROM");

    /* 探测序列结束后 BAR 恢复原值、decode 恢复 */
    setup();
    linux_bar_size(TEST_SLOT, 0);
    CHECK_U64(cfg_r(TEST_SLOT, PCI_BASE_ADDRESS_0, 4), MMIO_BASE, "BAR0 restored");
    CHECK_U64(mmio_r(MMIO_BASE, 4), PCI_TEST_MAGIC, "BAR0 decodes again after sizing");
}

static void case_routing(void)
{
    T_CASE("BAR 路由：两个 BAR 落到同一组寄存器，各自报告 BAR 编号");
    setup();

    CHECK_U64(mmio_r(MMIO_BASE + PCI_TEST_REG_MAGIC, 4), PCI_TEST_MAGIC, "magic via MMIO");
    CHECK_U64(in(IO_BASE + PCI_TEST_REG_MAGIC, 4), PCI_TEST_MAGIC, "magic via IO");
    CHECK_U64(mmio_r(MMIO_BASE + PCI_TEST_REG_BAR, 4), 0, "MMIO access went through BAR0");
    CHECK_U64(in(IO_BASE + PCI_TEST_REG_BAR, 4), 1, "IO access went through BAR1");

    out(IO_BASE + PCI_TEST_REG_SCRATCH, 4, 0x12345678u);
    CHECK_U64(mmio_r(MMIO_BASE + PCI_TEST_REG_SCRATCH, 4), 0x12345678u,
              "scratch written via IO is visible via MMIO");
    mmio_w(MMIO_BASE + PCI_TEST_REG_SCRATCH + 1, 1, 0xAB);
    CHECK_U64(in(IO_BASE + PCI_TEST_REG_SCRATCH, 4), 0x1234AB78u,
              "byte write at offset 5 changes only that byte");
    mmio_w(MMIO_BASE + PCI_TEST_REG_MAGIC, 4, 0);
    CHECK_U64(mmio_r(MMIO_BASE, 1), 't', "magic is read-only; byte 0 is 't'");
    CHECK_U64(mmio_r(MMIO_BASE + 0x800, 4), 0, "unused offsets read 0");
    CHECK_U64(mmio_r(MMIO_BASE + PCI_TEST_MMIO_SIZE, 4), 0xFFFFFFFFu,
              "one past BAR0 is unhandled");
}

static void case_bar_move(void)
{
    T_CASE("BAR 搬家：guest 写 BAR 后旧地址不再响应、新地址响应");
    setup();

    cfg_w(TEST_SLOT, PCI_BASE_ADDRESS_0, 4, 0xC0100000u);
    CHECK_U64(mmio_r(0xC0100000u, 4), PCI_TEST_MAGIC, "new MMIO address decodes");
    CHECK_U64(mmio_r(MMIO_BASE, 4), 0xFFFFFFFFu, "old MMIO address is free");

    cfg_w(TEST_SLOT, PCI_BASE_ADDRESS_1, 4, 0x2000);
    CHECK_U64(in(0x2000, 4), PCI_TEST_MAGIC, "new IO address decodes");
    CHECK_U64(in(IO_BASE, 4), 0xFFFFFFFFu, "old IO address is free");

    /* 低位不可写：写一个没对齐的地址，BAR 自动对齐到 size */
    cfg_w(TEST_SLOT, PCI_BASE_ADDRESS_0, 4, 0xC0200123u);
    CHECK_U64(cfg_r(TEST_SLOT, PCI_BASE_ADDRESS_0, 4), 0xC0200000u,
              "address bits below the BAR size are RO 0");
    CHECK_U64(mmio_r(0xC0200000u, 4), PCI_TEST_MAGIC, "aligned address decodes");

    /* 按字节写 BAR（setpci 之类的工具会这样做）：每一步都按当时的值重算 */
    cfg_w(TEST_SLOT, PCI_BASE_ADDRESS_0 + 2, 1, 0x30);
    CHECK_U64(mmio_r(0xC0300000u, 4), PCI_TEST_MAGIC, "byte-wide BAR write moves too");

    /* exit 统计记在 BAR 的名字下，不随搬家丢失 */
    {
        int i, found = 0;

        for (i = 0; i < g_vm.nr_mmio_devs; i++)
            if (!strcmp(g_vm.mmio_devs[i].name, "pci-test.bar0")) {
                found = 1;
                CHECK_U64(g_vm.mmio_stats[i].reads, 3,
                          "three MMIO reads hit BAR0 at three addresses");
            }
        CHECK(found, "pci-test.bar0 is in the MMIO device table");
    }
}

static void case_decode(void)
{
    T_CASE("command decode 位：关掉后对应空间的 BAR 不响应，打开后恢复");
    setup();

    cfg_w(TEST_SLOT, PCI_COMMAND, 2, PCI_COMMAND_IO);
    CHECK_U64(mmio_r(MMIO_BASE, 4), 0xFFFFFFFFu, "MEM off: BAR0 silent");
    CHECK_U64(in(IO_BASE, 4), PCI_TEST_MAGIC, "IO still on");

    cfg_w(TEST_SLOT, PCI_COMMAND, 2, 0);
    CHECK_U64(in(IO_BASE, 4), 0xFFFFFFFFu, "IO off: BAR1 silent");

    /* decode 关着时改 BAR，打开时才在新地址出现 */
    cfg_w(TEST_SLOT, PCI_BASE_ADDRESS_0, 4, 0xC0400000u);
    CHECK_U64(mmio_r(0xC0400000u, 4), 0xFFFFFFFFu, "still off after BAR write");
    cfg_w(TEST_SLOT, PCI_COMMAND, 2, PCI_COMMAND_IO | PCI_COMMAND_MEMORY);
    CHECK_U64(mmio_r(0xC0400000u, 4), PCI_TEST_MAGIC, "on at the new address");
    CHECK_U64(in(IO_BASE, 4), PCI_TEST_MAGIC, "IO back");
}

static void case_static_wins(void)
{
    T_CASE("BAR 编到已注册设备的地址上：先注册的设备优先，guest 不能劫持它");
    setup();

    /* 把 IO BAR 编到 conf1 自己的端口上 */
    cfg_w(TEST_SLOT, PCI_BASE_ADDRESS_1, 4, 0xCF0);
    CHECK_U64(in(0xCF0, 4), PCI_TEST_MAGIC, "non-overlapping part decodes");
    out(0xCF8, 4, 0x80000000u);
    CHECK_U64(in(0xCF8, 4), 0x80000000u, "0xCF8 still belongs to conf1");
    CHECK_U64(in(0xCFC, 2), PCI_VENDOR_ID_MINIVMM, "0xCFC still belongs to conf1");
}

static int dummy_rw(void *o, uint64_t off, uint32_t size, void *data)
{
    (void)o; (void)off;
    memset(data, 0, size);
    return VMM_OK;
}

static void case_register_errors(void)
{
    static struct pci_device devs[PCI_MAX_SLOTS + 1];
    int i, r;

    T_CASE("注册：非法 BAR 大小、窗口耗尽、slot 耗尽都要报错");
    vm_reset();
    CHECK_U64(pci_bus_init(&g_bus, &g_vm), VMM_OK, "pci_bus_init");

    memset(devs, 0, sizeof(devs));
    devs[0].name = "bad-io";
    devs[0].bars[0].type = PCI_BAR_IO;
    devs[0].bars[0].size = 512;
    devs[0].bars[0].read = dummy_rw;
    CHECK_U64(pci_register_device(&g_bus, &devs[0]), VMM_ERR_INVAL,
              "IO BAR larger than 256 bytes");
    devs[0].bars[0].size = 24;
    CHECK_U64(pci_register_device(&g_bus, &devs[0]), VMM_ERR_INVAL,
              "size not a power of two");
    CHECK(g_bus.devs[1] == NULL, "rejected device must not occupy a slot");

    /* 256 MiB 的 BAR：窗口 [0xC0000000, 0xFEC00000) 放得下 3 个 */
    vm_reset();
    pci_bus_init(&g_bus, &g_vm);
    for (i = 0; i < 4; i++) {
        devs[i].name = "big";
        devs[i].bars[0].type = PCI_BAR_MEM32;
        devs[i].bars[0].size = 256u << 20;
        devs[i].bars[0].read = dummy_rw;
        r = pci_register_device(&g_bus, &devs[i]);
        CHECK_U64(r, i < 3 ? VMM_OK : VMM_ERR_NOMEM,
                  "256 MiB BAR #%d (window ends at 0xFEC00000)", i);
    }

    vm_reset();
    pci_bus_init(&g_bus, &g_vm);
    memset(devs, 0, sizeof(devs));
    for (i = 1; i <= PCI_MAX_SLOTS; i++) {
        devs[i].name = "nobar";
        r = pci_register_device(&g_bus, &devs[i]);
        CHECK_U64(r, i < PCI_MAX_SLOTS ? VMM_OK : VMM_ERR_NOMEM,
                  "device #%d (slot 0 is the host bridge)", i);
    }
}

/* 共线的两个设备：slot 1 的 INTA（pci-test）和 slot 2 的 INTD 都接 GSI 17
 * （16 + ((slot + pin - 1) & 3)）；slot 3 的 INTA 接 GSI 19，用来确认
 * 别的线不受影响。 */
static struct pci_device g_peer, g_other;

static void setup_shared(void)
{
    setup();
    memset(&g_peer, 0, sizeof(g_peer));
    g_peer.name     = "peer";
    g_peer.intx_pin = 4;
    CHECK_U64(pci_register_device(&g_bus, &g_peer), VMM_OK, "register peer");
    memset(&g_other, 0, sizeof(g_other));
    g_other.name     = "other";
    g_other.intx_pin = 1;
    CHECK_U64(pci_register_device(&g_bus, &g_other), VMM_OK, "register other");
    CHECK_U64(pci_intx_gsi(g_dev.pci.slot, 1), 17, "pci-test INTA -> GSI 17");
    CHECK_U64(pci_intx_gsi(g_peer.slot, 4), 17, "peer INTD -> GSI 17");
    CHECK_U64(pci_intx_gsi(g_other.slot, 1), 19, "other INTA -> GSI 19");
}

#define IRQ_LOG(want, msg)  CHECK(!strcmp(g_irq_log, (want)), \
                                  "%s: KVM_IRQ_LINE log \"%s\", want \"%s\"", \
                                  (msg), g_irq_log, (want))

static uint32_t status_int(int slot)
{
    return cfg_r(slot, PCI_STATUS, 2) & PCI_STATUS_INTERRUPT;
}

static void case_intx_level(void)
{
    T_CASE("INTx 电平：拉高、撤销各发一次 KVM_IRQ_LINE，重复设置不重发");
    setup_shared();

    pci_set_irq(&g_dev.pci, true);
    IRQ_LOG("17=1", "assert");
    CHECK_U64(status_int(TEST_SLOT), PCI_STATUS_INTERRUPT, "status.INT set");
    pci_set_irq(&g_dev.pci, true);
    IRQ_LOG("17=1", "asserting again changes nothing");
    pci_set_irq(&g_dev.pci, false);
    IRQ_LOG("17=1 17=0", "deassert lowers the level-triggered pin");
    CHECK_U64(status_int(TEST_SLOT), 0, "status.INT clear");
    pci_set_irq(&g_dev.pci, false);
    IRQ_LOG("17=1 17=0", "deasserting again changes nothing");
}

static void case_intx_shared(void)
{
    T_CASE("INTx 共线：线电平是所有设备输出的或，一个撤销不能吞掉另一个");
    setup_shared();

    pci_set_irq(&g_dev.pci, true);
    pci_set_irq(&g_peer, true);
    IRQ_LOG("17=1", "second device on a high line: no new edge");
    pci_set_irq(&g_dev.pci, false);
    IRQ_LOG("17=1", "peer still requests: line stays high");
    CHECK_U64(status_int(TEST_SLOT), 0, "pci-test status.INT clear");
    CHECK_U64(status_int(g_peer.slot), PCI_STATUS_INTERRUPT, "peer status.INT set");
    pci_set_irq(&g_other, true);
    IRQ_LOG("17=1 19=1", "other line is independent");
    pci_set_irq(&g_peer, false);
    IRQ_LOG("17=1 19=1 17=0", "last requester gone: line low");
}

static void case_intx_disable(void)
{
    T_CASE("Interrupt Disable：屏蔽设备输出，status.INT 照样反映设备电平");
    setup_shared();

    pci_set_irq(&g_dev.pci, true);
    cfg_w(TEST_SLOT, PCI_COMMAND, 2,
          PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_INTX_DISABLE);
    IRQ_LOG("17=1 17=0", "setting INTx Disable drops a pending interrupt");
    CHECK_U64(status_int(TEST_SLOT), PCI_STATUS_INTERRUPT,
              "status.INT still reflects the device while masked");
    cfg_w(TEST_SLOT, PCI_COMMAND, 2, PCI_COMMAND_IO | PCI_COMMAND_MEMORY);
    IRQ_LOG("17=1 17=0 17=1", "clearing INTx Disable re-asserts the line");

    cfg_w(TEST_SLOT, PCI_COMMAND, 2,
          PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_INTX_DISABLE);
    pci_set_irq(&g_dev.pci, false);
    pci_set_irq(&g_dev.pci, true);
    IRQ_LOG("17=1 17=0 17=1 17=0", "requests while masked stay off the line");

    /* 被屏蔽的设备不能把共线设备的中断压下去 */
    pci_set_irq(&g_peer, true);
    IRQ_LOG("17=1 17=0 17=1 17=0 17=1", "unmasked peer still drives the line");

    /* 字节写 command 的高字节（bit 10 在第二个字节）也要生效 */
    pci_set_irq(&g_peer, false);
    g_irq_log[0] = 0;
    cfg_w(TEST_SLOT, PCI_COMMAND + 1, 1, 0);
    IRQ_LOG("17=1", "byte write to COMMAND[15:8] clears INTx Disable");
}

static void case_bus_master(void)
{
    static const uint8_t pattern[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    struct pci_device dma;
    uint8_t buf[4];
    void *hva;
    int slot;

    T_CASE("Bus Master：有 DMA 能力的设备该位可写、默认关闭，关闭时 DMA 被拒");
    setup();
    memset(&dma, 0, sizeof(dma));
    dma.name       = "dma";
    dma.bus_master = true;
    CHECK_U64(pci_register_device(&g_bus, &dma), VMM_OK, "register dma device");
    slot = dma.slot;

    CHECK_U64(cfg_r(slot, PCI_COMMAND, 2) & PCI_COMMAND_MASTER, 0,
              "bus master off until the driver calls pci_set_master");
    memset(g_ram, 0, sizeof(g_ram));
    CHECK_U64(pci_dma_write(&dma, 0x100, pattern, 4), VMM_ERR_GUEST,
              "DMA write refused with bus master off");
    CHECK_U64(g_ram[0x100], 0, "refused write left guest memory untouched");
    memset(buf, 0x55, sizeof(buf));
    CHECK_U64(pci_dma_read(&dma, 0x100, buf, 4), VMM_ERR_GUEST,
              "DMA read refused with bus master off");
    CHECK_U64(buf[0], 0x55, "refused read left the buffer untouched");

    cfg_w(slot, PCI_COMMAND, 2, PCI_COMMAND_MASTER);
    CHECK_U64(cfg_r(slot, PCI_COMMAND, 2), PCI_COMMAND_MASTER, "bus master is RW");
    CHECK_U64(pci_dma_write(&dma, 0x100, pattern, 4), VMM_OK, "DMA write allowed");
    CHECK_U64(pci_dma_read(&dma, 0x100, buf, 4), VMM_OK, "DMA read allowed");
    CHECK(!memcmp(buf, pattern, 4), "DMA round trip");
    CHECK_U64(pci_dma_read(&dma, sizeof(g_ram) - 2, buf, 4), VMM_ERR_GUEST,
              "out-of-range DMA still fails in the memory layer");
    hva = NULL;
    CHECK_U64(pci_dma_map(&dma, 0x100, 4, &hva), VMM_OK, "DMA map allowed");
    CHECK(hva == (void *)(g_ram + 0x100), "map points at the guest page");
    hva = &dma;
    CHECK_U64(pci_dma_map(&dma, sizeof(g_ram) - 2, 4, &hva), VMM_ERR_GUEST,
              "out-of-range map fails in the memory layer");
    CHECK(hva == (void *)&dma, "failed map leaves *hva untouched");

    cfg_w(slot, PCI_COMMAND, 2, 0);
    CHECK_U64(pci_dma_read(&dma, 0x100, buf, 4), VMM_ERR_GUEST,
              "turning bus master off stops DMA again");
    CHECK_U64(pci_dma_map(&dma, 0x100, 4, &hva), VMM_ERR_GUEST,
              "map refused with bus master off");
    CHECK(hva == (void *)&dma, "refused map leaves *hva untouched");
}

/* ---- MSI ---- */

static struct pci_device g_msi;

#define CAP     PCI_MSI_CAP_OFFSET
#define MSI_LOG(want, msg)  CHECK(!strcmp(g_msi_log, (want)), \
                                  "%s: KVM_SIGNAL_MSI log \"%s\", want \"%s\"", \
                                  (msg), g_msi_log, (want))

/* 一个带 MSI 和 INTA 的设备，放在 pci-test 后面的 slot 2（INTA -> GSI 18） */
static void setup_msi(void)
{
    setup();
    memset(&g_msi, 0, sizeof(g_msi));
    g_msi.name     = "msi";
    g_msi.intx_pin = 1;
    g_msi.msi      = true;
    CHECK_U64(pci_register_device(&g_bus, &g_msi), VMM_OK, "register msi device");
    CHECK_U64(g_msi.slot, 2, "msi device in slot 2");
    g_msi_log[0] = 0;
    g_msi_ret = VMM_OK;
    g_drop_logs = 0;
}

/* Linux 的 pci_find_capability：从 0x34 出发沿 next 指针找 ID，低两位屏蔽 */
static int find_cap(int slot, int id)
{
    int pos, ttl = 48;

    if (!(cfg_r(slot, PCI_STATUS, 2) & PCI_STATUS_CAP_LIST))
        return 0;
    pos = cfg_r(slot, PCI_CAPABILITY_LIST, 1);
    while (ttl-- && pos >= 0x40) {
        pos &= ~3;
        if (cfg_r(slot, pos + PCI_CAP_LIST_ID, 1) == (uint32_t)id)
            return pos;
        pos = cfg_r(slot, pos + PCI_CAP_LIST_NEXT, 1);
    }
    return 0;
}

/* 驱动的做法：写地址、高地址、数据，打开 Bus Master，最后置 MSI Enable */
static void msi_program(uint32_t lo, uint32_t hi, uint32_t data)
{
    cfg_w(2, CAP + PCI_MSI_ADDRESS_LO, 4, lo);
    cfg_w(2, CAP + PCI_MSI_ADDRESS_HI, 4, hi);
    cfg_w(2, CAP + PCI_MSI_DATA_64, 2, data);
    cfg_w(2, PCI_COMMAND, 2, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    cfg_w(2, CAP + PCI_MSI_FLAGS, 2, PCI_MSI_FLAGS_ENABLE);
}

static void case_msi_cap(void)
{
    T_CASE("MSI capability：链表、布局、只读位和可写位（PCI 3.0 第 6.7、6.8.1 节）");
    setup_msi();

    CHECK_U64(find_cap(TEST_SLOT, PCI_CAP_ID_MSI), 0, "device without msi has no capability list");
    CHECK_U64(cfg_r(TEST_SLOT, PCI_CAPABILITY_LIST, 1), 0, "and a zero capability pointer");
    CHECK_U64(find_cap(2, PCI_CAP_ID_MSI), CAP, "Linux walk finds MSI at 0x40");
    CHECK_U64(cfg_r(2, CAP + PCI_CAP_LIST_NEXT, 1), 0, "MSI is the last capability");
    CHECK_U64(cfg_r(2, CAP + PCI_MSI_FLAGS, 2), PCI_MSI_FLAGS_64BIT,
              "control after reset: 64-bit, one vector, no masking, disabled");
    CHECK(!pci_msi_enabled(&g_msi), "MSI disabled after reset");

    cfg_w(2, PCI_CAPABILITY_LIST, 1, 0x80);
    cfg_w(2, CAP, 2, 0xFFFF);
    CHECK_U64(cfg_r(2, PCI_CAPABILITY_LIST, 1), CAP, "capability pointer is read-only");
    CHECK_U64(cfg_r(2, CAP, 2), PCI_CAP_ID_MSI, "ID and next pointer are read-only");

    cfg_w(2, CAP + PCI_MSI_FLAGS, 2, 0xFFFF);
    CHECK_U64(cfg_r(2, CAP + PCI_MSI_FLAGS, 2),
              PCI_MSI_FLAGS_64BIT | PCI_MSI_FLAGS_QSIZE | PCI_MSI_FLAGS_ENABLE,
              "only MSI Enable and Multiple Message Enable are writable");
    cfg_w(2, CAP + PCI_MSI_FLAGS, 2, 0);
    CHECK_U64(cfg_r(2, CAP + PCI_MSI_FLAGS, 2), PCI_MSI_FLAGS_64BIT, "and they clear again");

    cfg_w(2, CAP + PCI_MSI_ADDRESS_LO, 4, 0xFFFFFFFFu);
    CHECK_U64(cfg_r(2, CAP + PCI_MSI_ADDRESS_LO, 4), 0xFFFFFFFCu, "address bits 1:0 read as 0");
    cfg_w(2, CAP + PCI_MSI_ADDRESS_HI, 4, 0xFFFFFFFFu);
    CHECK_U64(cfg_r(2, CAP + PCI_MSI_ADDRESS_HI, 4), 0xFFFFFFFFu, "upper address is RW");
    cfg_w(2, CAP + PCI_MSI_DATA_64, 4, 0xFFFFFFFFu);
    CHECK_U64(cfg_r(2, CAP + PCI_MSI_DATA_64, 4), 0xFFFF,
              "data is 16 bits at +0xC, the next two bytes are reserved");
    cfg_w(2, CAP + PCI_MSI_DATA_64 + 4, 4, 0xFFFFFFFFu);
    CHECK_U64(cfg_r(2, CAP + PCI_MSI_DATA_64 + 4, 4), 0,
              "no mask/pending registers after the data (no per-vector masking)");

    cfg_w(2, PCI_COMMAND, 2, 0xFFFF);
    CHECK_U64(cfg_r(2, PCI_COMMAND, 2) & PCI_COMMAND_MASTER, PCI_COMMAND_MASTER,
              "sending MSI makes Bus Master writable even without DMA");
}

static void case_msi_intx(void)
{
    T_CASE("MSI Enable 与 INTx 互斥：打开时撤下线上的请求，关掉时挂着的请求回到线上");
    setup_msi();

    pci_set_irq(&g_msi, true);
    IRQ_LOG("18=1", "INTx before MSI");
    msi_program(0xFEE01004u, 0, 0x21);
    CHECK(pci_msi_enabled(&g_msi), "MSI enabled");
    IRQ_LOG("18=1 18=0", "enabling MSI takes the request off the INTx line");
    CHECK_U64(status_int(2), PCI_STATUS_INTERRUPT, "status.INT still follows the device");
    pci_set_irq(&g_msi, false);
    pci_set_irq(&g_msi, true);
    IRQ_LOG("18=1 18=0", "level changes while MSI is on stay off the line");
    MSI_LOG("", "pci_set_irq alone never sends a message");

    cfg_w(2, CAP + PCI_MSI_FLAGS, 1, 0);
    CHECK(!pci_msi_enabled(&g_msi), "byte write clears MSI Enable");
    IRQ_LOG("18=1 18=0 18=1", "disabling MSI puts the pending request back on INTx");
    pci_set_irq(&g_msi, false);
    IRQ_LOG("18=1 18=0 18=1 18=0", "and INTx works as before");

    /* MSI 打开时撤下的是这个设备的输出，共线的别的设备照样能拉线 */
    cfg_w(2, CAP + PCI_MSI_FLAGS, 2, PCI_MSI_FLAGS_ENABLE);
    pci_set_irq(&g_msi, true);
    g_irq_log[0] = 0;
    cfg_w(2, CAP + PCI_MSI_FLAGS, 2, 0);
    IRQ_LOG("18=1", "16-bit write clearing MSI Enable");
}

static void case_msi_send(void)
{
    T_CASE("发消息：64 位地址拼接、数据只取 16 位、Bus Master 关着不发");
    setup_msi();

    msi_program(0xFEE01004u, 0, 0x21);
    cfg_w(2, CAP + PCI_MSI_DATA_64, 4, 0xABCD0021u);
    pci_msi_notify(&g_msi);
    MSI_LOG("fee01004:21", "one message with the programmed address and 16-bit data");
    CHECK_U64(g_msi.msi_sent, 1, "counted as sent");
    pci_msi_notify(&g_msi);
    MSI_LOG("fee01004:21 fee01004:21", "each notify is one message");

    /* KVM 不看高地址；高地址非 0 的消息在真实机器上是一次普通内存写 */
    cfg_w(2, CAP + PCI_MSI_ADDRESS_HI, 4, 1);
    pci_msi_notify(&g_msi);
    MSI_LOG("fee01004:21 fee01004:21", "upper address set: not sent");
    CHECK(strstr(g_last_log, "pci 00:02.0 msi: MSI address 0x1fee01004 data 0x0021 not delivered: "
                 "address is outside the interrupt window") != NULL,
          "diagnostic shows the 64-bit address: %s", g_last_log);
    cfg_w(2, CAP + PCI_MSI_ADDRESS_HI, 4, 0);

    cfg_w(2, PCI_COMMAND, 2, PCI_COMMAND_MEMORY);
    pci_msi_notify(&g_msi);
    MSI_LOG("fee01004:21 fee01004:21", "bus master off: not sent");
    CHECK(strstr(g_last_log, "not delivered: bus master disabled") != NULL,
          "diagnostic names bus master: %s", g_last_log);
    CHECK_U64(g_msi.msi_dropped, 2, "two dropped");

    cfg_w(2, PCI_COMMAND, 2, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    cfg_w(2, CAP + PCI_MSI_FLAGS, 2, 0);
    pci_msi_notify(&g_msi);
    MSI_LOG("fee01004:21 fee01004:21", "MSI disabled: notify is a device-model bug, not sent");
    CHECK(strstr(g_last_log, "pci_msi_notify with MSI disabled") != NULL,
          "bug is logged: %s", g_last_log);
}

/* 改写数据后发一条，返回是否交给了 KVM */
static bool send_with(uint32_t lo, uint32_t data)
{
    size_t n = strlen(g_msi_log);

    cfg_w(2, CAP + PCI_MSI_ADDRESS_LO, 4, lo);
    cfg_w(2, CAP + PCI_MSI_DATA_64, 2, data);
    pci_msi_notify(&g_msi);
    return strlen(g_msi_log) != n;
}

static void case_msi_invalid(void)
{
    unsigned i;

    T_CASE("非法消息：不在中断窗口、保留投递模式、异常向量，报原因不投递");
    setup_msi();
    msi_program(0xFEE01004u, 0, 0x21);

    CHECK(!send_with(0xFED01004u, 0x21), "address 0xfed01004 is not an interrupt");
    CHECK(strstr(g_last_log, "outside the interrupt window") != NULL, "%s", g_last_log);
    CHECK(!send_with(0xFEE01004u, 0x0321), "delivery mode 011 is reserved");
    CHECK(strstr(g_last_log, "reserved delivery mode") != NULL, "%s", g_last_log);
    CHECK(!send_with(0xFEE01004u, 0x0621), "delivery mode 110 is reserved");
    CHECK(!send_with(0xFEE01004u, 0x000F), "fixed vector 0x0f is an exception vector");
    CHECK(strstr(g_last_log, "vector outside 0x10-0xfe") != NULL, "%s", g_last_log);
    CHECK(!send_with(0xFEE01004u, 0x01FF), "lowest-priority vector 0xff is out of range");
    CHECK_U64(g_drop_logs, 5, "every new bad message is logged at once");

    CHECK(send_with(0xFEE01004u, 0x0010), "vector 0x10 is the lowest legal one");
    CHECK(send_with(0xFEE01004u, 0x00FE), "vector 0xfe is the highest legal one");
    CHECK(send_with(0xFEE01004u, 0x0405), "NMI ignores the vector field");
    CHECK(send_with(0xFEEFFFFCu, 0x8021), "reserved address bits, RH/DM and level trigger pass to KVM");

    /* KVM 没有 vCPU 收下：原因来自 irqchip 层 */
    g_msi_ret = VMM_ERR_GUEST;
    g_msi_why = "no LAPIC matches the destination";
    g_drop_logs = 0;
    for (i = 0; i < 5; i++)
        send_with(0xFEE00004u, 0x21);
    CHECK(strstr(g_last_log, "MSI address 0xfee00004 data 0x0021 not delivered: "
                 "no LAPIC matches the destination (4 with this address/data") != NULL,
          "KVM reason with the run count: %s", g_last_log);
    CHECK_U64(g_drop_logs, 3, "same message repeated: logged at the 1st, 2nd and 4th drop");
    send_with(0xFEE00004u, 0x22);
    CHECK_U64(g_drop_logs, 4, "a different bad message is logged at once");
    CHECK(strstr(g_last_log, "(1 with this address/data, 11 in total)") != NULL,
          "run restarts, the total also counts the 5 format errors above: %s", g_last_log);
    g_msi_ret = VMM_OK;
    CHECK(send_with(0xFEE01004u, 0x21), "delivered again once the guest fixes it");
    g_msi_ret = VMM_ERR_GUEST;
    g_drop_logs = 0;
    send_with(0xFEE00004u, 0x22);
    CHECK_U64(g_drop_logs, 1, "a success in between restarts the run");
    g_msi_ret = VMM_OK;
}

int main(int argc, char **argv)
{
    int i;

    T_MAIN_BEGIN();
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-v"))
            vmm_log_level = 3;

    case_conf1_address();
    case_conf1_data();
    case_ecs_bits();
    case_readonly();
    case_command();
    case_bar_sizing();
    case_routing();
    case_bar_move();
    case_decode();
    case_static_wins();
    case_register_errors();
    case_intx_level();
    case_intx_shared();
    case_intx_disable();
    case_bus_master();
    case_msi_cap();
    case_msi_intx();
    case_msi_send();
    case_msi_invalid();
    return T_SUMMARY();
}
