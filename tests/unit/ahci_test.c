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
 *   - FIS 布局：SATA 3.0 第 10.3.4/10.3.5/10.3.10 节，接收区偏移第 4.2.1 节
 *   - PRDT：第 4.2.3.3 节（DBA 字对齐、DBC 偶数），第 5.4.1 节（PRDBC），
 *     第 6.1.5 节（PRDT 不够长置 OFS）；PIO data-in 的结束状态取自 PIO
 *     Setup FIS 的 E_Status（第 5.3.9 节，Linux ahci_qc_fill_rtf 同样如此）
 *   - IDENTIFY DEVICE 数据：ATA8-ACS 表 22（各字的含义）、第 7.16.7.82 节
 *     （完整性字），字符串的字节顺序按 Linux ata_id_string 解码
 *   - 读写命令的 LBA/Count 位置：SATA 3.0 第 10.3.4 节、ATA8-ACS 第 7.24
 *     节起；数据和镜像内容由测试用另一个 fd 直接 pread/pwrite 核对，盘是
 *     真实的 src/block/blockdev.c 打开的稀疏文件
 *   - NCQ：FPDMA 命令的 Count/tag/Feature 位置见 ATA8-ACS 第 7.28/7.67 节；
 *     接收用 I=0 的 D2H FIS、完成用 SDB FIS 见 SATA 3.0 第 13.6.1.1、
 *     13.6.3 节，SDB 的布局见第 10.3.6 节；HBA 收到 SDB 后清 PxSACT、置
 *     SDBS 见 AHCI 第 5.3.13.1 节，PRDBC 对 NCQ 不要求有效见第 5.4.1 节；
 *     出错后只接受读日志 10h、读完先发 ACT 全 1 的 SDB 见 SATA 3.0 第
 *     13.6.3.4 节，日志 10h 的布局与校验和见第 13.7.3 节
 *   - 日志：READ/WRITE LOG EXT 见 ATA8-ACS 第 7.29/7.68 节，目录 00h 的
 *     布局见附录表 A.2，NCQ 设备必须支持 GPL 与主机日志 80h-9Fh 见第
 *     4.15.1、4.10 节；页号放在 LBA 15:8（与 47:40）按 Linux
 *     ata_read_log_page 的发法
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

int pci_dma_map(struct pci_device *dev, uint64_t gpa, size_t len, void **hva)
{
    (void)dev;
    if (!g_master || len == 0 || gpa > RAM_SIZE || len > RAM_SIZE - gpa)
        return VMM_ERR_GUEST;
    *hva = g_ram + gpa;
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
static struct blockdev *g_disk;
static int g_img = -1;            /* 同一个镜像的另一个 fd，核对用 */
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

static void fresh(void) { fresh_with(g_disk); }

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

/* 不支持的命令：SMART（B0h）。IDENTIFY 字 82 bit 0 为 0，ATA8-ACS 第 4.1 节
 * 要求设备中止它，测试命令出错路径时用它 */
#define CMD_UNSUPPORTED  0xB0

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
    CHECK(cap & (1u << 30), "CAP.SNCQ: native command queuing");
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
    put_cmd(3, 5, SATA_FIS_REG_H2D, SATA_FIS_H2D_C, CMD_UNSUPPORTED);
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

    put_cmd(4, 5, SATA_FIS_REG_H2D, SATA_FIS_H2D_C, CMD_UNSUPPORTED);
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
    put_cmd(0, 5, SATA_FIS_REG_H2D, SATA_FIS_H2D_C, CMD_UNSUPPORTED);
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
    put_cmd(0, 1, SATA_FIS_REG_H2D, SATA_FIS_H2D_C, CMD_UNSUPPORTED);   /* CFL=1 非法 */
    pwr(0, AHCI_PxCI, 1);
    CHECK(prd(0, AHCI_PxIS) & AHCI_PxIS_HBFS, "illegal CFL -> HBFS");

    fresh();
    port0_start();
    put_cmd(0, 5, SATA_FIS_REG_H2D, SATA_FIS_H2D_C, CMD_UNSUPPORTED);
    put32(g_ram + CLB + 8, 0xFFFFFF80u);                       /* 命令表在 RAM 外 */
    pwr(0, AHCI_PxCI, 1);
    CHECK(prd(0, AHCI_PxIS) & AHCI_PxIS_HBFS, "unreadable command table -> HBFS");

    fresh();
    port0_start();
    put_cmd(0, 5, SATA_FIS_REG_H2D, SATA_FIS_H2D_C, CMD_UNSUPPORTED);
    pwr(0, AHCI_PxCMD, prd(0, AHCI_PxCMD) & ~AHCI_PxCMD_ST);
    pwr(0, AHCI_PxCMD, prd(0, AHCI_PxCMD) & ~AHCI_PxCMD_FRE);
    pwr(0, AHCI_PxFB, 0xFFFFFF00u);                             /* FIS 接收区在 RAM 外 */
    pwr(0, AHCI_PxCMD, prd(0, AHCI_PxCMD) | AHCI_PxCMD_FRE | AHCI_PxCMD_ST);
    pwr(0, AHCI_PxCI, 1);
    CHECK(prd(0, AHCI_PxIS) & AHCI_PxIS_HBFS, "unwritable FIS area -> HBFS");
    CHECK_U64(prd(0, AHCI_PxCI), 1, "slot not completed");

    fresh();
    port0_start();
    put_cmd(0, 5, SATA_FIS_REG_H2D, SATA_FIS_H2D_C, CMD_UNSUPPORTED);
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

/* ------------------------------------------------------------------ */
/* 盘：IDENTIFY 与读写命令                                              */
/* ------------------------------------------------------------------ */

/* 9 GiB 的稀疏镜像：超过 2^24 个扇区，28 位命令放在 Device 低 4 位里的
 * LBA[27:24] 才能取到非零值 */
#define DISK_SIZE     (9ull << 30)
#define DISK_SECTORS  (DISK_SIZE / ATA_SECTOR_SIZE)
#define DATA          0x10000u     /* 数据缓冲区，在命令表之后 */
#define DATA2         0x80000u

static char g_dir[256];
static struct blockdev *g_big, *g_ro;

/* 新建一个 size 字节的稀疏镜像，经 --drive 的同一条路径打开。文件打开后
 * 立刻删除，fd 留着用；*oracle 是测试自己核对内容用的另一个 fd */
static struct blockdev *open_disk(const char *id, uint64_t size, bool ro, int *oracle)
{
    char path[512], arg[640];
    struct blockdev *bd;
    const char *tmp = getenv("TMPDIR");
    int fd;

    if (!g_dir[0]) {
        snprintf(g_dir, sizeof(g_dir), "%s/ahci_test.XXXXXX", tmp && *tmp ? tmp : "/tmp");
        if (!mkdtemp(g_dir)) {
            perror("fixture: mkdtemp");
            exit(1);
        }
    }
    snprintf(path, sizeof(path), "%s/%s.img", g_dir, id);
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || ftruncate(fd, (off_t)size) != 0) {
        perror("fixture: image");
        exit(1);
    }
    snprintf(arg, sizeof(arg), "id=%s,file=%s%s", id, path, ro ? ",readonly=on" : "");
    if (blockdev_parse(arg, &bd) != VMM_OK || blockdev_open(bd) != VMM_OK) {
        printf("fixture: blockdev: %s\n", g_last_log);
        exit(1);
    }
    unlink(path);
    if (oracle)
        *oracle = fd;
    else
        close(fd);
    return bd;
}

/* blockdev 在 VMM 里活到进程结束，没有释放函数；测试自己收拾，免得 LSan 报泄漏 */
static void free_disk(struct blockdev *bd)
{
    if (!bd)
        return;
    blockdev_close(bd);
    free(bd->filename);
    free(bd);
}

static uint8_t *hdr(int slot) { return g_ram + CLB + (uint32_t)slot * AHCI_CMDHDR_SIZE; }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void run(int slot) { pwr(0, AHCI_PxCI, 1u << slot); }

/* 出错停下之后按第 6.2.2.1 节恢复：清 ST、清中断状态、再置 ST */
static void port0_restart(void)
{
    pwr(0, AHCI_PxCMD, prd(0, AHCI_PxCMD) & ~AHCI_PxCMD_ST);
    pwr(0, AHCI_PxIS, 0xFFFFFFFFu);
    wr(AHCI_IS, 0xFFFFFFFFu);
    pwr(0, AHCI_PxCMD, prd(0, AHCI_PxCMD) | AHCI_PxCMD_ST);
}

/* PRDT 第 i 项，dw3 原样写入（DBC 是字节数减 1，bit 31 是 I） */
static void put_prd(int i, uint32_t dba, uint32_t dw3)
{
    uint8_t *e = g_ram + CTBA + AHCI_CT_PRDT + (uint32_t)i * AHCI_PRD_SIZE;

    memset(e, 0, AHCI_PRD_SIZE);
    put32(e, dba);
    put32(e + 12, dw3);
}

/* DMA 读写命令，LBA/Count 按 SATA 3.0 第 10.3.4 节摆进 FIS：LBA[23:0] 在
 * 字节 4-6，48 位命令的 LBA[47:24] 在字节 8-10，28 位命令的 LBA[27:24]
 * 在 Device 的低 4 位，Count 在字节 12-13 */
static void put_rw(int slot, uint8_t cmd, uint64_t lba, uint32_t count, uint32_t prdtl)
{
    bool ext = cmd == ATA_CMD_READ_DMA_EXT || cmd == ATA_CMD_WRITE_DMA_EXT;
    uint8_t *t = g_ram + CTBA;

    put_cmd(slot, 5, SATA_FIS_REG_H2D, SATA_FIS_H2D_C, cmd);
    put32(hdr(slot), 5 | prdtl << AHCI_CH_PRDTL_SHIFT);
    t[4] = (uint8_t)lba;
    t[5] = (uint8_t)(lba >> 8);
    t[6] = (uint8_t)(lba >> 16);
    t[7] = ATA_DEV_LBA | (ext ? 0 : (uint8_t)((lba >> 24) & 0xF));
    if (ext) {
        t[8]  = (uint8_t)(lba >> 24);
        t[9]  = (uint8_t)(lba >> 32);
        t[10] = (uint8_t)(lba >> 40);
        t[13] = (uint8_t)(count >> 8);
    }
    t[12] = (uint8_t)count;
}

/* 不带数据的命令（FLUSH、SET FEATURES 等）：Feature 在字节 3，Count 在 12 */
static void nondata(uint8_t cmd, uint8_t feature, uint8_t count)
{
    put_cmd(0, 5, SATA_FIS_REG_H2D, SATA_FIS_H2D_C, cmd);
    g_ram[CTBA + 3] = feature;
    g_ram[CTBA + 12] = count;
    run(0);
}

static void fill(uint8_t *p, size_t n, uint32_t seed)
{
    uint32_t x = seed * 2654435761u + 1;
    size_t i;

    for (i = 0; i < n; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        p[i] = (uint8_t)x;
    }
}

static bool image_is(int fd, uint64_t off, const void *want, size_t n)
{
    uint8_t *buf = malloc(n);
    bool ok = buf && pread(fd, buf, n, (off_t)off) == (ssize_t)n && !memcmp(buf, want, n);

    free(buf);
    return ok;
}

static bool image_zero(int fd, uint64_t off, size_t n)
{
    uint8_t *z = calloc(1, n);
    bool ok = z && image_is(fd, off, z, n);

    free(z);
    return ok;
}

/* Linux ata_id_string 的解码：每个字先取高字节 */
static const char *id_str(const uint16_t *id, int w, int nbytes, char *out)
{
    int i;

    for (i = 0; i < nbytes; i += 2) {
        out[i] = (char)(id[w + i / 2] >> 8);
        out[i + 1] = (char)id[w + i / 2];
    }
    out[nbytes] = 0;
    return out;
}

static uint64_t id_u64(const uint16_t *id, int w)
{
    return (uint64_t)id[w] | (uint64_t)id[w + 1] << 16 |
           (uint64_t)id[w + 2] << 32 | (uint64_t)id[w + 3] << 48;
}

static void case_identify_data(void)
{
    uint16_t id[ATA_ID_WORDS];
    char str[41];
    uint8_t sum = 0;
    int i;

    T_CASE("IDENTIFY DEVICE 数据：能力字、容量、字符串、完整性字");
    fresh();
    ahci_identify(S, id);
    for (i = 0; i < ATA_ID_WORDS; i++)
        sum = (uint8_t)(sum + (id[i] & 0xFF) + (id[i] >> 8));
    CHECK_U64(id[255] & 0xFF, 0xA5, "integrity word signature A5h");
    CHECK_U64(sum, 0, "all 512 bytes sum to zero");
    CHECK(!strcmp(id_str(id, 27, 40, str), "MINIVMM AHCI DISK                       "),
          "model: two characters per word, first in the high byte, space padded");
    CHECK(!strcmp(id_str(id, 10, 20, str), "d0                  "), "serial number is the drive id");
    CHECK(!strcmp(id_str(id, 23, 8, str), "1.0     "), "firmware revision");
    CHECK_U64(id[0] & 0x8000, 0, "word 0 bit 15 clear: ATA device");
    CHECK_U64(id[49] & 0x0300, 0x0300, "LBA and DMA supported");
    /* 以下按 Linux include/linux/ata.h 里 ata_id_* 的判断方式取位 */
    CHECK((id[83] & 0xC000) == 0x4000 && (id[83] & (1u << 10)), "48-bit address (ata_id_has_lba48)");
    CHECK((id[83] & (3u << 12)) == 3u << 12, "FLUSH CACHE and FLUSH CACHE EXT");
    CHECK(id[82] & (1u << 5), "volatile write cache supported");
    CHECK((id[87] & 0xC000) == 0x4000 && (id[85] & (1u << 5)), "write cache enabled at power-on");
    CHECK(id[93] == 0 && (int16_t)id[80] >= 0x20, "SATA device (ata_id_is_sata)");
    CHECK(id[80] & (1u << 8), "ATA8-ACS");
    CHECK_U64(id[76] & 0x810F, 0x010E, "SATA Gen1-3 signaling, NCQ, log 10h not via DMA");
    CHECK_U64(id[75] & 0x1F, 31, "queue depth 32 (word 75 = depth - 1)");
    CHECK_U64(id[77] | id[78], 0, "no NCQ priority/management/auto-activate: no extra log reads");
    CHECK((id[84] & 0xC000) == 0x4000 && (id[84] & (1u << 5)) && (id[87] & (1u << 5)),
          "GPL supported and enabled (required for NCQ)");
    CHECK_U64(id[82] & (1u << 10), 0, "no HPA: Linux sends no READ NATIVE MAX");
    CHECK_U64(id[59] & 0x100, 0, "no READ/WRITE MULTIPLE setting");
    CHECK_U64(id[88], 0x407F, "UDMA 0-6 supported, UDMA 6 selected at power-on");
    CHECK_U64(id[217], 1, "non-rotating media");
    CHECK_U64(id_u64(id, 100), DISK_SECTORS, "words 100-103: 48-bit capacity");
    CHECK_U64((uint64_t)id[60] | (uint64_t)id[61] << 16, DISK_SECTORS, "words 60-61: 28-bit capacity");

    g_big = open_disk("big", 129ull << 30, false, NULL);
    fresh_with(g_big);
    ahci_identify(S, id);
    CHECK_U64((uint64_t)id[60] | (uint64_t)id[61] << 16, 0x0FFFFFFF,
              "28-bit capacity saturates at 0FFFFFFFh beyond 128 GiB");
    CHECK_U64(id_u64(id, 100), (129ull << 30) / 512, "48-bit capacity is exact");
}

static void case_identify_cmd(void)
{
    uint16_t id[ATA_ID_WORDS];
    uint8_t want[ATA_SECTOR_SIZE];
    const uint8_t *ps = g_ram + FB + AHCI_RFIS_PSFIS;
    int i;

    T_CASE("IDENTIFY DEVICE 命令：PIO data-in，数据分两段 PRD，结束状态在 PIO Setup FIS");
    fresh();
    port0_start();
    put_cmd(2, 5, SATA_FIS_REG_H2D, SATA_FIS_H2D_C, ATA_CMD_IDENTIFY);
    put32(hdr(2), 5 | 2u << AHCI_CH_PRDTL_SHIFT);
    put32(hdr(2) + 4, 0xDEADBEEFu);              /* PRDBC 要被改写成实际字节数 */
    put_prd(0, DATA, 100 - 1);
    put_prd(1, DATA + 0x1000, 412 - 1);
    memset(g_ram + FB, 0, AHCI_RFIS_SIZE);
    run(2);

    ahci_identify(S, id);
    for (i = 0; i < ATA_ID_WORDS; i++) {
        want[2 * i] = (uint8_t)id[i];
        want[2 * i + 1] = (uint8_t)(id[i] >> 8);
    }
    CHECK(!memcmp(g_ram + DATA, want, 100) && !memcmp(g_ram + DATA + 0x1000, want + 100, 412),
          "512 bytes scattered over both PRDs, 16-bit words little endian");
    CHECK_U64(g_ram[DATA + 100], 0, "first PRD not overrun");
    CHECK_U64(get32(hdr(2) + 4), 512, "PRDBC = 512");
    CHECK_U64(prd(0, AHCI_PxCI), 0, "slot completed");
    CHECK_U64(prd(0, AHCI_PxTFD), 0x40, "PxTFD from E_Status: DRDY");
    CHECK(ps[0] == 0x5F && ps[1] == (SATA_FIS_PIO_D | SATA_FIS_I) && ps[15] == 0x40 &&
          ps[16] == 0x00 && ps[17] == 0x02,
          "PIO Setup FIS: D and I bits, E_Status 40h, transfer count 512");
    CHECK_U64(prd(0, AHCI_PxIS) & (AHCI_PxIS_PSS | AHCI_PxIS_DHRS | AHCI_PxIS_TFES),
              AHCI_PxIS_PSS, "PSS only, no D2H Register FIS");
    CHECK_U64(g_ram[FB + AHCI_RFIS_RFIS], 0, "D2H FIS area untouched");
    CHECK(g_irq, "interrupt");
}

static void case_rw(void)
{
    const uint8_t *f = g_ram + FB + AHCI_RFIS_RFIS;
    const uint64_t lba = 0x01000005;             /* LBA[27:24] = 1 */
    static uint8_t buf[1100 * ATA_SECTOR_SIZE];
    int i;

    T_CASE("WRITE DMA EXT 与 READ DMA：LBA48 和 LBA28 的字节位置，多段 PRD");
    fresh();
    port0_start();
    fill(g_ram + DATA, 2048, 1);
    put_rw(0, ATA_CMD_WRITE_DMA_EXT, lba, 4, 3);
    put_prd(0, DATA, 511);
    put_prd(1, DATA + 512, 1023);
    put_prd(2, DATA + 1536, 511);
    run(0);
    CHECK_U64(prd(0, AHCI_PxCI), 0, "write completed");
    CHECK_U64(prd(0, AHCI_PxTFD), 0x40, "status DRDY");
    CHECK(f[0] == 0x34 && (f[1] & SATA_FIS_I) && f[2] == 0x40 && f[3] == 0,
          "D2H Register FIS with I bit, status 40h");
    CHECK_U64(prd(0, AHCI_PxIS) & (AHCI_PxIS_DHRS | AHCI_PxIS_TFES), AHCI_PxIS_DHRS, "DHRS");
    CHECK_U64(get32(hdr(0) + 4), 2048, "PRDBC = 2048");
    CHECK(image_is(g_img, lba * 512, g_ram + DATA, 2048), "image holds the data at LBA * 512");

    put_rw(1, ATA_CMD_READ_DMA, lba, 4, 1);
    put_prd(0, DATA2, 2047);
    run(1);
    CHECK_U64(prd(0, AHCI_PxCI), 0, "read completed");
    CHECK(!memcmp(g_ram + DATA2, g_ram + DATA, 2048), "28-bit read takes LBA[27:24] from the Device nibble");

    T_CASE("Count 为 0：28 位命令是 256 个扇区，48 位命令是 65536 个");
    fresh();
    port0_start();
    fill(buf, 256 * 512, 2);
    CHECK(pwrite(g_img, buf, 256 * 512, 0x1000 * 512) == 256 * 512, "seed image");
    put_rw(0, ATA_CMD_READ_DMA, 0x1000, 0, 1);
    put_prd(0, DATA, 256 * 512 - 1);
    run(0);
    CHECK_U64(get32(hdr(0) + 4), 256 * 512, "PRDBC = 128 KiB");
    CHECK(!memcmp(g_ram + DATA, buf, 256 * 512), "256 sectors read");
    /* 32 MiB 放不进假内存，用 128 KiB 的 PRDT：按 65536 个扇区算就一定不够 */
    put_rw(0, ATA_CMD_READ_DMA_EXT, 0, 0, 1);
    run(0);
    CHECK(prd(0, AHCI_PxIS) & AHCI_PxIS_OFS, "EXT count 0 needs more than 128 KiB: overflow");

    T_CASE("PRDT 超过 BLK_MAX_IOV 段：分批交给后端，数据按顺序拼接");
    fresh();
    port0_start();
    fill(g_ram + DATA, 1100 * 512, 3);
    put_rw(0, ATA_CMD_WRITE_DMA_EXT, 9000, 1100, 1100);
    for (i = 0; i < 1100; i++)
        put_prd(i, DATA + (uint32_t)i * 512, 511);
    run(0);
    CHECK_U64(prd(0, AHCI_PxTFD), 0x40, "1100-entry write completed");
    CHECK(image_is(g_img, 9000ull * 512, g_ram + DATA, 1100 * 512), "image matches");
    fill(buf, sizeof(buf), 4);
    CHECK(pwrite(g_img, buf, sizeof(buf), 20000ull * 512) == (ssize_t)sizeof(buf), "seed image");
    put_rw(0, ATA_CMD_READ_DMA_EXT, 20000, 1100, 1100);
    run(0);
    CHECK_U64(prd(0, AHCI_PxTFD), 0x40, "1100-entry read completed");
    CHECK(!memcmp(g_ram + DATA, buf, sizeof(buf)), "guest memory matches");
}

static void case_rw_errors(void)
{
    int rofd;

    T_CASE("读写出错：越界报 IDNF、CHS 寻址和只读盘上的写中止，镜像都不变");
    fresh();
    port0_start();
    fill(g_ram + DATA, 1024, 5);
    put_rw(0, ATA_CMD_WRITE_DMA_EXT, DISK_SECTORS - 1, 2, 1);
    put_prd(0, DATA, 1023);
    run(0);
    CHECK_U64(prd(0, AHCI_PxTFD), 0x1041, "DRDY|ERR, error IDNF");
    CHECK(prd(0, AHCI_PxIS) & AHCI_PxIS_TFES, "TFES");
    CHECK_U64(prd(0, AHCI_PxCI), 1, "slot left in PxCI");
    CHECK(image_zero(g_img, (DISK_SECTORS - 1) * 512, 512), "last sector not written");

    port0_restart();
    put_rw(0, ATA_CMD_WRITE_DMA_EXT, DISK_SECTORS - 1, 1, 1);
    run(0);
    CHECK_U64(prd(0, AHCI_PxTFD), 0x40, "the last sector itself is writable");
    CHECK(image_is(g_img, (DISK_SECTORS - 1) * 512, g_ram + DATA, 512), "last sector written");

    /* LBA bit 32（字节 9）落在 9 GiB 之外：没解码字节 9 就会读到 LBA 5 */
    put_rw(0, ATA_CMD_READ_DMA_EXT, (1ull << 32) | 5, 1, 1);
    run(0);
    CHECK_U64(prd(0, AHCI_PxTFD), 0x1041, "LBA beyond capacity via byte 9: IDNF");

    port0_restart();
    put_rw(0, ATA_CMD_READ_DMA, 5, 1, 1);
    g_ram[CTBA + 7] = 0;                         /* Device bit 6 = 0：CHS */
    run(0);
    CHECK_U64(prd(0, AHCI_PxTFD), 0x0441, "CHS addressing aborted");

    g_ro = open_disk("ro", 1 << 20, true, &rofd);
    fresh_with(g_ro);
    port0_start();
    fill(g_ram + DATA, 512, 6);
    put_rw(0, ATA_CMD_WRITE_DMA, 0, 1, 1);
    put_prd(0, DATA, 511);
    run(0);
    CHECK_U64(prd(0, AHCI_PxTFD), 0x0441, "write to a read-only drive aborted");
    CHECK(image_zero(rofd, 0, 512), "read-only image unchanged");
    port0_restart();
    put_rw(0, ATA_CMD_READ_DMA, 0, 1, 1);
    run(0);
    CHECK_U64(prd(0, AHCI_PxTFD), 0x40, "read from a read-only drive works");
    close(rofd);
}

static void case_prdt(void)
{
    static const uint32_t bad[] = { 0xFFFFF000u, RAM_SIZE - 256 };
    unsigned i;

    T_CASE("PRDT 每一项单独检查：指向 RAM 外或跨出 RAM 末尾，按 HBFS 停下，一个字节都不写");
    for (i = 0; i < 2; i++) {
        fresh();
        port0_start();
        fill(g_ram + DATA, 1024, 7);
        put_rw(0, ATA_CMD_WRITE_DMA_EXT, 100, 2, 2);
        put_prd(0, DATA, 511);
        put_prd(1, bad[i], 511);
        run(0);
        CHECK(prd(0, AHCI_PxIS) & AHCI_PxIS_HBFS, "PRD 1 at 0x%x: HBFS", bad[i]);
        CHECK(prd(0, AHCI_PxSERR) & AHCI_SERR_ERR_E, "PxSERR.ERR.E");
        CHECK_U64(prd(0, AHCI_PxCI), 1, "slot left in PxCI");
        CHECK(image_zero(g_img, 100 * 512, 1024), "the good PRD 0 was not written either");
    }

    /* 命令表放在 RAM 最后 128 字节：CFIS 读得到，紧接其后的 PRDT 读不到 */
    fresh();
    port0_start();
    put_rw(0, ATA_CMD_READ_DMA, 0, 1, 1);
    memcpy(g_ram + RAM_SIZE - 0x80, g_ram + CTBA, 0x80);
    put32(hdr(0) + 8, RAM_SIZE - 0x80);
    run(0);
    CHECK(prd(0, AHCI_PxIS) & AHCI_PxIS_HBFS, "unreadable PRDT -> HBFS");

    T_CASE("PRDT 描述的字节数少于命令要传的：置 OFS 并停下");
    fresh();
    port0_start();
    put_rw(0, ATA_CMD_READ_DMA_EXT, 0, 2, 1);
    put_prd(0, DATA, 511);
    run(0);
    CHECK(prd(0, AHCI_PxIS) & AHCI_PxIS_OFS, "OFS");
    CHECK_U64(prd(0, AHCI_PxIS) & AHCI_PxIS_TFES, 0, "not a task file error");
    CHECK_U64(prd(0, AHCI_PxCI), 1, "slot left in PxCI");
    CHECK_U64(prd(0, AHCI_PxCMD) & AHCI_PxCMD_CR, 0, "halted");

    T_CASE("DBA、DBC 的 bit 0 按规定值处理；PRD 的 I 位置 DPS");
    fresh();
    port0_start();
    fill(g_ram + DATA2, 512, 8);
    CHECK(pwrite(g_img, g_ram + DATA2, 512, 200 * 512) == 512, "seed image");
    memset(g_ram + DATA, 0xEE, 1024);
    put_rw(0, ATA_CMD_READ_DMA, 200, 1, 1);
    put_prd(0, DATA | 1u, (512 - 2) | 1u << 31);  /* DBA 奇数、DBC 偶数（511 字节） */
    run(0);
    CHECK_U64(prd(0, AHCI_PxTFD), 0x40, "completed");
    CHECK(!memcmp(g_ram + DATA, g_ram + DATA2, 512), "data lands at DBA & ~1");
    CHECK_U64(g_ram[DATA + 512], 0xEE, "byte count rounded up to 512, not beyond");
    CHECK(prd(0, AHCI_PxIS) & AHCI_PxIS_DPS, "I bit -> DPS");
}

static void case_features(void)
{
    uint16_t id[ATA_ID_WORDS];
    uint64_t f0;

    T_CASE("FLUSH CACHE 与写缓存开关：关掉写缓存后每次写都落盘");
    fresh();
    port0_start();
    f0 = g_disk->stats.flush_ops;
    nondata(ATA_CMD_FLUSH_CACHE_EXT, 0, 0);
    CHECK_U64(prd(0, AHCI_PxTFD), 0x40, "FLUSH CACHE EXT completed");
    nondata(ATA_CMD_FLUSH_CACHE, 0, 0);
    CHECK_U64(g_disk->stats.flush_ops, f0 + 2, "both flushes reach the backend");
    nondata(ATA_CMD_SET_FEATURES, ATA_SF_WCACHE_OFF, 0);
    CHECK_U64(g_disk->stats.flush_ops, f0 + 3, "disabling the cache flushes it first");
    ahci_identify(S, id);
    CHECK_U64(id[85] & (1u << 5), 0, "IDENTIFY word 85: write cache disabled");
    put_rw(0, ATA_CMD_WRITE_DMA, 300, 1, 1);
    put_prd(0, DATA, 511);
    run(0);
    CHECK_U64(g_disk->stats.flush_ops, f0 + 4, "write with the cache off is flushed");
    nondata(ATA_CMD_SET_FEATURES, ATA_SF_WCACHE_ON, 0);
    put_rw(0, ATA_CMD_WRITE_DMA, 300, 1, 1);
    run(0);
    CHECK_U64(g_disk->stats.flush_ops, f0 + 4, "write with the cache on is not");

    T_CASE("SET FEATURES 传输模式：UDMA 与 MWDMA 互斥，非法值中止，COMRESET 回到默认");
    nondata(ATA_CMD_SET_FEATURES, ATA_SF_XFER_MODE, ATA_XFER_UDMA | 5);
    ahci_identify(S, id);
    CHECK(id[88] == 0x207F && id[63] == 0x0007, "UDMA 5 selected");
    nondata(ATA_CMD_SET_FEATURES, ATA_SF_XFER_MODE, ATA_XFER_MWDMA | 2);
    ahci_identify(S, id);
    CHECK(id[88] == 0x007F && id[63] == 0x0407, "MWDMA 2 selected, UDMA deselected");
    nondata(ATA_CMD_SET_FEATURES, ATA_SF_XFER_MODE, ATA_XFER_PIO | 4);
    ahci_identify(S, id);
    CHECK_U64(prd(0, AHCI_PxTFD), 0x40, "PIO 4 accepted");
    CHECK_U64(id[63], 0x0407, "PIO selection leaves the DMA mode alone");
    nondata(ATA_CMD_SET_FEATURES, ATA_SF_XFER_MODE, ATA_XFER_UDMA | 7);
    CHECK_U64(prd(0, AHCI_PxTFD), 0x0441, "UDMA 7 not supported: aborted");
    port0_restart();
    nondata(ATA_CMD_SET_FEATURES, 0x10, 0);
    CHECK_U64(prd(0, AHCI_PxTFD), 0x0441, "unsupported subcommand aborted");
    port0_restart();
    nondata(ATA_CMD_SET_FEATURES, ATA_SF_WCACHE_OFF, 0);
    pwr(0, AHCI_PxSCTL, 0x301);
    pwr(0, AHCI_PxSCTL, 0x300);
    ahci_identify(S, id);
    CHECK(id[88] == 0x407F && id[63] == 0x0007 && (id[85] & (1u << 5)),
          "COMRESET restores UDMA 6 and the write cache");

    T_CASE("STANDBY IMMEDIATE 完成；不支持的命令中止，每个操作码只记一次日志");
    fresh();
    port0_start();
    nondata(ATA_CMD_STANDBY_NOW, 0, 0);
    CHECK_U64(prd(0, AHCI_PxTFD), 0x40, "STANDBY IMMEDIATE completed");
    g_last_log[0] = 0;
    nondata(CMD_UNSUPPORTED, 0, 0);
    CHECK_U64(prd(0, AHCI_PxTFD), 0x0441, "SMART aborted");
    CHECK(strstr(g_last_log, "0xb0") != NULL, "opcode logged");
    port0_restart();
    g_last_log[0] = 0;
    nondata(CMD_UNSUPPORTED, 0, 0);
    CHECK_U64(g_last_log[0], 0, "second time not logged");
}

/* ---- NCQ 与 GPL 日志 ---- */

static struct blockdev *g_small;

/* READ/WRITE FPDMA QUEUED（ATA8-ACS 第 7.28、7.67 节）：扇区数在 Feature
 * （字节 3、11），tag 在 Count 的 bit 7:3（字节 12），LBA 48 位，FUA 在
 * Device bit 7。tag 按 Linux 的做法等于槽号 */
static void put_fpdma(int slot, uint8_t cmd, uint64_t lba, uint32_t count, uint32_t prdtl, bool fua)
{
    uint8_t *t = g_ram + CTBA;

    put_cmd(slot, 5, SATA_FIS_REG_H2D, SATA_FIS_H2D_C, cmd);
    put32(hdr(slot), 5 | prdtl << AHCI_CH_PRDTL_SHIFT);
    t[3]  = (uint8_t)count;
    t[11] = (uint8_t)(count >> 8);
    t[4]  = (uint8_t)lba;
    t[5]  = (uint8_t)(lba >> 8);
    t[6]  = (uint8_t)(lba >> 16);
    t[7]  = ATA_DEV_LBA | (fua ? ATA_DEV_FUA : 0);
    t[8]  = (uint8_t)(lba >> 24);
    t[9]  = (uint8_t)(lba >> 32);
    t[10] = (uint8_t)(lba >> 40);
    t[12] = (uint8_t)(slot << 3);
}

/* 软件发 NCQ 命令的顺序（Linux ahci_qc_issue）：先置 PxSACT 再置 PxCI */
static void run_ncq(int slot)
{
    pwr(0, AHCI_PxSACT, 1u << slot);
    run(slot);
}

/* READ/WRITE LOG EXT：日志地址在 LBA 7:0（字节 4），页号按 Linux 的布局
 * 在字节 5 和 9，页数在 Count（字节 12、13） */
static void put_log(int slot, uint8_t cmd, uint8_t log, unsigned page, unsigned count, uint32_t prdtl)
{
    uint8_t *t = g_ram + CTBA;

    put_cmd(slot, 5, SATA_FIS_REG_H2D, SATA_FIS_H2D_C, cmd);
    put32(hdr(slot), 5 | prdtl << AHCI_CH_PRDTL_SHIFT |
          (cmd == ATA_CMD_WRITE_LOG_EXT ? AHCI_CH_WRITE : 0));
    t[4]  = log;
    t[5]  = (uint8_t)page;
    t[9]  = (uint8_t)(page >> 8);
    t[7]  = ATA_DEV_LBA;
    t[12] = (uint8_t)count;
    t[13] = (uint8_t)(count >> 8);
}

static uint8_t sum512(const uint8_t *p)
{
    uint8_t s = 0;
    int i;

    for (i = 0; i < 512; i++)
        s = (uint8_t)(s + p[i]);
    return s;
}

static void case_ncq(void)
{
    const uint8_t *d2h = g_ram + FB + AHCI_RFIS_RFIS;
    const uint8_t *sdb = g_ram + FB + AHCI_RFIS_SDBFIS;
    const uint64_t lba = 0x01000005;             /* LBA 位 24 在字节 8 */
    uint64_t flushes;

    T_CASE("NCQ 读写：先置 PxSACT，接受时 I=0 的 D2H 清 PxCI，完成时 SDB FIS 清 PxSACT 置 SDBS");
    fresh();
    port0_start();
    fill(g_ram + DATA, 8 * 512, 21);
    put_fpdma(3, ATA_CMD_WRITE_FPDMA, lba, 8, 2, false);
    put_prd(0, DATA, 3 * 512 - 1);
    put_prd(1, DATA + 0x4000, 5 * 512 - 1);
    memcpy(g_ram + DATA + 0x4000, g_ram + DATA + 3 * 512, 5 * 512);
    memset(g_ram + FB, 0xEE, AHCI_RFIS_SIZE);
    put32(hdr(3) + 4, 0xDEADBEEFu);
    run_ncq(3);
    CHECK(image_is(g_img, lba * 512, g_ram + DATA, 3 * 512) &&
          image_is(g_img, (lba + 3) * 512, g_ram + DATA + 0x4000, 5 * 512),
          "8 sectors written at the 48-bit LBA from both PRDs");
    CHECK_U64(prd(0, AHCI_PxCI), 0, "PxCI cleared by the acceptance D2H FIS");
    CHECK_U64(prd(0, AHCI_PxSACT), 0, "PxSACT cleared by the SDB FIS");
    CHECK(d2h[0] == 0x34 && !(d2h[1] & SATA_FIS_I) && d2h[2] == 0x40,
          "acceptance: D2H Register FIS, I=0, DRDY and BSY=0");
    CHECK(sdb[0] == 0xA1 && (sdb[1] & SATA_FIS_I) && sdb[2] == 0x40 && sdb[3] == 0 &&
          get32(sdb + 4) == 1u << 3, "SDB FIS: I=1, status 40h, ACT = tag 3");
    CHECK_U64(prd(0, AHCI_PxIS) & (AHCI_PxIS_SDBS | AHCI_PxIS_DHRS | AHCI_PxIS_TFES),
              AHCI_PxIS_SDBS, "SDBS only: the acceptance FIS raises no DHRS");
    CHECK_U64(prd(0, AHCI_PxTFD), 0x40, "PxTFD = DRDY");
    CHECK_U64(get32(hdr(3) + 4), 0xDEADBEEFu, "PRDBC not written for NCQ (5.4.1)");
    CHECK(g_irq, "interrupt");

    pwr(0, AHCI_PxIS, 0xFFFFFFFFu);
    wr(AHCI_IS, 0xFFFFFFFFu);
    memset(g_ram + DATA2, 0, 8 * 512);
    put_fpdma(7, ATA_CMD_READ_FPDMA, lba, 8, 1, false);
    put_prd(0, DATA2, 8 * 512 - 1);
    run_ncq(7);
    CHECK(!memcmp(g_ram + DATA2, g_ram + DATA, 3 * 512) &&
          !memcmp(g_ram + DATA2 + 3 * 512, g_ram + DATA + 0x4000, 5 * 512),
          "READ FPDMA returns what was written");
    CHECK(get32(sdb + 4) == 1u << 7 && prd(0, AHCI_PxSACT) == 0, "completion for tag 7");

    flushes = g_disk->stats.flush_ops;
    put_fpdma(1, ATA_CMD_WRITE_FPDMA, 9, 1, 1, true);
    put_prd(0, DATA, 511);
    run_ncq(1);
    CHECK_U64(g_disk->stats.flush_ops - flushes, 1, "FUA write is flushed with the write cache on");
    put_fpdma(1, ATA_CMD_WRITE_FPDMA, 9, 1, 1, false);
    run_ncq(1);
    CHECK_U64(g_disk->stats.flush_ops - flushes, 1, "plain write is not flushed");
    CHECK_U64(prd(0, AHCI_PxTFD), 0x40, "both writes completed");

    /* Feature 为 0 表示 65536 个扇区：从倒数第 65535 个扇区开始就越界 */
    put_fpdma(2, ATA_CMD_READ_FPDMA, DISK_SECTORS - 65535, 0, 1, false);
    run_ncq(2);
    CHECK_U64(prd(0, AHCI_PxTFD), 0x0441, "count 0 means 65536 sectors: out of range, ABRT");
}

static void case_ncq_error(void)
{
    const uint8_t *d2h = g_ram + FB + AHCI_RFIS_RFIS;
    const uint8_t *sdb = g_ram + FB + AHCI_RFIS_SDBFIS;
    const uint8_t *log = g_ram + DATA2;
    int fd;

    T_CASE("NCQ 出错：设备停止接受命令，读出日志 10h（带出错的 tag）后恢复");
    fresh();
    port0_start();
    put_fpdma(4, ATA_CMD_READ_FPDMA, DISK_SECTORS, 1, 1, false);
    put_prd(0, DATA, 511);
    run_ncq(4);
    CHECK_U64(prd(0, AHCI_PxTFD), 0x0441, "acceptance error: ERR, ABRT (7.28.6)");
    CHECK(d2h[0] == 0x34 && (d2h[1] & SATA_FIS_I) && d2h[2] == 0x41 && d2h[3] == 0x04,
          "reported in a D2H Register FIS with I=1");
    CHECK(prd(0, AHCI_PxIS) & AHCI_PxIS_TFES, "TFES");
    CHECK_U64(prd(0, AHCI_PxCI), 1u << 4, "slot kept in PxCI, port halted");

    port0_restart();
    CHECK_U64(prd(0, AHCI_PxSACT), 0, "ST 1->0 clears PxSACT");
    put_rw(0, ATA_CMD_READ_DMA_EXT, 0, 1, 1);
    put_prd(0, DATA, 511);
    run(0);
    CHECK_U64(prd(0, AHCI_PxTFD), 0x0441, "other commands aborted until log 10h is read");
    port0_restart();
    put_log(0, ATA_CMD_READ_LOG_EXT, ATA_LOG_DIRECTORY, 0, 1, 1);
    put_prd(0, DATA2, 511);
    run(0);
    CHECK_U64(prd(0, AHCI_PxTFD), 0x0441, "even reading another log is aborted");

    port0_restart();
    memset(g_ram + FB, 0xEE, AHCI_RFIS_SIZE);
    memset(g_ram + DATA2, 0xEE, 512);
    put_log(0, ATA_CMD_READ_LOG_EXT, ATA_LOG_NCQ_ERR, 0, 1, 1);
    put_prd(0, DATA2, 511);
    run(0);
    CHECK(sdb[0] == 0xA1 && !(sdb[1] & SATA_FIS_I) && sdb[2] == 0x40 && sdb[3] == 0 &&
          get32(sdb + 4) == 0xFFFFFFFFu,
          "reading log 10h first sends SDB ACT=FFFFFFFFh, I=0, no error (13.6.3.4)");
    CHECK_U64(prd(0, AHCI_PxCI), 0, "READ LOG EXT completed");
    CHECK_U64(prd(0, AHCI_PxTFD), 0x40, "PxTFD = DRDY");
    CHECK(prd(0, AHCI_PxIS) & AHCI_PxIS_PSS, "PIO data-in");
    CHECK_U64(log[0], 4, "log 10h: NQ=0, tag 4");
    CHECK(log[2] == 0x41 && log[3] == 0x04, "log 10h: status 41h, error ABRT");
    CHECK(log[4] == (uint8_t)DISK_SECTORS && log[5] == (uint8_t)(DISK_SECTORS >> 8) &&
          log[6] == (uint8_t)(DISK_SECTORS >> 16) && log[8] == (uint8_t)(DISK_SECTORS >> 24) &&
          log[7] == ATA_DEV_LBA && log[12] == 4 << 3,
          "log 10h: LBA, Device and Count of the failed command");
    CHECK_U64(sum512(log), 0, "log 10h checksum");

    put_fpdma(5, ATA_CMD_READ_FPDMA, 0, 1, 1, false);
    put_prd(0, DATA, 511);
    run_ncq(5);
    CHECK(prd(0, AHCI_PxTFD) == 0x40 && prd(0, AHCI_PxSACT) == 0, "queued commands accepted again");
    memset(g_ram + FB, 0xEE, AHCI_RFIS_SIZE);
    put_log(0, ATA_CMD_READ_LOG_EXT, ATA_LOG_NCQ_ERR, 0, 1, 1);
    put_prd(0, DATA2, 511);
    run(0);
    CHECK_U64(sdb[0], 0xEE, "no SDB FIS when no error is pending");
    CHECK_U64(log[0], 4, "log still reports the first error until the next one");

    /* 执行阶段出错：镜像在运行中被截短，后端读到文件尾报 EIO */
    g_small = open_disk("small", 1 << 20, false, &fd);
    fresh_with(g_small);
    port0_start();
    if (ftruncate(fd, 512 << 10) != 0)
        perror("ftruncate");
    put_fpdma(6, ATA_CMD_READ_FPDMA, 1500, 8, 1, false);
    put_prd(0, DATA, 8 * 512 - 1);
    run_ncq(6);
    CHECK(sdb[0] == 0xA1 && (sdb[1] & SATA_FIS_I) && sdb[2] == 0x41 && sdb[3] == ATA_ERR_UNC &&
          get32(sdb + 4) == 0, "execution error: SDB FIS with ERR, UNC, ACT=0, I=1");
    CHECK_U64(prd(0, AHCI_PxTFD), 0x4041, "PxTFD = UNC, DRDY|ERR");
    CHECK(prd(0, AHCI_PxIS) & AHCI_PxIS_TFES, "TFES");
    CHECK_U64(prd(0, AHCI_PxSACT), 1u << 6, "failed tag stays in PxSACT");
    CHECK_U64(prd(0, AHCI_PxCI), 0, "already accepted: not in PxCI");
    port0_restart();
    put_log(0, ATA_CMD_READ_LOG_EXT, ATA_LOG_NCQ_ERR, 0, 1, 1);
    put_prd(0, DATA2, 511);
    run(0);
    CHECK(log[0] == 6 && log[3] == ATA_ERR_UNC && sum512(log) == 0, "log 10h: tag 6, UNC");

    /* COMRESET 清掉出错状态，日志回到 NQ=1。先发一条普通命令：日志 10h
     * 在出错状态下本来就能读，只读它看不出出错状态有没有清掉 */
    put_fpdma(6, ATA_CMD_READ_FPDMA, 1 << 20, 1, 1, false);
    run_ncq(6);
    pwr(0, AHCI_PxSCTL, 1);
    pwr(0, AHCI_PxSCTL, 0);
    port0_restart();
    put_fpdma(2, ATA_CMD_READ_FPDMA, 0, 1, 1, false);
    put_prd(0, DATA, 511);
    run_ncq(2);
    CHECK(prd(0, AHCI_PxTFD) == 0x40 && prd(0, AHCI_PxSACT) == 0,
          "after COMRESET: queued commands accepted without reading the log");
    put_log(0, ATA_CMD_READ_LOG_EXT, ATA_LOG_NCQ_ERR, 0, 1, 1);
    put_prd(0, DATA2, 511);
    run(0);
    CHECK((log[0] & ATA_LOG_NCQ_NQ) && sum512(log) == 0, "after COMRESET: log 10h has NQ=1");
    close(fd);
}

static void case_logs(void)
{
    const uint8_t *dir = g_ram + DATA2;
    const uint8_t *ps = g_ram + FB + AHCI_RFIS_PSFIS;
    const uint8_t *d2h = g_ram + FB + AHCI_RFIS_RFIS;
    static const uint8_t zero[512];
    int i, bad = 0;

    T_CASE("GPL 日志：目录、主机日志的读写、非法参数中止");
    fresh();
    port0_start();
    put_log(0, ATA_CMD_READ_LOG_EXT, ATA_LOG_DIRECTORY, 0, 1, 1);
    put_prd(0, DATA2, 511);
    run(0);
    CHECK(dir[0] == 1 && dir[1] == 0, "directory version 0001h");
    CHECK(dir[2 * 0x10] == 1 && dir[2 * 0x10 + 1] == 0, "log 10h: 1 page");
    for (i = 0x80; i <= 0x9F; i++)
        bad += dir[2 * i] != 16 || dir[2 * i + 1] != 0;
    CHECK_U64(bad, 0, "logs 80h-9Fh: 16 pages each");
    CHECK(dir[2 * 0x11] == 0 && dir[2 * 0x30] == 0 && dir[2 * 0xE0] == 0, "other logs absent");

    fill(g_ram + DATA, 1024, 33);
    put_log(0, ATA_CMD_WRITE_LOG_EXT, 0x85, 3, 2, 2);
    put_prd(0, DATA, 299);
    put_prd(1, DATA + 0x1000, 723);
    memcpy(g_ram + DATA + 0x1000, g_ram + DATA + 300, 724);
    memset(g_ram + FB, 0, AHCI_RFIS_SIZE);
    put32(hdr(0) + 4, 0);
    pwr(0, AHCI_PxIS, 0xFFFFFFFFu);              /* 清掉上一条读目录留下的 PSS */
    run(0);
    CHECK_U64(prd(0, AHCI_PxCI), 0, "WRITE LOG EXT completed");
    CHECK(ps[0] == 0x5F && !(ps[1] & (SATA_FIS_PIO_D | SATA_FIS_I)),
          "PIO Setup FIS for data-out: D=0, I=0");
    CHECK(d2h[0] == 0x34 && (d2h[1] & SATA_FIS_I) && d2h[2] == 0x40,
          "ends with a D2H Register FIS, I=1");
    CHECK_U64(prd(0, AHCI_PxIS) & (AHCI_PxIS_DHRS | AHCI_PxIS_PSS), AHCI_PxIS_DHRS, "DHRS, no PSS");
    CHECK_U64(get32(hdr(0) + 4), 1024, "PRDBC = 1024");

    memset(g_ram + DATA2, 0, 2048);
    put_log(0, ATA_CMD_READ_LOG_EXT, 0x85, 2, 3, 1);
    put_prd(0, DATA2, 3 * 512 - 1);
    run(0);
    CHECK(!memcmp(g_ram + DATA2, zero, 512), "page 2 of log 85h still empty");
    CHECK(!memcmp(g_ram + DATA2 + 512, g_ram + DATA, 300) &&
          !memcmp(g_ram + DATA2 + 812, g_ram + DATA + 0x1000, 724),
          "pages 3-4 read back what was written");

    struct { uint8_t cmd, log; unsigned page, count; const char *what; } bad_cmds[] = {
        { ATA_CMD_READ_LOG_EXT,  0x80, 0, 0,  "count 0" },
        { ATA_CMD_READ_LOG_EXT,  0x80, 15, 2, "page + count beyond the log size" },
        { ATA_CMD_READ_LOG_EXT,  0x10, 1, 1,  "page 1 of a one-page log" },
        { ATA_CMD_READ_LOG_EXT,  0x11, 0, 1,  "unsupported log" },
        { ATA_CMD_WRITE_LOG_EXT, 0x10, 0, 1,  "write to read-only log 10h" },
        { ATA_CMD_WRITE_LOG_EXT, 0x00, 0, 1,  "write to the directory" },
    };
    for (i = 0; i < (int)(sizeof(bad_cmds) / sizeof(bad_cmds[0])); i++) {
        port0_restart();
        put_log(0, bad_cmds[i].cmd, bad_cmds[i].log, bad_cmds[i].page, bad_cmds[i].count, 1);
        put_prd(0, DATA2, 8191);
        run(0);
        CHECK_U64(prd(0, AHCI_PxTFD), 0x0441, bad_cmds[i].what);
    }
}

int main(int argc, char **argv)
{
    T_MAIN_BEGIN();
    g_disk = open_disk("d0", DISK_SIZE, false, &g_img);
    case_identity();
    case_power_on();
    case_comreset();
    case_hba_reset();
    case_irq();
    case_cmd_abort();
    case_cmd_bus_errors();
    case_register_bits();
    case_identify_data();
    case_identify_cmd();
    case_rw();
    case_rw_errors();
    case_prdt();
    case_features();
    case_ncq();
    case_ncq_error();
    case_logs();
    free(S);
    free_disk(g_disk);
    free_disk(g_big);
    free_disk(g_ro);
    free_disk(g_small);
    close(g_img);
    rmdir(g_dir);
    return T_SUMMARY();
}
