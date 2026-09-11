/* tests/unit/boot_tables_test.c — 固件表与 boot_params 的离线检查
 *
 * 不碰 /dev/kvm：把 src/boot/{mptable,acpi,zeropage}.c 原样链接进来，
 * 让它们往一块 calloc 出来的平坦“guest 内存”里写表，然后逐字段检查：
 *   - 签名、长度、校验和（MP 两级、RSDP 两个、每张 ACPI 表）
 *   - MP 表与 MADT 描述的 LAPIC/IOAPIC 地址、ID、CPU 集合彼此一致
 *   - E820 至少两项、有序不重叠、1MiB 以下有 RAM、表所在区域不是 RAM
 *   - bzImage 路径：内核自带的 setup header 原样保留，loader 只填自己的字段
 *   - boot_params 里内核真正会读的字段（type_of_loader、cmd_line_ptr、
 *     acpi_rsdp_addr）以及命令行长度上限
 * 可选 --dump DIR：把每张 ACPI 表写成 DIR/<sig>.dat，交给 iasl -d 反汇编。
 *
 * 用法：tests/boot_tables.sh（负责编译和调用 iasl），也可以单独运行。
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <asm/bootparam.h>

#include "vmm.h"
#include "memory.h"
#include "boot/acpi.h"
#include "boot/mptable.h"
#include "boot/zeropage.h"

/* ------------------------------------------------------------------ */
/* 桩：用平坦缓冲区代替 guest 内存，其余接口什么也不做                  */
/* ------------------------------------------------------------------ */

#define RAM_SIZE (512ULL << 20)

static uint8_t *g_ram;
int vmm_log_level = 0;              /* 只让 vmm_err 输出 */

void vmm_log(int level, const char *file, int line, const char *fmt, ...)
{
    va_list ap;

    (void)file; (void)line;
    if (level > vmm_log_level)
        return;
    va_start(ap, fmt);
    fprintf(stderr, "  [vmm] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

void *gpa_to_hva(struct vmm_vm *vm, uint64_t gpa, uint64_t len)
{
    (void)vm;
    if (gpa > RAM_SIZE || len > RAM_SIZE - gpa)
        return NULL;
    return g_ram + gpa;
}

int mem_write(struct vmm_vm *vm, uint64_t gpa, const void *src, size_t len)
{
    void *p = gpa_to_hva(vm, gpa, len);

    if (!p)
        return VMM_ERR_INVAL;
    memcpy(p, src, len);
    return VMM_OK;
}

int mem_load_file(struct vmm_vm *vm, uint64_t gpa, const char *path,
                  size_t *len)
{
    (void)vm; (void)gpa; (void)path;
    *len = 0;
    return VMM_OK;
}

int vmm_register_pio(struct vmm_vm *vm, const struct vmm_io_dev *dev)
{
    (void)vm; (void)dev;
    return VMM_OK;
}

/* ------------------------------------------------------------------ */
/* 断言                                                               */
/* ------------------------------------------------------------------ */

static int g_fail, g_pass;
static const char *g_ctx = "";

#define CHECK(cond, ...) do {                                           \
    if (cond) {                                                         \
        g_pass++;                                                       \
    } else {                                                            \
        g_fail++;                                                       \
        fprintf(stderr, "FAIL [%s] %s:%d: ", g_ctx, __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__);                                   \
        fprintf(stderr, "\n");                                          \
    }                                                                   \
} while (0)

static uint8_t sum8(const void *p, size_t len)
{
    const uint8_t *b = p;
    uint8_t s = 0;

    while (len--)
        s += *b++;
    return s;
}

static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* 平台描述：两张表各自解析出来之后放在这里，最后互相比较 */
struct platform {
    uint32_t lapic_addr;
    uint32_t ioapic_addr;
    int      ioapic_id;
    int      nr_cpus;
    int      apic_ids[256];
};

/* ------------------------------------------------------------------ */
/* MP 表                                                              */
/* ------------------------------------------------------------------ */

static void check_mptable(int nr_cpus, struct platform *pf)
{
    const uint8_t *mpf = g_ram + MPTABLE_GPA;
    const uint8_t *mpc, *p, *end;
    uint32_t physptr;
    uint16_t len, count, n = 0;
    int nr_bus = 0, nr_intsrc = 0, nr_lint = 0, bsp = -1;

    g_ctx = "mptable";
    memset(pf, 0, sizeof(*pf));
    pf->ioapic_id = -1;

    CHECK(!memcmp(mpf, "_MP_", 4), "floating pointer signature");
    CHECK(mpf[8] == 1, "floating pointer length = %u, want 1", mpf[8]);
    CHECK(mpf[9] == 1 || mpf[9] == 4, "spec_rev = %u", mpf[9]);
    CHECK(sum8(mpf, 16) == 0, "floating pointer checksum");
    CHECK(mpf[11] == 0, "feature1 = %u, want 0 (config table present)", mpf[11]);
    /* Linux 的 default_find_smp_config() 只扫这几个区间 */
    CHECK(MPTABLE_GPA < 0x400 ||
          (MPTABLE_GPA >= 639 * 0x400 && MPTABLE_GPA + 16 <= 640 * 0x400) ||
          (MPTABLE_GPA >= 0xF0000 && MPTABLE_GPA + 16 <= 0x100000),
          "floating pointer at 0x%llx is outside the Linux scan windows",
          (unsigned long long)MPTABLE_GPA);

    physptr = rd32(mpf + 4);
    CHECK(physptr != 0 && physptr < RAM_SIZE, "physptr = 0x%x", physptr);
    mpc = g_ram + physptr;
    len = rd16(mpc + 4);
    count = rd16(mpc + 34);
    CHECK(!memcmp(mpc, "PCMP", 4), "config table signature");
    CHECK(mpc[6] == 1 || mpc[6] == 4, "config table spec = %u", mpc[6]);
    CHECK(sum8(mpc, len) == 0, "config table checksum over %u bytes", len);
    CHECK(physptr + len <= MPTABLE_GPA + MPTABLE_MAX_SIZE,
          "config table overruns MPTABLE_MAX_SIZE");
    pf->lapic_addr = rd32(mpc + 36);
    CHECK(pf->lapic_addr != 0, "null local APIC address");

    p = mpc + 44;
    end = mpc + len;
    while (p < end) {
        switch (p[0]) {
        case 0:                         /* processor, 20 bytes */
            if (p[3] & 1) {
                pf->apic_ids[pf->nr_cpus++] = p[1];
                if (p[3] & 2) {
                    CHECK(bsp < 0, "more than one BP flag");
                    bsp = p[1];
                }
            }
            p += 20;
            break;
        case 1:                         /* bus */
            CHECK(!memcmp(p + 2, "ISA   ", 6), "bus type is not ISA");
            nr_bus++;
            p += 8;
            break;
        case 2:                         /* ioapic */
            CHECK(p[3] & 1, "IOAPIC entry not usable");
            pf->ioapic_id = p[1];
            pf->ioapic_addr = rd32(p + 4);
            p += 8;
            break;
        case 3:                         /* I/O interrupt */
            CHECK(p[1] == 0 && p[4] == 0, "intsrc %d: type/bus", nr_intsrc);
            /* 恒等映射：ISA IRQ i -> IOAPIC pin i，和 KVM 默认 GSI 路由一致 */
            CHECK(p[5] == p[7], "intsrc: ISA IRQ %u -> pin %u (KVM routes GSI "
                  "identically)", p[5], p[7]);
            CHECK(p[6] == pf->ioapic_id, "intsrc: dst ioapic %u", p[6]);
            nr_intsrc++;
            p += 8;
            break;
        case 4:                         /* local interrupt */
            CHECK((p[1] == 3 && p[7] == 0) || (p[1] == 1 && p[7] == 1),
                  "lintsrc type %u -> LINT%u", p[1], p[7]);
            nr_lint++;
            p += 8;
            break;
        default:
            CHECK(0, "unknown entry type %u (Linux stops parsing here)", p[0]);
            p = end;
            break;
        }
        n++;
    }
    CHECK(p == end, "entries overrun base_table_length");
    CHECK(n == count, "entry_count = %u, parsed %u", count, n);
    CHECK(pf->nr_cpus == nr_cpus, "%d processors, want %d", pf->nr_cpus, nr_cpus);
    CHECK(bsp == 0, "BP flag on APIC ID %d, want 0", bsp);
    CHECK(nr_bus == 1 && nr_intsrc == 16 && nr_lint == 2,
          "bus %d intsrc %d lint %d", nr_bus, nr_intsrc, nr_lint);
}

/* ------------------------------------------------------------------ */
/* ACPI                                                               */
/* ------------------------------------------------------------------ */

static const uint8_t *acpi_table(uint64_t gpa, const char *sig)
{
    const uint8_t *t;
    uint32_t len;

    if (gpa == 0 || gpa >= RAM_SIZE) {
        CHECK(0, "%s address 0x%llx", sig, (unsigned long long)gpa);
        return NULL;
    }
    t = g_ram + gpa;
    len = rd32(t + 4);
    CHECK(!memcmp(t, sig, 4), "signature %.4s, want %s", (const char *)t, sig);
    CHECK(len >= 36 && gpa + len <= ACPI_TABLES_GPA + ACPI_TABLES_MAX,
          "%s length %u", sig, len);
    CHECK(sum8(t, len) == 0, "%s checksum", sig);
    return t;
}

static void check_acpi(uint64_t rsdp_gpa, int nr_cpus, struct platform *pf)
{
    const uint8_t *rsdp = g_ram + rsdp_gpa;
    const uint8_t *xsdt, *fadt = NULL, *madt = NULL, *dsdt, *p, *end;
    uint32_t xlen, i;
    int nr_lapic = 0;

    g_ctx = "acpi";
    memset(pf, 0, sizeof(*pf));
    pf->ioapic_id = -1;

    /* RSDP：扫描路径要求在 [0xE0000, 0x100000) 的 16 字节边界上，两个校验和都对 */
    CHECK(!memcmp(rsdp, "RSD PTR ", 8), "RSDP signature");
    CHECK(rsdp_gpa >= 0xE0000 && rsdp_gpa < 0x100000 && !(rsdp_gpa & 15),
          "RSDP at 0x%llx is not reachable by the legacy scan",
          (unsigned long long)rsdp_gpa);
    CHECK(sum8(rsdp, 20) == 0, "RSDP checksum (20 bytes)");
    CHECK(rsdp[15] >= 2, "RSDP revision %u", rsdp[15]);
    CHECK(rd32(rsdp + 20) == 36, "RSDP length %u", rd32(rsdp + 20));
    CHECK(sum8(rsdp, 36) == 0, "RSDP extended checksum (36 bytes)");

    xsdt = acpi_table(rd64(rsdp + 24), "XSDT");
    if (!xsdt)
        return;
    xlen = rd32(xsdt + 4);
    CHECK(xlen >= 36 + 8 && (xlen - 36) % 8 == 0, "XSDT length %u", xlen);
    for (i = 36; i + 8 <= xlen; i += 8) {
        uint64_t a = rd64(xsdt + i);
        const uint8_t *t = a < RAM_SIZE ? g_ram + a : NULL;

        if (!t)
            continue;
        if (!memcmp(t, "FACP", 4))
            fadt = acpi_table(a, "FACP");
        else if (!memcmp(t, "APIC", 4))
            madt = acpi_table(a, "APIC");
        else
            CHECK(memcmp(t, "DSDT", 4) && memcmp(t, "FACS", 4),
                  "%.4s must be referenced from the FADT, not the XSDT",
                  (const char *)t);
    }
    CHECK(fadt != NULL, "XSDT has no FADT");
    CHECK(madt != NULL, "XSDT has no MADT (Linux would then drop the MP table too)");
    if (!fadt || !madt)
        return;

    /* FADT：非 HW_REDUCED 平台必须有 PM1a 事件块与控制块 */
    CHECK(rd32(fadt + 4) == 276, "FADT length %u", rd32(fadt + 4));
    CHECK(!(rd32(fadt + 112) & (1u << 20)), "HW_REDUCED_ACPI set: Linux would "
          "drop the PIT and the 8259");
    CHECK(rd32(fadt + 56) && fadt[88] >= 4, "PM1a_EVT_BLK 0x%x len %u",
          rd32(fadt + 56), fadt[88]);
    CHECK(rd32(fadt + 64) && fadt[89] >= 2, "PM1a_CNT_BLK 0x%x len %u",
          rd32(fadt + 64), fadt[89]);
    CHECK(rd32(fadt + 56) == ACPI_PM1A_EVT_PORT && rd32(fadt + 64) == ACPI_PM1A_CNT_PORT,
          "PM1a ports differ from the acpi_pm device");
    CHECK(rd16(fadt + 46) == ACPI_SCI_IRQ, "SCI_INT %u", rd16(fadt + 46));
    CHECK(rd32(fadt + 48) == 0, "SMI_CMD must be 0 (no SMM, always in ACPI mode)");
    CHECK(rd16(fadt + 109) & (1u << 1), "IAPC_BOOT_ARCH lacks 8042: i8042 "
          "would not be probed");
    CHECK(rd32(fadt + 40) == (uint32_t)rd64(fadt + 140),
          "DSDT 0x%x != X_DSDT 0x%llx", rd32(fadt + 40),
          (unsigned long long)rd64(fadt + 140));

    /* DSDT：\_S5 的 SLP_TYP 必须是 3 位以内，并与 PM1 设备的约定一致 */
    dsdt = acpi_table(rd64(fadt + 140), "DSDT");
    if (dsdt) {
        const uint8_t *aml = dsdt + 36;
        uint32_t aml_len = rd32(dsdt + 4) - 36;
        const uint8_t *s5 = memmem(aml, aml_len, "\x08_S5_\x12", 6);

        CHECK(s5 != NULL, "DSDT has no Name(_S5, Package(...))");
        if (s5) {
            /* [0]=08 NameOp [1..4]="_S5_" [5]=12 PackageOp [6]=PkgLength
             * [7]=NumElements [8]=0A BytePrefix [9]=SLP_TYPa [10]=0A [11]=SLP_TYPb */
            CHECK(!(s5[6] & 0xc0) && s5[7] >= 2 && s5[8] == 0x0a && s5[10] == 0x0a,
                  "\\_S5 package layout");
            CHECK(s5[9] <= 7 && s5[11] <= 7, "\\_S5 SLP_TYP exceeds 3 bits "
                  "(ACPI_SLEEP_TYPE_MAX)");
            CHECK(s5[9] == 5, "\\_S5 SLP_TYPa = %u, the PM1 device expects 5",
                  s5[9]);
        }
    }

    /* MADT */
    pf->lapic_addr = rd32(madt + 36);
    CHECK(rd32(madt + 40) & 1, "MADT lacks PCAT_COMPAT");
    p = madt + 44;
    end = madt + rd32(madt + 4);
    while (p + 2 <= end) {
        uint8_t type = p[0], len = p[1];
        static const uint8_t minlen[] = { 8, 12, 10, 8, 6, 12 };

        CHECK(len >= 2 && p + len <= end, "subtable type %u length %u", type, len);
        if (len < 2 || p + len > end)
            break;
        if (type < sizeof(minlen))
            CHECK(len >= minlen[type], "subtable type %u length %u < %u "
                  "(BAD_MADT_ENTRY -> ACPI disabled)", type, len, minlen[type]);
        switch (type) {
        case 0:
            CHECK(p[3] != 0xff, "LAPIC entry with APIC ID 0xff is ignored");
            if (rd32(p + 4) & 1)
                pf->apic_ids[pf->nr_cpus++] = p[3];
            else
                CHECK(rd32(p + 4) & 2, "disabled LAPIC without Online Capable "
                      "is ignored on ACPI >= 6.3");
            nr_lapic++;
            break;
        case 1:
            pf->ioapic_id = p[2];
            pf->ioapic_addr = rd32(p + 4);
            CHECK(rd32(p + 8) == 0, "IOAPIC GSI base %u", rd32(p + 8));
            break;
        case 2:
            /* KVM 默认路由是恒等的，任何 ISO 都会让 Linux 与 KVM 对不上 */
            CHECK(0, "unexpected ISO: IRQ %u -> GSI %u", p[3], rd32(p + 4));
            break;
        case 4:
            CHECK(p[5] == 1, "LAPIC NMI on LINT%u, want LINT1", p[5]);
            break;
        default:
            break;
        }
        p += len;
    }
    CHECK(nr_lapic == nr_cpus && pf->nr_cpus == nr_cpus,
          "%d LAPIC entries (%d enabled), want %d", nr_lapic, pf->nr_cpus, nr_cpus);
}

/* ------------------------------------------------------------------ */
/* 两张表之间、表与 KVM 之间                                           */
/* ------------------------------------------------------------------ */

static void check_consistency(const struct platform *mp,
                              const struct platform *acpi, int nr_cpus)
{
    int i;

    g_ctx = "consistency";
    /* KVM：vCPU 复位时 IA32_APIC_BASE = 0xFEE00000，IOAPIC 固定在 0xFEC00000 */
    CHECK(mp->lapic_addr == 0xfee00000u && acpi->lapic_addr == 0xfee00000u,
          "LAPIC address MP 0x%x / MADT 0x%x, KVM uses 0xfee00000",
          mp->lapic_addr, acpi->lapic_addr);
    CHECK(mp->ioapic_addr == 0xfec00000u && acpi->ioapic_addr == 0xfec00000u,
          "IOAPIC address MP 0x%x / MADT 0x%x, KVM uses 0xfec00000",
          mp->ioapic_addr, acpi->ioapic_addr);
    CHECK(mp->ioapic_id == acpi->ioapic_id, "IOAPIC ID MP %d / MADT %d",
          mp->ioapic_id, acpi->ioapic_id);
    /* KVM 把 LAPIC ID 复位为 vcpu_id，表里必须正好是 0..nr_cpus-1 */
    for (i = 0; i < nr_cpus; i++) {
        CHECK(mp->apic_ids[i] == i && acpi->apic_ids[i] == i,
              "CPU %d: APIC ID MP %d / MADT %d, KVM uses vcpu_id",
              i, mp->apic_ids[i], acpi->apic_ids[i]);
        CHECK(acpi->ioapic_id != acpi->apic_ids[i],
              "IOAPIC ID %d collides with a LAPIC ID", acpi->ioapic_id);
    }
}

/* ------------------------------------------------------------------ */
/* boot_params                                                        */
/* ------------------------------------------------------------------ */

static int in_ram(const struct boot_params *bp, uint64_t a, uint64_t len)
{
    int i;

    for (i = 0; i < bp->e820_entries; i++) {
        const struct boot_e820_entry *e = &bp->e820_table[i];

        if (e->type == 1 && a < e->addr + e->size && a + len > e->addr)
            return 1;
    }
    return 0;
}

static void check_boot_params(uint64_t rsdp_gpa, const char *cmdline)
{
    const struct boot_params *bp = (const void *)(g_ram + ZP_BOOT_PARAMS_GPA);
    uint64_t prev_end = 0, ram_end = 0;
    int i, low_ram = 0;

    g_ctx = "boot_params";
    CHECK(bp->sentinel == 0, "sentinel != 0 would make the kernel wipe "
          "acpi_rsdp_addr and ext_* fields");
    CHECK(bp->hdr.header == 0x53726448 && bp->hdr.boot_flag == 0xAA55,
          "setup header magic");
    CHECK(bp->hdr.version != 0, "hdr.version is 0");
    CHECK(bp->hdr.type_of_loader != 0, "type_of_loader = 0: initrd ignored");
    CHECK(bp->hdr.cmdline_size == 0, "cmdline_size is a kernel->loader "
          "field, loader must not write it");
    CHECK(bp->acpi_rsdp_addr == rsdp_gpa, "acpi_rsdp_addr 0x%llx",
          (unsigned long long)bp->acpi_rsdp_addr);
    CHECK(bp->hdr.cmd_line_ptr == ZP_CMDLINE_GPA && bp->ext_cmd_line_ptr == 0,
          "cmd_line_ptr 0x%x", bp->hdr.cmd_line_ptr);
    CHECK(!strcmp((const char *)g_ram + ZP_CMDLINE_GPA, cmdline), "cmdline text");
    /* copy_bootdata() 从 cmd_line_ptr 固定读 COMMAND_LINE_SIZE 字节 */
    CHECK(in_ram(bp, ZP_CMDLINE_GPA, ZP_CMDLINE_MAX), "cmdline buffer not in RAM");

    /* E820：少于两项会被 append_e820_table() 整个丢弃 */
    CHECK(bp->e820_entries >= 2 && bp->e820_entries <= 128,
          "%u e820 entries", bp->e820_entries);
    for (i = 0; i < bp->e820_entries; i++) {
        const struct boot_e820_entry *e = &bp->e820_table[i];

        CHECK(e->size != 0, "e820[%d] has size 0", i);
        CHECK(e->addr >= prev_end, "e820[%d] overlaps or is unsorted", i);
        prev_end = e->addr + e->size;
        if (e->type == 1) {
            if (e->addr < 0x100000)
                low_ram = 1;
            if (prev_end > ram_end)
                ram_end = prev_end;
        }
    }
    CHECK(low_ram, "no RAM below 1MiB: real-mode trampoline cannot be placed");
    CHECK(ram_end <= RAM_SIZE, "e820 RAM ends at 0x%llx beyond the memslot",
          (unsigned long long)ram_end);
    CHECK(!in_ram(bp, MPTABLE_GPA, MPTABLE_MAX_SIZE), "MP table is in e820 RAM");
    CHECK(!in_ram(bp, ACPI_TABLES_GPA, ACPI_TABLES_MAX), "ACPI tables are in e820 RAM");
    CHECK(in_ram(bp, ZP_BOOT_PARAMS_GPA, sizeof(*bp)), "boot_params not in RAM");
}

/* ------------------------------------------------------------------ */

static int build_all(struct vmm_vm *vm, int nr_cpus, const char *cmdline,
                     uint64_t *rsdp)
{
    struct zp_config zc;

    memset(g_ram, 0, RAM_SIZE);
    memset(&zc, 0, sizeof(zc));
    if (mptable_setup(vm, nr_cpus) != VMM_OK)
        return -1;
    if (acpi_setup(vm, nr_cpus, rsdp) != VMM_OK)
        return -1;
    zc.cmdline    = cmdline;
    zc.acpi_rsdp  = *rsdp;
    zc.kernel_end = 0x2a30000;
    return zeropage_setup(vm, &zc) == VMM_OK ? 0 : -1;
}

/* bzImage 路径：内核自带的 setup header 必须原样保留，loader 只填自己的字段 */
static void check_bzimage_header(struct vmm_vm *vm)
{
    const struct boot_params *bp = (const void *)(g_ram + ZP_BOOT_PARAMS_GPA);
    struct setup_header kh;
    struct zp_config zc;
    uint64_t rsdp = 0;
    static char line[2048];

    g_ctx = "bzImage header";
    memset(&kh, 0, sizeof(kh));
    kh.setup_sects      = 31;
    kh.boot_flag        = 0xAA55;
    kh.header           = 0x53726448;
    kh.version          = 0x020f;
    kh.loadflags        = LOADED_HIGH;
    kh.code32_start     = 0x1000000;
    kh.initrd_addr_max  = 0x7fffffff;
    kh.kernel_alignment = 0x200000;
    kh.relocatable_kernel = 1;
    kh.xloadflags       = XLF_KERNEL_64 | XLF_CAN_BE_LOADED_ABOVE_4G;
    kh.cmdline_size     = 100;          /* 故意比 ZP_CMDLINE_MAX 小 */
    kh.pref_address     = 0x1000000;
    kh.init_size        = 0x1c66000;

    memset(g_ram, 0, RAM_SIZE);
    CHECK(acpi_setup(vm, 1, &rsdp) == VMM_OK, "acpi_setup");
    memset(&zc, 0, sizeof(zc));
    zc.cmdline        = "console=ttyS0";
    zc.acpi_rsdp      = rsdp;
    zc.kernel_end     = 0x2c66000;
    zc.kernel_hdr     = &kh;
    zc.kernel_hdr_len = sizeof(kh);
    CHECK(zeropage_setup(vm, &zc) == VMM_OK, "zeropage_setup with a kernel header");
    CHECK(bp->hdr.version == 0x020f && bp->hdr.cmdline_size == 100 &&
          bp->hdr.initrd_addr_max == 0x7fffffff && bp->hdr.init_size == 0x1c66000 &&
          bp->hdr.kernel_alignment == 0x200000 && bp->hdr.setup_sects == 31,
          "kernel-declared setup header fields were modified");
    CHECK(bp->hdr.type_of_loader == 0xff, "type_of_loader not filled");
    CHECK(bp->hdr.loadflags & LOADED_HIGH, "LOADED_HIGH cleared");
    CHECK(bp->hdr.cmd_line_ptr == ZP_CMDLINE_GPA, "cmd_line_ptr not filled");
    CHECK(bp->acpi_rsdp_addr == rsdp, "acpi_rsdp_addr not filled");
    CHECK(bp->sentinel == 0, "sentinel must stay 0");

    /* 超过内核声明的 cmdline_size 必须拒绝 */
    memset(line, 'a', 101);
    line[101] = '\0';
    zc.cmdline = line;
    fprintf(stderr, "  (the next error line is expected)\n");
    CHECK(zeropage_setup(vm, &zc) != VMM_OK,
          "a cmdline longer than the kernel's cmdline_size must be rejected");
}

static void dump_tables(const char *dir, uint64_t rsdp_gpa)
{
    const uint8_t *rsdp = g_ram + rsdp_gpa;
    const uint8_t *xsdt = g_ram + rd64(rsdp + 24);
    uint64_t addrs[8];
    int n = 0, i;
    uint32_t off;
    char path[4096];

    addrs[n++] = rd64(rsdp + 24);                       /* XSDT */
    for (off = 36; off + 8 <= rd32(xsdt + 4) && n < 6; off += 8) {
        uint64_t a = rd64(xsdt + off);

        addrs[n++] = a;
        if (!memcmp(g_ram + a, "FACP", 4))
            addrs[n++] = rd64(g_ram + a + 140);         /* X_DSDT */
    }
    for (i = 0; i < n; i++) {
        const uint8_t *t = g_ram + addrs[i];
        FILE *fp;

        snprintf(path, sizeof(path), "%s/%.4s.dat", dir, (const char *)t);
        fp = fopen(path, "wb");
        if (!fp) {
            perror(path);
            g_fail++;
            continue;
        }
        fwrite(t, 1, rd32(t + 4), fp);
        fclose(fp);
    }
}

int main(int argc, char **argv)
{
    static const int cpu_counts[] = { 1, 2, 4, VMM_MAX_VCPUS };
    static struct vmm_vm vm;
    static char longline[ZP_CMDLINE_MAX + 1];
    const char *dump_dir = NULL;
    const char *cmdline = "console=ttyS0 reboot=k panic=-1";
    struct platform mp, acpi;
    uint64_t rsdp = 0;
    unsigned i;

    if (argc == 3 && !strcmp(argv[1], "--dump"))
        dump_dir = argv[2];
    else if (argc != 1) {
        fprintf(stderr, "usage: %s [--dump DIR]\n", argv[0]);
        return 2;
    }

    g_ram = calloc(1, RAM_SIZE);
    if (!g_ram) {
        perror("calloc");
        return 2;
    }
    vm.mem.ram_size = RAM_SIZE;

    for (i = 0; i < sizeof(cpu_counts) / sizeof(cpu_counts[0]); i++) {
        int n = cpu_counts[i];

        printf("nr_cpus = %d\n", n);
        g_ctx = "build";
        CHECK(build_all(&vm, n, cmdline, &rsdp) == 0, "builders failed");
        check_mptable(n, &mp);
        check_acpi(rsdp, n, &acpi);
        check_consistency(&mp, &acpi, n);
        check_boot_params(rsdp, cmdline);
        if (dump_dir && n == 4)
            dump_tables(dump_dir, rsdp);
    }

    check_bzimage_header(&vm);

    /* 命令行上限：2047 个字符可以，2048 个必须被拒绝 */
    g_ctx = "cmdline limit";
    memset(longline, 'a', ZP_CMDLINE_MAX - 1);
    longline[ZP_CMDLINE_MAX - 1] = '\0';
    CHECK(build_all(&vm, 1, longline, &rsdp) == 0,
          "a %d-byte cmdline must be accepted", ZP_CMDLINE_MAX - 1);
    longline[ZP_CMDLINE_MAX - 1] = 'a';
    longline[ZP_CMDLINE_MAX] = '\0';
    fprintf(stderr, "  (the next error line is expected)\n");
    CHECK(build_all(&vm, 1, longline, &rsdp) != 0,
          "a %d-byte cmdline must be rejected", ZP_CMDLINE_MAX);

    printf("%d checks passed, %d failed\n", g_pass, g_fail);
    free(g_ram);
    return g_fail ? 1 : 0;
}
