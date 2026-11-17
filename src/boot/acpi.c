/* src/boot/acpi.c — 最小 ACPI 表                                  [linux-boot]
 *
 * 内存布局（ACPI_TABLES_GPA 起，每张表 16 字节对齐，FACS 64 字节对齐）：
 *   RSDP  -> XSDT -> { FADT -> { DSDT, FACS }, MADT }
 *
 * 参考 ACPI 6.5 spec（ref/ACPI_Spec_6_5_Aug29.pdf）：5.2.5 RSDP, 5.2.8 XSDT,
 * 5.2.9 FADT, 5.2.10 FACS, 5.2.11.1 DSDT, 5.2.12 MADT。FADT 指向的 PM1 寄存器由
 * power.c 和 devices/acpi_pm.c 实现。
 */
#include <string.h>

#include "vmm.h"
#include "memory.h"
#include "boot/acpi.h"
#include "boot/mptable.h"
#include "devices/i8042.h"
#include "devices/acpi_pm.h"
#include "devices/pvpanic.h"
#include "boot/aml.h"
#include "pci/pci.h"

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

/* FACS（ACPI 6.5 5.2.10，Table 5.13）。非硬件精简平台必须有它（5.2.10 第一
 * 段），全局锁的锁字就在这里（5.2.10.1）。没有固件会来抢锁，所以锁字从 0
 * 开始、只有 OSPM 自己用；ACPICA 还要看到 PM1_EN.GBL_EN 能写进去才认为锁
 * 存在，见 devices/acpi_pm.h。唤醒向量和 S4BIOS 只对睡眠状态有意义，这台
 * 机器只有 S5，flags 为 0。FACS 没有校验和。 */
struct acpi_facs {
    char     signature[4];
    uint32_t length;
    uint32_t hardware_signature;
    uint32_t firmware_waking_vector;
    uint32_t global_lock;
    uint32_t flags;
    uint64_t x_firmware_waking_vector;
    uint8_t  version;
    uint8_t  reserved[3];
    uint32_t ospm_flags;
    uint8_t  reserved2[24];
} __attribute__((packed));
_Static_assert(sizeof(struct acpi_facs) == 64, "facs size");
#define FACS_VERSION 3

#define FADT_F_WBINVD        (1u << 0)
/* PWR_BUTTON / SLP_BUTTON 置 1 表示按钮是控制方法设备或者没有，清 0 表示
 * 固定功能按钮（ACPI 6.5 Table 5.10、4.8.2.2.1 Table 4.8）。电源按钮用固定
 * 功能的（PM1 的 PWRBTN_STS/EN，见 devices/acpi_pm.h），所以 flags 里不置
 * PWR_BUTTON；没有睡眠按钮，SLP_BUTTON 置 1。 */
#define FADT_F_PWR_BUTTON    (1u << 4)
#define FADT_F_SLP_BUTTON    (1u << 5)
#define FADT_F_RESET_REG_SUP (1u << 10)  /* RESET_REG / RESET_VALUE 有效 */
#define FADT_BOOT_LEGACY_DEV (1u << 0)
#define FADT_BOOT_8042       (1u << 1)

#define MADT_PCAT_COMPAT     (1u << 0)   /* 同时存在 8259，Linux 会先屏蔽它 */

/* DSDT 的 AML 写在这块缓冲区里，生成后整段拷进 guest 内存 */
#define DSDT_AML_MAX 8192

/* 资源描述符（ACPI 6.4 第 6.4 节；ACPICA amlresrc.h 的 aml_resource_*）。
 * 大型描述符的第一个字节是 0x80 | 类型，后面是 16 位的长度（不含这 3 字节）。 */
#define RES_WORD_ADDRESS     0x88    /* Word Address Space Descriptor */
#define RES_DWORD_ADDRESS    0x87    /* DWord Address Space Descriptor */
#define RES_END_TAG          0x79    /* 小型描述符，类型 0xF，长度 1 */
#define RES_IO_PORT          0x47    /* 小型描述符，类型 0x8，长度 7（6.4.2.5） */
#define RES_IO_DECODE16      0x01    /* _DEC = 1：译码全部 16 位地址 */
#define RES_WORD_LEN         13
#define RES_DWORD_LEN        23

/* Address Space Descriptor 的字段取值 */
#define RES_TYPE_MEMORY      0
#define RES_TYPE_IO          1
#define RES_TYPE_BUS         2
/* general flags：bit 3 _MAF、bit 2 _MIF（地址范围固定），bit 1 _DEC = 0
 * 正向译码，bit 0 = 0 表示 ResourceProducer。host bridge 的窗口是它向下
 * 游提供的地址空间，必须标成 producer：Linux 只给 producer 的范围打上
 * IORESOURCE_WINDOW（drivers/acpi/resource.c acpi_decode_space）。 */
#define RES_GFLAGS_WINDOW    0x0C
/* type specific flags：IO 的 _RNG = 3（ISA 和非 ISA 范围都译码）；
 * 内存的 _RW = 1（可读写）、_MEM = 0（不可缓存，MMIO 寄存器的属性） */
#define RES_IO_ENTIRE_RANGE  0x03
#define RES_MEM_RW_UNCACHED  0x01

static void res_word(struct aml *r, uint8_t type, uint8_t tflags,
                     uint16_t min, uint16_t max)
{
    aml_byte(r, RES_WORD_ADDRESS);
    aml_word(r, RES_WORD_LEN);
    aml_byte(r, type);
    aml_byte(r, RES_GFLAGS_WINDOW);
    aml_byte(r, tflags);
    aml_word(r, 0);                         /* granularity */
    aml_word(r, min);
    aml_word(r, max);
    aml_word(r, 0);                         /* translation offset */
    aml_word(r, (uint16_t)(max - min + 1)); /* length */
}

static void res_dword(struct aml *r, uint8_t type, uint8_t tflags,
                      uint32_t min, uint32_t max)
{
    aml_byte(r, RES_DWORD_ADDRESS);
    aml_word(r, RES_DWORD_LEN);
    aml_byte(r, type);
    aml_byte(r, RES_GFLAGS_WINDOW);
    aml_byte(r, tflags);
    aml_dword(r, 0);
    aml_dword(r, min);
    aml_dword(r, max);
    aml_dword(r, 0);
    aml_dword(r, max - min + 1);
}

/* host bridge 的 _CRS：总线号 0-255，两段 IO 窗口（挖掉 conf1 端口），
 * 一段 MMIO 窗口。Linux 用它建立根总线的资源窗口，并据此判断固件分配的
 * BAR 是否合法（落在窗口外的 BAR 会被重新分配）。 */
static void build_pci0_crs(struct aml *a)
{
    uint8_t buf[128];
    struct aml r;

    aml_init(&r, buf, sizeof(buf));
    res_word(&r, RES_TYPE_BUS, 0, 0x00, 0xFF);
    res_word(&r, RES_TYPE_IO, RES_IO_ENTIRE_RANGE,
             0x0000, PCI_CONF1_ADDR_PORT - 1);
    res_word(&r, RES_TYPE_IO, RES_IO_ENTIRE_RANGE,
             PCI_CONF1_DATA_PORT + 4, PCI_IO_WINDOW_END - 1);
    res_dword(&r, RES_TYPE_MEMORY, RES_MEM_RW_UNCACHED,
              (uint32_t)PCI_MMIO_WINDOW_START,
              (uint32_t)(PCI_MMIO_WINDOW_END - 1));
    aml_byte(&r, RES_END_TAG);
    aml_byte(&r, 0);                        /* 校验和为 0 表示不校验 */
    if (r.overflow) {
        a->overflow = true;
        return;
    }
    aml_name_decl(a, "_CRS");
    aml_buffer(a, r.buf, r.len);
}

/* _PRT：每个 slot 的 INTA..INTD 各一项 Package { 地址, pin, 源, 源下标 }。
 * 地址 0xSSSSFFFF 表示“slot SSSS 的所有 function”；源为 0 表示直接接在
 * GSI 上（不经过 PCI Interrupt Link 设备），源下标就是 GSI 号（ACPI 6.4
 * 第 6.2.13 节）。直接接线的 GSI 按规范默认是电平触发、低有效，正是
 * PCI INTx 的电气特性，不需要另外声明。
 *
 * 列出全部 32 个 slot 而不只是已插设备：_PRT 描述的是主板上的走线，
 * 和插了什么无关，以后热插拔进来的设备也按这张表路由。 */
static void build_pci0_prt(struct aml *a)
{
    size_t prt;
    int slot, pin;

    aml_name_decl(a, "_PRT");
    prt = aml_package(a, PCI_MAX_SLOTS * PCI_INTX_LINES);
    for (slot = 0; slot < PCI_MAX_SLOTS; slot++) {
        for (pin = 1; pin <= PCI_INTX_LINES; pin++) {
            size_t e = aml_package(a, 4);

            aml_int(a, (uint32_t)slot << 16 | 0xFFFF);
            aml_int(a, (uint64_t)(pin - 1));  /* _PRT 里 0 = INTA */
            aml_int(a, 0);
            aml_int(a, (uint64_t)pci_intx_gsi(slot, pin));
            aml_close(a, e);
        }
    }
    aml_close(a, prt);
}

/* pvpanic（devices/pvpanic.h）：
 *   Device (PEVT) {
 *     Name (_HID, "QEMU0001")
 *     Name (_CRS, ResourceTemplate () { IO (Decode16, 0x505, 0x505, 1, 1) })
 *   }
 * Linux 的 pvpanic-mmio 按 _HID 绑定，从 _CRS 取端口（platform_get_mem_or_io）。
 * 没有 _STA：不在可枚举总线上的设备缺 _STA 时，OSPM 当它存在、启用、工作
 * 正常（ACPI 6.5 6.3.7），这个设备不会被拔出或关闭，没有别的状态要报。
 * QEMU 规范里的 RDPT/WRPT 方法也不提供：Linux 驱动直接读写端口，不调用
 * 它们，没有调用方就验证不了它们写得对不对。 */
static void build_pvpanic(struct aml *a)
{
    uint8_t buf[16];
    struct aml r;
    size_t dev;

    aml_init(&r, buf, sizeof(buf));
    aml_byte(&r, RES_IO_PORT);
    aml_byte(&r, RES_IO_DECODE16);
    aml_word(&r, PVPANIC_PORT);             /* _MIN */
    aml_word(&r, PVPANIC_PORT);             /* _MAX */
    aml_byte(&r, 1);                        /* _ALN */
    aml_byte(&r, 1);                        /* _LEN */
    aml_byte(&r, RES_END_TAG);
    aml_byte(&r, 0);

    dev = aml_device(a, "PEVT");
    aml_name_decl(a, "_HID");
    aml_string(a, PVPANIC_ACPI_HID);
    aml_name_decl(a, "_CRS");
    aml_buffer(a, r.buf, r.len);
    aml_close(a, dev);
}

/* DSDT：
 *   Name (_S5, Package () { 5, 5, 0, 0 })
 *   Scope (\_SB) {
 *     Device (PCI0) {
 *       Name (_HID, EisaId ("PNP0A03"))    PCI host bridge
 *       Name (_UID, 0)
 *       Name (_CRS, ResourceTemplate () { ... })
 *       Name (_PRT, Package () { ... })
 *     }
 *     Device (PEVT) { ... }               挂了 pvpanic 时才有
 *   }
 * Linux poweroff 时查 \_S5 拿 SLP_TYP，然后写 PM1a_CNT = SLP_TYP<<10 | SLP_EN。
 * 找到 PNP0A03 后 Linux 的 acpi_pci_root 驱动接管 bus 0 的枚举，资源窗口
 * 取自 _CRS，INTx 路由取自 _PRT。 */
static int build_dsdt(const struct vmm_vm *vm, uint8_t *out, size_t cap,
                      size_t *len)
{
    static uint8_t buf[DSDT_AML_MAX];
    struct aml a;
    size_t pkg, sb, dev;

    aml_init(&a, buf, sizeof(buf));

    aml_name_decl(&a, "_S5_");
    pkg = aml_package(&a, 4);
    aml_int(&a, ACPI_S5_SLP_TYP);            /* PM1a_CNT.SLP_TYP */
    aml_int(&a, ACPI_S5_SLP_TYP);            /* PM1b_CNT.SLP_TYP */
    aml_int(&a, 0);                          /* 保留 */
    aml_int(&a, 0);
    aml_close(&a, pkg);

    sb  = aml_scope(&a, "\\_SB_");
    dev = aml_device(&a, "PCI0");
    aml_name_decl(&a, "_HID");
    aml_int(&a, aml_eisaid("PNP0A03"));
    aml_name_decl(&a, "_UID");
    aml_int(&a, 0);
    build_pci0_crs(&a);
    build_pci0_prt(&a);
    aml_close(&a, dev);
    if (vm->pvpanic)
        build_pvpanic(&a);
    aml_close(&a, sb);

    if (a.overflow || a.len > cap) {
        vmm_err("DSDT AML does not fit into %zu bytes", a.overflow ? sizeof(buf) : cap);
        return VMM_ERR_NOMEM;
    }
    memcpy(out, a.buf, a.len);
    *len = a.len;
    return VMM_OK;
}


/* Generic Address Structure 的 Address Space ID 与 Access Size */
#define GAS_SPACE_SYSTEM_IO 1
#define GAS_ACCESS_BYTE     1

/* 12 字节 GAS：space_id, bit_width, bit_offset, access_size, address(64) */
static void fill_gas(uint8_t gas[12], uint8_t space, uint8_t bit_width,
                     uint8_t access, uint64_t addr)
{
    gas[0] = space;
    gas[1] = bit_width;
    gas[2] = 0;
    gas[3] = access;
    memcpy(gas + 4, &addr, 8);
}

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
#define ALIGN64(x) (((x) + 63) & ~(uint64_t)63)

/* FACS 的 hardware signature：规范要求 ACPI 表的内容或结构一变它就变
 * （Table 5.13），OSPM 据此判断能不能从 S4 镜像恢复。取 FACS 之前全部表
 * 字节的 FNV-1a，同样的配置每次启动得到同一个值 */
static uint32_t tables_signature(const uint8_t *p, size_t len)
{
    uint32_t h = 2166136261u;

    while (len--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

int acpi_setup(struct vmm_vm *vm, int nr_cpus, uint64_t *rsdp_gpa)
{
    enum gpa_status gs;
    uint8_t *base;
    uint64_t off, xsdt_off, fadt_off, dsdt_off, madt_off, facs_off;
    struct acpi_facs *facs;
    struct acpi_rsdp *rsdp;
    struct acpi_fadt *fadt;
    struct acpi_madt *madt;
    struct acpi_header *h;
    uint64_t xsdt_ent[2];
    uint8_t *p;
    uint32_t len;
    size_t aml_len;
    int i, r;

    gs = gpa_to_hva(vm, ACPI_TABLES_GPA, ACPI_TABLES_MAX, (void **)&base);
    if (gs != GPA_OK) {
        vmm_err("ACPI tables at GPA 0x%llx: %s",
                (unsigned long long)ACPI_TABLES_GPA, gpa_status_str(gs));
        return VMM_ERR_INVAL;
    }
    memset(base, 0, ACPI_TABLES_MAX);

    off      = 0;
    xsdt_off = ALIGN16(off + sizeof(struct acpi_rsdp));
    fadt_off = ALIGN16(xsdt_off + sizeof(struct acpi_header) + 2 * 8);
    dsdt_off = ALIGN16(fadt_off + sizeof(struct acpi_fadt));

    /* DSDT */
    h = (struct acpi_header *)(base + dsdt_off);
    r = build_dsdt(vm, (uint8_t *)(h + 1),
                   ACPI_TABLES_MAX - dsdt_off - sizeof(*h), &aml_len);
    if (r != VMM_OK)
        return r;
    fill_header(h, "DSDT", (uint32_t)(sizeof(*h) + aml_len), 2);
    madt_off = ALIGN16(dsdt_off + sizeof(*h) + aml_len);

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
    fadt->flags          = FADT_F_WBINVD | FADT_F_SLP_BUTTON | FADT_F_RESET_REG_SUP;
    /* RESET_REG 指向 i8042 命令口，RESET_VALUE 是脉冲 reset 命令，复用
     * i8042 设备已有的 reset 路径。x86 Linux 的默认重启顺序先试 ACPI
     * （reboot_type 缺省 BOOT_ACPI），用户用 --cmdline 换掉默认的 reboot=k
     * 时靠它一次成功；只支持硬件精简 ACPI 的平台上这也是规范规定的重启方式。
     * 不用 q35 常见的 0xCF9：那要新增一个设备，且它和 PCI conf1 的
     * 0xCF8-0xCFF 窗口重叠，Linux 的 pci_check_type1() 会探测这段端口；
     * i8042 的 reset 已经有实现和测试。 */
    fill_gas(fadt->reset_reg, GAS_SPACE_SYSTEM_IO, 8, GAS_ACCESS_BYTE,
             I8042_CMD_PORT);
    fadt->reset_value    = I8042_CMD_PULSE_RESET;
    fadt->minor_version  = 4;
    fill_gas(fadt->sleep_control, GAS_SPACE_SYSTEM_IO, 8, GAS_ACCESS_BYTE,
             ACPI_SLEEP_CTL_PORT);
    fill_gas(fadt->sleep_status, GAS_SPACE_SYSTEM_IO, 8, GAS_ACCESS_BYTE,
             ACPI_SLEEP_STS_PORT);
    /* FIRMWARE_CTRL 要等 MADT 定长后才知道 FACS 在哪，表头在那之后填 */

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
     * Linux 在没有 override 时默认就是恒等；kernel irqchip 时 KVM 的默认路由
     * 也是恒等，split irqchip 时 split_irqchip_set_irq 按同样的恒等关系接线。
     * SCI（IRQ 9）同样不需要：ISO 只用于非恒等映射或极性不合标准的输入
     * （ACPI 6.5 5.2.12.5），SCI 的标准属性是 level、active low（FADT
     * SCI_INT），Linux 没看到 ISO 时就按这个配置引脚
     * （arch/x86/kernel/acpi/boot.c acpi_sci_ioapic_setup）。 */
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

    /* FACS 的位置写进 FADT 的 32 位和 64 位两个字段，取值相同：
     * X_FIRMWARE_CTRL 非 0 时 OSPM 忽略 FIRMWARE_CTRL（Table 5.9），两者
     * 一致时不管 OSPM 用哪个都指向同一处 */
    facs_off = ALIGN64(madt_off + len);
    if (facs_off + sizeof(struct acpi_facs) > ACPI_TABLES_MAX)
        return VMM_ERR_INVAL;
    fadt->firmware_ctrl   = (uint32_t)(ACPI_TABLES_GPA + facs_off);
    fadt->x_firmware_ctrl = ACPI_TABLES_GPA + facs_off;
    fill_header(&fadt->h, "FACP", sizeof(*fadt), 6);

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

    /* FACS 最后填：hardware signature 覆盖它前面已经定稿的全部表 */
    facs = (struct acpi_facs *)(base + facs_off);
    memcpy(facs->signature, "FACS", 4);
    facs->length             = sizeof(*facs);
    facs->hardware_signature = tables_signature(base, facs_off);
    facs->version            = FACS_VERSION;

    *rsdp_gpa = ACPI_TABLES_GPA + off;
    vmm_info("ACPI @ GPA 0x%llx: RSDP XSDT@0x%llx FADT@0x%llx DSDT@0x%llx MADT@0x%llx "
             "FACS@0x%llx",
             (unsigned long long)ACPI_TABLES_GPA,
             (unsigned long long)(ACPI_TABLES_GPA + xsdt_off),
             (unsigned long long)(ACPI_TABLES_GPA + fadt_off),
             (unsigned long long)(ACPI_TABLES_GPA + dsdt_off),
             (unsigned long long)(ACPI_TABLES_GPA + madt_off),
             (unsigned long long)(ACPI_TABLES_GPA + facs_off));
    return VMM_OK;
}
