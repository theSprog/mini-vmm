/* include/devices/ahci.h — AHCI SATA 控制器（--device ahci）        [emu-blk]
 *
 * 选它的理由：AHCI 是 PC 上 SATA 控制器的标准接口，Linux 的 ahci 驱动按
 * PCI class 绑定任何厂商的实现；它的命令列表、FIS 接收区、PRDT 三层
 * 内存结构，正好和 e1000 的描述符环、以后 virtio-blk 的 virtqueue 对照。
 *
 * PCI 外观：vendor 0x1234（本项目），class 010601（大容量存储 / SATA /
 * AHCI 1.0），INTA 和单向量 MSI。寄存器在 BAR5（ABAR，AHCI 规范第 2.1.11 节规定的
 * 位置，Linux 的 AHCI_PCI_BAR_STANDARD）。
 *   - 用自己的 vendor ID 而不冒充 Intel ICH9：Linux 对 Intel 控制器在复位
 *     之后会改写配置空间 0x92（ahci_intel_pcs_quirk），那是 ICH 私有的
 *     端口使能寄存器，冒充 Intel 就得一并模拟它；按 class 匹配的通用
 *     条目（board_ahci）没有任何厂商相关的分支
 *   - ABAR 8 KiB：覆盖 32 个端口时的完整寄存器映射（到 0x10FF，规范
 *     第 3 节的表），端口数以后增加不用改 BAR 大小
 *
 * 能力（CAP，第 3.1.1 节）：6 个端口（PI = 0x3F），32 个命令槽，64 位
 * 寻址，NCQ，Gen3 速率，只支持 AHCI 模式（GHC.AE 只读为 1）。不报告
 * 端口倍增器、链路省电、交错上电、命令合并、机箱管理：这些位为 0，
 * 驱动就不会使用对应的寄存器和流程。
 *
 * 端口 0 接 --drive 指定的磁盘（没有 drive= 时为空），其余 5 个端口永远
 * 没有设备：它们完整地走一遍 COMRESET，只是收不到设备的 COMINIT，链路
 * 停在 DET=0，guest 由此看到“端口存在但没插盘”，这正是端口枚举要演示的。
 *
 * 链路是瞬时建立的：写 PxSCTL.DET=1 立刻断链，写回 0 时有设备的端口立刻
 * 进入 DET=3，同时“收到”设备的复位签名 FIS。真实硬件这里要几毫秒的 OOB
 * 序列，驱动只是轮询 PxSSTS/PxTFD 等它完成，不依赖它花了多长时间。
 *
 * 命令在写 PxCI 的 vCPU 线程上同步执行，执行完才从这次 MMIO 写返回，
 * 所以 guest 看到的 PxCI 位要么还没置上，要么已经完成清掉；PxCMD.CR
 * 只在 ST=1 且端口没有因错误停下时为 1。磁盘读写也在这里同步完成
 * （blk_preadv/blk_pwritev），这期间 vCPU 停在这次 exit 里，其他设备
 * 也拿不到 io_lock。异步块 I/O 是 blk-io-uring 里程碑的内容，先保留同步
 * 这条慢路径，以后才有加速前后的对照。
 *
 * 盘（ATA8-ACS）：512 字节扇区，LBA28 和 48 位地址，DMA 读写，volatile
 * 写缓存（FLUSH CACHE 落到 fdatasync）。实现的命令只有 Linux libata
 * 对这样一块盘实际会发的那些，见 ahci.c 的 exec_ata；其余命令按 ATA8-ACS
 * 第 4.1 节对“不支持的命令”的规定中止（ABRT），每个操作码记一次日志。
 * IDENTIFY DEVICE 里不声明 HPA、SMART、安全、链路省电：声明了 Linux 就会
 * 发对应的命令（SET FEATURES 10h 等）。
 *
 * NCQ（CAP.SNCQ，IDENTIFY 字 75/76，队列深度 32）：READ/WRITE FPDMA QUEUED
 * 按 SATA 3.0 第 13.6 节的协议走，软件先置 PxSACT 再置 PxCI；设备接受
 * 命令时回一个 I=0 的 D2H FIS（清 PxCI），完成时回 Set Device Bits FIS
 * （ACT 里是完成的 tag，清 PxSACT，置 PxIS.SDBS）。因为命令在写 PxCI 时
 * 同步执行完，同一时刻在设备里的 NCQ 命令最多一条，队列真正被填满要等
 * 异步后端（blk-io-uring）；协议本身、PxSACT 与 PxCI 的分工、SDB FIS 和
 * 出错后的恢复流程都与真实盘一样。NCQ 命令出错时设备停止接受命令，直到
 * 软件用 READ LOG EXT 读出 NCQ Command Error 日志（10h），ATA8-ACS 第
 * 4.15.1 节因此要求 NCQ 盘支持 GPL（字 84/87 bit 5）；GPL 又要求支持日志
 * 目录（00h）、WRITE LOG EXT 和全部 Host Vendor Specific 日志（80h-9Fh，
 * 第 4.10 节），所以这些一并实现，主机日志只存在内存里，不随镜像保存。
 * PRDT 的每一项都单独经 pci_dma_map 检查后才读写，见 ahci.c 的 prdt_map。
 *
 * 中断（第 10.7 节）：IS 寄存器是两种中断方式共同的来源。INTx 是电平：
 * GHC.IE 为 1 且 IS 非零时线有效。MSI 是消息，按第 10.7.2.1 节和图 22，
 * IS 的每次变化只要结果不是全零就发一条：有新位置上、软件清掉一部分位，
 * 都算变化。软件清掉的位因为端口的“虚拟线”还是 1 而立刻被重新置上，图 22
 * 没有单列，这里按“任何变化”的总规则也算一次变化，理由见下一段。
 * 只在“IS 从 0 变非 0”时发是错的：驱动先清 PxIS、再清 IS，两步之间如果有
 * 命令完成，端口线重新变 1，IS 那一位一直没变成 0，不会再出现上升沿，这次
 * 完成的中断就丢了；按“清掉后被重新置上也发一条”的规则，驱动写 IS 的那一刻
 * 正好补发。命令在发起它的 vCPU 上同步执行时，这个窗口碰不到：Linux 发命令
 * 和整段中断处理（包括写 IS）持着同一把 host->lock，完成只发生在发命令的
 * 那次 MMIO 写里。完成由 I/O 线程产生（异步后端）时窗口就真实存在，所以
 * 这里按规范实现，不依赖执行方式。GHC.IE 从 0 变 1 时 IS 已经非零，也发一条，
 * 对应 INTx 下线在这一刻变为有效。只支持单向量，所以 GHC.MRSM 为 0、
 * 多向量的端口映射（第 10.7.2.2 节）不适用。
 *
 * 线程与锁：所有状态由 vm->io_lock 保护，寄存器访问经 exit 分发进来，
 * 本来就持着这把锁；这个设备没有在 I/O 线程上运行的回调。
 *
 * 参考：ref/serial-ata-ahci-spec-rev1-3-1.pdf 第 3 章（寄存器）、第 4.2 节
 * （命令列表、FIS 接收区、命令表，PRD 见第 4.2.3.3 节）、第 5.3.8/5.3.9 节
 * （D2H 与 PIO Setup FIS 之后何时清 PxCI）、第 5.4.1 节（PRDBC）、第 6 章
 * （错误，溢出见第 6.1.5 节）、第 10.4 节（复位）、第 10.7 节（中断，MSI
 * 见第 10.7.2.1 节与图 22）；
 * ref/SerialATA_Revision_3_0_Gold.pdf 第 10.3 节（FIS 格式）、第 11.3 节
 * （复位签名）；ref/ATA8-ACS_d1699r4a.pdf 第 4.1 节（不支持的命令）、表 22
 * 与第 7.16 节（IDENTIFY DEVICE）、第 7.24/7.25/7.61/7.62 节（DMA 读写）、
 * 第 7.14/7.15 节（FLUSH CACHE）、第 7.48 节（SET FEATURES）、第 7.55 节
 * （STANDBY IMMEDIATE）、表 92（签名）、表 113/131（读写的错误位）、
 * 第 4.10/4.15 节与第 7.28/7.67 节（GPL、NCQ 与 FPDMA 命令）、第 7.29/7.68
 * 节与表 A.2（READ/WRITE LOG EXT、日志目录）；SATA 3.0 第 13.6 节（NCQ
 * 协议）、第 13.7.3 节（日志 10h）；AHCI 第 3.3.13 节（PxSACT）、第
 * 5.3.13.1 节（SDB FIS）；
 * Linux drivers/ata/ahci.c、libahci.c、libata-core.c。
 */
#ifndef DEVICES_AHCI_H
#define DEVICES_AHCI_H

#include <stdbool.h>
#include <stdint.h>

#include "pci/pci.h"
#include "devices/device.h"

struct blockdev;

#define PCI_DEVICE_ID_AHCI   0x0003
#define AHCI_CLASS_CODE      0x010601
#define AHCI_ABAR            5
#define AHCI_ABAR_SIZE       0x2000
#define AHCI_NR_PORTS        6
#define AHCI_NR_SLOTS        32

/* 全局寄存器（第 3.1 节） */
#define AHCI_CAP             0x00
#define AHCI_GHC             0x04
#define AHCI_IS              0x08
#define AHCI_PI              0x0C
#define AHCI_VS              0x10
#define AHCI_CCC_CTL         0x14
#define AHCI_CCC_PORTS       0x18
#define AHCI_EM_LOC          0x1C
#define AHCI_EM_CTL          0x20
#define AHCI_CAP2            0x24
#define AHCI_BOHC            0x28

#define AHCI_CAP_S64A        (1u << 31)
#define AHCI_CAP_SNCQ        (1u << 30)
#define AHCI_CAP_SCLO        (1u << 24)
#define AHCI_CAP_ISS_SHIFT   20
#define AHCI_CAP_SAM         (1u << 18)
#define AHCI_CAP_PMD         (1u << 15)
#define AHCI_CAP_NCS_SHIFT   8           /* 0 起算的槽数 */

#define AHCI_GHC_AE          (1u << 31)
#define AHCI_GHC_IE          (1u << 1)
#define AHCI_GHC_HR          (1u << 0)

#define AHCI_VS_1_3_1        0x00010301

/* 接口速率代号，PxSSTS.SPD / PxSCTL.SPD / CAP.ISS 共用（第 3.3.10 节） */
#define AHCI_SPEED_GEN3      3

/* 端口寄存器（第 3.3 节），端口 i 在 0x100 + i * 0x80 */
#define AHCI_PORT_BASE       0x100
#define AHCI_PORT_SIZE       0x80
#define AHCI_PxCLB           0x00
#define AHCI_PxCLBU          0x04
#define AHCI_PxFB            0x08
#define AHCI_PxFBU           0x0C
#define AHCI_PxIS            0x10
#define AHCI_PxIE            0x14
#define AHCI_PxCMD           0x18
#define AHCI_PxTFD           0x20
#define AHCI_PxSIG           0x24
#define AHCI_PxSSTS          0x28
#define AHCI_PxSCTL          0x2C
#define AHCI_PxSERR          0x30
#define AHCI_PxSACT          0x34
#define AHCI_PxCI            0x38
#define AHCI_PxSNTF          0x3C
#define AHCI_PxFBS           0x40
#define AHCI_PxDEVSLP        0x44

/* PxIS / PxIE 的位（第 3.3.5、3.3.6 节） */
#define AHCI_PxIS_TFES       (1u << 30)
#define AHCI_PxIS_HBFS       (1u << 29)
#define AHCI_PxIS_HBDS       (1u << 28)
#define AHCI_PxIS_IFS        (1u << 27)
#define AHCI_PxIS_INFS       (1u << 26)
#define AHCI_PxIS_OFS        (1u << 24)
#define AHCI_PxIS_IPMS       (1u << 23)
#define AHCI_PxIS_PRCS       (1u << 22)  /* 只读，镜像 PxSERR.DIAG.N */
#define AHCI_PxIS_PCS        (1u << 6)   /* 只读，镜像 PxSERR.DIAG.X */
#define AHCI_PxIS_DPS        (1u << 5)
#define AHCI_PxIS_UFS        (1u << 4)
#define AHCI_PxIS_SDBS       (1u << 3)
#define AHCI_PxIS_DSS        (1u << 2)
#define AHCI_PxIS_PSS        (1u << 1)
#define AHCI_PxIS_DHRS       (1u << 0)

/* PxCMD 的位（第 3.3.7 节） */
#define AHCI_PxCMD_ICC_MASK  (0xFu << 28)
#define AHCI_PxCMD_DLAE      (1u << 25)
#define AHCI_PxCMD_ATAPI     (1u << 24)
#define AHCI_PxCMD_CR        (1u << 15)
#define AHCI_PxCMD_FR        (1u << 14)
#define AHCI_PxCMD_CCS_SHIFT 8
#define AHCI_PxCMD_CCS_MASK  (0x1Fu << AHCI_PxCMD_CCS_SHIFT)
#define AHCI_PxCMD_FRE       (1u << 4)
#define AHCI_PxCMD_CLO       (1u << 3)
#define AHCI_PxCMD_POD       (1u << 2)
#define AHCI_PxCMD_SUD       (1u << 1)
#define AHCI_PxCMD_ST        (1u << 0)

/* PxSSTS / PxSCTL 的字段（第 3.3.10、3.3.11 节） */
#define AHCI_SSTS_DET_NONE      0x0
#define AHCI_SSTS_DET_PHY       0x3      /* 有设备且 Phy 通信建立 */
#define AHCI_SSTS_DET_OFFLINE   0x4
#define AHCI_SSTS_IPM_ACTIVE    0x1
#define AHCI_SCTL_DET_MASK      0xF
#define AHCI_SCTL_DET_COMRESET  0x1
#define AHCI_SCTL_DET_OFFLINE   0x4
#define AHCI_SCTL_SPD_SHIFT     4

/* PxSERR.DIAG 里本模型会置的位（第 3.3.12 节） */
#define AHCI_SERR_DIAG_X     (1u << 26)  /* 收到 COMINIT：设备存在性变了 */
#define AHCI_SERR_DIAG_N     (1u << 16)  /* PhyRdy 变了 */
#define AHCI_SERR_ERR_E      (1u << 11)  /* HBA 内部错误，含访问内存失败 */

/* 接收 FIS 区（第 4.2.1 节），256 字节 */
#define AHCI_RFIS_SIZE       256
#define AHCI_RFIS_PSFIS      0x20
#define AHCI_RFIS_RFIS       0x40
#define AHCI_RFIS_SDBFIS     0x58

/* 命令头（第 4.2.2 节），每槽 32 字节，命令列表共 1 KiB */
#define AHCI_CMDHDR_SIZE     32
#define AHCI_CH_CFL_MASK     0x1Fu       /* CFIS 长度，单位 dword */
#define AHCI_CH_ATAPI        (1u << 5)
#define AHCI_CH_WRITE        (1u << 6)
#define AHCI_CH_RESET        (1u << 8)
#define AHCI_CH_BIST         (1u << 9)
#define AHCI_CH_CLEAR_BSY    (1u << 10)
#define AHCI_CH_PRDTL_SHIFT  16

/* 命令表（第 4.2.3 节） */
#define AHCI_CT_CFIS         0x00
#define AHCI_CT_ACMD         0x40
#define AHCI_CT_PRDT         0x80
#define AHCI_PRD_SIZE        16
#define AHCI_PRD_DBC_MASK    0x3FFFFFu   /* DW3[21:0]，字节数减 1 */

/* FIS 类型与格式（SATA 3.0 第 10.3 节） */
#define SATA_FIS_REG_H2D     0x27
#define SATA_FIS_REG_D2H     0x34
#define SATA_FIS_PIO_SETUP   0x5F
#define SATA_FIS_SDB         0xA1        /* Set Device Bits，第 10.3.6 节 */
#define SATA_FIS_H2D_C       (1u << 7)   /* byte 1：这是命令（不是设备控制） */
#define SATA_FIS_I           (1u << 6)   /* D2H / PIO Setup byte 1：中断位 */
#define SATA_FIS_PIO_D       (1u << 5)   /* PIO Setup byte 1：数据方向设备到主机 */
#define SATA_FIS_REG_LEN     20
#define SATA_FIS_PIO_LEN     20
#define SATA_FIS_SDB_LEN     8

/* ATA 状态与错误寄存器的位（ATA8-ACS 第 6.1、6.2 节） */
#define ATA_STS_BSY          0x80
#define ATA_STS_DRDY         0x40
#define ATA_STS_DRQ          0x08
#define ATA_STS_ERR          0x01
#define ATA_ERR_UNC          0x40
#define ATA_ERR_IDNF         0x10
#define ATA_ERR_ABRT         0x04

/* Device 寄存器的 bit 6（ATA8-ACS 的 Device 字段 bit 14“shall be set to
 * one”，即 LBA 寻址）；28 位命令的 LBA[27:24] 在它的低 4 位 */
#define ATA_DEV_LBA          0x40
#define ATA_DEV_FUA          0x80        /* FPDMA 命令的 Device bit 7（第 7.28.3.2 节） */

/* 实现的 ATA 命令（ATA8-ACS 第 7 章） */
#define ATA_CMD_READ_DMA_EXT    0x25
#define ATA_CMD_READ_LOG_EXT    0x2F
#define ATA_CMD_WRITE_DMA_EXT   0x35
#define ATA_CMD_WRITE_LOG_EXT   0x3F
#define ATA_CMD_READ_FPDMA      0x60
#define ATA_CMD_WRITE_FPDMA     0x61
#define ATA_CMD_READ_DMA        0xC8
#define ATA_CMD_WRITE_DMA       0xCA
#define ATA_CMD_STANDBY_NOW     0xE0
#define ATA_CMD_FLUSH_CACHE     0xE7
#define ATA_CMD_FLUSH_CACHE_EXT 0xEA
#define ATA_CMD_IDENTIFY        0xEC
#define ATA_CMD_SET_FEATURES    0xEF

/* SET FEATURES 子命令（Feature 字段，表 39）与传输模式（Count 字段，表 40） */
#define ATA_SF_WCACHE_ON     0x02
#define ATA_SF_XFER_MODE     0x03
#define ATA_SF_WCACHE_OFF    0x82
#define ATA_XFER_PIO         0x08        /* 00001b：PIO 流控模式 n */
#define ATA_XFER_MWDMA       0x20        /* 00100b：Multiword DMA 模式 n */
#define ATA_XFER_UDMA        0x40        /* 01000b：Ultra DMA 模式 n */

#define ATA_SECTOR_SIZE      512
#define ATA_ID_WORDS         256
#define ATA_NCQ_DEPTH        32

/* GPL 日志地址（ATA8-ACS 表 A.2）。日志按 512 字节的页读写 */
#define ATA_LOG_DIRECTORY    0x00
#define ATA_LOG_NCQ_ERR      0x10        /* SATA 3.0 第 13.7.3 节，1 页，只读 */
#define ATA_LOG_HOST_FIRST   0x80        /* Host Vendor Specific，80h-9Fh 可读写 */
#define ATA_LOG_HOST_LAST    0x9F
#define ATA_LOG_HOST_PAGES   16
#define ATA_LOG_NCQ_NQ       0x80        /* 日志 10h 字节 0：最近出错的不是 NCQ 命令 */

/* 复位后的 PxTFD.STS（第 10.4.2 节） */
#define AHCI_TFD_STS_RESET   0x7F

struct ahci_port {
    uint32_t clb, clbu, fb, fbu;
    uint32_t is;                   /* 只存可写清的位，PCS/PRCS 读时由 serr 算出 */
    uint32_t ie;
    uint32_t cmd;                  /* 只存软件可写的位，CR/FR/CCS 读时算出 */
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint8_t  ccs;                  /* 当前（或出错时）处理的槽 */
    bool     halted;               /* 第 6.2.2 节的 ERR:Fatal：等软件重启 ST */
    bool     has_disk;
};

struct ahci {
    struct pci_device pci;
    struct vmm_vm    *vm;
    struct blockdev  *blk;         /* 端口 0 的磁盘，可以为 NULL */

    uint32_t ghc;                  /* 只存 IE，AE 恒为 1，HR 写入时同步完成 */
    uint32_t is;
    struct ahci_port port[AHCI_NR_PORTS];

    uint32_t warned;               /* warn_once 的位图，见 ahci.c */
    uint64_t cmd_warned[4];        /* 已经记过日志的不支持操作码，256 位 */

    /* 盘的状态。SET FEATURES 改的设置在复位（COMRESET）后回到上电默认值：
     * 不支持 Software Settings Preservation（IDENTIFY 字 78 bit 6 为 0），
     * ATA8-ACS 第 7.48.2 节由此规定复位后的值由设备自定 */
    uint64_t sectors;              /* 容量，512 字节扇区数 */
    bool     wcache;               /* volatile 写缓存开着 */
    uint8_t  dma_mode;             /* 选中的 DMA 模式：SET FEATURES 03h 的 Count 值 */

    /* NCQ 出错之后、日志 10h 被读出之前为真：这期间除读日志 10h 外的
     * 命令都中止（SATA 3.0 第 13.6.3.4 节）。COMRESET 清掉它 */
    bool     ncq_err;
    uint8_t  ncq_log[ATA_SECTOR_SIZE];
    uint8_t  host_log[ATA_LOG_HOST_LAST - ATA_LOG_HOST_FIRST + 1]
                     [ATA_LOG_HOST_PAGES][ATA_SECTOR_SIZE];
};

extern const struct vmm_device_type ahci_device_type;

/* 以下供单元测试直接驱动，guest 走的是 BAR 回调。ahci_init 不注册总线，
 * 相当于上电：寄存器取复位值，有设备的端口完成一次 COMRESET。 */
void     ahci_init(struct ahci *s, struct blockdev *blk);
uint32_t ahci_reg_read(struct ahci *s, uint32_t off);
void     ahci_reg_write(struct ahci *s, uint32_t off, uint32_t val);

/* 按当前设置生成 IDENTIFY DEVICE 数据（256 个字，主机字节序）。 */
void     ahci_identify(const struct ahci *s, uint16_t id[ATA_ID_WORDS]);

#endif /* DEVICES_AHCI_H */
