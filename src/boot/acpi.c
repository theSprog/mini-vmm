/* src/boot/acpi.c — 最小 ACPI 表                                  [linux-boot]
 *
 * 内存布局（ACPI_TABLES_GPA 起，每张表 16 字节对齐，FACS 64 字节对齐）：
 *   RSDP  -> XSDT -> { FADT -> { DSDT, FACS }, MADT }
 *
 * 参考 ACPI 6.5 spec（ref/ACPI_Spec_6_5_Aug29.pdf）：5.2.5 RSDP, 5.2.8 XSDT,
 * 5.2.9 FADT, 5.2.10 FACS, 5.2.11.1 DSDT, 5.2.12 MADT。FADT 指向的 PM1 寄存器由
 * power.c 和 devices/acpi_pm.c 实现。
 */
#include <stddef.h>
#include <string.h>

#include "vmm.h"
#include "memory.h"
#include "boot/acpi.h"
#include "boot/mptable.h"
#include "devices/i8042.h"
#include "devices/acpi_pm.h"
#include "devices/pvpanic.h"
#include "devices/rtc.h"
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

/* 各表表头的 revision。DSDT 的 revision 还决定 AML 整数的宽度：小于 2 时
 * 是 32 位，2 起是 64 位（5.2.11.1） */
#define DSDT_REVISION 2
#define FADT_REVISION 6          /* FADT 的主版本号，次版本号在 minor_version */
#define FADT_MINOR_VERSION 4
#define MADT_REVISION 5
#define XSDT_REVISION 1

struct acpi_rsdp {
    char     signature[8];      /* "RSD PTR " */
    uint8_t  checksum;          /* 只覆盖 ACPI 1.0 的部分，到 rsdt_address 为止 */
    char     oem_id[6];
    uint8_t  revision;          /* RSDP_REVISION */
    uint32_t rsdt_address;
    uint32_t length;            /* 以下是 ACPI 2.0 加的字段 */
    uint64_t xsdt_address;
    uint8_t  ext_checksum;      /* 覆盖全部 36 字节 */
    uint8_t  reserved[3];
} __attribute__((packed));
#define RSDP_REVISION 2         /* ACPI 2.0+，有 XSDT */
#define RSDP_V1_LEN   offsetof(struct acpi_rsdp, length)

/* XSDT 列出的表。DSDT 不在其中：它由 FADT 的 DSDT/X_DSDT 字段引用 */
enum { XSDT_FADT, XSDT_MADT, XSDT_NR_ENTRIES };

/* 条目紧跟在 36 字节的表头之后，所以这些 64 位字段只有 4 字节对齐，ACPI
 * 规范就是这么定的（ACPICA 为此专门有 ACPI_MOVE_64_TO_64）。packed 让编译器
 * 按未对齐访问生成代码；用普通的 uint64_t * 去写是未定义行为，UBSan 会报
 * misaligned store。 */
struct acpi_xsdt {
    struct acpi_header h;
    uint64_t entry[XSDT_NR_ENTRIES];
} __attribute__((packed));

/* Generic Address Structure（5.2.3.2）：FADT 用它描述寄存器的位置 */
struct acpi_gas {
    uint8_t  space_id;
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} __attribute__((packed));
#define GAS_SPACE_SYSTEM_IO 1
#define GAS_ACCESS_BYTE     1

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
    struct acpi_gas reset_reg;
    uint8_t  reset_value;
    uint16_t arm_boot_arch;
    uint8_t  minor_version;
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
    struct acpi_gas x_blocks[8];   /* X_PM1a_EVT ... X_GPE1，留 0 让 ACPICA 从 32 位字段转换 */
    struct acpi_gas sleep_control;
    struct acpi_gas sleep_status;
    uint64_t hypervisor_id;
} __attribute__((packed));

struct acpi_madt {
    struct acpi_header h;
    uint32_t lapic_addr;
    uint32_t flags;
} __attribute__((packed));

/* MADT 子表的类型（5.2.12，Table 5.21） */
#define MADT_TYPE_LAPIC     0
#define MADT_TYPE_IOAPIC    1
#define MADT_TYPE_ISO       2
#define MADT_TYPE_LAPIC_NMI 4

struct madt_lapic {
    uint8_t  type;              /* MADT_TYPE_LAPIC */
    uint8_t  length;
    uint8_t  processor_id;
    uint8_t  apic_id;
    uint32_t flags;
} __attribute__((packed));
#define MADT_LAPIC_ENABLED  (1u << 0)

struct madt_ioapic {
    uint8_t  type;              /* MADT_TYPE_IOAPIC */
    uint8_t  length;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t address;
    uint32_t gsi_base;
} __attribute__((packed));

struct madt_iso {
    uint8_t  type;              /* MADT_TYPE_ISO */
    uint8_t  length;
    uint8_t  bus;               /* 0 = ISA */
    uint8_t  source;            /* ISA IRQ */
    uint32_t gsi;
    uint16_t flags;             /* 0 = 按总线默认极性/触发方式 */
} __attribute__((packed));

struct madt_lapic_nmi {
    uint8_t  type;              /* MADT_TYPE_LAPIC_NMI */
    uint8_t  length;
    uint8_t  processor_id;
    uint16_t flags;             /* 0 = 按总线默认的极性和触发方式 */
    uint8_t  lint;              /* NMI 接在 LAPIC 的哪个 LINT 引脚 */
} __attribute__((packed));
#define MADT_ALL_PROCESSORS 0xff

_Static_assert(sizeof(struct acpi_header) == 36, "acpi_header size");
_Static_assert(sizeof(struct acpi_rsdp) == 36, "rsdp size");
_Static_assert(RSDP_V1_LEN == 20, "rsdp v1 length");
_Static_assert(sizeof(struct acpi_xsdt) == 36 + 8 * XSDT_NR_ENTRIES, "xsdt size");
_Static_assert(sizeof(struct acpi_gas) == 12, "gas size");
_Static_assert(sizeof(struct acpi_fadt) == 276, "fadt size");
_Static_assert(sizeof(struct madt_lapic) == 8, "madt_lapic size");
_Static_assert(sizeof(struct madt_ioapic) == 12, "madt_ioapic size");
_Static_assert(sizeof(struct madt_iso) == 10, "madt_iso size");
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
static int build_dsdt_aml(const struct acpi_devices *devs, uint8_t *out,
                          size_t cap, size_t *len)
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
    if (devs->pvpanic)
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

/* 一个字节宽的 IO 端口寄存器 */
static struct acpi_gas gas_io8(uint16_t port)
{
    struct acpi_gas g = {
        .space_id    = GAS_SPACE_SYSTEM_IO,
        .bit_width   = 8,
        .access_size = GAS_ACCESS_BYTE,
        .address     = port,
    };

    return g;
}

/* DSDT = 表头 + build_dsdt_aml 生成的 AML，*len 返回整张表的长度 */
static int build_dsdt(struct acpi_header *h, size_t cap,
                      const struct acpi_devices *devs, size_t *len)
{
    size_t aml_len;
    int r;

    r = build_dsdt_aml(devs, (uint8_t *)(h + 1), cap - sizeof(*h), &aml_len);
    if (r != VMM_OK)
        return r;
    *len = sizeof(*h) + aml_len;
    fill_header(h, "DSDT", (uint32_t)*len, DSDT_REVISION);
    return VMM_OK;
}

/* MADT 依次是：表头，每个 CPU 一个 LAPIC，一个 IOAPIC，一条 ISO，一条 LAPIC NMI */
static uint32_t madt_len(int nr_cpus)
{
    return (uint32_t)(sizeof(struct acpi_madt)
                      + (size_t)nr_cpus * sizeof(struct madt_lapic)
                      + sizeof(struct madt_ioapic)
                      + sizeof(struct madt_iso)
                      + sizeof(struct madt_lapic_nmi));
}

/* PC 上 8254 的输出接在 IOAPIC 的 pin 2（pin 0 留给 8259 的 INTR），
 * 所以 ISA IRQ 0 要用一条 Interrupt Source Override 映射到 GSI 2
 * （ACPI 6.5 5.2.12.5）。其余 ISA IRQ 与 GSI 恒等映射，不需要 override。
 * SCI（IRQ 9）同样不需要：ISO 只用于非恒等映射或极性不合标准的输入
 * （ACPI 6.5 5.2.12.5），SCI 的标准属性是 level、active low（FADT
 * SCI_INT），Linux 没看到 ISO 时就按这个配置引脚
 * （arch/x86/kernel/acpi/boot.c acpi_sci_ioapic_setup）。 */
static void build_madt(struct acpi_madt *madt, int nr_cpus)
{
    struct madt_lapic *lapic = (struct madt_lapic *)(madt + 1);
    struct madt_ioapic *io;
    struct madt_iso *iso;
    struct madt_lapic_nmi *nmi;
    int i;

    madt->lapic_addr = MP_LAPIC_BASE;
    madt->flags      = MADT_PCAT_COMPAT;

    for (i = 0; i < nr_cpus; i++) {
        lapic[i].type         = MADT_TYPE_LAPIC;
        lapic[i].length       = sizeof(lapic[i]);
        lapic[i].processor_id = (uint8_t)i;
        lapic[i].apic_id      = (uint8_t)i;
        lapic[i].flags        = MADT_LAPIC_ENABLED;
    }

    io = (struct madt_ioapic *)(lapic + nr_cpus);
    io->type      = MADT_TYPE_IOAPIC;
    io->length    = sizeof(*io);
    io->ioapic_id = (uint8_t)nr_cpus;   /* 和 MP table 保持一致 */
    io->address   = MP_IOAPIC_BASE;
    io->gsi_base  = 0;

    iso = (struct madt_iso *)(io + 1);
    iso->type   = MADT_TYPE_ISO;
    iso->length = sizeof(*iso);
    iso->bus    = 0;
    iso->source = 0;
    iso->gsi    = 2;
    iso->flags  = 0;

    /* 所有 CPU 的 NMI 都接在 LINT1 上 */
    nmi = (struct madt_lapic_nmi *)(iso + 1);
    nmi->type         = MADT_TYPE_LAPIC_NMI;
    nmi->length       = sizeof(*nmi);
    nmi->processor_id = MADT_ALL_PROCESSORS;
    nmi->lint         = 1;

    fill_header(&madt->h, "APIC", madt_len(nr_cpus), MADT_REVISION);
}

static void build_fadt(struct acpi_fadt *fadt, uint64_t dsdt_gpa,
                       uint64_t facs_gpa)
{
    /* 32 位和 64 位两个字段写同一个地址：64 位字段非 0 时 OSPM 忽略 32 位
     * 字段（Table 5.9），两者一致时不管 OSPM 用哪个都指向同一处 */
    fadt->dsdt            = (uint32_t)dsdt_gpa;
    fadt->x_dsdt          = dsdt_gpa;
    fadt->firmware_ctrl   = (uint32_t)facs_gpa;
    fadt->x_firmware_ctrl = facs_gpa;

    fadt->sci_int        = ACPI_SCI_IRQ;
    fadt->pm1a_evt_blk   = ACPI_PM1A_EVT_PORT;
    fadt->pm1a_cnt_blk   = ACPI_PM1A_CNT_PORT;
    fadt->pm1_evt_len    = ACPI_PM1_EVT_LEN;
    fadt->pm1_cnt_len    = ACPI_PM1_CNT_LEN;
    fadt->century        = RTC_CENTURY;
    fadt->iapc_boot_arch = FADT_BOOT_LEGACY_DEV | FADT_BOOT_8042;
    fadt->flags          = FADT_F_WBINVD | FADT_F_SLP_BUTTON | FADT_F_RESET_REG_SUP;
    /* RESET_REG 指向 i8042 命令口，RESET_VALUE 是脉冲 reset 命令，复用
     * i8042 设备已有的 reset 路径。x86 Linux 的默认重启顺序先试 ACPI
     * （reboot_type 缺省 BOOT_ACPI），用户用 --cmdline 换掉默认的 reboot=k
     * 时靠它一次成功；只支持硬件精简 ACPI 的平台上这也是规范规定的重启方式。
     * 不用 q35 常见的 0xCF9：那要新增一个设备，且它和 PCI conf1 的
     * 0xCF8-0xCFF 窗口重叠，Linux 的 pci_check_type1() 会探测这段端口；
     * i8042 的 reset 已经有实现和测试。 */
    fadt->reset_reg      = gas_io8(I8042_CMD_PORT);
    fadt->reset_value    = I8042_CMD_PULSE_RESET;
    fadt->minor_version  = FADT_MINOR_VERSION;
    fadt->sleep_control  = gas_io8(ACPI_SLEEP_CTL_PORT);
    fadt->sleep_status   = gas_io8(ACPI_SLEEP_STS_PORT);

    fill_header(&fadt->h, "FACP", sizeof(*fadt), FADT_REVISION);
}

static void build_xsdt(struct acpi_xsdt *xsdt, uint64_t fadt_gpa,
                       uint64_t madt_gpa)
{
    xsdt->entry[XSDT_FADT] = fadt_gpa;
    xsdt->entry[XSDT_MADT] = madt_gpa;
    fill_header(&xsdt->h, "XSDT", sizeof(*xsdt), XSDT_REVISION);
}

static void build_rsdp(struct acpi_rsdp *rsdp, uint64_t xsdt_gpa)
{
    memcpy(rsdp->signature, "RSD PTR ", 8);
    memcpy(rsdp->oem_id, ACPI_OEM_ID, 6);
    rsdp->revision     = RSDP_REVISION;
    rsdp->length       = sizeof(*rsdp);
    rsdp->xsdt_address = xsdt_gpa;
    rsdp->checksum     = acpi_checksum(rsdp, RSDP_V1_LEN);
    rsdp->ext_checksum = acpi_checksum(rsdp, sizeof(*rsdp));
}

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

static void build_facs(struct acpi_facs *facs, uint32_t hw_signature)
{
    memcpy(facs->signature, "FACS", 4);
    facs->length             = sizeof(*facs);
    facs->hardware_signature = hw_signature;
    facs->version            = FACS_VERSION;
}

#define ALIGN16(x) (((x) + 15) & ~(uint64_t)15)
#define ALIGN64(x) (((x) + 63) & ~(uint64_t)63)

/* 每张表在 ACPI_TABLES_GPA 区域内的偏移 */
struct acpi_layout {
    uint64_t rsdp, xsdt, fadt, dsdt, madt, facs;
};

int acpi_setup(struct vmm_vm *vm, int nr_cpus,
               const struct acpi_devices *devs, uint64_t *rsdp_gpa)
{
    struct acpi_layout at;
    enum gpa_status gs;
    uint8_t *base;
    size_t dsdt_len;
    int r;

    gs = gpa_to_hva(vm, ACPI_TABLES_GPA, ACPI_TABLES_MAX, (void **)&base);
    if (gs != GPA_OK) {
        vmm_err("ACPI tables at GPA 0x%llx: %s",
                (unsigned long long)ACPI_TABLES_GPA, gpa_status_str(gs));
        return VMM_ERR_INVAL;
    }
    memset(base, 0, ACPI_TABLES_MAX);

    /* 布局：定长的表排在前面，DSDT 和 MADT 的长度生成后才知道，排在后面。
     * 每张表 16 字节对齐，FACS 要求 64 字节对齐（Table 5.13） */
    at.rsdp = 0;
    at.xsdt = ALIGN16(at.rsdp + sizeof(struct acpi_rsdp));
    at.fadt = ALIGN16(at.xsdt + sizeof(struct acpi_xsdt));
    at.dsdt = ALIGN16(at.fadt + sizeof(struct acpi_fadt));
    r = build_dsdt((struct acpi_header *)(base + at.dsdt),
                   ACPI_TABLES_MAX - at.dsdt, devs, &dsdt_len);
    if (r != VMM_OK)
        return r;
    at.madt = ALIGN16(at.dsdt + dsdt_len);
    at.facs = ALIGN64(at.madt + madt_len(nr_cpus));
    if (at.facs + sizeof(struct acpi_facs) > ACPI_TABLES_MAX)
        return VMM_ERR_INVAL;

    /* 表之间互相引用，被引用的地址都已经在布局里定下，按任意顺序填都行；
     * 只有 FACS 必须最后填：它的 hardware signature 覆盖前面全部表 */
    build_madt((struct acpi_madt *)(base + at.madt), nr_cpus);
    build_fadt((struct acpi_fadt *)(base + at.fadt),
               ACPI_TABLES_GPA + at.dsdt, ACPI_TABLES_GPA + at.facs);
    build_xsdt((struct acpi_xsdt *)(base + at.xsdt),
               ACPI_TABLES_GPA + at.fadt, ACPI_TABLES_GPA + at.madt);
    build_rsdp((struct acpi_rsdp *)(base + at.rsdp), ACPI_TABLES_GPA + at.xsdt);
    build_facs((struct acpi_facs *)(base + at.facs),
               tables_signature(base, at.facs));

    *rsdp_gpa = ACPI_TABLES_GPA + at.rsdp;
    vmm_info("ACPI @ GPA 0x%llx: RSDP XSDT@0x%llx FADT@0x%llx DSDT@0x%llx MADT@0x%llx "
             "FACS@0x%llx",
             (unsigned long long)ACPI_TABLES_GPA,
             (unsigned long long)(ACPI_TABLES_GPA + at.xsdt),
             (unsigned long long)(ACPI_TABLES_GPA + at.fadt),
             (unsigned long long)(ACPI_TABLES_GPA + at.dsdt),
             (unsigned long long)(ACPI_TABLES_GPA + at.madt),
             (unsigned long long)(ACPI_TABLES_GPA + at.facs));
    return VMM_OK;
}
