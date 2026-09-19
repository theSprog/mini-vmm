/* src/boot/acpi.c — 最小 ACPI 表 + PM1a 寄存器块               [Step 2]
 *
 * 内存布局（ACPI_TABLES_GPA 起，每张表 16 字节对齐）：
 *   RSDP  -> XSDT -> { FADT -> DSDT, MADT }
 *
 * 参考 ACPI 6.4 spec：5.2.5 RSDP, 5.2.8 XSDT, 5.2.9 FADT,
 * 5.2.11.1 DSDT, 5.2.12 MADT, 4.8.3 PM1 寄存器。
 */
#include <string.h>

#include "vmm.h"
#include "memory.h"
#include "boot/acpi.h"
#include "boot/mptable.h"

#define ACPI_OEM_ID       "MINVMM"
#define ACPI_OEM_TABLE_ID "MINIVMM "
#define ACPI_CREATOR_ID   "MVMM"

struct acpi_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    char     creator_id[4];
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_rsdp {
    char     signature[8];      /* "RSD PTR " */
    uint8_t  checksum;          /* 前 20 字节 */
    char     oem_id[6];
    uint8_t  revision;          /* 2 = ACPI 2.0+，有 XSDT */
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  ext_checksum;      /* 全部 36 字节 */
    uint8_t  reserved[3];
} __attribute__((packed));

/* FADT rev 6，276 字节。只列我们要填的字段，其余靠 memset 0。 */
struct acpi_fadt {
    struct acpi_header h;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved0;
    uint8_t  preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;           /* 0 = 已经在 ACPI 模式，不用切换 */
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t  pm1_evt_len;
    uint8_t  pm1_cnt_len;
    uint8_t  pm2_cnt_len;
    uint8_t  pm_tmr_len;
    uint8_t  gpe0_blk_len;
    uint8_t  gpe1_blk_len;
    uint8_t  gpe1_base;
    uint8_t  cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alrm;
    uint8_t  mon_alrm;
    uint8_t  century;
    uint16_t iapc_boot_arch;
    uint8_t  reserved1;
    uint32_t flags;
    uint8_t  reset_reg[12];
    uint8_t  reset_value;
    uint16_t arm_boot_arch;
    uint8_t  minor_version;
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
    uint8_t  x_blocks[8][12];   /* X_PM1a_EVT ... X_GPE1，留 0 让 ACPICA 从 32 位字段转换 */
    uint8_t  sleep_control[12];
    uint8_t  sleep_status[12];
    uint64_t hypervisor_id;
} __attribute__((packed));

struct acpi_madt {
    struct acpi_header h;
    uint32_t lapic_addr;
    uint32_t flags;
} __attribute__((packed));

struct madt_lapic {
    uint8_t  type;              /* 0 */
    uint8_t  length;            /* 8 */
    uint8_t  processor_id;
    uint8_t  apic_id;
    uint32_t flags;             /* bit0 = enabled */
} __attribute__((packed));

struct madt_ioapic {
    uint8_t  type;              /* 1 */
    uint8_t  length;            /* 12 */
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t address;
    uint32_t gsi_base;
} __attribute__((packed));

struct madt_lapic_nmi {
    uint8_t  type;              /* 4 */
    uint8_t  length;            /* 6 */
    uint8_t  processor_id;      /* 0xff = 所有 CPU */
    uint16_t flags;
    uint8_t  lint;
} __attribute__((packed));

_Static_assert(sizeof(struct acpi_header) == 36, "acpi_header size");
_Static_assert(sizeof(struct acpi_rsdp) == 36, "rsdp size");
_Static_assert(sizeof(struct acpi_fadt) == 276, "fadt size");
_Static_assert(sizeof(struct madt_lapic) == 8, "madt_lapic size");
_Static_assert(sizeof(struct madt_ioapic) == 12, "madt_ioapic size");
_Static_assert(sizeof(struct madt_lapic_nmi) == 6, "madt_lapic_nmi size");

#define FADT_F_WBINVD        (1u << 0)
#define FADT_F_PWR_BUTTON    (1u << 4)   /* 置 1 = 没有固定功能电源键 */
#define FADT_F_SLP_BUTTON    (1u << 5)
#define FADT_BOOT_LEGACY_DEV (1u << 0)
#define FADT_BOOT_8042       (1u << 1)

#define MADT_PCAT_COMPAT     (1u << 0)   /* 同时存在 8259，Linux 会先屏蔽它 */

/* DSDT 的全部 AML：Name (_S5, Package () { 5, 5, 0, 0 })
 *   08          NameOp
 *   5F 53 35 5F "_S5_"
 *   12          PackageOp
 *   08          PkgLength（含自身共 8 字节）
 *   04          NumElements
 *   0A 05       BytePrefix 5   -> PM1a_CNT.SLP_TYP
 *   0A 05       BytePrefix 5   -> PM1b_CNT.SLP_TYP
 *   00 00       Zero Zero      保留
 * Linux poweroff 时查 \_S5 拿 SLP_TYP，然后写 PM1a_CNT = SLP_TYP<<10 | SLP_EN。 */
static const uint8_t dsdt_aml[] = {
    0x08, '_', 'S', '5', '_', 0x12, 0x08, 0x04,
    0x0a, 0x05, 0x0a, 0x05, 0x00, 0x00,
};

#define S5_SLP_TYP 5

static uint8_t acpi_checksum(const void *p, size_t len)
{
    const uint8_t *b = p;
    uint8_t sum = 0;

    while (len--)
        sum += *b++;
    return (uint8_t)(-sum);
}

static void fill_header(struct acpi_header *h, const char *sig,
                        uint32_t len, uint8_t rev)
{
    memcpy(h->signature, sig, 4);
    h->length   = len;
    h->revision = rev;
    memcpy(h->oem_id, ACPI_OEM_ID, 6);
    memcpy(h->oem_table_id, ACPI_OEM_TABLE_ID, 8);
    h->oem_revision = 1;
    memcpy(h->creator_id, ACPI_CREATOR_ID, 4);
    h->creator_revision = 1;
    h->checksum = 0;
    h->checksum = acpi_checksum(h, len);
}

#define ALIGN16(x) (((x) + 15) & ~(uint64_t)15)

int acpi_setup(struct vmm_vm *vm, int nr_cpus, uint64_t *rsdp_gpa)
{
    uint8_t *base;
    uint64_t off, xsdt_off, fadt_off, dsdt_off, madt_off;
    struct acpi_rsdp *rsdp;
    struct acpi_fadt *fadt;
    struct acpi_madt *madt;
    struct acpi_header *h;
    uint64_t xsdt_ent[2];
    uint8_t *p;
    uint32_t len;
    int i;

    base = gpa_to_hva(vm, ACPI_TABLES_GPA, ACPI_TABLES_MAX);
    if (!base)
        return VMM_ERR_INVAL;
    memset(base, 0, ACPI_TABLES_MAX);

    off      = 0;
    xsdt_off = ALIGN16(off + sizeof(struct acpi_rsdp));
    fadt_off = ALIGN16(xsdt_off + sizeof(struct acpi_header) + 2 * 8);
    dsdt_off = ALIGN16(fadt_off + sizeof(struct acpi_fadt));
    madt_off = ALIGN16(dsdt_off + sizeof(struct acpi_header) + sizeof(dsdt_aml));

    /* DSDT */
    h = (struct acpi_header *)(base + dsdt_off);
    memcpy(h + 1, dsdt_aml, sizeof(dsdt_aml));
    fill_header(h, "DSDT", sizeof(*h) + sizeof(dsdt_aml), 2);

    /* FADT */
    fadt = (struct acpi_fadt *)(base + fadt_off);
    fadt->dsdt           = (uint32_t)(ACPI_TABLES_GPA + dsdt_off);
    fadt->x_dsdt         = ACPI_TABLES_GPA + dsdt_off;
    fadt->sci_int        = ACPI_SCI_IRQ;
    fadt->pm1a_evt_blk   = ACPI_PM1A_EVT_PORT;
    fadt->pm1a_cnt_blk   = ACPI_PM1A_CNT_PORT;
    fadt->pm1_evt_len    = ACPI_PM1_EVT_LEN;
    fadt->pm1_cnt_len    = ACPI_PM1_CNT_LEN;
    fadt->century        = 0x32;          /* CMOS 世纪寄存器，见 rtc.c */
    fadt->iapc_boot_arch = FADT_BOOT_LEGACY_DEV | FADT_BOOT_8042;
    fadt->flags          = FADT_F_WBINVD | FADT_F_PWR_BUTTON | FADT_F_SLP_BUTTON;
    fadt->minor_version  = 4;
    fill_header(&fadt->h, "FACP", sizeof(*fadt), 6);

    /* MADT */
    madt = (struct acpi_madt *)(base + madt_off);
    madt->lapic_addr = MP_LAPIC_BASE;
    madt->flags      = MADT_PCAT_COMPAT;
    p = (uint8_t *)(madt + 1);
    for (i = 0; i < nr_cpus; i++) {
        struct madt_lapic *l = (struct madt_lapic *)p;

        l->type         = 0;
        l->length       = sizeof(*l);
        l->processor_id = (uint8_t)i;
        l->apic_id      = (uint8_t)i;
        l->flags        = 1;
        p += sizeof(*l);
    }
    {
        struct madt_ioapic *io = (struct madt_ioapic *)p;

        io->type      = 1;
        io->length    = sizeof(*io);
        io->ioapic_id = (uint8_t)nr_cpus;   /* 和 MP table 保持一致 */
        io->address   = MP_IOAPIC_BASE;
        io->gsi_base  = 0;
        p += sizeof(*io);
    }
    /* ISA IRQ 0-15 与 GSI 恒等映射，不需要 Interrupt Source Override：
     * Linux 在没有 override 时默认就是恒等，而 KVM 的默认路由也是恒等。 */
    {
        struct madt_lapic_nmi *n = (struct madt_lapic_nmi *)p;

        n->type         = 4;
        n->length       = sizeof(*n);
        n->processor_id = 0xff;
        n->lint         = 1;
        p += sizeof(*n);
    }
    len = (uint32_t)(p - (uint8_t *)madt);
    if (madt_off + len > ACPI_TABLES_MAX)
        return VMM_ERR_INVAL;
    fill_header(&madt->h, "APIC", len, 5);

    /* XSDT：列出除 DSDT 以外的顶层表（DSDT 由 FADT 引用） */
    h = (struct acpi_header *)(base + xsdt_off);
    /* XSDT 的条目紧跟在 36 字节的表头之后，所以这些 64 位字段天生只有
     * 4 字节对齐——ACPI 规范就是这么定的，ACPICA 为此专门有
     * ACPI_MOVE_64_TO_64。直接用 uint64_t* 写是未定义行为（UBSan 报
     * misaligned store），改成 memcpy；x86 上生成的代码是一样的。 */
    xsdt_ent[0] = ACPI_TABLES_GPA + fadt_off;
    xsdt_ent[1] = ACPI_TABLES_GPA + madt_off;
    memcpy((uint8_t *)(h + 1), xsdt_ent, sizeof(xsdt_ent));
    fill_header(h, "XSDT", sizeof(*h) + 2 * 8, 1);

    /* RSDP */
    rsdp = (struct acpi_rsdp *)(base + off);
    memcpy(rsdp->signature, "RSD PTR ", 8);
    memcpy(rsdp->oem_id, ACPI_OEM_ID, 6);
    rsdp->revision     = 2;
    rsdp->length       = sizeof(*rsdp);
    rsdp->xsdt_address = ACPI_TABLES_GPA + xsdt_off;
    rsdp->checksum     = acpi_checksum(rsdp, 20);
    rsdp->ext_checksum = acpi_checksum(rsdp, sizeof(*rsdp));

    *rsdp_gpa = ACPI_TABLES_GPA + off;
    vmm_info("ACPI @ GPA 0x%llx: RSDP XSDT@0x%llx FADT@0x%llx DSDT@0x%llx MADT@0x%llx",
             (unsigned long long)ACPI_TABLES_GPA,
             (unsigned long long)(ACPI_TABLES_GPA + xsdt_off),
             (unsigned long long)(ACPI_TABLES_GPA + fadt_off),
             (unsigned long long)(ACPI_TABLES_GPA + dsdt_off),
             (unsigned long long)(ACPI_TABLES_GPA + madt_off));
    return VMM_OK;
}

/* ------------------------------------------------------------------ */
/* PM1a 事件块（STS/EN 各 16 位）+ 控制块（CNT 16 位）                  */
/* ------------------------------------------------------------------ */

#define PM1_CNT_SCI_EN   (1u << 0)
#define PM1_CNT_SLP_TYP(v) (((v) >> 10) & 0x7)
#define PM1_CNT_SLP_EN   (1u << 13)

static struct {
    uint16_t sts;
    uint16_t en;
    uint16_t cnt;
} g_pm;

/* 端口 [0x600, 0x606)：0x600 STS, 0x602 EN, 0x604 CNT。
 * guest 可能按字节或按字访问，统一当成 6 字节的小端寄存器文件处理。 */
static int pm_read(void *opaque, uint64_t off, uint32_t size, void *data)
{
    uint8_t regs[6];
    uint16_t cnt = g_pm.cnt | PM1_CNT_SCI_EN;   /* 永远处于 ACPI 模式 */

    (void)opaque;
    memcpy(regs + 0, &g_pm.sts, 2);
    memcpy(regs + 2, &g_pm.en, 2);
    memcpy(regs + 4, &cnt, 2);

    memset(data, 0, size);
    if (off + size <= sizeof(regs))
        memcpy(data, regs + off, size);
    return VMM_OK;
}

static int pm_write(void *opaque, uint64_t off, uint32_t size,
                    const void *data)
{
    struct vmm_vm *vm = opaque;
    uint16_t v = 0;

    memcpy(&v, data, size > 2 ? 2 : size);
    switch (off) {
    case 0:
        g_pm.sts &= (uint16_t)~v;    /* 写 1 清零 */
        break;
    case 2:
        g_pm.en = v;
        break;
    case 4:
        if ((v & PM1_CNT_SLP_EN) && PM1_CNT_SLP_TYP(v) == S5_SLP_TYP) {
            vmm_info("guest requested poweroff via ACPI S5");
            vm->exit_code = 0;
            return VMM_ERR_EXIT;
        }
        g_pm.cnt = v & (uint16_t)~PM1_CNT_SLP_EN;
        break;
    }
    return VMM_OK;
}

int acpi_pm_attach(struct vmm_vm *vm)
{
    struct vmm_io_dev dev;

    memset(&g_pm, 0, sizeof(g_pm));
    memset(&dev, 0, sizeof(dev));
    dev.name   = "acpi-pm1a";
    dev.base   = ACPI_PM1A_EVT_PORT;
    dev.len    = ACPI_PM1_EVT_LEN + ACPI_PM1_CNT_LEN;
    dev.opaque = vm;
    dev.read   = pm_read;
    dev.write  = pm_write;
    return vmm_register_pio(vm, &dev);
}
