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
    return T_SUMMARY();
}
