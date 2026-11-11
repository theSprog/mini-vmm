/* src/devices/e1000.c — Intel 82540EM 设备模型                      [emu-net]
 *
 * 寄存器、简化项和锁的约定见 include/devices/e1000.h。本文件按数据流分段：
 *   寄存器表与读写分发 → EEPROM（Microwire / EERD）→ PHY（MDIC）
 *   → 中断 → TX（描述符环、校验和、TSO）→ RX（过滤、描述符环）→ PCI 与 --device
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmm.h"
#include "opts.h"
#include "net/netdev.h"
#include "devices/e1000.h"

/* ------------------------------------------------------------------ */
/* 日志                                                                */
/* ------------------------------------------------------------------ */

/* guest 驱动的错误用法和本模型没实现的路径各只报一次：它们往往在每个包
 * 上都会重复，刷屏只会淹没第一条有用的现场。 */
enum e1000_warn {
    W_PARTIAL_WRITE,
    W_TX_RING_INDEX,
    W_RX_RING_INDEX,
    W_TX_DMA,
    W_TX_OVERSIZE,
    W_TX_RUNT,
    W_TX_DTYP,
    W_TX_MIXED,
    W_TSO_CTX,
    W_TSO_IPV6,
    W_CSUM_RANGE,
    W_RX_DMA,
    W_RX_RING_SMALL,
    W_EE_OPCODE,
    W_EERD_ADDR,
    W_MDIC_PHY,
};

#define warn_once(s, w, ...)                                \
    do {                                                    \
        if (!((s)->warned & (1u << (w)))) {                 \
            (s)->warned |= 1u << (w);                       \
            vmm_warn(__VA_ARGS__);                          \
        }                                                   \
    } while (0)

static inline uint32_t *reg(struct e1000 *s, uint32_t off)
{
    return &s->regs[off >> 2];
}

static inline uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) | (uint32_t)get16(p + 2) << 16; }
static inline uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }
static inline uint16_t get_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t get_be32(const uint8_t *p) { return (uint32_t)get_be16(p) << 16 | get_be16(p + 2); }
static inline void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void put_be32(uint8_t *p, uint32_t v) { put_be16(p, (uint16_t)(v >> 16)); put_be16(p + 2, (uint16_t)v); }

/* ------------------------------------------------------------------ */
/* 中断                                                                */
/* ------------------------------------------------------------------ */

/* INTx 电平 = 有没有一个没被屏蔽的原因位（SDM 第 13.4.17-13.4.21 节）。
 * 每次 ICR 或 IMS 变化都重新算一遍，PCI 核心只在电平变化时打 KVM。 */
static void update_irq(struct e1000 *s)
{
    pci_set_irq(&s->pci, (*reg(s, E1000_ICR) & *reg(s, E1000_IMS)) != 0);
}

static void raise_icr(struct e1000 *s, uint32_t bits)
{
    *reg(s, E1000_ICR) |= bits & E1000_ICR_MASK;
    update_irq(s);
}

/* ------------------------------------------------------------------ */
/* EEPROM                                                              */
/* ------------------------------------------------------------------ */

/* 按第 5.6 节的映射生成 64 个字。没列出的字是厂商保留或板级配置，取 0；
 * 列出的取手册给出的 82540EM 值。最后一个字让总和等于 0xBABA，
 * Linux 的 e1000_validate_eeprom_checksum 校验不过会拒绝 probe。 */
static void eeprom_build(struct e1000 *s)
{
    uint16_t *w = s->eeprom, sum = 0;
    int i;

    memset(w, 0, sizeof(s->eeprom));
    for (i = 0; i < 3; i++)                  /* MAC，每个字低字节在前 */
        w[i] = (uint16_t)(s->mac[2 * i] | s->mac[2 * i + 1] << 8);
    w[0x04] = 0xFFFF;                        /* SerDes 配置：铜缆芯片不用 */
    w[0x0A] = 0x4408;                        /* Initialization Control 1 */
    w[0x0B] = E1000_SUBSYS_ID;
    w[0x0C] = E1000_VENDOR_ID;               /* subsystem vendor */
    w[0x0D] = E1000_DEVICE_ID_82540EM;
    w[0x0E] = E1000_VENDOR_ID;
    w[0x0F] = 0x3040;                        /* Initialization Control 2 */
    w[0x21] = 0x7061;                        /* 82540EM 的 Initialization Control 3 */
    w[0x22] = 0x280C;
    w[0x23] = 0x00C8;
    w[0x24] = 0x00C8;
    w[0x2F] = 0x0602;                        /* LEDCTL 默认 */
    for (i = 0; i < E1000_EEPROM_WORDS - 1; i++)
        sum = (uint16_t)(sum + w[i]);
    w[E1000_EEPROM_WORDS - 1] = (uint16_t)(E1000_EEPROM_CHECKSUM - sum);
}

/* 93C46 的 Microwire 协议（DS21749H 第 3 节），x16 组织：
 *
 *   CS 拉高后，SK 每个上升沿采样一次 DI。第一个 DI=1 的上升沿是起始位，
 *   接着 2 位操作码、6 位地址。READ（10b）的地址最后一位采样后 DO 先输出
 *   一个哑 0，此后每个上升沿输出一位数据，高位在前；CS 一直高着就顺序读
 *   下一个字。CS 拉低回到待机。
 *
 * Linux 的 e1000_shift_in_ee_bits 在拉高 SK 之后读 DO，所以数据位在上升沿
 * 更新；它不读那个哑 0，第一次读到的就是 D15。 */
static void ee_clock(struct e1000 *s, bool di)
{
    switch (s->ee.state) {
    case EE_IDLE:
        if (di) {
            s->ee.state = EE_CMD;
            s->ee.bits = 0;
            s->ee.nbits = 0;
        }
        break;
    case EE_CMD:
        s->ee.bits = s->ee.bits << 1 | di;
        if (++s->ee.nbits < 2 + 6)
            break;
        if ((s->ee.bits >> 6) == E1000_EE_OPCODE_READ) {
            s->ee.state = EE_READ;
            s->ee.addr = (uint8_t)(s->ee.bits & 0x3F);
            s->ee.bitpos = 16;
            s->ee.dout = false;               /* 哑 0 */
        } else {
            /* 擦写和写使能命令。镜像每次启动按 mac= 重新生成，写进去也
             * 留不到下次，与其假装成功，不如让 guest 的校验读回旧值 */
            warn_once(s, W_EE_OPCODE,
                      "e1000: EEPROM command 0x%02x ignored (read-only image)",
                      s->ee.bits);
            s->ee.state = EE_IGNORE;
        }
        break;
    case EE_READ:
        if (s->ee.bitpos == 0) {
            s->ee.addr = (uint8_t)((s->ee.addr + 1) % E1000_EEPROM_WORDS);
            s->ee.bitpos = 16;
        }
        s->ee.bitpos--;
        s->ee.dout = (s->eeprom[s->ee.addr] >> s->ee.bitpos) & 1;
        break;
    case EE_IGNORE:
        break;
    }
}

static void eecd_write(struct e1000 *s, uint32_t val)
{
    uint32_t old = *reg(s, E1000_EECD);
    const uint32_t rw = E1000_EECD_SK | E1000_EECD_CS | E1000_EECD_DI |
                        E1000_EECD_FWE | E1000_EECD_REQ;

    *reg(s, E1000_EECD) = (val & rw) | E1000_EECD_PRES;
    if (!(val & E1000_EECD_CS)) {
        s->ee.state = EE_IDLE;
        s->ee.dout = false;
        return;
    }
    if (!(old & E1000_EECD_SK) && (val & E1000_EECD_SK))
        ee_clock(s, (val & E1000_EECD_DI) != 0);
}

static uint32_t eecd_read(struct e1000 *s)
{
    uint32_t v = *reg(s, E1000_EECD);

    /* 软件和硬件的 EEPROM 仲裁（REQ/GNT）：这里没有别的主控，立即授权 */
    if (v & E1000_EECD_REQ)
        v |= E1000_EECD_GNT;
    if (s->ee.dout)
        v |= E1000_EECD_DO;
    return v;
}

/* EERD：写 START 和地址，硬件读完置 DONE 并把数据放在高 16 位。
 * 读是瞬间完成的，写进来就已经 DONE。 */
static void eerd_write(struct e1000 *s, uint32_t val)
{
    uint32_t addr = (val >> E1000_EERD_ADDR_SHIFT) & 0xFF;
    uint16_t data = 0;

    if (!(val & E1000_EERD_START)) {
        *reg(s, E1000_EERD) = val & (0xFFu << E1000_EERD_ADDR_SHIFT);
        return;
    }
    if (addr < E1000_EEPROM_WORDS)
        data = s->eeprom[addr];
    else
        warn_once(s, W_EERD_ADDR, "e1000: EERD read past the 64-word EEPROM (word 0x%x)", addr);
    *reg(s, E1000_EERD) = E1000_EERD_DONE | addr << E1000_EERD_ADDR_SHIFT |
                          (uint32_t)data << E1000_EERD_DATA_SHIFT;
}

/* ------------------------------------------------------------------ */
/* PHY                                                                 */
/* ------------------------------------------------------------------ */

/* 88E1011 链路已经以 1000BASE-T 全双工建立、自协商完成时的寄存器值。
 * PHY_STATUS 0x796D：能力位 + 自协商完成 + link up；对端能力 0x41E1：
 * 10/100 全半双工、ACK；1000T_STATUS 0x3C00：本端和对端都接收正常、
 * 对端支持 1000 全/半双工；PHY_SPEC_STATUS 0xAC00：1000 Mb/s、全双工、
 * 速率双工已解析、实时 link up。驱动读 ID 认出 PHY 型号，读 STATUS 判断
 * link，其余寄存器写什么记什么。 */
static void phy_reset(struct e1000 *s)
{
    memset(s->phy, 0, sizeof(s->phy));
    s->phy[PHY_CTRL]         = 0x1140;  /* 自协商使能、全双工、1000 */
    s->phy[PHY_STATUS]       = 0x796D;
    s->phy[PHY_ID1]          = 0x0141;
    s->phy[PHY_ID2]          = 0x0C20;  /* 88E1011，revision 0 */
    s->phy[PHY_LP_ABILITY]   = 0x41E1;
    s->phy[PHY_AUTONEG_EXP]  = 0x0001;  /* 对端支持自协商 */
    s->phy[PHY_1000T_STATUS] = 0x3C00;
    s->phy[PHY_EXT_STATUS]   = 0x3000;
    s->phy[PHY_SPEC_CTRL]    = 0x0068;
    s->phy[PHY_SPEC_STATUS]  = 0xAC00;
}

static bool phy_reg_readonly(uint32_t r)
{
    return r == PHY_STATUS || r == PHY_ID1 || r == PHY_ID2 ||
           r == PHY_LP_ABILITY || r == PHY_AUTONEG_EXP ||
           r == PHY_1000T_STATUS || r == PHY_EXT_STATUS ||
           r == PHY_SPEC_STATUS;
}

static void phy_write(struct e1000 *s, uint32_t r, uint16_t v)
{
    if (r == PHY_CTRL) {
        if (v & PHY_CTRL_RESET) {
            phy_reset(s);
            return;
        }
        /* 重启自协商：瞬间完成，自清零 */
        s->phy[PHY_CTRL] = v & ~PHY_CTRL_RESTART_AN;
        return;
    }
    if (!phy_reg_readonly(r))
        s->phy[r] = v;
}

/* MDIC：写一次就完成一次 MDIO 事务，READY 立即置位（第 13.4.7 节）。
 * 只有 PHY 地址 1 上有设备，访问别的地址置 ERROR，驱动据此报错而不是
 * 读到一个假的 PHY。 */
static void mdic_write(struct e1000 *s, uint32_t val)
{
    uint32_t phy = (val >> E1000_MDIC_PHY_SHIFT) & 0x1F;
    uint32_t r   = (val >> E1000_MDIC_REG_SHIFT) & 0x1F;
    uint32_t op  = (val >> E1000_MDIC_OP_SHIFT) & 3;
    uint32_t out = val & ~(E1000_MDIC_READY | E1000_MDIC_ERROR);

    if (phy != E1000_PHY_ADDR) {
        warn_once(s, W_MDIC_PHY, "e1000: MDIC access to PHY address %u (only %u exists)",
                  phy, E1000_PHY_ADDR);
        out |= E1000_MDIC_ERROR;
    } else if (op == E1000_MDIC_OP_READ) {
        out = (out & ~E1000_MDIC_DATA_MASK) | s->phy[r];
    } else if (op == E1000_MDIC_OP_WRITE) {
        phy_write(s, r, (uint16_t)(val & E1000_MDIC_DATA_MASK));
    } else {
        out |= E1000_MDIC_ERROR;
    }
    *reg(s, E1000_MDIC) = out | E1000_MDIC_READY;
    if (val & E1000_MDIC_INT_EN)
        raise_icr(s, E1000_ICR_MDAC);
}

/* ------------------------------------------------------------------ */
/* 复位                                                                */
/* ------------------------------------------------------------------ */

static void tx_reset(struct e1000 *s)
{
    struct e1000_tx *t = &s->tx;

    memset(&t->ctx, 0, sizeof(t->ctx));
    memset(&t->tso, 0, sizeof(t->tso));
    t->active = t->drop = t->have_proto = false;
    t->len = t->seg = t->sent = 0;
}

/* CTRL.RST：除 PCI 配置空间外全部回到上电状态（第 13.4.1 节）。
 * 复位值取自第 13 章各寄存器的 Initial Value 栏，没写的（标 X 或 0）为 0。 */
static void e1000_reset(struct e1000 *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    *reg(s, E1000_CTRL)   = E1000_CTRL_FD | E1000_CTRL_LRST |
                            E1000_CTRL_SPD_1000 | E1000_CTRL_FRCSPD;
    *reg(s, E1000_EECD)   = E1000_EECD_FWE_DIS | E1000_EECD_PRES;
    *reg(s, E1000_LEDCTL) = 0x07060302;     /* LED0..3 模式 2/3/6/7 */
    *reg(s, E1000_PBA)    = 0x0030;         /* 48 KiB 给 RX */
    /* 上电时硬件从 EEPROM 把 MAC 装进第 0 个接收地址并置 AV（第 13.4.27 节） */
    *reg(s, E1000_RA) = (uint32_t)s->mac[0] | s->mac[1] << 8 | s->mac[2] << 16 |
                        (uint32_t)s->mac[3] << 24;
    *reg(s, E1000_RA + 4) = (uint32_t)s->mac[4] | s->mac[5] << 8 | E1000_RAH_AV;

    memset(&s->ee, 0, sizeof(s->ee));
    s->ioaddr = 0;
    phy_reset(s);
    tx_reset(s);
    if (s->pci.bus)
        update_irq(s);                       /* ICR、IMS 都清零了，撤销 INTx */
}

/* ------------------------------------------------------------------ */
/* 校验和                                                              */
/* ------------------------------------------------------------------ */

static uint32_t csum_add(uint32_t sum, const uint8_t *p, uint32_t len)
{
    uint32_t i;

    for (i = 0; i + 1 < len; i += 2)
        sum += get_be16(p + i);
    if (len & 1)
        sum += (uint32_t)p[len - 1] << 8;
    return sum;
}

static uint16_t csum_fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* 对 [css, cse] 求和（cse 为 0 表示到包尾，含端点），取反后写到 cso。
 * 被求和的范围包含校验和字段当时的内容：驱动在那里预先放了伪首部的和
 * （第 3.3.6 节），硬件不清零它。extra 用来补上 TSO 每段不同的 L4 长度。
 * 越界的设置说明驱动和本模型对上下文的理解不一致，跳过插入并报一次。 */
static void csum_insert(struct e1000 *s, uint8_t *pkt, uint32_t len,
                        uint32_t css, uint32_t cse, uint32_t cso,
                        uint32_t extra, bool l4)
{
    uint16_t c;

    if (cse == 0 || cse >= len)
        cse = len - 1;
    if (css >= len || css > cse || cso + 2 > len) {
        warn_once(s, W_CSUM_RANGE,
                  "e1000: checksum range css=%u cse=%u cso=%u outside a %u-byte packet",
                  css, cse, cso, len);
        return;
    }
    c = csum_fold(csum_add(extra, pkt + css, cse - css + 1));
    /* TCP/UDP 里 0 和 0xFFFF 在反码运算下等价，但 UDP 的 0 表示“没有校验和”，
     * 所以 L4 结果为 0 时写 0xFFFF（RFC 768） */
    if (l4 && c == 0)
        c = 0xFFFF;
    put_be16(pkt + cso, c);
}

/* ------------------------------------------------------------------ */
/* TX                                                                  */
/* ------------------------------------------------------------------ */

#define ETH_ALEN      6
#define ETH_HLEN      14
#define ETH_ZLEN      60          /* 不含 FCS 的最短帧 */
#define VLAN_HLEN     4
#define ETH_FCS_LEN   4

/* 把一帧交给后端：按需插入 VLAN tag、补齐短帧。
 * IFCS（让硬件追加 FCS）不需要处理：后端收发的帧不带 FCS（见 net/netdev.h），
 * 线上的 FCS 由 host 一侧的网络栈负责。 */
static void tx_emit(struct e1000 *s, uint8_t *pkt, uint32_t len)
{
    static const uint8_t zeros[ETH_ZLEN];
    struct iovec iov[4];
    uint8_t tag[VLAN_HLEN];
    uint32_t total = len;
    int n = 0;

    if (len < ETH_HLEN) {
        warn_once(s, W_TX_RUNT, "e1000: dropping a %u-byte TX frame (shorter than an Ethernet header)", len);
        return;
    }
    if (s->tx.vle && (*reg(s, E1000_CTRL) & E1000_CTRL_VME)) {
        put_be16(tag, (uint16_t)*reg(s, E1000_VET));
        put_be16(tag + 2, s->tx.vlan);
        iov[n++] = (struct iovec){ pkt, 2 * ETH_ALEN };
        iov[n++] = (struct iovec){ tag, VLAN_HLEN };
        iov[n++] = (struct iovec){ pkt + 2 * ETH_ALEN, len - 2 * ETH_ALEN };
        total += VLAN_HLEN;
    } else {
        iov[n++] = (struct iovec){ pkt, len };
    }
    /* PSP：短帧补到 64 字节（含 FCS），即不含 FCS 的 60 字节 */
    if ((*reg(s, E1000_TCTL) & E1000_TCTL_PSP) && total < ETH_ZLEN)
        iov[n++] = (struct iovec){ (void *)zeros, ETH_ZLEN - total };
    netdev_send(s->net, iov, n);
}

/* 非 TSO 包的校验和插入：legacy 描述符的 IC，或者扩展描述符的 POPTS
 * 加普通上下文。 */
static void tx_finish_plain(struct e1000 *s, uint8_t cso_legacy, bool ic)
{
    struct e1000_tx *t = &s->tx;
    const struct e1000_tx_ctx *c = &t->ctx;

    if (t->legacy) {
        if (ic)
            csum_insert(s, t->buf, t->len, t->css, 0, cso_legacy, 0, true);
    } else {
        if (t->popts & E1000_TXD_POPTS_IXSM)
            csum_insert(s, t->buf, t->len, c->ipcss, c->ipcse, c->ipcso, 0, false);
        if (t->popts & E1000_TXD_POPTS_TXSM)
            csum_insert(s, t->buf, t->len, c->tucss, c->tucse, c->tucso, 0, true);
    }
    tx_emit(s, t->buf, t->len);
}

/* TSO（第 3.5 节）：数据流的前 HDRLEN 字节是原型头，其后 PAYLEN 字节的
 * 载荷按 MSS 切段，每段都以原型头开头，并按段修正：
 *   IPv4 总长度 = 段长 - IPCSS，IP ID 每段加 1
 *   TCP 序号加上之前各段的载荷字节数，FIN/PSH 只留在最后一段
 *   UDP 长度 = 段长 - TUCSS
 *   IP 校验和、TCP/UDP 校验和按段重算；驱动在 TCP 校验和字段里放的是
 *   长度为 0 的伪首部和（Linux e1000_tso 的 csum_tcpudp_magic(.., 0, ..)），
 *   每段的 L4 长度由硬件补进去。
 * 缓冲里同时只放原型头加一段的数据，不必把整个 64 KiB 的 GSO 包攒齐。 */
static void tso_segment(struct e1000 *s, uint32_t len, bool last)
{
    struct e1000_tx *t = &s->tx;
    const struct e1000_tx_ctx *c = &t->tso;
    uint8_t *p = t->buf;
    uint32_t hdr = c->hdrlen, payload = len - hdr;
    uint32_t l4len = len - c->tucss;

    memcpy(p, t->proto, hdr);
    if (c->tucmd & E1000_TUCMD_IP) {
        put_be16(p + c->ipcss + 2, (uint16_t)(len - c->ipcss));
        put_be16(p + c->ipcss + 4, (uint16_t)(get_be16(t->proto + c->ipcss + 4) + t->seg));
    }
    if (c->tucmd & E1000_TUCMD_TCP) {
        put_be32(p + c->tucss + 4, get_be32(t->proto + c->tucss + 4) + t->sent);
        if (!last)
            p[c->tucss + 13] &= (uint8_t)~0x09;     /* FIN 0x01、PSH 0x08 */
    } else {
        put_be16(p + c->tucss + 4, (uint16_t)l4len);
    }
    if (t->popts & E1000_TXD_POPTS_IXSM)
        csum_insert(s, p, len, c->ipcss, c->ipcse, c->ipcso, 0, false);
    if (t->popts & E1000_TXD_POPTS_TXSM)
        csum_insert(s, p, len, c->tucss, c->tucse, c->tucso, l4len, true);

    tx_emit(s, p, len);
    t->seg++;
    t->sent += payload;
}

static bool tso_start(struct e1000 *s)
{
    const struct e1000_tx_ctx *c = &s->tx.tso;

    if (!(c->tucmd & E1000_TUCMD_IP)) {
        /* 手册对 IPv6 分段时各字段怎样修正写得不完整；82540 的 Linux 驱动
         * 也不声明 TSO6，走不到这里 */
        warn_once(s, W_TSO_IPV6, "e1000: TSO without the IPv4 bit is not supported, packet dropped");
        return false;
    }
    if (c->mss == 0 || c->hdrlen == 0 ||
        (uint32_t)c->hdrlen + c->mss >= E1000_TX_BUF ||
        c->tucss + 18u > c->hdrlen || c->ipcss + 20u > c->hdrlen) {
        warn_once(s, W_TSO_CTX, "e1000: bad TSO context hdrlen=%u mss=%u ipcss=%u tucss=%u, packet dropped",
                  c->hdrlen, c->mss, c->ipcss, c->tucss);
        return false;
    }
    return true;
}

/* 把一个描述符的数据追加进当前包。TSO 时每攒满“原型头 + 一个 MSS 且还有
 * 多余”就切出一段：严格大于才切，这样恰好在 EOP 处凑满的最后一段仍然
 * 按“最后一段”处理（保留 FIN/PSH）。 */
static void tx_append(struct e1000 *s, uint64_t addr, uint32_t len)
{
    struct e1000_tx *t = &s->tx;

    while (len && !t->drop) {
        uint32_t room = E1000_TX_BUF - t->len;
        uint32_t n = len < room ? len : room;

        if (n == 0) {
            warn_once(s, W_TX_OVERSIZE, "e1000: TX packet exceeds %u bytes, dropped", E1000_TX_BUF);
            t->drop = true;
            break;
        }
        if (pci_dma_read(&s->pci, addr, t->buf + t->len, n) != VMM_OK) {
            warn_once(s, W_TX_DMA, "e1000: TX buffer gpa=0x%llx len=%u unreadable, packet dropped",
                      (unsigned long long)addr, n);
            t->drop = true;
            break;
        }
        t->len += n;
        addr += n;
        len -= n;

        if (!t->tse)
            continue;
        if (!t->have_proto && t->len >= t->tso.hdrlen) {
            memcpy(t->proto, t->buf, t->tso.hdrlen);
            t->have_proto = true;
        }
        while (t->have_proto && t->len > (uint32_t)t->tso.hdrlen + t->tso.mss) {
            uint32_t seg = t->tso.hdrlen + t->tso.mss;

            tso_segment(s, seg, false);
            memmove(t->buf + t->tso.hdrlen, t->buf + seg, t->len - seg);
            t->len -= t->tso.mss;
        }
    }
}

static void tx_end_packet(struct e1000 *s)
{
    struct e1000_tx *t = &s->tx;

    t->active = t->drop = t->have_proto = false;
    t->len = t->seg = t->sent = 0;
}

static void tx_context_desc(struct e1000 *s, const uint8_t *d)
{
    struct e1000_tx_ctx c = {
        .ipcss  = d[0],  .ipcso = d[1],  .ipcse = get16(d + 2),
        .tucss  = d[4],  .tucso = d[5],  .tucse = get16(d + 6),
        .paylen = get32(d + 8) & 0xFFFFF,
        .tucmd  = d[11], .hdrlen = d[13], .mss = get16(d + 14),
    };

    /* 两种上下文分开保存（第 3.3.6 节）：TSO 包之间穿插的普通校验和包
     * 不会冲掉分段参数 */
    if (c.tucmd & E1000_TUCMD_TSE)
        s->tx.tso = c;
    else
        s->tx.ctx = c;
}

/* 处理一个描述符。d 是从环上读出的 16 字节 */
static void tx_desc(struct e1000 *s, const uint8_t *d)
{
    struct e1000_tx *t = &s->tx;
    uint8_t cmd = d[11];
    bool dext = cmd & E1000_TXD_CMD_DEXT;
    uint64_t addr = get64(d);
    uint32_t len;

    if (dext) {
        uint8_t dtyp = d[10] >> 4;

        if (dtyp == E1000_TXD_DTYP_CTX) {
            tx_context_desc(s, d);
            return;
        }
        if (dtyp != E1000_TXD_DTYP_DATA) {
            warn_once(s, W_TX_DTYP, "e1000: TX descriptor type %u ignored", dtyp);
            return;
        }
        len = get32(d + 8) & 0xFFFFF;
    } else {
        len = get16(d + 8);
    }

    if (!t->active) {
        t->active = true;
        t->legacy = !dext;
        t->tse = dext && (cmd & E1000_TXD_CMD_TSE);
        t->popts = dext ? d[13] : 0;
        t->css = dext ? 0 : d[13];
        /* TSO 包的 VLAN 信息取第一个数据描述符（第 3.3.7 节），其余取 EOP */
        t->vle = cmd & E1000_TXD_CMD_VLE;
        t->vlan = get16(d + 14);
        if (t->tse && !tso_start(s))
            t->drop = true;
    } else if (t->legacy == dext) {
        warn_once(s, W_TX_MIXED, "e1000: legacy and extended descriptors mixed in one packet, dropped");
        t->drop = true;
    }

    /* 空地址或零长度的描述符不搬数据（第 3.3.3 节） */
    if (addr && len)
        tx_append(s, addr, len);

    if (!(cmd & E1000_TXD_CMD_EOP))
        return;
    if (!t->tse) {
        t->vle = cmd & E1000_TXD_CMD_VLE;
        t->vlan = get16(d + 14);
    }
    if (!t->drop) {
        if (t->tse) {
            if (t->have_proto && t->len > t->tso.hdrlen)
                tso_segment(s, t->len, true);
        } else {
            tx_finish_plain(s, d[10], dext ? false : (cmd & E1000_TXD_CMD_IC) != 0);
        }
    }
    tx_end_packet(s);
}

/* 描述符环中硬件拥有 [TDH, TDT)（第 3.4 节）。写 TDT 时在当前 vCPU 上
 * 一次处理完，回写 RS 描述符的 DD，最后按结果置 TXDW/TXQE。
 * 循环最多走一圈：TDH/TDT 都由 guest 控制，坏值不能让 VMM 卡死。 */
static void tx_process(struct e1000 *s)
{
    uint64_t base = (uint64_t)*reg(s, E1000_TDBAH) << 32 | (*reg(s, E1000_TDBAL) & ~0xFu);
    uint32_t n = *reg(s, E1000_TDLEN) / E1000_DESC_SIZE;
    uint32_t head = *reg(s, E1000_TDH), tail = *reg(s, E1000_TDT), budget;
    uint32_t cause = 0;
    uint8_t d[E1000_DESC_SIZE];

    if (!(*reg(s, E1000_TCTL) & E1000_TCTL_EN) || n == 0)
        return;
    if (head >= n || tail >= n) {
        warn_once(s, W_TX_RING_INDEX, "e1000: TX ring index out of range (TDH=%u TDT=%u, %u descriptors)",
                  head, tail, n);
        return;
    }
    for (budget = n; head != tail && budget; budget--) {
        uint64_t gpa = base + (uint64_t)head * E1000_DESC_SIZE;

        if (pci_dma_read(&s->pci, gpa, d, sizeof(d)) != VMM_OK) {
            warn_once(s, W_TX_DMA, "e1000: TX descriptor %u at gpa=0x%llx unreadable, ring stopped",
                      head, (unsigned long long)gpa);
            break;
        }
        tx_desc(s, d);
        if (d[11] & E1000_TXD_CMD_RS) {
            uint8_t sta = d[12] | E1000_TXD_STA_DD;

            pci_dma_write(&s->pci, gpa + 12, &sta, 1);
            cause |= E1000_ICR_TXDW;
        }
        head = (head + 1) % n;
        *reg(s, E1000_TDH) = head;
    }
    if (head == tail)
        cause |= E1000_ICR_TXQE;
    if (cause)
        raise_icr(s, cause);
}

/* ------------------------------------------------------------------ */
/* RX                                                                  */
/* ------------------------------------------------------------------ */

static uint32_t crc32_table[256];

static void crc32_init(void)
{
    uint32_t i, j, c;

    for (i = 0; i < 256; i++) {
        for (c = i, j = 0; j < 8; j++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc32_table[i] = c;
    }
}

/* IEEE 802.3 的 FCS：反射的 CRC-32，结果按小端字节序跟在帧后 */
static uint32_t eth_crc32(const uint8_t *p, size_t len)
{
    uint32_t c = 0xFFFFFFFFu;

    while (len--)
        c = crc32_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

/* 目的地址过滤（第 3.2.1 节）：精确匹配 16 个接收地址、单播/组播混杂、
 * 广播、组播哈希表。 */
static bool rx_filter(struct e1000 *s, const uint8_t *dst)
{
    uint32_t rctl = *reg(s, E1000_RCTL);
    bool mcast = dst[0] & 1;
    uint32_t hash;
    int i;

    if (mcast && (rctl & E1000_RCTL_BAM) &&
        !memcmp(dst, "\xff\xff\xff\xff\xff\xff", ETH_ALEN))
        return true;
    if (!mcast && (rctl & E1000_RCTL_UPE))
        return true;
    if (mcast && (rctl & E1000_RCTL_MPE))
        return true;
    for (i = 0; i < E1000_RA_N; i++) {
        uint32_t lo = *reg(s, E1000_RA + 8 * i), hi = *reg(s, E1000_RA + 8 * i + 4);
        uint8_t a[ETH_ALEN] = { (uint8_t)lo, (uint8_t)(lo >> 8), (uint8_t)(lo >> 16),
                                (uint8_t)(lo >> 24), (uint8_t)hi, (uint8_t)(hi >> 8) };

        /* AS（17:16）非 0 是按源地址匹配，不参与目的地址过滤 */
        if ((hi & E1000_RAH_AV) && !(hi & 0x30000) && !memcmp(a, dst, ETH_ALEN))
            return true;
    }
    if (!mcast)
        return false;
    /* MO 选取目的地址里的哪 12 位做哈希（第 13.4.22 节） */
    switch ((rctl >> E1000_RCTL_MO_SHIFT) & 3) {
    case 0:  hash = dst[4] >> 4 | (uint32_t)dst[5] << 4; break;
    case 1:  hash = dst[4] >> 3 | (uint32_t)dst[5] << 5; break;
    case 2:  hash = dst[4] >> 2 | (uint32_t)dst[5] << 6; break;
    default: hash = dst[4]      | (uint32_t)dst[5] << 8; break;
    }
    hash &= 0xFFF;
    return (*reg(s, E1000_MTA + 4 * (hash >> 5)) >> (hash & 31)) & 1;
}

static uint32_t rx_buf_size(uint32_t rctl)
{
    static const uint32_t normal[4] = { 2048, 1024, 512, 256 };
    static const uint32_t ext[4]    = { 0, 16384, 8192, 4096 };
    uint32_t b = (rctl >> E1000_RCTL_BSIZE_SHIFT) & 3;

    if ((rctl & E1000_RCTL_BSEX) && b)
        return ext[b];
    return normal[b];
}

/* 把 rx_frame 里 len 字节写进描述符环。返回 false 表示环里空闲缓冲不够，
 * 什么都没写。null 地址的描述符只回写 DD、不装数据（第 3.2.5.2 节）。 */
static bool rx_ring_put(struct e1000 *s, uint32_t len, uint8_t status, uint16_t special)
{
    uint64_t base = (uint64_t)*reg(s, E1000_RDBAH) << 32 | (*reg(s, E1000_RDBAL) & ~0xFu);
    uint32_t n = *reg(s, E1000_RDLEN) / E1000_DESC_SIZE;
    uint32_t head = *reg(s, E1000_RDH), tail = *reg(s, E1000_RDT);
    uint32_t bsize = rx_buf_size(*reg(s, E1000_RCTL));
    uint32_t i, cap = 0, need = 0, off = 0, avail, thr;
    uint8_t d[E1000_DESC_SIZE];

    if (n == 0 || head >= n || tail >= n) {
        if (n)
            warn_once(s, W_RX_RING_INDEX, "e1000: RX ring index out of range (RDH=%u RDT=%u, %u descriptors)",
                      head, tail, n);
        return false;
    }
    /* 第一遍只读描述符，确认容量够装下整帧，避免写了一半才发现不够 */
    for (i = head; i != tail && cap < len; i = (i + 1) % n, need++) {
        if (pci_dma_read(&s->pci, base + (uint64_t)i * E1000_DESC_SIZE, d, 8) != VMM_OK) {
            warn_once(s, W_RX_DMA, "e1000: RX descriptor %u unreadable", i);
            return false;
        }
        if (get64(d))
            cap += bsize;
    }
    if (cap < len) {
        if ((uint64_t)(n - 1) * bsize < len)
            warn_once(s, W_RX_RING_SMALL, "e1000: a %u-byte frame cannot fit the whole RX ring (%u x %u bytes)",
                      len, n - 1, bsize);
        return false;
    }

    for (i = 0; i < need; i++) {
        uint64_t gpa = base + (uint64_t)head * E1000_DESC_SIZE;
        uint64_t buf;
        uint32_t chunk = 0;
        uint8_t wb[8] = { 0 }, st;

        if (pci_dma_read(&s->pci, gpa, d, 8) != VMM_OK)
            return false;
        buf = get64(d);
        st = E1000_RXD_STAT_DD;
        if (buf) {
            chunk = len - off < bsize ? len - off : bsize;
            if (pci_dma_write(&s->pci, buf, s->rx_frame + off, chunk) != VMM_OK)
                warn_once(s, W_RX_DMA, "e1000: RX buffer gpa=0x%llx len=%u (descriptor %u) not writable",
                          (unsigned long long)buf, chunk, head);
            off += chunk;
            if (off == len)
                st |= status | E1000_RXD_STAT_EOP;
            /* 描述符的 length、csum、errors、special 先写，status 最后写：
             * 驱动看到 DD 才去读其余字段（e1000_clean_rx_irq 里 dma_rmb 之后），
             * 一次 memcpy 写 16 字节不保证 status 最后落地。x86 上存储之间不
             * 重排，函数调用边界也阻止编译器把两次写交换 */
            wb[0] = (uint8_t)chunk;
            wb[1] = (uint8_t)(chunk >> 8);
            if (st & E1000_RXD_STAT_EOP) {
                wb[6] = (uint8_t)special;
                wb[7] = (uint8_t)(special >> 8);
            }
            pci_dma_write(&s->pci, gpa + 8, wb, 4);
            pci_dma_write(&s->pci, gpa + 13, wb + 5, 3);
        }
        pci_dma_write(&s->pci, gpa + 12, &st, 1);
        head = (head + 1) % n;
    }
    *reg(s, E1000_RDH) = head;

    /* 剩余空闲描述符降到 RDMTS 选定的 1/2、1/4、1/8 以下时提醒驱动补充 */
    avail = (tail + n - head) % n;
    thr = n >> (1 + ((*reg(s, E1000_RCTL) >> E1000_RCTL_RDMTS_SHIFT) & 3));
    raise_icr(s, E1000_ICR_RXT0 | (avail <= thr ? E1000_ICR_RXDMT0 : 0));
    return true;
}

/* I/O 线程上的后端回调 */
static enum netdev_rx_result e1000_receive(void *opaque, const uint8_t *frame, size_t len)
{
    struct e1000 *s = opaque;
    uint32_t rctl, vet, max, total;
    uint8_t status = E1000_RXD_STAT_IXSM;
    uint16_t special = 0;
    enum netdev_rx_result res = NETDEV_RX_DONE;
    bool tagged;

    pthread_mutex_lock(&s->vm->io_lock);
    rctl = *reg(s, E1000_RCTL);
    if (!(rctl & E1000_RCTL_EN) || len < ETH_HLEN || !rx_filter(s, frame))
        goto out;

    vet = *reg(s, E1000_VET) & 0xFFFF;
    tagged = len >= ETH_HLEN + VLAN_HLEN && vet && get_be16(frame + 12) == vet;
    /* 长度限制按线上的帧算，含 FCS（第 13.4.22 节 LPE） */
    max = rctl & E1000_RCTL_LPE ? 16384 : 1522;
    if (len + ETH_FCS_LEN > max)
        goto out;
    if (tagged && (rctl & E1000_RCTL_VFE)) {
        uint16_t tci = get_be16(frame + 14), vid = tci & 0xFFF;

        if ((rctl & E1000_RCTL_CFIEN) && !!(tci & 0x1000) != !!(rctl & E1000_RCTL_CFI))
            goto out;
        if (!((*reg(s, E1000_VFTA + 4 * (vid >> 5)) >> (vid & 31)) & 1))
            goto out;
    }

    /* 线上的帧至少 60 字节加 FCS，发送端负责补齐；TAP 交上来的帧没经过线，
     * 这里替发送端补。FCS 按补齐后、去 tag 之前的线上帧计算 */
    total = (uint32_t)len;
    memcpy(s->rx_frame, frame, len);
    if (total < ETH_ZLEN) {
        memset(s->rx_frame + total, 0, ETH_ZLEN - total);
        total = ETH_ZLEN;
    }
    if (!(rctl & E1000_RCTL_SECRC)) {
        uint32_t fcs = eth_crc32(s->rx_frame, total);

        memcpy(s->rx_frame + total, &fcs, ETH_FCS_LEN);   /* x86 小端 */
        total += ETH_FCS_LEN;
    }
    /* VME 时剥掉 tag，TCI 放进描述符的 special 字段 */
    if (tagged && (*reg(s, E1000_CTRL) & E1000_CTRL_VME)) {
        special = get_be16(s->rx_frame + 14);
        status |= E1000_RXD_STAT_VP;
        memmove(s->rx_frame + 12, s->rx_frame + 16, total - 16);
        total -= VLAN_HLEN;
    }

    if (!rx_ring_put(s, total, status, special))
        res = NETDEV_RX_FULL;
out:
    pthread_mutex_unlock(&s->vm->io_lock);
    return res;
}

/* ------------------------------------------------------------------ */
/* 寄存器分发                                                          */
/* ------------------------------------------------------------------ */

/* 普通存储型寄存器和它们的可写位。表外、也不在下面 switch 里的偏移
 * 视为未实现。 */
struct reg_desc {
    uint32_t off;
    uint32_t count;     /* 连续多少个 dword，数组寄存器用 */
    uint32_t wmask;
};

static const struct reg_desc plain_regs[] = {
    { E1000_CTRL_EXT, 1, 0xFFFFFFFF },
    { E1000_FCAL,     1, 0xFFFFFFFF },
    { E1000_FCAH,     1, 0x0000FFFF },
    { E1000_FCT,      1, 0x0000FFFF },
    { E1000_VET,      1, 0x0000FFFF },
    { E1000_ITR,      1, 0x0000FFFF },
    { E1000_FCTTV,    1, 0x0000FFFF },
    { E1000_TIPG,     1, 0x3FFFFFFF },
    { E1000_AIT,      1, 0x0000FFFF },
    { E1000_LEDCTL,   1, 0xFFFFFFFF },
    { E1000_PBA,      1, 0x0000FFFF },
    { E1000_FCRTL,    1, 0x8000FFF8 },
    { E1000_FCRTH,    1, 0x0000FFF8 },
    { E1000_RDBAL,    1, 0xFFFFFFF0 },
    { E1000_RDBAH,    1, 0xFFFFFFFF },
    { E1000_RDLEN,    1, 0x000FFF80 },
    { E1000_RDH,      1, 0x0000FFFF },
    { E1000_RDTR,     1, 0x8000FFFF },
    { E1000_RXDCTL,   1, 0x01FFFFFF },
    { E1000_RADV,     1, 0x0000FFFF },
    { E1000_RSRPD,    1, 0x00000FFF },
    { E1000_TDBAL,    1, 0xFFFFFFF0 },
    { E1000_TDBAH,    1, 0xFFFFFFFF },
    { E1000_TDLEN,    1, 0x000FFF80 },
    { E1000_TDH,      1, 0x0000FFFF },
    { E1000_TIDV,     1, 0x0000FFFF },
    { E1000_TXDCTL,   1, 0x01FFFFFF },
    { E1000_TADV,     1, 0x0000FFFF },
    { E1000_TSPMT,    1, 0xFFFFFFFF },
    { E1000_RXCSUM,   1, 0x000007FF },
    { E1000_MTA,      E1000_MTA_N,      0xFFFFFFFF },
    { E1000_RA,       E1000_RA_N * 2,   0xFFFFFFFF },
    { E1000_VFTA,     E1000_VFTA_N,     0xFFFFFFFF },
    { E1000_WUC,      1, 0x0000001F },
    { E1000_WUFC,     1, 0x000F00FF },
    { E1000_WUS,      1, 0x00000000 },
    { E1000_MANC,     1, 0xFFFFFFFF },
};

static const struct reg_desc *plain_lookup(uint32_t off)
{
    size_t i;

    for (i = 0; i < sizeof(plain_regs) / sizeof(plain_regs[0]); i++)
        if (off >= plain_regs[i].off && off < plain_regs[i].off + 4 * plain_regs[i].count)
            return &plain_regs[i];
    return NULL;
}

static void unknown_reg(struct e1000 *s, uint32_t off, bool write, uint32_t val)
{
    uint32_t i = off >> 2;

    if (s->reg_warned[i / 8] & (1u << (i % 8)))
        return;
    s->reg_warned[i / 8] |= (uint8_t)(1u << (i % 8));
    if (write)
        vmm_warn("e1000: write 0x%08x to unimplemented register 0x%05x ignored", val, off);
    else
        vmm_warn("e1000: read of unimplemented register 0x%05x returns 0", off);
}

uint32_t e1000_reg_read(struct e1000 *s, uint32_t off)
{
    uint32_t v;

    off &= ~3u;
    switch (off) {
    case E1000_CTRL:
    case E1000_MDIC:
    case E1000_EERD:
    case E1000_IMS:
    case E1000_RCTL:
    case E1000_TCTL:
    case E1000_RDT:
    case E1000_TDT:
        return *reg(s, off);
    case E1000_STATUS:
        return E1000_STATUS_FD | E1000_STATUS_LU | E1000_STATUS_SPD_1000;
    case E1000_EECD:
        return eecd_read(s);
    case E1000_ICR:
        /* 读清零（第 13.4.17 节），随之撤销 INTx */
        v = *reg(s, E1000_ICR);
        *reg(s, E1000_ICR) = 0;
        update_irq(s);
        return v;
    case E1000_ICS:
    case E1000_IMC:
        return 0;                            /* 只写 */
    }
    if (off >= E1000_STATS_BEGIN && off < E1000_STATS_END)
        return 0;
    if (plain_lookup(off))
        return *reg(s, off);
    unknown_reg(s, off, false, 0);
    return 0;
}

void e1000_reg_write(struct e1000 *s, uint32_t off, uint32_t val)
{
    const struct reg_desc *r;

    off &= ~3u;
    switch (off) {
    case E1000_CTRL:
        if (val & E1000_CTRL_RST) {
            e1000_reset(s);                  /* RST 自清零 */
            return;
        }
        if ((val & E1000_CTRL_PHY_RST) && !(*reg(s, E1000_CTRL) & E1000_CTRL_PHY_RST))
            phy_reset(s);
        *reg(s, E1000_CTRL) = val;
        return;
    case E1000_STATUS:
        return;
    case E1000_EECD:
        eecd_write(s, val);
        return;
    case E1000_EERD:
        eerd_write(s, val);
        return;
    case E1000_MDIC:
        mdic_write(s, val);
        return;
    case E1000_ICR:
        *reg(s, E1000_ICR) &= ~val;          /* 写 1 清零 */
        update_irq(s);
        return;
    case E1000_ICS:
        raise_icr(s, val);
        return;
    case E1000_IMS:
        *reg(s, E1000_IMS) |= val & E1000_ICR_MASK;
        update_irq(s);
        return;
    case E1000_IMC:
        *reg(s, E1000_IMS) &= ~val;
        update_irq(s);
        return;
    case E1000_RCTL:
        *reg(s, E1000_RCTL) = val;
        /* 驱动可能先填好缓冲再打开 EN，这时后端也许正停着等缓冲 */
        if (s->net)
            netdev_rx_resume(s->net);
        return;
    case E1000_RDT:
        *reg(s, E1000_RDT) = val & 0xFFFF;
        if (s->net)
            netdev_rx_resume(s->net);
        return;
    case E1000_TCTL:
        *reg(s, E1000_TCTL) = val;
        tx_process(s);
        return;
    case E1000_TDT:
        *reg(s, E1000_TDT) = val & 0xFFFF;
        tx_process(s);
        return;
    }
    if (off >= E1000_STATS_BEGIN && off < E1000_STATS_END)
        return;
    r = plain_lookup(off);
    if (r) {
        *reg(s, off) = val & r->wmask;
        return;
    }
    unknown_reg(s, off, true, val);
}

/* BAR0：寄存器窗口。32 位访问是正常路径；更窄的读取整个寄存器（包括
 * ICR 读清零这类副作用）再截取，更窄的写被忽略（第 13.2 节）。 */
static int mmio_read(void *opaque, uint64_t off, uint32_t size, void *data)
{
    struct e1000 *s = opaque;
    uint32_t v = 0, sh = (uint32_t)(off & 3);

    if (size <= 4 && sh + size <= 4)
        v = e1000_reg_read(s, (uint32_t)off) >> (8 * sh);
    memcpy(data, &v, size < 4 ? size : 4);
    if (size > 4)
        memset((uint8_t *)data + 4, 0, size - 4);
    return VMM_OK;
}

static int mmio_write(void *opaque, uint64_t off, uint32_t size, const void *data)
{
    struct e1000 *s = opaque;
    uint32_t v;

    if (size != 4 || (off & 3)) {
        warn_once(s, W_PARTIAL_WRITE, "e1000: %u-byte write at 0x%05llx ignored (registers take 32-bit writes)",
                  size, (unsigned long long)off);
        return VMM_OK;
    }
    memcpy(&v, data, 4);
    e1000_reg_write(s, (uint32_t)off, v);
    return VMM_OK;
}

/* BAR2：IOADDR（偏移 0）选寄存器，IODATA（偏移 4）读写它（第 13.3 节）。
 * IOADDR 只认 0..0x1FFFC，超出寄存器窗口的 IODATA 访问读 0、写丢弃。 */
static int io_read(void *opaque, uint64_t off, uint32_t size, void *data)
{
    struct e1000 *s = opaque;
    uint32_t v = 0;

    if (off == 0 && size == 4)
        v = s->ioaddr;
    else if (off == 4 && size == 4 && s->ioaddr < E1000_MMIO_SIZE)
        v = e1000_reg_read(s, s->ioaddr);
    memcpy(data, &v, size < 4 ? size : 4);
    return VMM_OK;
}

static int io_write(void *opaque, uint64_t off, uint32_t size, const void *data)
{
    struct e1000 *s = opaque;
    uint32_t v;

    if (size != 4) {
        warn_once(s, W_PARTIAL_WRITE, "e1000: %u-byte IO write at +%llu ignored", size,
                  (unsigned long long)off);
        return VMM_OK;
    }
    memcpy(&v, data, 4);
    if (off == 0)
        s->ioaddr = v & 0x1FFFC;
    else if (off == 4 && s->ioaddr < E1000_MMIO_SIZE)
        e1000_reg_write(s, s->ioaddr, v);
    return VMM_OK;
}

/* ------------------------------------------------------------------ */
/* PCI 与 --device                                                     */
/* ------------------------------------------------------------------ */

int e1000_init(struct e1000 *s, const uint8_t mac[6])
{
    static bool crc_ready;

    if (!crc_ready) {
        crc32_init();
        crc_ready = true;
    }
    memcpy(s->mac, mac, ETH_ALEN);
    eeprom_build(s);
    e1000_reset(s);
    return VMM_OK;
}

static int parse_mac(const char *str, uint8_t mac[6])
{
    unsigned int b[6];
    char tail;
    int i;

    if (sscanf(str, "%2x:%2x:%2x:%2x:%2x:%2x%c", &b[0], &b[1], &b[2], &b[3],
               &b[4], &b[5], &tail) != 6)
        return VMM_ERR_INVAL;
    for (i = 0; i < 6; i++)
        mac[i] = (uint8_t)b[i];
    return VMM_OK;
}

static int e1000_create(const struct opts *o, void **dev)
{
    const char *m = opts_get(o, "mac");
    struct e1000 *s;
    uint8_t mac[6];

    if (!opts_require(o, "netdev"))
        return VMM_ERR_INVAL;
    if (parse_mac(m ? m : E1000_DEFAULT_MAC, mac) != VMM_OK) {
        vmm_err("%s %s: mac '%s' is not of the form xx:xx:xx:xx:xx:xx", o->what, o->arg, m);
        return VMM_ERR_INVAL;
    }
    if (mac[0] & 1) {
        vmm_err("%s %s: mac '%s' is a multicast address", o->what, o->arg, m);
        return VMM_ERR_INVAL;
    }
    s = calloc(1, sizeof(*s));
    if (!s)
        return VMM_ERR_NOMEM;
    e1000_init(s, mac);
    *dev = s;
    return VMM_OK;
}

static int e1000_attach(const struct vmm_device *d, struct vmm_vm *vm, struct pci_bus *bus)
{
    struct e1000 *s = d->dev;
    struct pci_device *p = &s->pci;

    s->vm  = vm;
    s->net = d->net;
    p->name             = "e1000";
    p->vendor_id        = E1000_VENDOR_ID;
    p->device_id        = E1000_DEVICE_ID_82540EM;
    p->class_code       = 0x020000;
    p->subsys_vendor_id = E1000_VENDOR_ID;
    p->subsys_id        = E1000_SUBSYS_ID;
    p->intx_pin         = 1;
    p->bus_master       = true;
    p->bars[0] = (struct pci_bar){ .type = PCI_BAR_MEM32, .size = E1000_MMIO_SIZE,
                                   .opaque = s, .read = mmio_read, .write = mmio_write };
    p->bars[2] = (struct pci_bar){ .type = PCI_BAR_IO, .size = E1000_IO_SIZE,
                                   .opaque = s, .read = io_read, .write = io_write };
    netdev_set_frontend(s->net, e1000_receive, s);
    vmm_info("e1000: mac %02x:%02x:%02x:%02x:%02x:%02x, netdev %s", s->mac[0], s->mac[1],
             s->mac[2], s->mac[3], s->mac[4], s->mac[5], s->net->id);
    return pci_register_device(bus, p);
}

static const char *const e1000_props[] = { "driver", "netdev", "mac", NULL };

const struct vmm_device_type e1000_device_type = {
    .name   = "e1000",
    .desc   = "Intel 82540EM gigabit NIC (fully emulated), needs netdev=, optional mac=",
    .props  = e1000_props,
    .create = e1000_create,
    .attach = e1000_attach,
};
