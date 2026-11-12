/* include/devices/ahci.h — AHCI SATA 控制器（--device ahci）        [emu-blk]
 *
 * 选它的理由：AHCI 是 PC 上 SATA 控制器的标准接口，Linux 的 ahci 驱动按
 * PCI class 绑定任何厂商的实现；它的命令列表、FIS 接收区、PRDT 三层
 * 内存结构，正好和 e1000 的描述符环、以后 virtio-blk 的 virtqueue 对照。
 *
 * PCI 外观：vendor 0x1234（本项目），class 010601（大容量存储 / SATA /
 * AHCI 1.0），INTA。寄存器在 BAR5（ABAR，AHCI 规范第 2.1.11 节规定的
 * 位置，Linux 的 AHCI_PCI_BAR_STANDARD）。
 *   - 用自己的 vendor ID 而不冒充 Intel ICH9：Linux 对 Intel 控制器在复位
 *     之后会改写配置空间 0x92（ahci_intel_pcs_quirk），那是 ICH 私有的
 *     端口使能寄存器，冒充 Intel 就得一并模拟它；按 class 匹配的通用
 *     条目（board_ahci）没有任何厂商相关的分支
 *   - ABAR 8 KiB：覆盖 32 个端口时的完整寄存器映射（到 0x10FF，规范
 *     第 3 节的表），端口数以后增加不用改 BAR 大小
 *
 * 能力（CAP，第 3.1.1 节）：6 个端口（PI = 0x3F），32 个命令槽，64 位
 * 寻址，Gen3 速率，只支持 AHCI 模式（GHC.AE 只读为 1）。不报告 NCQ、
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
 * 只在 ST=1 且端口没有因错误停下时为 1。
 *
 * 线程与锁：所有状态由 vm->io_lock 保护，寄存器访问经 exit 分发进来，
 * 本来就持着这把锁；这个设备没有在 I/O 线程上运行的回调。
 *
 * 参考：ref/serial-ata-ahci-spec-rev1-3-1.pdf 第 3 章（寄存器）、第 4.2 节
 * （命令列表、FIS 接收区、命令表）、第 6 章（错误）、第 10.4 节（复位）、
 * 第 10.7 节（中断）；ref/SerialATA_Revision_3_0_Gold.pdf 第 10.3 节
 * （FIS 格式）、第 11.3 节（复位签名）；ref/ATA8-ACS_d1699r4a.pdf
 * 表 92（签名）；Linux drivers/ata/ahci.c、libahci.c。
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

/* FIS 类型与格式（SATA 3.0 第 10.3 节） */
#define SATA_FIS_REG_H2D     0x27
#define SATA_FIS_REG_D2H     0x34
#define SATA_FIS_PIO_SETUP   0x5F
#define SATA_FIS_H2D_C       (1u << 7)   /* byte 1：这是命令（不是设备控制） */
#define SATA_FIS_I           (1u << 6)   /* D2H / PIO Setup byte 1：中断位 */
#define SATA_FIS_REG_LEN     20

/* ATA 状态与错误寄存器的位（ATA8-ACS 第 6.1、6.2 节） */
#define ATA_STS_BSY          0x80
#define ATA_STS_DRDY         0x40
#define ATA_STS_DRQ          0x08
#define ATA_STS_ERR          0x01
#define ATA_ERR_ABRT         0x04

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
};

extern const struct vmm_device_type ahci_device_type;

/* 以下供单元测试直接驱动，guest 走的是 BAR 回调。ahci_init 不注册总线，
 * 相当于上电：寄存器取复位值，有设备的端口完成一次 COMRESET。 */
void     ahci_init(struct ahci *s, struct blockdev *blk);
uint32_t ahci_reg_read(struct ahci *s, uint32_t off);
void     ahci_reg_write(struct ahci *s, uint32_t off, uint32_t val);

#endif /* DEVICES_AHCI_H */
