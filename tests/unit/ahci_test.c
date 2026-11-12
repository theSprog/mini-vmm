/* tests/unit/ahci_test.c — AHCI HBA 设备模型
 *
 * 链接真实的 src/devices/ahci.c，外部依赖全换成桩：PCI 的 DMA 落在一块
 * 假的 guest 内存上，INTx 电平记录下来。寄存器按 Linux libahci 的访问
 * 顺序驱动，命令列表、命令表、FIS 接收区由测试按规范的布局在假内存里
 * 摆好。
 *
 * 判据来源，都不来自被测代码本身：
 *   - 寄存器复位值、读写类型（RO/RW/RWC/RW1）：AHCI 1.3.1 第 3 章
 *   - 复位：第 10.4.2 节（DET=1 时 PxTFD.STS=7Fh、PxSSTS.DET=0），
 *     第 10.4.3 节（GHC.HR 保留 PxCLB/PxFB，清其余端口寄存器）
 *   - 复位签名：ATA8-ACS 表 92（Count 01h、LBA 000001h），PxSIG 的字节
 *     顺序见第 3.3.9 节；Linux 的 ata_dev_classify 把它认成 ATA 磁盘
 *   - 两级中断：第 10.7.2.1 节（虚拟线、IS 粘滞、先清 PxIS 再清 IS）
 *   - 命令出错：第 6.1.4 节（TFES 后停下、PxCI 保留）、第 6.2.2 节
 *     （ST 1→0 清 PxCI 后恢复）
 *   - FIS 布局：SATA 3.0 第 10.3.4/10.3.5 节，接收区偏移第 4.2.1 节
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmm.h"
#include "opts.h"
#include "block/blockdev.h"
#include "devices/ahci.h"
#include "harness.h"

/* ------------------------------------------------------------------ */
/* 桩                                                                  */
/* ------------------------------------------------------------------ */

int vmm_log_level = -1;
static char g_last_log[512];

void vmm_log(int level, const char *file, int line, const char *fmt, ...)
{
    va_list ap;

    (void)file; (void)line;
    va_start(ap, fmt);
    vsnprintf(g_last_log, sizeof(g_last_log), fmt, ap);
    va_end(ap);
    if (level <= vmm_log_level)
        fprintf(stderr, "       [vmm] %s\n", g_last_log);
}

#define RAM_SIZE  (1u << 20)
static uint8_t g_ram[RAM_SIZE];
static bool    g_master = true;
static unsigned g_dma_writes;

int pci_dma_read(struct pci_device *dev, uint64_t gpa, void *dst, size_t len)
{
    (void)dev;
    if (!g_master || gpa > RAM_SIZE || len > RAM_SIZE - gpa)
        return VMM_ERR_GUEST;
    memcpy(dst, g_ram + gpa, len);
    return VMM_OK;
}

int pci_dma_write(struct pci_device *dev, uint64_t gpa, const void *src, size_t len)
{
    (void)dev;
    if (!g_master || gpa > RAM_SIZE || len > RAM_SIZE - gpa)
        return VMM_ERR_GUEST;
    memcpy(g_ram + gpa, src, len);
    g_dma_writes++;
    return VMM_OK;
}

static bool     g_irq;
static unsigned g_irq_edges;

void pci_set_irq(struct pci_device *dev, bool level)
{
    (void)dev;
    if (level && !g_irq)
        g_irq_edges++;
    g_irq = level;
}

static struct pci_device *g_registered;

int pci_register_device(struct pci_bus *bus, struct pci_device *dev)
{
    (void)bus;
    g_registered = dev;
    return VMM_OK;
}

/* ------------------------------------------------------------------ */
/* 夹具                                                                */
/* ------------------------------------------------------------------ */

static struct vmm_vm g_vm;
static struct blockdev g_disk;
static struct ahci *S;

/* 每个用例一台新控制器，经 --device 的同一条路径创建和挂载 */
static void fresh_with(struct blockdev *blk)
{
    struct opts o;
    struct vmm_device d = { 0 };
    void *dev = NULL;

    free(S);
    memset(g_ram, 0, sizeof(g_ram));
    g_master = true;
    g_irq = false;
    g_irq_edges = 0;
    g_dma_writes = 0;
    g_registered = NULL;
    if (opts_parse(&o, "--device", blk ? "ahci,drive=d0" : "ahci", "driver") != VMM_OK ||
        ahci_device_type.create(&o, &dev) != VMM_OK) {
        printf("fixture: create failed: %s\n", g_last_log);
        exit(1);
    }
    opts_free(&o);
    d.type = &ahci_device_type;
    d.dev = dev;
    d.blk = blk;
    if (ahci_device_type.attach(&d, &g_vm, NULL) != VMM_OK) {
        printf("fixture: attach failed: %s\n", g_last_log);
        exit(1);
    }
    S = dev;
}

static void fresh(void) { fresh_with(&g_disk); }

static uint32_t rd(uint32_t off) { return ahci_reg_read(S, off); }
static void wr(uint32_t off, uint32_t v) { ahci_reg_write(S, off, v); }
static uint32_t prd(int port, uint32_t reg) { return rd(AHCI_PORT_BASE + (uint32_t)port * AHCI_PORT_SIZE + reg); }
static void pwr(int port, uint32_t reg, uint32_t v) { wr(AHCI_PORT_BASE + (uint32_t)port * AHCI_PORT_SIZE + reg, v); }

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* 假内存布局：命令列表 1 KiB 对齐、FIS 接收区 256 字节对齐、命令表 128 字节对齐 */
#define CLB   0x1000u
#define FB    0x2000u
#define CTBA  0x3000u

/* 按 Linux 的顺序把端口 0 准备到能发命令：清 SERR/PxIS/IS，设 CLB/FB，
 * FRE，打开中断，ST（ahci_port_init、ahci_start_fis_rx、ahci_thaw、
 * ahci_start_engine） */
static void port0_start(void)
{
    pwr(0, AHCI_PxSERR, 0xFFFFFFFFu);
    pwr(0, AHCI_PxIS, 0xFFFFFFFFu);
    wr(AHCI_IS, 0xFFFFFFFFu);
    pwr(0, AHCI_PxCLB, CLB);
    pwr(0, AHCI_PxCLBU, 0);
    pwr(0, AHCI_PxFB, FB);
    pwr(0, AHCI_PxFBU, 0);
    pwr(0, AHCI_PxCMD, prd(0, AHCI_PxCMD) | AHCI_PxCMD_FRE);
    pwr(0, AHCI_PxIE, 0x7DC0007Fu);   /* Linux 的 DEF_PORT_IRQ 加错误位都在可写范围内 */
    wr(AHCI_GHC, rd(AHCI_GHC) | AHCI_GHC_IE);
    pwr(0, AHCI_PxCMD, prd(0, AHCI_PxCMD) | AHCI_PxCMD_ST);
}

/* 在槽 slot 摆一条 H2D Register FIS 命令 */
static void put_cmd(int slot, uint32_t cfl, uint8_t fis_type, uint8_t flags, uint8_t command)
{
    uint8_t *h = g_ram + CLB + (uint32_t)slot * AHCI_CMDHDR_SIZE;
    uint8_t *t = g_ram + CTBA;

    memset(h, 0, AHCI_CMDHDR_SIZE);
    put32(h, cfl);
    put32(h + 8, CTBA);
    memset(t, 0, 0x80);
    t[0] = fis_type;
    t[1] = flags;
    t[2] = command;
}

/* ------------------------------------------------------------------ */
/* 用例                                                                */
/* ------------------------------------------------------------------ */

static void case_identity(void)
{
    uint32_t cap;

    T_CASE("PCI 外观和 HBA 能力寄存器");
    fresh();
    CHECK_PTR(g_registered, &S->pci, "attach registers the PCI function");
    CHECK_U64(S->pci.class_code, 0x010601, "class mass storage / SATA / AHCI 1.0");
    CHECK_U64(S->pci.bars[5].type, PCI_BAR_MEM32, "ABAR is BAR5, 32-bit memory");
    CHECK_U64(S->pci.bars[5].size, 0x2000, "ABAR covers the 32-port register map");
    CHECK(S->pci.intx_pin == 1 && S->pci.bus_master, "INTA, bus master");

    cap = rd(AHCI_CAP);
    CHECK_U64(cap & 0x1F, 5, "CAP.NP: 6 ports (0-based)");
    CHECK_U64((cap >> 8) & 0x1F, 31, "CAP.NCS: 32 slots (0-based)");
    CHECK_U64((cap >> 20) & 0xF, 3, "CAP.ISS: Gen3");
    CHECK(cap & (1u << 31), "CAP.S64A");
    CHECK(cap & (1u << 18), "CAP.SAM: AHCI only");
    CHECK(!(cap & (1u << 30)), "CAP.SNCQ clear: no NCQ");
    CHECK(!(cap & (1u << 17)), "CAP.SPM clear: no port multiplier");
    CHECK(!(cap & (3u << 13)) && !(cap & (1u << 26)), "no partial/slumber, no ALPM");
    CHECK(!(cap & (1u << 27)), "CAP.SSS clear: ports spin up on reset");
    CHECK_U64(rd(AHCI_PI), 0x3F, "PI: ports 0-5");
    CHECK_U64(rd(AHCI_VS), 0x00010301, "VS 1.3.1");
    CHECK_U64(rd(AHCI_CAP2), 0, "CAP2: nothing extended");

    /* CAP.SAM=1 时 GHC.AE 只读为 1（第 3.1.2 节）；HwInit 寄存器写不进去 */
    CHECK_U64(rd(AHCI_GHC), AHCI_GHC_AE, "GHC after reset: AE only");
    wr(AHCI_GHC, 0);
    CHECK_U64(rd(AHCI_GHC), AHCI_GHC_AE, "AE cannot be cleared");
    wr(AHCI_CAP, 0);
    wr(AHCI_PI, 1);
    CHECK_U64(rd(AHCI_CAP), cap, "CAP write ignored (Linux writes saved_cap back)");
    CHECK_U64(rd(AHCI_PI), 0x3F, "PI write ignored");
}

static void case_power_on(void)
{
    T_CASE("上电：有盘的端口链路建立并带复位签名，空端口停在 DET=0");
    fresh();
    /* SStatus 0x133：DET=3、SPD=Gen3、IPM=active。Linux 打印成
     * "SATA link up 6.0 Gbps (SStatus 133 ...)" */
    CHECK_U64(prd(0, AHCI_PxSSTS), 0x133, "port 0 link up");
    CHECK_U64(prd(0, AHCI_PxSIG), 0x00000101, "ATA disk signature");
    CHECK_U64(prd(0, AHCI_PxTFD), 0x0140, "status DRDY, error = diagnostic code 01h");
    CHECK_U64(prd(0, AHCI_PxSERR), AHCI_SERR_DIAG_X | AHCI_SERR_DIAG_N, "COMINIT and PhyRdy change latched");
    CHECK_U64(prd(0, AHCI_PxIS), AHCI_PxIS_PCS | AHCI_PxIS_PRCS, "PCS/PRCS mirror PxSERR");
    CHECK_U64(prd(0, AHCI_PxCMD), AHCI_PxCMD_POD | AHCI_PxCMD_SUD, "PxCMD: only the read-only POD/SUD");
    CHECK_U64(g_dma_writes, 0, "FRE=0: the signature FIS is not posted to memory");
    CHECK(!g_irq, "GHC.IE=0: no interrupt");

    CHECK_U64(prd(1, AHCI_PxSSTS), 0, "port 1: no device detected");
    CHECK_U64(prd(1, AHCI_PxSIG), 0xFFFFFFFFu, "port 1: signature never received");
    CHECK_U64(prd(1, AHCI_PxTFD), 0x7F, "port 1: TFD reset value");
    CHECK_U64(prd(1, AHCI_PxSERR), 0, "port 1: no COMINIT");

    fresh_with(NULL);
    CHECK_U64(prd(0, AHCI_PxSSTS), 0, "no drive=: port 0 empty too");
}

static void case_comreset(void)
{
    const uint8_t *f = g_ram + FB + AHCI_RFIS_RFIS;

    T_CASE("COMRESET：DET=1 断链，DET=0 重建并收到签名 FIS");
    fresh();
    pwr(0, AHCI_PxSERR, 0xFFFFFFFFu);
    pwr(0, AHCI_PxFB, FB);
    pwr(0, AHCI_PxCMD, AHCI_PxCMD_FRE);
    memset(g_ram + FB, 0xAA, AHCI_RFIS_SIZE);

    /* sata_link_hardreset：SControl 0x301，至少 1 ms，再写 0x300 */
    pwr(0, AHCI_PxSCTL, 0x301);
    CHECK_U64(prd(0, AHCI_PxSSTS), 0, "DET=1: link down");
    CHECK_U64(prd(0, AHCI_PxTFD), 0x7F, "DET=1: TFD.STS = 7Fh");
    CHECK_U64(prd(0, AHCI_PxSERR), AHCI_SERR_DIAG_N, "PhyRdy dropped");
    pwr(0, AHCI_PxSCTL, 0x300);
    CHECK_U64(prd(0, AHCI_PxSCTL), 0x300, "SControl reads back");
    CHECK_U64(prd(0, AHCI_PxSSTS), 0x133, "DET=0: link up again");
    CHECK_U64(prd(0, AHCI_PxSIG), 0x101, "signature");
    CHECK_U64(prd(0, AHCI_PxTFD) & ATA_STS_BSY, 0, "BSY clear: ahci_check_ready succeeds");
    CHECK_U64(f[0], 0x34, "D2H Register FIS at FB+0x40");
    CHECK_U64(f[1], 0, "I bit clear");
    CHECK_U64(f[2], ATA_STS_DRDY, "FIS status");
    CHECK_U64(f[3], 0x01, "FIS error = diagnostic code");
    CHECK(f[4] == 0x01 && f[5] == 0 && f[6] == 0 && f[12] == 0x01, "FIS LBA 000001h, count 01h");
    CHECK_U64(f[20], 0xAA, "only the 20-byte FIS is written");
    CHECK(!(prd(0, AHCI_PxIS) & AHCI_PxIS_DHRS), "signature FIS does not set DHRS");

    pwr(0, AHCI_PxSCTL, 0x301);
    pwr(0, AHCI_PxSCTL, 0x300);
    CHECK_U64(prd(0, AHCI_PxSSTS), 0x133, "rewriting DET=0 is a no-op");

    /* SPD 限速：sata_set_spd 降速后再 hardreset */
    pwr(0, AHCI_PxSCTL, 0x311);
    pwr(0, AHCI_PxSCTL, 0x310);
    CHECK_U64(prd(0, AHCI_PxSSTS), 0x113, "SPD limit 1: Gen1 negotiated");

    pwr(0, AHCI_PxSCTL, 0x304);
    CHECK_U64(prd(0, AHCI_PxSSTS), 4, "DET=4: Phy offline");

    pwr(2, AHCI_PxSCTL, 0x301);
    pwr(2, AHCI_PxSCTL, 0x300);
    CHECK_U64(prd(2, AHCI_PxSSTS), 0, "empty port stays at DET=0");
    CHECK_U64(prd(2, AHCI_PxTFD), 0x7F, "empty port TFD 7Fh");
    CHECK_U64(prd(2, AHCI_PxSERR), 0, "empty port: no COMINIT, no PhyRdy change");
}

static void case_hba_reset(void)
{
    T_CASE("GHC.HR：保留 CLB/FB，清其余端口状态，重新 COMRESET");
    fresh();
    port0_start();
    pwr(0, AHCI_PxCLBU, 0x1);
    pwr(0, AHCI_PxSCTL, 0x310);
    wr(AHCI_GHC, AHCI_GHC_HR | AHCI_GHC_IE);
    CHECK_U64(rd(AHCI_GHC), AHCI_GHC_AE, "HR self-clears, IE reset");
    CHECK_U64(rd(AHCI_IS), 0, "IS reset");
    CHECK_U64(prd(0, AHCI_PxCLB), CLB, "PxCLB kept");
    CHECK_U64(prd(0, AHCI_PxCLBU), 1, "PxCLBU kept");
    CHECK_U64(prd(0, AHCI_PxFB), FB, "PxFB kept");
    CHECK_U64(prd(0, AHCI_PxIE), 0, "PxIE reset");
    CHECK_U64(prd(0, AHCI_PxCMD), AHCI_PxCMD_POD | AHCI_PxCMD_SUD, "ST/FRE reset");
    CHECK_U64(prd(0, AHCI_PxSCTL), 0, "PxSCTL reset (SPD limit gone)");
    CHECK_U64(prd(0, AHCI_PxSSTS), 0x133, "COMRESET after HBA reset: link up at Gen3");
    CHECK(!g_irq, "no interrupt after reset");
}

static void case_irq(void)
{
    T_CASE("两级中断：虚拟线置 IS，先清 PxIS 再清 IS");
    fresh();
    port0_start();
    CHECK(!g_irq, "clean start");

    /* 用 PRCS（PhyRdy 变化）做中断源：COMRESET 置 PxSERR.DIAG.N */
    pwr(0, AHCI_PxSCTL, 0x301);
    CHECK(g_irq, "PRCS & PRCE -> IS[0] -> INTx");
    CHECK_U64(rd(AHCI_IS), 1, "IS bit 0");
    wr(AHCI_IS, 1);
    CHECK_U64(rd(AHCI_IS), 1, "IS cleared while the wire is still high: set again");
    CHECK(g_irq, "line stays asserted");
    pwr(0, AHCI_PxSERR, AHCI_SERR_DIAG_N);
    CHECK(!(prd(0, AHCI_PxIS) & AHCI_PxIS_PRCS), "PRCS clears with PxSERR.DIAG.N");
    CHECK_U64(rd(AHCI_IS), 1, "IS is sticky");
    CHECK(g_irq, "still asserted until IS is cleared");
    wr(AHCI_IS, 1);
    CHECK_U64(rd(AHCI_IS), 0, "IS clear");
    CHECK(!g_irq, "line deasserted");

    /* PxIE 屏蔽：状态照样置，IS 不置 */
    pwr(0, AHCI_PxIE, 0);
    pwr(0, AHCI_PxSCTL, 0x300);
    CHECK(prd(0, AHCI_PxIS) & AHCI_PxIS_PRCS, "status set while masked");
    CHECK_U64(rd(AHCI_IS), 0, "masked: IS not set");
    CHECK(!g_irq, "masked: no interrupt");
    pwr(0, AHCI_PxIE, AHCI_PxIS_PRCS);
    CHECK(g_irq, "unmasking a pending cause asserts the line");

    /* GHC.IE 屏蔽线，不屏蔽 IS */
    wr(AHCI_GHC, 0);
    CHECK(!g_irq, "GHC.IE=0 masks the line");
    CHECK_U64(rd(AHCI_IS), 1, "IS still records the port");
    wr(AHCI_GHC, AHCI_GHC_IE);
    CHECK(g_irq, "GHC.IE=1 asserts it again");
}

static void case_cmd_abort(void)
{
    const uint8_t *f = g_ram + FB + AHCI_RFIS_RFIS;

    T_CASE("命令路径：取命令头和 CFIS，设备中止，TFES 后端口停下");
    fresh();
    port0_start();
    CHECK_U64(prd(0, AHCI_PxCMD) & (AHCI_PxCMD_CR | AHCI_PxCMD_FR),
              AHCI_PxCMD_CR | AHCI_PxCMD_FR, "ST/FRE -> CR/FR");
    put_cmd(3, 5, SATA_FIS_REG_H2D, SATA_FIS_H2D_C, 0xEC);   /* IDENTIFY DEVICE */
    pwr(0, AHCI_PxCI, 1u << 3);
    CHECK_U64(prd(0, AHCI_PxTFD), 0x0441, "TFD: ABRT, DRDY|ERR");
    CHECK(f[0] == 0x34 && (f[1] & 0x40) && f[2] == 0x41 && f[3] == 0x04,
          "D2H FIS with I bit, status 41h, error 04h");
    CHECK_U64(prd(0, AHCI_PxIS) & (AHCI_PxIS_TFES | AHCI_PxIS_DHRS),
              AHCI_PxIS_TFES | AHCI_PxIS_DHRS, "TFES and DHRS");
    CHECK(g_irq, "interrupt");
    CHECK_U64(prd(0, AHCI_PxCI), 1u << 3, "failed slot stays in PxCI");
    CHECK_U64(prd(0, AHCI_PxCMD) & AHCI_PxCMD_CR, 0, "ERR:Fatal clears CR");
    CHECK_U64((prd(0, AHCI_PxCMD) & AHCI_PxCMD_CCS_MASK) >> AHCI_PxCMD_CCS_SHIFT, 3, "CCS = failed slot");

    put_cmd(4, 5, SATA_FIS_REG_H2D, SATA_FIS_H2D_C, 0xEC);
    memset(g_ram + FB, 0, AHCI_RFIS_SIZE);
    pwr(0, AHCI_PxCI, 1u << 4);
    CHECK_U64(prd(0, AHCI_PxCI), (1u << 3) | (1u << 4), "halted port does not run new slots");
    CHECK_U64(f[0], 0, "nothing posted while halted");

    /* 第 6.2.2.1 节的恢复：清 ST（PxCI 随之清空）、清状态、再置 ST */
    pwr(0, AHCI_PxCMD, prd(0, AHCI_PxCMD) & ~AHCI_PxCMD_ST);
    CHECK_U64(prd(0, AHCI_PxCI), 0, "ST 1->0 clears PxCI");
    CHECK_U64(prd(0, AHCI_PxCMD) & (AHCI_PxCMD_CR | AHCI_PxCMD_CCS_MASK), 0, "CR and CCS clear");
    pwr(0, AHCI_PxIS, 0xFFFFFFFFu);
    wr(AHCI_IS, 1);
    CHECK(!g_irq, "interrupt cleared");
    pwr(0, AHCI_PxCMD, prd(0, AHCI_PxCMD) | AHCI_PxCMD_ST);
    CHECK(prd(0, AHCI_PxCMD) & AHCI_PxCMD_CR, "running again");
    pwr(0, AHCI_PxCI, 1u << 4);
    CHECK_U64(prd(0, AHCI_PxTFD), 0x0441, "next command reaches the device");

    T_CASE("命令路径：PxCI 只在 ST=1 时生效，非命令 FIS 也被中止");
    fresh();
    port0_start();
    pwr(0, AHCI_PxCMD, prd(0, AHCI_PxCMD) & ~AHCI_PxCMD_ST);
    put_cmd(0, 5, SATA_FIS_REG_H2D, SATA_FIS_H2D_C, 0xEC);
    pwr(0, AHCI_PxCI, 1);
    CHECK_U64(prd(0, AHCI_PxCI), 0, "PxCI write with ST=0 ignored");
    CHECK_U64(prd(0, AHCI_PxIS) & AHCI_PxIS_DHRS, 0, "nothing executed");
    pwr(0, AHCI_PxCMD, prd(0, AHCI_PxCMD) | AHCI_PxCMD_ST);
    put_cmd(0, 5, SATA_FIS_REG_H2D, 0, 0);        /* C=0：设备控制（SRST） */
    pwr(0, AHCI_PxCI, 1);
    CHECK(prd(0, AHCI_PxIS) & AHCI_PxIS_TFES, "SRST FIS aborted");
}

static void case_cmd_bus_errors(void)
{
    T_CASE("命令路径：访问 guest 内存失败按主机总线致命错误处理");
    fresh();
    port0_start();
    pwr(0, AHCI_PxCLB, 0xFFFFFC00u);               /* 命令列表在 RAM 外 */
    pwr(0, AHCI_PxCI, 1);
    CHECK(prd(0, AHCI_PxIS) & AHCI_PxIS_HBFS, "HBFS");
    CHECK(prd(0, AHCI_PxSERR) & AHCI_SERR_ERR_E, "PxSERR.ERR.E");
    CHECK_U64(prd(0, AHCI_PxCI), 1, "slot left in PxCI");
    CHECK_U64(prd(0, AHCI_PxCMD) & AHCI_PxCMD_CR, 0, "halted");
    CHECK(g_irq, "HBFE enabled: interrupt");

    fresh();
    port0_start();
    put_cmd(0, 1, SATA_FIS_REG_H2D, SATA_FIS_H2D_C, 0xEC);   /* CFL=1 非法 */
    pwr(0, AHCI_PxCI, 1);
    CHECK(prd(0, AHCI_PxIS) & AHCI_PxIS_HBFS, "illegal CFL -> HBFS");

    fresh();
    port0_start();
    put_cmd(0, 5, SATA_FIS_REG_H2D, SATA_FIS_H2D_C, 0xEC);
    put32(g_ram + CLB + 8, 0xFFFFFF80u);                       /* 命令表在 RAM 外 */
    pwr(0, AHCI_PxCI, 1);
    CHECK(prd(0, AHCI_PxIS) & AHCI_PxIS_HBFS, "unreadable command table -> HBFS");

    fresh();
    port0_start();
    put_cmd(0, 5, SATA_FIS_REG_H2D, SATA_FIS_H2D_C, 0xEC);
    pwr(0, AHCI_PxCMD, prd(0, AHCI_PxCMD) & ~AHCI_PxCMD_ST);
    pwr(0, AHCI_PxCMD, prd(0, AHCI_PxCMD) & ~AHCI_PxCMD_FRE);
    pwr(0, AHCI_PxFB, 0xFFFFFF00u);                             /* FIS 接收区在 RAM 外 */
    pwr(0, AHCI_PxCMD, prd(0, AHCI_PxCMD) | AHCI_PxCMD_FRE | AHCI_PxCMD_ST);
    pwr(0, AHCI_PxCI, 1);
    CHECK(prd(0, AHCI_PxIS) & AHCI_PxIS_HBFS, "unwritable FIS area -> HBFS");
    CHECK_U64(prd(0, AHCI_PxCI), 1, "slot not completed");

    fresh();
    port0_start();
    put_cmd(0, 5, SATA_FIS_REG_H2D, SATA_FIS_H2D_C, 0xEC);
    g_master = false;                                           /* Bus Master 关闭 */
    pwr(0, AHCI_PxCI, 1);
    CHECK(prd(0, AHCI_PxIS) & AHCI_PxIS_HBFS, "DMA refused by the PCI core -> HBFS");
}

static void case_register_bits(void)
{
    uint8_t b;
    uint64_t q;

    T_CASE("寄存器位：对齐掩码、RW1、ICC、CLO、访问宽度");
    fresh();
    pwr(0, AHCI_PxCLB, 0xFFFFFFFFu);
    CHECK_U64(prd(0, AHCI_PxCLB), 0xFFFFFC00u, "PxCLB 1 KiB aligned");
    pwr(0, AHCI_PxFB, 0xFFFFFFFFu);
    CHECK_U64(prd(0, AHCI_PxFB), 0xFFFFFF00u, "PxFB 256-byte aligned");
    pwr(0, AHCI_PxIE, 0xFFFFFFFFu);
    CHECK_U64(prd(0, AHCI_PxIE), 0x7DC0007Fu, "PxIE: CPDE, DMPE and reserved bits read 0");

    pwr(0, AHCI_PxCMD, 0x10000000u | AHCI_PxCMD_ATAPI);   /* ICC = Active */
    CHECK_U64(prd(0, AHCI_PxCMD) & AHCI_PxCMD_ICC_MASK, 0, "ICC returns to idle");
    CHECK(prd(0, AHCI_PxCMD) & AHCI_PxCMD_ATAPI, "ATAPI bit is RW");
    pwr(0, AHCI_PxCMD, 0);

    pwr(0, AHCI_PxSCTL, 0x301);                    /* TFD = 7Fh：DRQ 置位 */
    pwr(0, AHCI_PxCMD, AHCI_PxCMD_CLO);
    CHECK_U64(prd(0, AHCI_PxTFD) & (ATA_STS_BSY | ATA_STS_DRQ), 0, "CLO clears BSY and DRQ");
    CHECK_U64(prd(0, AHCI_PxCMD) & AHCI_PxCMD_CLO, 0, "CLO self-clears");

    pwr(0, AHCI_PxSERR, 0xFFFFFFFFu);
    CHECK_U64(prd(0, AHCI_PxSERR), 0, "PxSERR is RWC");
    CHECK_U64(prd(1, AHCI_PxSNTF), 0, "unsupported PxSNTF reads 0");

    /* 小于 32 位的读取对应字节，64 位读两个 dword */
    S->pci.bars[5].read(S, AHCI_VS + 1, 1, &b);
    CHECK_U64(b, 0x03, "byte read of VS[15:8]");
    S->pci.bars[5].read(S, AHCI_CAP, 8, &q);
    CHECK_U64(q, ((uint64_t)AHCI_GHC_AE << 32) | rd(AHCI_CAP), "64-bit read of CAP+GHC");
    b = 0xFF;
    S->pci.bars[5].write(S, AHCI_PORT_BASE + AHCI_PxIE, 1, &b);
    CHECK_U64(prd(0, AHCI_PxIE), 0x7DC0007Fu, "byte write ignored");
    q = (uint64_t)0x5000 << 32 | 0x4000;
    S->pci.bars[5].write(S, AHCI_PORT_BASE + AHCI_PxCLB, 8, &q);
    CHECK(prd(0, AHCI_PxCLB) == 0x4000 && prd(0, AHCI_PxCLBU) == 0x5000, "64-bit write of CLB+CLBU");
}

int main(int argc, char **argv)
{
    T_MAIN_BEGIN();
    strcpy(g_disk.id, "d0");
    g_disk.size = 64ull << 20;
    case_identity();
    case_power_on();
    case_comreset();
    case_hba_reset();
    case_irq();
    case_cmd_abort();
    case_cmd_bus_errors();
    case_register_bits();
    free(S);
    return T_SUMMARY();
}
