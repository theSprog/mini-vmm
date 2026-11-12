/* src/devices/ahci.c — AHCI SATA 控制器，设计说明见 devices/ahci.h */
#include <stdlib.h>
#include <string.h>

#include "vmm.h"
#include "opts.h"
#include "block/blockdev.h"
#include "devices/ahci.h"

enum {
    W_ACCESS_SIZE,
    W_CI_STOPPED,
    W_SRST,
    W_ATA_CMD,
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
static void host_bus_fatal(struct ahci *s, struct ahci_port *p, int slot)
{
    p->is |= AHCI_PxIS_HBFS;
    p->serr |= AHCI_SERR_ERR_E;
    p->halted = true;
    if (slot >= 0)
        p->ccs = (uint8_t)slot;
    update_irq(s);
}

/* ---- 链路与复位 ---- */

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
    warn_once(s, W_ATA_CMD, "ahci: port %d: ATA command 0x%02x not supported, aborted",
              port_no(s, p), cfis[2]);
    abort_cmd(s, p, slot);
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
    if (d->blk)
        vmm_info("ahci: port 0 drive %s (%llu bytes%s)", d->blk->id,
                 (unsigned long long)d->blk->size, d->blk->read_only ? ", read-only" : "");
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
