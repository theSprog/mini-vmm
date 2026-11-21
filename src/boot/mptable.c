/* src/boot/mptable.c — 构造 MP 浮动指针 + MP 配置表           [linux-boot]
 *
 * 内存布局（全部在 MPTABLE_GPA 起的 1KiB 内）：
 *   mpf_intel      16 字节，"_MP_"，physptr 指向紧随其后的 mpc_table
 *   mpc_table      44 字节表头，"PCMP"
 *   processor × N  每个 vCPU 一项，20 字节
 *   bus × 2        PCI 总线（id 0）、ISA 总线（id 1），各 8 字节
 *   ioapic         8 字节
 *   intsrc × 16    ISA IRQ i -> IOAPIC pin i，每项 8 字节
 *   intsrc × k     每个带 INTx 的 PCI 设备一项，接线同 DSDT 的 _PRT
 *   lintsrc × 2    LINT0 = ExtINT（8259 级联），LINT1 = NMI
 *
 * PCI 总线的 MP bus id 必须等于它的 PCI 总线号：Linux 查 PCI 中断时拿
 * dev->bus->number 直接去比 intsrc 的 srcbus（io_apic.c
 * IO_APIC_get_PCI_irq_vector）。所以 bus 0 留给 PCI，ISA 排在后面，
 * 和 SeaBIOS 生成的表一样。
 *
 * PCI 中断项只列已插的设备，不像 _PRT 那样列满 32 个 slot：MP table 的
 * 1KiB 空间放下 32 个 CPU 之后只剩 156 字节（19 项），而 MP table 本来也只在
 * 没有 ACPI（acpi=off）时使用，那种情况下没有热插拔。
 *
 * ISA IRQ i 直接接 IOAPIC pin i，是因为 KVM 内核态 irqchip 的默认 GSI
 * 路由就是这样（包括 PIT 的 GSI 0 -> pin 0），split irqchip 的用户态接线
 * （devices/split_irqchip.c）也按这个关系。和 qemu 那种
 * "IRQ0 -> pin2 + 中断源覆盖" 的做法不同，照抄 qemu 的表会让时钟中断丢失。
 */
#include <string.h>

#include "vmm.h"
#include "memory.h"
#include "boot/mptable.h"
#include "boot/acpi.h"
#include "pci/pci.h"

#define MPC_SPEC          4      /* MP spec 1.4 */
#define APIC_VERSION      0x14
#define IOAPIC_VERSION    0x11

#define MP_PROCESSOR      0
#define MP_BUS            1
#define MP_IOAPIC         2
#define MP_INTSRC         3
#define MP_LINTSRC        4

#define CPU_ENABLED       1
#define CPU_BOOTPROCESSOR 2

#define MP_IRQTYPE_INT    0
#define MP_IRQTYPE_NMI    1
#define MP_IRQTYPE_EXTINT 3

#define MP_IOAPIC_USABLE  1

/* intsrc 的 irqflag：bit 1:0 极性（PO），bit 3:2 触发方式（EL），编码与
 * MADT 的 MPS INTI flags 相同（ACPI 6.5 Table 5.26，那里注明它和 MPS 1.4
 * Table 4-10 一致）。11 = 低有效 / 电平触发 */
#define MP_IRQFLAG_ACTIVE_LOW  0x3
#define MP_IRQFLAG_LEVEL       (0x3 << 2)

#define MP_BUS_ID_PCI     0
#define MP_BUS_ID_ISA     1

struct mpf_intel {
    char     signature[4];
    uint32_t physptr;
    uint8_t  length;          /* 以 16 字节为单位 */
    uint8_t  specification;
    uint8_t  checksum;
    uint8_t  feature1;        /* 0 = 后面跟着真正的配置表 */
    uint8_t  feature2;
    uint8_t  feature3;
    uint8_t  feature4;
    uint8_t  feature5;
} __attribute__((packed));

struct mpc_table {
    char     signature[4];
    uint16_t length;          /* 表头 + 所有 entry */
    uint8_t  spec;
    uint8_t  checksum;
    char     oem[8];
    char     productid[12];
    uint32_t oemptr;
    uint16_t oemsize;
    uint16_t oemcount;        /* entry 个数 */
    uint32_t lapic;
    uint32_t reserved;
} __attribute__((packed));

struct mpc_cpu {
    uint8_t  type;
    uint8_t  apicid;
    uint8_t  apicver;
    uint8_t  cpuflag;
    uint32_t cpufeature;
    uint32_t featureflag;
    uint32_t reserved[2];
} __attribute__((packed));

struct mpc_bus {
    uint8_t type;
    uint8_t busid;
    char    bustype[6];
} __attribute__((packed));

struct mpc_ioapic {
    uint8_t  type;
    uint8_t  apicid;
    uint8_t  apicver;
    uint8_t  flags;
    uint32_t apicaddr;
} __attribute__((packed));

struct mpc_intsrc {
    uint8_t  type;
    uint8_t  irqtype;
    uint16_t irqflag;         /* 0 = 按总线默认极性/触发方式 */
    uint8_t  srcbus;
    uint8_t  srcbusirq;
    uint8_t  dstapic;
    uint8_t  dstirq;
} __attribute__((packed));

_Static_assert(sizeof(struct mpf_intel) == 16, "mpf_intel size");
_Static_assert(sizeof(struct mpc_table) == 44, "mpc_table size");
_Static_assert(sizeof(struct mpc_cpu) == 20, "mpc_cpu size");
_Static_assert(sizeof(struct mpc_bus) == 8, "mpc_bus size");
_Static_assert(sizeof(struct mpc_ioapic) == 8, "mpc_ioapic size");
_Static_assert(sizeof(struct mpc_intsrc) == 8, "mpc_intsrc size");

/* 规范要求：整张表所有字节相加 mod 256 == 0 */
static uint8_t mp_checksum(const void *p, size_t len)
{
    const uint8_t *b = p;
    uint8_t sum = 0;

    while (len--)
        sum += *b++;
    return (uint8_t)(-sum);
}

#define MP_ISA_IRQS       16     /* ISA IRQ 0-15 */
#define MP_ALL_LAPICS     0xff   /* lintsrc 的 dstapic：所有 LAPIC */
#define MP_LINT0          0
#define MP_LINT1          1
/* LAPIC id 是 0..N-1，IOAPIC 的 id 紧跟其后取 N；8 位 APIC id 里 0xff 是
 * 广播，所以 N 最大 254 */
#define MP_MAX_CPUS       254

/* processor entry 的 family/model/stepping 与特性位 Linux 只拿来打印，填个
 * 合理值 */
#define MP_CPU_SIGNATURE  0x600
#define MP_CPU_FEATURES   0x201  /* FPU | APIC */

static int count_pci_irqs(const struct pci_bus *pci)
{
    int i, n = 0;

    for (i = 0; pci && i < PCI_MAX_SLOTS; i++)
        if (pci->devs[i] && pci->devs[i]->intx_pin)
            n++;
    return n;
}

static size_t mp_table_size(int nr_cpus, int nr_pci_irqs)
{
    return sizeof(struct mpf_intel)
         + sizeof(struct mpc_table)
         + (size_t)nr_cpus * sizeof(struct mpc_cpu)
         + sizeof(struct mpc_bus)                          /* PCI */
         + sizeof(struct mpc_bus)                          /* ISA */
         + sizeof(struct mpc_ioapic)
         + MP_ISA_IRQS * sizeof(struct mpc_intsrc)         /* ISA IRQ 0-15 */
         + (size_t)nr_pci_irqs * sizeof(struct mpc_intsrc) /* PCI INTx */
         + sizeof(struct mpc_intsrc)                       /* LINT0 */
         + sizeof(struct mpc_intsrc);                      /* LINT1 */
}

/* entry 一项接一项写在 mpc_table 表头之后：每加一项游标后移、计数加一 */
struct mp_writer {
    uint8_t *p;
    uint16_t nr_entries;
    uint8_t  ioapic_id;
};

static void *mp_add(struct mp_writer *w, size_t size)
{
    void *e = w->p;

    w->p += size;
    w->nr_entries++;
    return e;
}

static void mp_add_cpus(struct mp_writer *w, int nr_cpus)
{
    int i;

    for (i = 0; i < nr_cpus; i++) {
        struct mpc_cpu *c = mp_add(w, sizeof(*c));

        c->type        = MP_PROCESSOR;
        c->apicid      = (uint8_t)i;
        c->apicver     = APIC_VERSION;
        c->cpuflag     = CPU_ENABLED | (i == 0 ? CPU_BOOTPROCESSOR : 0);
        c->cpufeature  = MP_CPU_SIGNATURE;
        c->featureflag = MP_CPU_FEATURES;
    }
}

static void mp_add_bus(struct mp_writer *w, uint8_t id, const char type[6])
{
    struct mpc_bus *b = mp_add(w, sizeof(*b));

    b->type  = MP_BUS;
    b->busid = id;
    memcpy(b->bustype, type, 6);
}

static void mp_add_ioapic(struct mp_writer *w)
{
    struct mpc_ioapic *io = mp_add(w, sizeof(*io));

    io->type     = MP_IOAPIC;
    io->apicid   = w->ioapic_id;
    io->apicver  = IOAPIC_VERSION;
    io->flags    = MP_IOAPIC_USABLE;
    io->apicaddr = MP_IOAPIC_BASE;
}

static struct mpc_intsrc *mp_add_intsrc(struct mp_writer *w, uint8_t bus,
                                        uint8_t bus_irq, uint8_t pin)
{
    struct mpc_intsrc *s = mp_add(w, sizeof(*s));

    s->type      = MP_INTSRC;
    s->irqtype   = MP_IRQTYPE_INT;
    s->srcbus    = bus;
    s->srcbusirq = bus_irq;
    s->dstapic   = w->ioapic_id;
    s->dstirq    = pin;
    return s;
}

static void mp_add_isa_irqs(struct mp_writer *w)
{
    int i;

    for (i = 0; i < MP_ISA_IRQS; i++) {
        struct mpc_intsrc *s = mp_add_intsrc(w, MP_BUS_ID_ISA, (uint8_t)i,
                                             (uint8_t)i);

        /* SCI 不是 ISA 的边沿中断：FADT SCI_INT 规定它是 level、active low，
         * MADT 没写 ISO 时 Linux 正是这样配置 IRQ 9 的。这里写成同样的属性，
         * 两份表描述的接线才一致 */
        if (i == ACPI_SCI_IRQ)
            s->irqflag = MP_IRQFLAG_ACTIVE_LOW | MP_IRQFLAG_LEVEL;
    }
}

/* PCI 中断：srcbusirq 的 bit 6:2 是 slot，bit 1:0 是 pin（0 = INTA），
 * Linux 按同样的位取用（io_apic.c IO_APIC_get_PCI_irq_vector）。irqflag 0
 * 表示“按总线默认”，PCI 总线默认就是电平触发、低有效。 */
static void mp_add_pci_irqs(struct mp_writer *w, const struct pci_bus *pci)
{
    int i;

    for (i = 0; pci && i < PCI_MAX_SLOTS; i++) {
        const struct pci_device *d = pci->devs[i];

        if (!d || !d->intx_pin)
            continue;
        mp_add_intsrc(w, MP_BUS_ID_PCI, (uint8_t)(i << 2 | (d->intx_pin - 1)),
                      (uint8_t)pci_intx_gsi(i, d->intx_pin));
    }
}

/* lintsrc 和 intsrc 结构相同，只是 type 不同，dstirq 是 LAPIC 的 LINT 引脚 */
static void mp_add_lint(struct mp_writer *w, uint8_t irqtype, uint8_t lint)
{
    struct mpc_intsrc *l = mp_add(w, sizeof(*l));

    l->type    = MP_LINTSRC;
    l->irqtype = irqtype;
    l->dstapic = MP_ALL_LAPICS;
    l->dstirq  = lint;
}

int mptable_setup(struct vmm_vm *vm, int nr_cpus, const struct pci_bus *pci)
{
    enum gpa_status gs;
    uint8_t *base;
    struct mpf_intel *mpf;
    struct mpc_table *mpc;
    struct mp_writer w;
    int nr_pci_irqs = count_pci_irqs(pci);

    if (nr_cpus < 1 || nr_cpus > MP_MAX_CPUS ||
        mp_table_size(nr_cpus, nr_pci_irqs) > MPTABLE_MAX_SIZE) {
        vmm_err("mptable: %d CPUs and %d PCI interrupts do not fit into %d bytes",
                nr_cpus, nr_pci_irqs, MPTABLE_MAX_SIZE);
        return VMM_ERR_INVAL;
    }

    gs = gpa_to_hva(vm, MPTABLE_GPA, MPTABLE_MAX_SIZE, (void **)&base);
    if (gs != GPA_OK) {
        vmm_err("MP table at GPA 0x%llx: %s",
                (unsigned long long)MPTABLE_GPA, gpa_status_str(gs));
        return VMM_ERR_INVAL;
    }
    memset(base, 0, MPTABLE_MAX_SIZE);

    mpf = (struct mpf_intel *)base;
    mpc = (struct mpc_table *)(mpf + 1);
    w.p          = (uint8_t *)(mpc + 1);
    w.nr_entries = 0;
    w.ioapic_id  = (uint8_t)nr_cpus;

    /* 顺序就是文件头列出的布局 */
    mp_add_cpus(&w, nr_cpus);
    mp_add_bus(&w, MP_BUS_ID_PCI, "PCI   ");
    mp_add_bus(&w, MP_BUS_ID_ISA, "ISA   ");
    mp_add_ioapic(&w);
    mp_add_isa_irqs(&w);
    mp_add_pci_irqs(&w, pci);
    mp_add_lint(&w, MP_IRQTYPE_EXTINT, MP_LINT0);   /* 8259 的 INTR */
    mp_add_lint(&w, MP_IRQTYPE_NMI, MP_LINT1);

    memcpy(mpc->signature, "PCMP", 4);
    mpc->length   = (uint16_t)(w.p - (uint8_t *)mpc);
    mpc->spec     = MPC_SPEC;
    memcpy(mpc->oem, "MINIVMM ", 8);
    memcpy(mpc->productid, "000000000000", 12);
    mpc->oemcount = w.nr_entries;
    mpc->lapic    = MP_LAPIC_BASE;
    mpc->checksum = mp_checksum(mpc, mpc->length);

    memcpy(mpf->signature, "_MP_", 4);
    mpf->physptr       = (uint32_t)(MPTABLE_GPA + sizeof(*mpf));
    mpf->length        = 1;
    mpf->specification = MPC_SPEC;
    mpf->checksum      = mp_checksum(mpf, sizeof(*mpf));

    vmm_info("MP table @ GPA 0x%llx: %d CPU(s), IOAPIC id %u @ 0x%x, %u entries",
             (unsigned long long)MPTABLE_GPA, nr_cpus, w.ioapic_id,
             MP_IOAPIC_BASE, w.nr_entries);
    return VMM_OK;
}
