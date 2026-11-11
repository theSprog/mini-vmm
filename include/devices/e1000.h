/* include/devices/e1000.h — Intel 82540EM 千兆网卡（--device e1000）    [emu-net]
 *
 * 选它的理由：Linux 自带驱动（drivers/net/ethernet/intel/e1000），手册公开，
 * 而且它是“真硬件”的典型样子：寄存器窗口、描述符环、DMA、中断原因寄存器、
 * 外挂 EEPROM 和 PHY 都齐全，正好拿来和以后的 virtio-net 对照。
 *
 * PCI 外观（SDM 第 4.1 节）：8086:100E，class 020000（以太网），INTA。
 *   BAR0  32 位 MMIO，128 KiB，全部寄存器
 *   BAR1  不实现：手册里是可选的外挂 flash，Linux 驱动不用
 *   BAR2  IO，8 字节，IOADDR/IODATA 间接访问同一组寄存器。82540 必须有：
 *         Linux 的 e1000_reset_hw 对这一代芯片经 IO BAR 写 CTRL.RST
 *         （e1000_is_need_ioport），没有 IO BAR 时 probe 失败
 *
 * 寄存器（SDM 第 13 章）按 32 位建模：小于 32 位的写被忽略、读返回整个
 * 寄存器里对应的字节（第 13.2 节）。没有实现的寄存器读 0、写丢弃，每个
 * 偏移第一次被访问时记一条 warn，guest 驱动碰了什么新寄存器一眼可见。
 *
 * 模型做了这些简化，都不影响 guest 能观察到的协议：
 *   - 中断延迟（ITR、RDTR/RADV、TIDV/TADV）一律按 0 处理，事件立即置位
 *     ICR：这些定时器只是把多次中断合并，驱动不依赖它们何时到期
 *   - PHY 永远是 1000 Mb/s 全双工、自协商瞬间完成，链路始终 up
 *   - EEPROM 只支持 READ（Microwire 位操作和 EERD 两条路）；写命令被忽略
 *     并记一条 warn，镜像每次启动按 mac= 重新生成
 *   - RX 校验和 offload 不实现：每个 RX 描述符都置 IXSM（“忽略校验和
 *     指示”），驱动退回软件校验。Linux 驱动默认写 RXCSUM.TUOFL 打开
 *     这项 offload，但 e1000_rx_checksum() 见到 IXSM 直接返回，效果
 *     和没打开一样
 *   - 统计寄存器（0x4000-0x40FC）读 0：Linux 驱动的收发包数、字节数在
 *     软件里计，这组寄存器只汇入错误计数和 ethtool -S
 * TX 方向的 offload（scatter-gather、校验和插入、TSO）按手册实现，因为
 * Linux 驱动默认打开它们（e1000_probe 的 netdev->features），一旦不做，
 * guest 发出的 TCP 包校验和就是错的。
 *
 * 线程与锁：所有状态由 vm->io_lock 保护。寄存器访问经 exit 分发进来，
 * 本来就持着这把锁；RX 回调在 I/O 线程上，先拿 io_lock 再动设备状态。
 * TX 在写 TDT 的那个 vCPU 线程上同步完成。
 *
 * 参考：ref/8254x_GBe_SDM.pdf（Intel 317453）第 3 章（描述符与 offload）、
 * 第 5.6 节（EEPROM 映射）、第 8 章和第 13 章（MAC/PHY 寄存器）、第 14 章
 * （初始化）；ref/93XX46_EEPROM_DS21749H.pdf（Microwire 时序）。
 */
#ifndef DEVICES_E1000_H
#define DEVICES_E1000_H

#include <stdbool.h>
#include <stdint.h>

#include "pci/pci.h"
#include "devices/device.h"

struct netdev;

#define E1000_VENDOR_ID         0x8086
#define E1000_DEVICE_ID_82540EM 0x100E
#define E1000_SUBSYS_ID         0x1008   /* SDM 第 4.1 节：EEPROM 不加载时的默认值 */
#define E1000_MMIO_SIZE         0x20000
#define E1000_IO_SIZE           8
#define E1000_NREGS             (E1000_MMIO_SIZE / 4)

/* 默认 MAC，QEMU 也用它：0x52 的 bit1 是“本地管理地址”，bit0 为 0 是单播 */
#define E1000_DEFAULT_MAC       "52:54:00:12:34:56"

/* ---- 寄存器偏移（SDM 表 13-2） ---- */
#define E1000_CTRL      0x00000
#define E1000_STATUS    0x00008
#define E1000_EECD      0x00010
#define E1000_EERD      0x00014
#define E1000_CTRL_EXT  0x00018
#define E1000_MDIC      0x00020
#define E1000_FCAL      0x00028
#define E1000_FCAH      0x0002C
#define E1000_FCT       0x00030
#define E1000_VET       0x00038
#define E1000_ICR       0x000C0
#define E1000_ITR       0x000C4
#define E1000_ICS       0x000C8
#define E1000_IMS       0x000D0
#define E1000_IMC       0x000D8
#define E1000_RCTL      0x00100
#define E1000_FCTTV     0x00170
#define E1000_TCTL      0x00400
#define E1000_TIPG      0x00410
#define E1000_AIT       0x00458
#define E1000_LEDCTL    0x00E00
#define E1000_PBA       0x01000
#define E1000_FCRTL     0x02160
#define E1000_FCRTH     0x02168
#define E1000_RDBAL     0x02800
#define E1000_RDBAH     0x02804
#define E1000_RDLEN     0x02808
#define E1000_RDH       0x02810
#define E1000_RDT       0x02818
#define E1000_RDTR      0x02820
#define E1000_RXDCTL    0x02828
#define E1000_RADV      0x0282C
#define E1000_RSRPD     0x02C00
#define E1000_TDBAL     0x03800
#define E1000_TDBAH     0x03804
#define E1000_TDLEN     0x03808
#define E1000_TDH       0x03810
#define E1000_TDT       0x03818
#define E1000_TIDV      0x03820
#define E1000_TXDCTL    0x03828
#define E1000_TADV      0x0382C
#define E1000_TSPMT     0x03830
#define E1000_STATS_BEGIN 0x04000        /* CRCERRS */
#define E1000_STATS_END   0x04100        /* TSCTFC 之后，不含 */
#define E1000_RXCSUM    0x05000
#define E1000_MTA       0x05200          /* 128 个 */
#define E1000_RA        0x05400          /* 16 对 RAL/RAH */
#define E1000_VFTA      0x05600          /* 128 个 */
#define E1000_WUC       0x05800
#define E1000_WUFC      0x05808
#define E1000_WUS       0x05810
#define E1000_MANC      0x05820

#define E1000_MTA_N     128
#define E1000_RA_N      16
#define E1000_VFTA_N    128

/* CTRL（第 13.4.1 节） */
#define E1000_CTRL_FD       (1u << 0)
#define E1000_CTRL_LRST     (1u << 3)
#define E1000_CTRL_SLU      (1u << 6)
#define E1000_CTRL_SPD_1000 (2u << 8)
#define E1000_CTRL_FRCSPD   (1u << 11)
#define E1000_CTRL_RST      (1u << 26)
#define E1000_CTRL_VME      (1u << 30)
#define E1000_CTRL_PHY_RST  (1u << 31)

/* STATUS（第 13.4.2 节） */
#define E1000_STATUS_FD       (1u << 0)
#define E1000_STATUS_LU       (1u << 1)
#define E1000_STATUS_SPD_1000 (2u << 6)

/* EECD（第 13.4.3 节） */
#define E1000_EECD_SK    (1u << 0)
#define E1000_EECD_CS    (1u << 1)
#define E1000_EECD_DI    (1u << 2)
#define E1000_EECD_DO    (1u << 3)
#define E1000_EECD_FWE   (3u << 4)
#define E1000_EECD_FWE_DIS (1u << 4)    /* 01b：flash 写禁止，复位值 */
#define E1000_EECD_REQ   (1u << 6)
#define E1000_EECD_GNT   (1u << 7)
#define E1000_EECD_PRES  (1u << 8)

/* EERD（第 13.4.4 节，82540 的位布局） */
#define E1000_EERD_START      (1u << 0)
#define E1000_EERD_DONE       (1u << 4)
#define E1000_EERD_ADDR_SHIFT 8
#define E1000_EERD_DATA_SHIFT 16

/* MDIC（第 13.4.7 节） */
#define E1000_MDIC_DATA_MASK  0xFFFFu
#define E1000_MDIC_REG_SHIFT  16
#define E1000_MDIC_PHY_SHIFT  21
#define E1000_MDIC_OP_SHIFT   26
#define E1000_MDIC_OP_WRITE   1u
#define E1000_MDIC_OP_READ    2u
#define E1000_MDIC_READY      (1u << 28)
#define E1000_MDIC_INT_EN     (1u << 29)
#define E1000_MDIC_ERROR      (1u << 30)
#define E1000_PHY_ADDR        1          /* 82540 内部 PHY 的 MDIO 地址 */

/* ICR/ICS/IMS/IMC（第 13.4.17 节） */
#define E1000_ICR_TXDW    (1u << 0)
#define E1000_ICR_TXQE    (1u << 1)
#define E1000_ICR_LSC     (1u << 2)
#define E1000_ICR_RXDMT0  (1u << 4)
#define E1000_ICR_RXO     (1u << 6)
#define E1000_ICR_RXT0    (1u << 7)
#define E1000_ICR_MDAC    (1u << 9)
/* ICR 的原因位都在 16:0（TXD_LOW、SRPD 是最高的两位），高位读 0 */
#define E1000_ICR_MASK    0x0001FFFFu

/* RCTL（第 13.4.22 节） */
#define E1000_RCTL_EN      (1u << 1)
#define E1000_RCTL_UPE     (1u << 3)
#define E1000_RCTL_MPE     (1u << 4)
#define E1000_RCTL_LPE     (1u << 5)
#define E1000_RCTL_RDMTS_SHIFT 8
#define E1000_RCTL_MO_SHIFT    12
#define E1000_RCTL_BAM     (1u << 15)
#define E1000_RCTL_BSIZE_SHIFT 16
#define E1000_RCTL_VFE     (1u << 18)
#define E1000_RCTL_CFIEN   (1u << 19)
#define E1000_RCTL_CFI     (1u << 20)
#define E1000_RCTL_BSEX    (1u << 25)
#define E1000_RCTL_SECRC   (1u << 26)

/* TCTL（第 13.4.33 节） */
#define E1000_TCTL_EN      (1u << 1)
#define E1000_TCTL_PSP     (1u << 3)

#define E1000_RAH_AV       (1u << 31)

/* ---- 描述符（第 3.2.3、3.3 节），都是 16 字节、小端 ---- */
#define E1000_DESC_SIZE     16

/* RX 描述符 status / errors */
#define E1000_RXD_STAT_DD   0x01
#define E1000_RXD_STAT_EOP  0x02
#define E1000_RXD_STAT_IXSM 0x04
#define E1000_RXD_STAT_VP   0x08

/* TX：legacy 的 CMD、扩展描述符的 DCMD/TUCMD 都在第 11 字节，
 * RS、DEXT 的位置三者一致 */
#define E1000_TXD_CMD_EOP   0x01
#define E1000_TXD_CMD_IFCS  0x02
#define E1000_TXD_CMD_IC    0x04         /* legacy */
#define E1000_TXD_CMD_TSE   0x04         /* 数据描述符 */
#define E1000_TXD_CMD_RS    0x08
#define E1000_TXD_CMD_DEXT  0x20
#define E1000_TXD_CMD_VLE   0x40
#define E1000_TXD_STA_DD    0x01
#define E1000_TXD_DTYP_CTX  0x0
#define E1000_TXD_DTYP_DATA 0x1
#define E1000_TXD_POPTS_IXSM 0x01
#define E1000_TXD_POPTS_TXSM 0x02
/* 上下文描述符 TUCMD */
#define E1000_TUCMD_TCP     0x01
#define E1000_TUCMD_IP      0x02         /* 1 = IPv4 */
#define E1000_TUCMD_TSE     0x04

/* ---- EEPROM（第 5.6 节） ---- */
#define E1000_EEPROM_WORDS     64        /* 93C46，EECD.SIZE = 0 */
#define E1000_EEPROM_CHECKSUM  0xBABA    /* word 0..0x3F 之和 */
#define E1000_EE_OPCODE_READ   0x2       /* 起始位之后的 2 位操作码 10b */

/* ---- PHY（Marvell 88E1011，经 MDIC 访问，SDM 第 13.4.7 节之后的 PHY 寄存器表） ---- */
#define E1000_PHY_REGS      32
#define PHY_CTRL            0x00
#define PHY_STATUS          0x01
#define PHY_ID1             0x02
#define PHY_ID2             0x03
#define PHY_AUTONEG_ADV     0x04
#define PHY_LP_ABILITY      0x05
#define PHY_AUTONEG_EXP     0x06
#define PHY_1000T_CTRL      0x09
#define PHY_1000T_STATUS    0x0A
#define PHY_EXT_STATUS      0x0F
#define PHY_SPEC_CTRL       0x10
#define PHY_SPEC_STATUS     0x11
#define PHY_CTRL_RESTART_AN (1u << 9)
#define PHY_CTRL_RESET      (1u << 15)

/* TX 拼包缓冲。普通帧最长 16288 字节（单个描述符上限，第 3.3.3 节），
 * TSO 时缓冲里同时只放原型头加一个 MSS 的数据，见 e1000.c 的 tso_* */
#define E1000_TX_BUF        0x10000
/* RX 最长帧：TAP 交上来的最长帧加 FCS */
#define E1000_RX_BUF        (65535 + 4 + 4)

struct e1000_tx_ctx {
    uint8_t  ipcss, ipcso;
    uint16_t ipcse;
    uint8_t  tucss, tucso;
    uint16_t tucse;
    uint32_t paylen;
    uint8_t  tucmd;
    uint8_t  hdrlen;
    uint16_t mss;
};

/* 正在拼的那一个包。一个包从第一个数据描述符开始，到 EOP 结束 */
struct e1000_tx {
    struct e1000_tx_ctx ctx;       /* 普通校验和上下文（TSE=0 的上下文描述符） */
    struct e1000_tx_ctx tso;       /* 分段上下文（TSE=1），两者互不覆盖 */

    bool     active;               /* 已经收到包的第一个描述符 */
    bool     legacy;               /* 这个包用 legacy 描述符 */
    bool     tse;
    bool     drop;                 /* 包出错，消费描述符直到 EOP 但不发送 */
    uint8_t  popts;
    uint8_t  css;                  /* legacy IC 的起点，取自第一个描述符 */
    bool     vle;
    uint16_t vlan;

    uint32_t len;                  /* buf 里的字节数 */
    uint8_t  proto[256];           /* TSO 原型头（HDRLEN 最大 255） */
    bool     have_proto;
    uint32_t seg;                  /* 已发出的段数 */
    uint32_t sent;                 /* 已发出的 TCP/UDP 载荷字节 */
    uint8_t  buf[E1000_TX_BUF];
};

/* 93C46 Microwire 状态机（DS21749H 第 2 节、第 3.4 节） */
enum e1000_ee_state {
    EE_IDLE,        /* CS 高，等起始位 */
    EE_CMD,         /* 收操作码和地址 */
    EE_READ,        /* 往 DO 上送数据 */
    EE_IGNORE,      /* 不支持的命令：吞掉后续时钟直到 CS 拉低 */
};

struct e1000 {
    struct pci_device pci;
    struct vmm_vm    *vm;
    struct netdev    *net;
    uint8_t           mac[6];

    uint32_t regs[E1000_NREGS];
    uint32_t ioaddr;               /* IO BAR 的 IOADDR */
    uint16_t eeprom[E1000_EEPROM_WORDS];
    uint16_t phy[E1000_PHY_REGS];

    struct {
        enum e1000_ee_state state;
        uint32_t bits;
        int      nbits;
        uint8_t  addr;
        int      bitpos;           /* 下一个时钟沿输出 data 的第 bitpos-1 位 */
        bool     dout;
    } ee;

    struct e1000_tx tx;
    uint8_t  rx_frame[E1000_RX_BUF];

    uint32_t warned;               /* warn_once 的位图，见 e1000.c */
    uint8_t  reg_warned[E1000_NREGS / 8];
};

extern const struct vmm_device_type e1000_device_type;

/* 以下供单元测试直接驱动，guest 走的是 BAR 回调 */
int      e1000_init(struct e1000 *s, const uint8_t mac[6]);   /* 不注册总线 */
uint32_t e1000_reg_read(struct e1000 *s, uint32_t off);
void     e1000_reg_write(struct e1000 *s, uint32_t off, uint32_t val);

#endif /* DEVICES_E1000_H */
