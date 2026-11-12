/* src/devices/ahci.c — AHCI SATA 控制器，设计说明见 devices/ahci.h */
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

#include "vmm.h"
#include "opts.h"
#include "block/blockdev.h"
#include "devices/ahci.h"

enum {
    W_ACCESS_SIZE,
    W_CI_STOPPED,
    W_SRST,
    W_CHS,
    W_SET_FEATURES,
    W_READ_ONLY,
};

#define warn_once(s, w, ...)                                \
    do {                                                    \
        if (!((s)->warned & (1u << (w)))) {                 \
            (s)->warned |= 1u << (w);                       \
            vmm_warn(__VA_ARGS__);                          \
        }                                                   \
    } while (0)

/* CAP 由下面几项拼成，含义见 ahci.h 开头 */
#define AHCI_CAP_VALUE  (AHCI_CAP_S64A | AHCI_CAP_SCLO |                  \
                         (AHCI_SPEED_GEN3 << AHCI_CAP_ISS_SHIFT) |        \
                         AHCI_CAP_SAM | AHCI_CAP_PMD |                    \
                         ((AHCI_NR_SLOTS - 1) << AHCI_CAP_NCS_SHIFT) |    \
                         (AHCI_NR_PORTS - 1))
#define AHCI_PI_VALUE   ((1u << AHCI_NR_PORTS) - 1)

/* PxIS 里软件写 1 清除的位。PCS、PRCS 是 PxSERR 的镜像，要清 PxSERR；
 * UFS 同理（本模型不会收到未知 FIS） */
#define PxIS_RWC_MASK   (AHCI_PxIS_TFES | AHCI_PxIS_HBFS | AHCI_PxIS_HBDS |  \
                         AHCI_PxIS_IFS | AHCI_PxIS_INFS | AHCI_PxIS_OFS |    \
                         AHCI_PxIS_IPMS | AHCI_PxIS_DPS | AHCI_PxIS_SDBS |   \
                         AHCI_PxIS_DSS | AHCI_PxIS_PSS | AHCI_PxIS_DHRS)

/* PxIE 里可写的位：CPDE、DMPE 在不支持冷插拔检测、机械开关时只读为 0 */
#define PxIE_MASK       (PxIS_RWC_MASK | AHCI_PxIS_PRCS | AHCI_PxIS_PCS |    \
                         AHCI_PxIS_UFS)

/* PxCMD 里软件可写、读回原值的位。ICC 写入后立刻回到 0（链路本来就在
 * active，第 3.3.7 节：请求进入当前状态时直接回到 Idle）；CLO 是写 1
 * 触发；ALPE/ASP/APSTE/PMA 在对应能力为 0 时只读 */
#define PxCMD_RW_MASK   (AHCI_PxCMD_DLAE | AHCI_PxCMD_ATAPI | AHCI_PxCMD_FRE | \
                         AHCI_PxCMD_ST)

/* PxSCTL 的 IPM、SPD、DET 三个字段可写，PMP、SPM 不被 AHCI 使用 */
#define PxSCTL_MASK     0xFFFu

/* ATA8-ACS 表 92：通用 ATA 设备（非 PACKET）的复位签名是 Count 01h、
 * LBA 000001h；PxSIG 的字节顺序是 LBA High、LBA Mid、LBA Low、Count
 * （第 3.3.9 节），拼出来是 0x00000101。Error 填诊断码 01h（表 21：
 * 设备 0 通过）；Status 取 BSY=0、DRDY=1、DRQ=0，ATA8-ACS 第 6.1.9 节
 * 说命令集用这个组合表示“设备就绪”，Linux 等复位完成时只看 BSY
 * （ahci_check_ready -> ata_check_ready）。 */
#define ATA_SIG_DISK    0x00000101u
#define ATA_DIAG_PASSED 0x01

static inline struct ahci_port *port_at(struct ahci *s, uint32_t off, uint32_t *reg)
{
    uint32_t i;

    if (off < AHCI_PORT_BASE)
        return NULL;
    i = (off - AHCI_PORT_BASE) / AHCI_PORT_SIZE;
    if (i >= AHCI_NR_PORTS)
        return NULL;
    *reg = (off - AHCI_PORT_BASE) % AHCI_PORT_SIZE;
    return &s->port[i];
}

static inline int port_no(struct ahci *s, struct ahci_port *p)
{
    return (int)(p - s->port);
}

static inline uint64_t addr64(uint32_t lo, uint32_t hi)
{
    return ((uint64_t)hi << 32) | lo;
}

/* PxIS 的完整读值：存下的可清位加上由 PxSERR 派生的两个只读位 */
static uint32_t port_is(const struct ahci_port *p)
{
    uint32_t v = p->is;

    if (p->serr & AHCI_SERR_DIAG_X)
        v |= AHCI_PxIS_PCS;
    if (p->serr & AHCI_SERR_DIAG_N)
        v |= AHCI_PxIS_PRCS;
    return v;
}

/* 两级中断（第 10.7 节）：每个端口的 PxIS & PxIE 是一根“虚拟线”，线为 1
 * 时把 IS 里该端口的位置 1。IS 是粘滞的，软件写 1 才清；但线还是 1 时
 * 清掉的位会立刻再被置上，所以每次状态变化后都要重新扫一遍所有端口。
 * INTx 电平是 GHC.IE 与“IS 非零”的与（第 10.7.2.1 节，引脚模式）。 */
static void update_irq(struct ahci *s)
{
    int i;

    for (i = 0; i < AHCI_NR_PORTS; i++)
        if (port_is(&s->port[i]) & s->port[i].ie)
            s->is |= 1u << i;
    pci_set_irq(&s->pci, (s->ghc & AHCI_GHC_IE) && s->is);
}

/* 设备发来的 FIS 写进 FIS 接收区（第 4.2.1 节）。FRE 为 0 时 HBA 不往
 * 内存里写（第 3.3.7 节），只更新寄存器。写失败按主机总线致命错误处理
 * （第 6.1.1 节），调用者据此停下端口。 */
static int post_fis(struct ahci *s, struct ahci_port *p, uint32_t off,
                    const uint8_t *fis, size_t len)
{
    uint64_t gpa;

    if (!(p->cmd & AHCI_PxCMD_FRE))
        return VMM_OK;
    gpa = addr64(p->fb, p->fbu) + off;
    if (pci_dma_write(&s->pci, gpa, fis, len) != VMM_OK) {
        vmm_warn("ahci: port %d: FIS receive area gpa=0x%llx (offset 0x%x, %zu bytes) not writable",
                 port_no(s, p), (unsigned long long)gpa, off, len);
        return VMM_ERR_GUEST;
    }
    return VMM_OK;
}

/* 访问 guest 内存失败：置 HBFS 和 PxSERR.ERR.E，端口进入 ERR:Fatal
 * （第 6.1.1、6.2.2 节），出错的槽留在 PxCI 里、CCS 指向它，等软件
 * 清 ST 再重新置 ST 来恢复。 */
static void port_halt(struct ahci *s, struct ahci_port *p, int slot, uint32_t is_bits)
{
    p->is |= is_bits;
    p->halted = true;
    if (slot >= 0)
        p->ccs = (uint8_t)slot;
    update_irq(s);
}

static void host_bus_fatal(struct ahci *s, struct ahci_port *p, int slot)
{
    p->serr |= AHCI_SERR_ERR_E;
    port_halt(s, p, slot, AHCI_PxIS_HBFS);
}

/* ---- 链路与复位 ---- */

/* 盘的上电默认设置，复位后同样回到这里（ahci.h 里 struct ahci 的说明）。
 * 写缓存默认打开、选中最高的 UDMA 模式，这是 SATA 盘的常见出厂设置；
 * Linux 探测时会自己再发一遍 SET FEATURES 03h */
static void disk_reset(struct ahci *s)
{
    s->wcache = true;
    s->dma_mode = ATA_XFER_UDMA | 6;
}

static void link_down(struct ahci_port *p, uint32_t det)
{
    if ((p->ssts & 0xF) == AHCI_SSTS_DET_PHY)
        p->serr |= AHCI_SERR_DIAG_N;       /* PhyRdy 从 1 变 0 */
    p->ssts = det;
    p->tfd = AHCI_TFD_STS_RESET;           /* 第 10.4.2 节 */
}

/* COMRESET 结束（PxSCTL.DET 回到 0，或 HBA 复位）。没有设备的端口收不到
 * COMINIT，停在 DET=0。有设备时链路建立，设备接着发出带复位签名的 D2H
 * Register FIS：HBA 用它更新 PxTFD 和 PxSIG（第 10.4 节）。这个 FIS 的
 * I 位为 0（SATA 3.0 第 11.3 节只规定了它的寄存器内容），所以不置
 * PxIS.DHRS。 */
static void link_up(struct ahci *s, struct ahci_port *p)
{
    uint32_t limit = (p->sctl >> AHCI_SCTL_SPD_SHIFT) & 0xF;
    uint32_t spd = AHCI_SPEED_GEN3;
    uint8_t fis[SATA_FIS_REG_LEN] = { 0 };

    if (!p->has_disk) {
        p->ssts = AHCI_SSTS_DET_NONE;
        return;
    }
    if (limit && limit < spd)
        spd = limit;
    p->ssts = AHCI_SSTS_DET_PHY | (spd << 4) | (AHCI_SSTS_IPM_ACTIVE << 8);
    p->serr |= AHCI_SERR_DIAG_X | AHCI_SERR_DIAG_N;

    disk_reset(s);
    p->sig = ATA_SIG_DISK;
    p->tfd = (ATA_DIAG_PASSED << 8) | ATA_STS_DRDY;
    fis[0]  = SATA_FIS_REG_D2H;
    fis[2]  = ATA_STS_DRDY;
    fis[3]  = ATA_DIAG_PASSED;
    fis[4]  = (uint8_t)(ATA_SIG_DISK >> 8);      /* LBA Low */
    fis[12] = (uint8_t)ATA_SIG_DISK;             /* Count */
    if (post_fis(s, p, AHCI_RFIS_RFIS, fis, sizeof(fis)) != VMM_OK)
        host_bus_fatal(s, p, -1);
}

/* HBA 复位（第 10.4.3 节）：除 PxCLB/PxCLBU/PxFB/PxFBU 和 HwInit 位以外的
 * 端口寄存器回到复位值；不支持交错上电，所以紧接着对每个端口发一次
 * COMRESET。 */
static void port_reset(struct ahci *s, struct ahci_port *p)
{
    p->is = 0;
    p->ie = 0;
    p->cmd = 0;
    p->tfd = AHCI_TFD_STS_RESET;
    p->sig = 0xFFFFFFFFu;
    p->ssts = AHCI_SSTS_DET_NONE;
    p->sctl = 0;
    p->serr = 0;
    p->sact = 0;
    p->ci = 0;
    p->ccs = 0;
    p->halted = false;
    link_up(s, p);
}

static void hba_reset(struct ahci *s)
{
    int i;

    s->ghc = 0;
    s->is = 0;
    for (i = 0; i < AHCI_NR_PORTS; i++)
        port_reset(s, &s->port[i]);
    update_irq(s);
}

static void write_sctl(struct ahci *s, struct ahci_port *p, uint32_t val)
{
    uint32_t old_det = p->sctl & AHCI_SCTL_DET_MASK;
    uint32_t det = val & AHCI_SCTL_DET_MASK;

    p->sctl = val & PxSCTL_MASK;
    if (det == AHCI_SCTL_DET_COMRESET)
        link_down(p, AHCI_SSTS_DET_NONE);
    else if (det == AHCI_SCTL_DET_OFFLINE)
        link_down(p, AHCI_SSTS_DET_OFFLINE);
    else if (det == 0 && old_det != 0)
        link_up(s, p);
    update_irq(s);
}

/* ---- 命令 ---- */

/* 设备完成一条命令，回一个 D2H Register FIS（I=1）。HBA 把它写进 FIS
 * 接收区、更新 PxTFD、置 PxIS.DHRS。状态里带 ERR 时置 TFES 并停下
 * （第 6.1.4 节），这一槽不清 PxCI；否则清掉这一槽的 PxCI 位
 * （第 3.3.14 节：收到清掉 BSY、DRQ、ERR 的 FIS 时清）。 */
static void complete_cmd(struct ahci *s, struct ahci_port *p, int slot,
                         uint8_t status, uint8_t error)
{
    uint8_t fis[SATA_FIS_REG_LEN] = { 0 };

    fis[0] = SATA_FIS_REG_D2H;
    fis[1] = SATA_FIS_I;
    fis[2] = status;
    fis[3] = error;
    p->tfd = ((uint32_t)error << 8) | status;
    if (post_fis(s, p, AHCI_RFIS_RFIS, fis, sizeof(fis)) != VMM_OK) {
        host_bus_fatal(s, p, slot);
        return;
    }
    p->is |= AHCI_PxIS_DHRS;
    if (status & ATA_STS_ERR) {
        p->is |= AHCI_PxIS_TFES;
        p->halted = true;
        p->ccs = (uint8_t)slot;
    } else {
        p->ci &= ~(1u << slot);
    }
    update_irq(s);
}

static void abort_cmd(struct ahci *s, struct ahci_port *p, int slot)
{
    complete_cmd(s, p, slot, ATA_STS_DRDY | ATA_STS_ERR, ATA_ERR_ABRT);
}

/* ---- IDENTIFY DEVICE 数据（ATA8-ACS 表 22） ---- */

#define ID_SERIAL_BYTES   20
#define ID_FW_BYTES       8
#define ID_MODEL_BYTES    40
#define ID_FIRMWARE       "1.0"
#define ID_MODEL          "MINIVMM AHCI DISK"

/* ATA 字符串：每个字放两个字符，前一个在高字节（Linux ata_id_string 按
 * 这个顺序取），长度不够补空格（第 7.16.7.9 节） */
static void id_string(uint16_t *w, const char *str, size_t nbytes)
{
    size_t len = strlen(str), i;
    uint8_t a, b;

    for (i = 0; i < nbytes; i += 2) {
        a = i < len ? (uint8_t)str[i] : ' ';
        b = i + 1 < len ? (uint8_t)str[i + 1] : ' ';
        w[i / 2] = (uint16_t)(a << 8 | b);
    }
}

void ahci_identify(const struct ahci *s, uint16_t id[ATA_ID_WORDS])
{
    uint64_t lba28 = s->sectors < 0x0FFFFFFF ? s->sectors : 0x0FFFFFFF;
    uint8_t sum = 0;
    int i;

    memset(id, 0, ATA_ID_WORDS * sizeof(id[0]));
    /* 字 0 为 0：bit 15 = 0 表示 ATA 设备，其余位保留或已废弃 */
    id_string(&id[10], s->blk ? s->blk->id : "", ID_SERIAL_BYTES);
    id_string(&id[23], ID_FIRMWARE, ID_FW_BYTES);
    id_string(&id[27], ID_MODEL, ID_MODEL_BYTES);
    /* 字 47 低字节是 READ/WRITE MULTIPLE 每块的扇区数。这两条 PIO 命令
     * 没有实现，填 0；Linux 只在字 59 bit 8 有效时才用它（multi_count） */
    id[47] = 0x8000;
    id[49] = (1u << 9) | (1u << 8);              /* LBA、DMA */
    id[50] = 0x4000 | 1u;                        /* bit 14 恒 1；bit 0 规定为 1 */
    id[53] = (1u << 2) | (1u << 1);              /* 字 88、64-70 有效 */
    id[60] = (uint16_t)lba28;                    /* 28 位命令可寻址的扇区数 */
    id[61] = (uint16_t)(lba28 >> 16);
    id[63] = 0x0007;                             /* 支持 MWDMA 0-2 */
    if ((s->dma_mode & 0xF8) == ATA_XFER_MWDMA)
        id[63] |= 1u << (8 + (s->dma_mode & 7));
    id[64] = 0x0003;                             /* 支持 PIO 3、4 */
    for (i = 65; i <= 68; i++)
        id[i] = 120;                             /* 周期时间（ns），只对并口有意义 */
    /* 字 76：SATA Gen1/2/3（SATA 3.0 第 13.2.1 节表 69 的 bit 1-3），
     * 不报 NCQ（bit 8）和主机发起的链路省电（bit 9） */
    id[76] = (1u << 3) | (1u << 2) | (1u << 1);
    id[80] = 0x01F0;                             /* ATA/ATAPI-4 到 ATA8-ACS */
    /* 字 82-87：支持与已启用的命令集。写缓存（82/85 bit 5）、强制的电源
     * 管理（82/85 bit 3），FLUSH CACHE 和 FLUSH CACHE EXT（83/86 bit 12/13），
     * 48 位地址（83/86 bit 10）。83、84、87 的 bit 14 恒 1 表示字有效，
     * Linux 的 ata_id_has_* 先看这一位 */
    id[82] = (1u << 5) | (1u << 3);
    id[83] = 0x4000 | (1u << 13) | (1u << 12) | (1u << 10);
    id[84] = 0x4000;
    id[85] = (1u << 3) | (s->wcache ? 1u << 5 : 0);
    id[86] = (1u << 13) | (1u << 12) | (1u << 10);
    id[87] = 0x4000;
    id[88] = 0x007F;                             /* 支持 UDMA 0-6 */
    if ((s->dma_mode & 0xF8) == ATA_XFER_UDMA)
        id[88] |= 1u << (8 + (s->dma_mode & 7));
    /* 字 93 对 SATA 设备规定为 0（第 7.16.7.44 节）；Linux 的
     * ata_id_is_sata 靠它和字 80 判断是不是 SATA 盘 */
    for (i = 0; i < 4; i++)
        id[100 + i] = (uint16_t)(s->sectors >> (16 * i));
    id[106] = 0x4000;                            /* 字有效，1 个逻辑扇区对 1 个物理扇区 */
    id[217] = 1;                                 /* 非旋转介质（表 24） */
    id[222] = 0x1000 | 0x1F;                     /* 串行传输，ATA8-AST 到 SATA 2.6 */
    /* 字 255：签名 A5h，高字节使 512 字节之和为 0（第 7.16.7.82 节） */
    id[255] = 0xA5;
    for (i = 0; i < ATA_ID_WORDS; i++)
        sum = (uint8_t)(sum + (id[i] & 0xFF) + (id[i] >> 8));
    id[255] |= (uint16_t)((uint8_t)-sum << 8);
}

/* ---- 数据传输 ---- */

enum prdt_result {
    PRDT_OK,
    PRDT_HOST_BUS,      /* PRDT 本身或它指向的内存访问失败，已记日志 */
    PRDT_OVERFLOW,      /* PRDT 描述的字节数少于命令要传的，已记日志 */
};

/* 读 PRDT（第 4.2.3 节）并把前 len 字节映射成 iovec。每一项单独检查：
 * DBA 和 DBC 都由 guest 填写，[DBA, DBA + 字节数) 必须整个落在一个 RAM
 * slot 里（pci_dma_map），否则按主机总线错误停下端口，不读写任何数据。
 * DBA bit 0、DBC bit 0 是规定值（字对齐、偶数字节，第 4.2.3.3 节），
 * 按硬件的做法强制成规定值，不当作错误。
 * 多出 len 的 PRD 项不读也不检查；PRD 项的 I 位通过 *dps 返回，数据传完
 * 之后置 PxIS.DPS。成功时 *iovp 由调用方 free。 */
static enum prdt_result prdt_map(struct ahci *s, struct ahci_port *p, int slot,
                                 uint64_t ctba, uint32_t prdtl, uint64_t len,
                                 struct iovec **iovp, int *niovp, bool *dps)
{
    enum { BATCH = 32 };
    uint32_t prd[BATCH][4];
    struct iovec *iov;
    uint64_t left = len, dba, bytes;
    uint32_t i, k, nb;
    void *hva;
    int n = 0;

    *dps = false;
    iov = calloc(prdtl ? prdtl : 1, sizeof(*iov));
    if (!iov) {
        vmm_err("ahci: port %d slot %d: out of memory for %u PRD entries", port_no(s, p), slot, prdtl);
        return PRDT_HOST_BUS;
    }
    for (i = 0; i < prdtl && left; i += nb) {
        nb = prdtl - i < BATCH ? prdtl - i : BATCH;
        if (pci_dma_read(&s->pci, ctba + AHCI_CT_PRDT + (uint64_t)i * AHCI_PRD_SIZE,
                         prd, (size_t)nb * AHCI_PRD_SIZE) != VMM_OK) {
            vmm_warn("ahci: port %d slot %d: PRDT entries %u..%u at gpa=0x%llx unreadable",
                     port_no(s, p), slot, i, i + nb - 1,
                     (unsigned long long)(ctba + AHCI_CT_PRDT + (uint64_t)i * AHCI_PRD_SIZE));
            goto fail;
        }
        for (k = 0; k < nb && left; k++) {
            dba = addr64(prd[k][0] & ~1u, prd[k][1]);
            bytes = (uint64_t)((prd[k][3] & AHCI_PRD_DBC_MASK) | 1u) + 1;
            if (bytes > left)
                bytes = left;
            if (pci_dma_map(&s->pci, dba, bytes, &hva) != VMM_OK) {
                vmm_warn("ahci: port %d slot %d: PRD %u dba=0x%llx dbc=0x%x (%llu bytes used) is not guest RAM",
                         port_no(s, p), slot, i + k, (unsigned long long)dba,
                         prd[k][3] & AHCI_PRD_DBC_MASK, (unsigned long long)bytes);
                goto fail;
            }
            if (prd[k][3] & (1u << 31))
                *dps = true;
            iov[n].iov_base = hva;
            iov[n].iov_len = bytes;
            n++;
            left -= bytes;
        }
    }
    if (left) {
        vmm_warn("ahci: port %d slot %d: PRDT has %u entries for %llu bytes, command transfers %llu",
                 port_no(s, p), slot, prdtl, (unsigned long long)(len - left),
                 (unsigned long long)len);
        free(iov);
        return PRDT_OVERFLOW;
    }
    *iovp = iov;
    *niovp = n;
    return PRDT_OK;

fail:
    free(iov);
    return PRDT_HOST_BUS;
}

/* PRDT 出错时端口停下。溢出置 PxIS.OFS（第 6.1.5 节：严重错误，软件要走
 * 致命错误恢复）；Linux 不打开 OFE，所以这条路径上它等到命令超时再恢复，
 * Linux 自己构造的 PRDT 总是恰好覆盖传输长度，不会走到这里 */
static void prdt_fail(struct ahci *s, struct ahci_port *p, int slot, enum prdt_result r)
{
    if (r == PRDT_OVERFLOW)
        port_halt(s, p, slot, AHCI_PxIS_OFS);
    else
        host_bus_fatal(s, p, slot);
}

/* 命令头 DW1 的 PRDBC：清 PxCI 之前写成实际传输的字节数（第 5.4.1 节） */
static int set_prdbc(struct ahci *s, struct ahci_port *p, int slot, uint64_t hdr_gpa,
                     uint32_t bytes)
{
    if (pci_dma_write(&s->pci, hdr_gpa + 4, &bytes, sizeof(bytes)) != VMM_OK) {
        vmm_warn("ahci: port %d slot %d: PRDBC at gpa=0x%llx not writable",
                 port_no(s, p), slot, (unsigned long long)(hdr_gpa + 4));
        host_bus_fatal(s, p, slot);
        return VMM_ERR_GUEST;
    }
    return VMM_OK;
}

/* iovec 可能多于后端一次接受的 BLK_MAX_IOV 段，分批读写 */
static int disk_io(struct blockdev *bd, bool write, const struct iovec *iov, int n,
                   uint64_t off)
{
    int k, i, rc;
    uint64_t bytes;

    while (n > 0) {
        k = n < BLK_MAX_IOV ? n : BLK_MAX_IOV;
        rc = write ? blk_pwritev(bd, iov, k, off) : blk_preadv(bd, iov, k, off);
        if (rc != VMM_OK)
            return rc;
        for (bytes = 0, i = 0; i < k; i++)
            bytes += iov[i].iov_len;
        off += bytes;
        iov += k;
        n -= k;
    }
    return VMM_OK;
}

/* ---- ATA 命令 ---- */

/* IDENTIFY DEVICE（ECh，PIO data-in，第 7.16 节）。设备先发 PIO Setup FIS
 * 再发 Data FIS，结束时不再发 D2H Register FIS：最终状态在 PIO Setup FIS
 * 的 E_Status 里，HBA 在 PIO:Update 状态用它更新 PxTFD，再清 PxCI
 * （AHCI 第 5.3.9 节）。Linux 的 ahci_qc_fill_rtf 也从 PIO Setup FIS 的
 * 第 15 字节取结果状态。 */
static void cmd_identify(struct ahci *s, struct ahci_port *p, int slot,
                         uint64_t hdr_gpa, uint32_t prdtl, uint64_t ctba)
{
    uint16_t id[ATA_ID_WORDS];
    uint8_t data[ATA_SECTOR_SIZE], fis[SATA_FIS_PIO_LEN] = { 0 };
    struct iovec *iov;
    enum prdt_result r;
    size_t off = 0;
    bool dps;
    int n, i;

    ahci_identify(s, id);
    for (i = 0; i < ATA_ID_WORDS; i++) {        /* 数据按小端的 16 位字传 */
        data[2 * i] = (uint8_t)id[i];
        data[2 * i + 1] = (uint8_t)(id[i] >> 8);
    }
    r = prdt_map(s, p, slot, ctba, prdtl, sizeof(data), &iov, &n, &dps);
    if (r != PRDT_OK) {
        prdt_fail(s, p, slot, r);
        return;
    }
    for (i = 0; i < n; i++) {
        memcpy(iov[i].iov_base, data + off, iov[i].iov_len);
        off += iov[i].iov_len;
    }
    free(iov);
    if (set_prdbc(s, p, slot, hdr_gpa, sizeof(data)) != VMM_OK)
        return;

    fis[0]  = SATA_FIS_PIO_SETUP;
    fis[1]  = SATA_FIS_PIO_D | SATA_FIS_I;
    fis[2]  = ATA_STS_DRDY | ATA_STS_DRQ;        /* 传输开始时的状态 */
    fis[15] = ATA_STS_DRDY;                      /* E_Status：传完之后 */
    fis[16] = (uint8_t)sizeof(data);             /* Transfer Count */
    fis[17] = (uint8_t)(sizeof(data) >> 8);
    if (post_fis(s, p, AHCI_RFIS_PSFIS, fis, sizeof(fis)) != VMM_OK) {
        host_bus_fatal(s, p, slot);
        return;
    }
    p->tfd = ATA_STS_DRDY;
    p->is |= AHCI_PxIS_PSS | (dps ? AHCI_PxIS_DPS : 0);
    p->ci &= ~(1u << slot);
    update_irq(s);
}

/* READ/WRITE DMA（C8h/CAh，28 位）与 READ/WRITE DMA EXT（25h/35h，48 位），
 * 第 7.24、7.25、7.61、7.62 节。Register H2D FIS 里 LBA 的字节位置见
 * SATA 3.0 第 10.3.4 节：LBA[23:0] 在字节 4-6，48 位命令的 LBA[47:24] 在
 * 字节 8-10，28 位命令的 LBA[27:24] 在 Device（字节 7）的低 4 位；Count
 * 在字节 12-13，0 表示 256（28 位）或 65536（48 位）。
 * 出错时的 Error 位按表 113、131：地址越界 IDNF，读失败 UNC，写失败
 * ABRT。规范要求同时报告第一个出错扇区的 LBA，这里不报（D2H FIS 的 LBA
 * 字段为 0）：后端一次读写整段，不知道是哪一个扇区失败。 */
static void cmd_rw(struct ahci *s, struct ahci_port *p, int slot, uint64_t hdr_gpa,
                   uint32_t prdtl, uint64_t ctba, const uint8_t *cfis)
{
    uint8_t cmd = cfis[2];
    bool ext = cmd == ATA_CMD_READ_DMA_EXT || cmd == ATA_CMD_WRITE_DMA_EXT;
    bool write = cmd == ATA_CMD_WRITE_DMA || cmd == ATA_CMD_WRITE_DMA_EXT;
    uint64_t lba, count, len;
    struct iovec *iov;
    enum prdt_result r;
    bool dps;
    int n, rc;

    if (!(cfis[7] & ATA_DEV_LBA)) {
        /* ATA8-ACS 规定这一位为 1；为 0 是旧标准的 CHS 寻址，不支持 */
        warn_once(s, W_CHS, "ahci: port %d: ATA command 0x%02x with CHS addressing (device 0x%02x), aborted",
                  port_no(s, p), cmd, cfis[7]);
        abort_cmd(s, p, slot);
        return;
    }
    lba = (uint64_t)cfis[4] | (uint64_t)cfis[5] << 8 | (uint64_t)cfis[6] << 16;
    if (ext) {
        lba |= (uint64_t)cfis[8] << 24 | (uint64_t)cfis[9] << 32 | (uint64_t)cfis[10] << 40;
        count = (uint64_t)cfis[12] | (uint64_t)cfis[13] << 8;
        if (!count)
            count = 65536;
    } else {
        lba |= (uint64_t)(cfis[7] & 0xF) << 24;
        count = cfis[12] ? cfis[12] : 256;
    }
    if (lba >= s->sectors || count > s->sectors - lba) {
        vmm_dbg("ahci: port %d slot %d: cmd 0x%02x lba %llu count %llu beyond %llu sectors",
                port_no(s, p), slot, cmd, (unsigned long long)lba,
                (unsigned long long)count, (unsigned long long)s->sectors);
        complete_cmd(s, p, slot, ATA_STS_DRDY | ATA_STS_ERR, ATA_ERR_IDNF);
        return;
    }
    if (write && s->blk->read_only) {
        warn_once(s, W_READ_ONLY, "ahci: port %d: write to read-only drive %s aborted",
                  port_no(s, p), s->blk->id);
        abort_cmd(s, p, slot);
        return;
    }
    len = count * ATA_SECTOR_SIZE;
    r = prdt_map(s, p, slot, ctba, prdtl, len, &iov, &n, &dps);
    if (r != PRDT_OK) {
        prdt_fail(s, p, slot, r);
        return;
    }
    rc = disk_io(s->blk, write, iov, n, lba * ATA_SECTOR_SIZE);
    free(iov);
    /* 写缓存关着时，写命令要等数据落到持久介质才算完成 */
    if (rc == VMM_OK && write && !s->wcache)
        rc = blk_flush(s->blk);
    if (rc != VMM_OK) {
        complete_cmd(s, p, slot, ATA_STS_DRDY | ATA_STS_ERR,
                     write ? ATA_ERR_ABRT : ATA_ERR_UNC);
        return;
    }
    if (set_prdbc(s, p, slot, hdr_gpa, (uint32_t)len) != VMM_OK)
        return;
    if (dps)
        p->is |= AHCI_PxIS_DPS;
    complete_cmd(s, p, slot, ATA_STS_DRDY, 0);
}

/* SET FEATURES（EFh，第 7.48 节）。传输模式只记下来反映到 IDENTIFY 的
 * 字 63/88：SATA 链路上它不影响任何传输。选中一种 DMA 模式会取消另一类
 * DMA 模式（第 7.48.5 节），PIO 模式在 IDENTIFY 里没有“已选中”字段，
 * 只检查取值。关写缓存之前先刷一次（第 7.48.4 节）。 */
static void cmd_set_features(struct ahci *s, struct ahci_port *p, int slot, const uint8_t *cfis)
{
    uint8_t sub = cfis[3], val = cfis[12], mode = val & 7;
    bool ok;

    switch (sub) {
    case ATA_SF_WCACHE_ON:
        s->wcache = true;
        ok = true;
        break;
    case ATA_SF_WCACHE_OFF:
        ok = blk_flush(s->blk) == VMM_OK;
        if (ok)
            s->wcache = false;
        break;
    case ATA_SF_XFER_MODE:
        switch (val & 0xF8) {
        case 0:               ok = mode == 0; break;  /* 取消 IORDY（1）不支持：字 49 bit 10 为 0 */
        case ATA_XFER_PIO:    ok = mode <= 4; break;
        case ATA_XFER_MWDMA:  ok = mode <= 2; break;
        case ATA_XFER_UDMA:   ok = mode <= 6; break;
        default:              ok = false; break;
        }
        if (ok && (val & 0xF8) >= ATA_XFER_MWDMA)
            s->dma_mode = val;
        break;
    default:
        warn_once(s, W_SET_FEATURES, "ahci: port %d: SET FEATURES subcommand 0x%02x not supported, aborted",
                  port_no(s, p), sub);
        ok = false;
        break;
    }
    if (ok)
        complete_cmd(s, p, slot, ATA_STS_DRDY, 0);
    else
        abort_cmd(s, p, slot);
}

/* 把一条 ATA 命令交给盘。只实现 Linux libata 对这块盘实际会发的命令：
 * 探测时 IDENTIFY DEVICE 和 SET FEATURES（设传输模式），读写用 DMA
 * 命令（数据量小、地址在 28 位以内时用 28 位版本，否则用 EXT 版本，见
 * ata_build_rw_tf），sd 的 SYNCHRONIZE CACHE 被翻译成 FLUSH CACHE (EXT)，
 * 关机时的 START STOP UNIT 被翻译成 STANDBY IMMEDIATE。其余命令中止，
 * 第一次遇到时记下操作码。 */
static void exec_ata(struct ahci *s, struct ahci_port *p, int slot, uint64_t hdr_gpa,
                     uint32_t prdtl, uint64_t ctba, const uint8_t *cfis)
{
    uint8_t cmd = cfis[2];

    switch (cmd) {
    case ATA_CMD_IDENTIFY:
        cmd_identify(s, p, slot, hdr_gpa, prdtl, ctba);
        break;
    case ATA_CMD_READ_DMA:
    case ATA_CMD_READ_DMA_EXT:
    case ATA_CMD_WRITE_DMA:
    case ATA_CMD_WRITE_DMA_EXT:
        cmd_rw(s, p, slot, hdr_gpa, prdtl, ctba, cfis);
        break;
    case ATA_CMD_FLUSH_CACHE:
    case ATA_CMD_FLUSH_CACHE_EXT:
        if (blk_flush(s->blk) == VMM_OK)
            complete_cmd(s, p, slot, ATA_STS_DRDY, 0);
        else
            abort_cmd(s, p, slot);
        break;
    case ATA_CMD_SET_FEATURES:
        cmd_set_features(s, p, slot, cfis);
        break;
    case ATA_CMD_STANDBY_NOW:
        /* STANDBY IMMEDIATE（E0h，第 7.55 节），sd 关机时经 START STOP UNIT
         * 发来（之前已经发过 SYNCHRONIZE CACHE）。镜像文件没有可以停转的
         * 盘片，不模拟电源状态，直接完成；下一条读写命令照常执行，和真实
         * 盘在 Standby 下收到介质访问命令时自动转起来的结果一样 */
        complete_cmd(s, p, slot, ATA_STS_DRDY, 0);
        break;
    default:
        if (!(s->cmd_warned[cmd / 64] & (1ull << (cmd % 64)))) {
            s->cmd_warned[cmd / 64] |= 1ull << (cmd % 64);
            vmm_warn("ahci: port %d: ATA command 0x%02x not supported, aborted (logged once per command)",
                     port_no(s, p), cmd);
        }
        abort_cmd(s, p, slot);
        break;
    }
}

/* 执行一个槽：读命令头（第 4.2.2 节），再从命令表读 Command FIS
 * （第 4.2.3 节）交给设备。 */
static void exec_slot(struct ahci *s, struct ahci_port *p, int slot)
{
    uint64_t hdr_gpa = addr64(p->clb, p->clbu) + (uint64_t)slot * AHCI_CMDHDR_SIZE;
    uint32_t hdr[4];
    uint8_t cfis[64];
    uint32_t cfl;
    uint64_t ctba;

    p->ccs = (uint8_t)slot;
    if (pci_dma_read(&s->pci, hdr_gpa, hdr, sizeof(hdr)) != VMM_OK) {
        vmm_warn("ahci: port %d slot %d: command header gpa=0x%llx unreadable",
                 port_no(s, p), slot, (unsigned long long)hdr_gpa);
        host_bus_fatal(s, p, slot);
        return;
    }
    cfl = hdr[0] & AHCI_CH_CFL_MASK;
    ctba = addr64(hdr[2] & ~0x7Fu, hdr[3]);
    /* CFL 为 0、1 是非法值，最大 16 dword（第 4.2.2 节 DW0） */
    if (cfl < 2 || cfl > 16) {
        vmm_warn("ahci: port %d slot %d: illegal command FIS length %u dwords (header dw0=0x%08x)",
                 port_no(s, p), slot, cfl, hdr[0]);
        host_bus_fatal(s, p, slot);
        return;
    }
    if (pci_dma_read(&s->pci, ctba + AHCI_CT_CFIS, cfis, cfl * 4) != VMM_OK) {
        vmm_warn("ahci: port %d slot %d: command FIS at gpa=0x%llx (%u bytes) unreadable",
                 port_no(s, p), slot, (unsigned long long)ctba, cfl * 4);
        host_bus_fatal(s, p, slot);
        return;
    }
    if (!p->has_disk) {
        /* 链路没建立时软件不该发命令（第 10.3.1 节）。真实 HBA 会一直等
         * 设备回应，这里直接以中止结束，免得 guest 卡在超时上 */
        vmm_warn("ahci: port %d slot %d: command issued with no device attached, aborted",
                 port_no(s, p), slot);
        abort_cmd(s, p, slot);
        return;
    }
    if (cfis[0] != SATA_FIS_REG_H2D) {
        vmm_warn("ahci: port %d slot %d: command FIS type 0x%02x is not a Register H2D FIS, aborted",
                 port_no(s, p), slot, cfis[0]);
        abort_cmd(s, p, slot);
        return;
    }
    if (!(cfis[1] & SATA_FIS_H2D_C)) {
        /* C=0 是设备控制寄存器的更新，即软件复位（SRST）序列。Linux 的
         * ahci 驱动在 COMRESET 拿到签名之后不再走软件复位 */
        warn_once(s, W_SRST, "ahci: port %d: software reset (SRST) FIS not supported, aborted",
                  port_no(s, p));
        abort_cmd(s, p, slot);
        return;
    }
    exec_ata(s, p, slot, hdr_gpa, hdr[0] >> AHCI_CH_PRDTL_SHIFT, ctba, cfis);
}

/* 处理进入时 PxCI 里已发出的槽，从低号开始，每个槽执行一次。端口停下
 * （ERR:Fatal）时不再取新命令。只遍历进入时的快照而不是“PxCI 非零就
 * 继续”：后者要求每条执行路径都清掉 CI 位或停下端口，哪一处漏了，
 * 写 PxCI 的 vCPU 线程就在这里死循环。非 NCQ 的 Linux 驱动一次只发一条
 * 命令，所以槽的先后顺序不影响它。 */
static void port_run(struct ahci *s, struct ahci_port *p)
{
    uint32_t pending = p->ci;
    int slot;

    while (pending && (p->cmd & AHCI_PxCMD_ST) && !p->halted) {
        slot = __builtin_ctz(pending);
        pending &= pending - 1;
        exec_slot(s, p, slot);
    }
}

/* ---- 寄存器 ---- */

static uint32_t port_read(struct ahci *s, struct ahci_port *p, uint32_t reg)
{
    uint32_t v;

    (void)s;
    switch (reg) {
    case AHCI_PxCLB:   return p->clb;
    case AHCI_PxCLBU:  return p->clbu;
    case AHCI_PxFB:    return p->fb;
    case AHCI_PxFBU:   return p->fbu;
    case AHCI_PxIS:    return port_is(p);
    case AHCI_PxIE:    return p->ie;
    case AHCI_PxCMD:
        /* POD、SUD 在不支持冷插拔检测、交错上电时只读为 1 */
        v = p->cmd | AHCI_PxCMD_POD | AHCI_PxCMD_SUD;
        if (p->cmd & AHCI_PxCMD_FRE)
            v |= AHCI_PxCMD_FR;
        if ((p->cmd & AHCI_PxCMD_ST) && !p->halted)
            v |= AHCI_PxCMD_CR;
        if (p->cmd & AHCI_PxCMD_ST)
            v |= (uint32_t)p->ccs << AHCI_PxCMD_CCS_SHIFT;
        return v;
    case AHCI_PxTFD:   return p->tfd;
    case AHCI_PxSIG:   return p->sig;
    case AHCI_PxSSTS:  return p->ssts;
    case AHCI_PxSCTL:  return p->sctl;
    case AHCI_PxSERR:  return p->serr;
    case AHCI_PxSACT:  return p->sact;
    case AHCI_PxCI:    return p->ci;
    default:           return 0;      /* 保留、厂商定义、未支持的功能 */
    }
}

static void write_cmd(struct ahci *s, struct ahci_port *p, uint32_t val)
{
    uint32_t old = p->cmd;

    p->cmd = val & PxCMD_RW_MASK;
    if (val & AHCI_PxCMD_CLO)
        p->tfd &= ~(uint32_t)(ATA_STS_BSY | ATA_STS_DRQ);
    if ((old & AHCI_PxCMD_ST) && !(val & AHCI_PxCMD_ST)) {
        /* 第 3.3.7 节：ST 1→0 时 HBA 清 PxCI、PxSACT、CCS，回到空闲 */
        p->ci = 0;
        p->sact = 0;
        p->ccs = 0;
        p->halted = false;
    } else if (!(old & AHCI_PxCMD_ST) && (val & AHCI_PxCMD_ST)) {
        p->ccs = 0;
        p->halted = false;
    }
}

static void port_write(struct ahci *s, struct ahci_port *p, uint32_t reg, uint32_t val)
{
    switch (reg) {
    case AHCI_PxCLB:   p->clb = val & ~0x3FFu; break;   /* 1 KiB 对齐 */
    case AHCI_PxCLBU:  p->clbu = val; break;
    case AHCI_PxFB:    p->fb = val & ~0xFFu; break;     /* 256 字节对齐 */
    case AHCI_PxFBU:   p->fbu = val; break;
    case AHCI_PxIS:
        p->is &= ~(val & PxIS_RWC_MASK);
        update_irq(s);
        break;
    case AHCI_PxIE:
        p->ie = val & PxIE_MASK;
        update_irq(s);
        break;
    case AHCI_PxCMD:
        write_cmd(s, p, val);
        update_irq(s);
        break;
    case AHCI_PxSCTL:
        write_sctl(s, p, val);
        break;
    case AHCI_PxSERR:
        p->serr &= ~val;
        update_irq(s);
        break;
    case AHCI_PxSACT:
        if (p->cmd & AHCI_PxCMD_ST)
            p->sact |= val;
        break;
    case AHCI_PxCI:
        if (!(p->cmd & AHCI_PxCMD_ST)) {
            /* 第 3.3.14 节：只能在 ST=1 时置位 */
            warn_once(s, W_CI_STOPPED, "ahci: port %d: PxCI write 0x%08x while PxCMD.ST=0 ignored",
                      port_no(s, p), val);
            break;
        }
        p->ci |= val;
        port_run(s, p);
        break;
    default:
        /* 只读寄存器（TFD、SIG、SSTS）和未支持的功能（SNTF、FBS、DEVSLP）：
         * 写入不改变任何状态 */
        break;
    }
}

uint32_t ahci_reg_read(struct ahci *s, uint32_t off)
{
    struct ahci_port *p;
    uint32_t reg;

    off &= ~3u;
    p = port_at(s, off, &reg);
    if (p)
        return port_read(s, p, reg);
    switch (off) {
    case AHCI_CAP:  return AHCI_CAP_VALUE;
    case AHCI_GHC:  return AHCI_GHC_AE | s->ghc;
    case AHCI_IS:   return s->is;
    case AHCI_PI:   return AHCI_PI_VALUE;
    case AHCI_VS:   return AHCI_VS_1_3_1;
    default:        return 0;   /* CAP2 为 0；CCC、EM、BOHC 未支持；保留区 */
    }
}

void ahci_reg_write(struct ahci *s, uint32_t off, uint32_t val)
{
    struct ahci_port *p;
    uint32_t reg;

    off &= ~3u;
    p = port_at(s, off, &reg);
    if (p) {
        port_write(s, p, reg, val);
        return;
    }
    switch (off) {
    case AHCI_GHC:
        if (val & AHCI_GHC_HR) {
            hba_reset(s);          /* 同步完成，HR 读回 0 */
            return;
        }
        s->ghc = val & AHCI_GHC_IE;
        update_irq(s);
        break;
    case AHCI_IS:
        s->is &= ~val;
        update_irq(s);
        break;
    default:
        /* CAP、PI、CAP2 是 HwInit：Linux 复位后把读到的值写回去
         * （ahci_restore_initial_config），写入没有效果 */
        break;
    }
}

/* ABAR 访问。规范允许最大 64 位、不跨 8 字节边界的访问（第 3 节）：
 * 64 位按两个 dword 处理；小于 32 位的读返回寄存器里对应的字节，
 * 小于 32 位的写没有意义（寄存器的位跨字节），忽略并记一次 warn。 */
static int abar_read(void *opaque, uint64_t off, uint32_t size, void *data)
{
    struct ahci *s = opaque;
    uint32_t sh = (uint32_t)(off & 3);
    uint32_t v[2] = { 0, 0 };

    if (size == 8 && !(off & 7)) {
        v[0] = ahci_reg_read(s, (uint32_t)off);
        v[1] = ahci_reg_read(s, (uint32_t)off + 4);
    } else if (size <= 4 && sh + size <= 4) {
        v[0] = ahci_reg_read(s, (uint32_t)off) >> (8 * sh);
    }
    memcpy(data, v, size <= 8 ? size : 8);
    return VMM_OK;
}

static int abar_write(void *opaque, uint64_t off, uint32_t size, const void *data)
{
    struct ahci *s = opaque;
    uint32_t v[2];

    if (size == 4 && !(off & 3)) {
        memcpy(v, data, 4);
        ahci_reg_write(s, (uint32_t)off, v[0]);
    } else if (size == 8 && !(off & 7)) {
        memcpy(v, data, 8);
        ahci_reg_write(s, (uint32_t)off, v[0]);
        ahci_reg_write(s, (uint32_t)off + 4, v[1]);
    } else {
        warn_once(s, W_ACCESS_SIZE, "ahci: %u-byte write at ABAR+0x%03llx ignored (registers take 32-bit writes)",
                  size, (unsigned long long)off);
    }
    return VMM_OK;
}

/* ---- 设备类型 ---- */

void ahci_init(struct ahci *s, struct blockdev *blk)
{
    struct pci_device *p = &s->pci;
    int i;

    s->blk = blk;
    s->sectors = blk ? blk->size / ATA_SECTOR_SIZE : 0;
    p->name       = "ahci";
    p->vendor_id  = PCI_VENDOR_ID_MINIVMM;
    p->device_id  = PCI_DEVICE_ID_AHCI;
    p->class_code = AHCI_CLASS_CODE;
    p->revision   = 1;
    p->intx_pin   = 1;
    p->bus_master = true;
    p->bars[AHCI_ABAR] = (struct pci_bar){ .type = PCI_BAR_MEM32, .size = AHCI_ABAR_SIZE,
                                           .opaque = s, .read = abar_read, .write = abar_write };
    for (i = 0; i < AHCI_NR_PORTS; i++) {
        memset(&s->port[i], 0, sizeof(s->port[i]));
        s->port[i].has_disk = (i == 0 && blk != NULL);
    }
    hba_reset(s);
}

static const char *const ahci_props[] = { "driver", "drive", NULL };

static int ahci_create(const struct opts *o, void **dev)
{
    struct ahci *s;

    (void)o;
    s = calloc(1, sizeof(*s));
    if (!s)
        return VMM_ERR_NOMEM;
    *dev = s;
    return VMM_OK;
}

static int ahci_attach(const struct vmm_device *d, struct vmm_vm *vm, struct pci_bus *bus)
{
    struct ahci *s = d->dev;

    s->vm = vm;
    ahci_init(s, d->blk);
    if (d->blk) {
        vmm_info("ahci: port 0 drive %s (%llu sectors%s)", d->blk->id,
                 (unsigned long long)s->sectors, d->blk->read_only ? ", read-only" : "");
        if (d->blk->size % ATA_SECTOR_SIZE)
            vmm_warn("ahci: drive %s: size %llu is not a multiple of %d, the last %llu bytes are not addressable",
                     d->blk->id, (unsigned long long)d->blk->size, ATA_SECTOR_SIZE,
                     (unsigned long long)(d->blk->size % ATA_SECTOR_SIZE));
    }
    else
        vmm_info("ahci: no drive, all %d ports empty", AHCI_NR_PORTS);
    return pci_register_device(bus, &s->pci);
}

const struct vmm_device_type ahci_device_type = {
    .name   = "ahci",
    .desc   = "AHCI SATA controller, 6 ports; drive= attaches a disk to port 0",
    .props  = ahci_props,
    .create = ahci_create,
    .attach = ahci_attach,
};
