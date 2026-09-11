/* src/boot/mptable.c — 构造 MP 浮动指针 + MP 配置表           [Step 2]
 *
 * 内存布局（全部在 MPTABLE_GPA 起的 1KiB 内）：
 *   mpf_intel      16 字节，"_MP_"，physptr 指向紧随其后的 mpc_table
 *   mpc_table      44 字节表头，"PCMP"
 *   processor × N  每个 vCPU 一项，20 字节
 *   bus            ISA 总线，8 字节
 *   ioapic         8 字节
 *   intsrc × 16    ISA IRQ i -> IOAPIC pin i，每项 8 字节
 *   lintsrc × 2    LINT0 = ExtINT（8259 级联），LINT1 = NMI
 *
 * ISA IRQ i 直接接 IOAPIC pin i，是因为 KVM 内核态 irqchip 的默认 GSI
 * 路由就是这样（包括 PIT 的 GSI 0 -> pin 0）。和 qemu 那种
 * "IRQ0 -> pin2 + 中断源覆盖" 的做法不同，照抄 qemu 的表会让时钟中断丢失。
 */
#include <string.h>

#include "vmm.h"
#include "memory.h"
#include "boot/mptable.h"

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

int mptable_setup(struct vmm_vm *vm, int nr_cpus)
{
    uint8_t *base, *p;
    struct mpf_intel *mpf;
    struct mpc_table *mpc;
    uint8_t ioapic_id = (uint8_t)nr_cpus;   /* 紧跟在 LAPIC id 之后 */
    uint16_t nr_entries = 0;
    size_t size;
    int i;

    size = sizeof(*mpf) + sizeof(*mpc)
         + (size_t)nr_cpus * sizeof(struct mpc_cpu)
         + sizeof(struct mpc_bus) + sizeof(struct mpc_ioapic)
         + 18 * sizeof(struct mpc_intsrc);
    if (nr_cpus < 1 || nr_cpus > 254 || size > MPTABLE_MAX_SIZE) {
        vmm_err("mptable: %d CPUs do not fit into %d bytes",
                nr_cpus, MPTABLE_MAX_SIZE);
        return VMM_ERR_INVAL;
    }

    base = gpa_to_hva(vm, MPTABLE_GPA, MPTABLE_MAX_SIZE);
    if (!base)
        return VMM_ERR_INVAL;
    memset(base, 0, MPTABLE_MAX_SIZE);

    mpf = (struct mpf_intel *)base;
    mpc = (struct mpc_table *)(base + sizeof(*mpf));
    p   = (uint8_t *)(mpc + 1);

    for (i = 0; i < nr_cpus; i++) {
        struct mpc_cpu *c = (struct mpc_cpu *)p;

        c->type    = MP_PROCESSOR;
        c->apicid  = (uint8_t)i;
        c->apicver = APIC_VERSION;
        c->cpuflag = CPU_ENABLED | (i == 0 ? CPU_BOOTPROCESSOR : 0);
        /* family/model/stepping 与特性位 Linux 只拿来打印，填个合理值 */
        c->cpufeature  = 0x600;
        c->featureflag = 0x201;   /* FPU | APIC */
        p += sizeof(*c);
        nr_entries++;
    }

    {
        struct mpc_bus *b = (struct mpc_bus *)p;

        b->type  = MP_BUS;
        b->busid = 0;
        memcpy(b->bustype, "ISA   ", 6);
        p += sizeof(*b);
        nr_entries++;
    }

    {
        struct mpc_ioapic *io = (struct mpc_ioapic *)p;

        io->type     = MP_IOAPIC;
        io->apicid   = ioapic_id;
        io->apicver  = IOAPIC_VERSION;
        io->flags    = MP_IOAPIC_USABLE;
        io->apicaddr = MP_IOAPIC_BASE;
        p += sizeof(*io);
        nr_entries++;
    }

    for (i = 0; i < 16; i++) {
        struct mpc_intsrc *s = (struct mpc_intsrc *)p;

        s->type      = MP_INTSRC;
        s->irqtype   = MP_IRQTYPE_INT;
        s->srcbus    = 0;
        s->srcbusirq = (uint8_t)i;
        s->dstapic   = ioapic_id;
        s->dstirq    = (uint8_t)i;
        p += sizeof(*s);
        nr_entries++;
    }

    /* LINT0 接 8259 的 INTR（ExtINT），LINT1 接 NMI；dstapic 0xff =
     * 所有 LAPIC。结构和 intsrc 一样，只是 type 不同。 */
    {
        struct mpc_intsrc *l = (struct mpc_intsrc *)p;

        l->type    = MP_LINTSRC;
        l->irqtype = MP_IRQTYPE_EXTINT;
        l->dstapic = 0xff;
        l->dstirq  = 0;
        p += sizeof(*l);

        l = (struct mpc_intsrc *)p;
        l->type    = MP_LINTSRC;
        l->irqtype = MP_IRQTYPE_NMI;
        l->dstapic = 0xff;
        l->dstirq  = 1;
        p += sizeof(*l);
        nr_entries += 2;
    }

    memcpy(mpc->signature, "PCMP", 4);
    mpc->length   = (uint16_t)(p - (uint8_t *)mpc);
    mpc->spec     = MPC_SPEC;
    memcpy(mpc->oem, "MINIVMM ", 8);
    memcpy(mpc->productid, "000000000000", 12);
    mpc->oemcount = nr_entries;
    mpc->lapic    = MP_LAPIC_BASE;
    mpc->checksum = mp_checksum(mpc, mpc->length);

    memcpy(mpf->signature, "_MP_", 4);
    mpf->physptr       = (uint32_t)(MPTABLE_GPA + sizeof(*mpf));
    mpf->length        = 1;
    mpf->specification = MPC_SPEC;
    mpf->checksum      = mp_checksum(mpf, sizeof(*mpf));

    vmm_info("MP table @ GPA 0x%llx: %d CPU(s), IOAPIC id %u @ 0x%x, %u entries",
             (unsigned long long)MPTABLE_GPA, nr_cpus, ioapic_id,
             MP_IOAPIC_BASE, nr_entries);
    return VMM_OK;
}
