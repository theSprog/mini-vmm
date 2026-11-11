/* tests/unit/e1000_test.c — e1000（82540EM）设备模型
 *
 * 链接真实的 src/devices/e1000.c，外部依赖全换成桩：PCI 的 DMA 落在一块
 * 假的 guest 内存上、INTx 电平记录成序列、后端发出的帧收进缓冲。RX 回调
 * 从 attach 登记给后端的函数指针拿到，走的是 I/O 线程会走的同一个入口。
 *
 * 判据来源，都不来自被测代码本身：
 *   - EEPROM：按 Linux e1000_hw.c 的 Microwire 读序列（acquire / shift_out
 *     / shift_in / standby）逐位驱动 EECD，校验和 0xBABA 是 SDM 第 5.6 节
 *   - PHY：Linux 认 82540 的 PHY ID 0x01410C20（M88E1011_I_PHY_ID），
 *     MII 状态寄存器 link 位 0x0004、自协商完成 0x0020（IEEE 802.3 第 22 章）
 *   - 校验和：按 RFC 1071 对收到的帧独立验算（含 TCP 伪首部），不复用
 *     被测的求和函数；FCS 用逐位 CRC-32 独立计算
 *   - TSO：各段拼回去必须等于原载荷，序号/ID/长度/FIN 按 SDM 第 3.5 节
 *   - 描述符布局与环语义：SDM 第 3.2、3.3、3.4 节
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmm.h"
#include "opts.h"
#include "net/netdev.h"
#include "devices/e1000.h"
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
    return VMM_OK;
}

static bool     g_irq;
static unsigned g_irq_edges;     /* 0 -> 1 的次数 */

void pci_set_irq(struct pci_device *dev, bool level)
{
    (void)dev;
    if (level && !g_irq)
        g_irq_edges++;
    g_irq = level;
}

int pci_register_device(struct pci_bus *bus, struct pci_device *dev)
{
    (void)bus;
    dev->bus = (struct pci_bus *)1;   /* 只要非 NULL：e1000_reset 据此决定是否更新 INTx */
    return VMM_OK;
}

/* 后端：收集发出的帧 */
#define MAX_FRAMES 64
static uint8_t  g_tx[MAX_FRAMES][16384];
static uint32_t g_tx_len[MAX_FRAMES];
static int      g_ntx;
static int      g_resumes;
static netdev_receive_fn g_rx_fn;
static void    *g_rx_opaque;

int netdev_send(struct netdev *nd, const struct iovec *iov, int iovcnt)
{
    uint32_t off = 0;
    int i;

    (void)nd;
    if (g_ntx == MAX_FRAMES)
        return VMM_ERR_SYS;
    for (i = 0; i < iovcnt; i++) {
        memcpy(g_tx[g_ntx] + off, iov[i].iov_base, iov[i].iov_len);
        off += (uint32_t)iov[i].iov_len;
    }
    g_tx_len[g_ntx++] = off;
    return VMM_OK;
}

void netdev_rx_resume(struct netdev *nd) { (void)nd; g_resumes++; }

void netdev_set_frontend(struct netdev *nd, netdev_receive_fn fn, void *opaque)
{
    (void)nd;
    g_rx_fn = fn;
    g_rx_opaque = opaque;
}

/* ------------------------------------------------------------------ */
/* 夹具                                                                */
/* ------------------------------------------------------------------ */

static const uint8_t MAC[6] = { 0x52, 0x54, 0x00, 0xAB, 0xCD, 0xEF };
static struct vmm_vm g_vm;
static struct netdev g_net;
static struct e1000 *S;

/* 每个用例一台新设备，经 --device 的同一条路径创建和挂载 */
static void fresh(void)
{
    struct opts o;
    struct vmm_device d = { 0 };
    void *dev = NULL;

    free(S);
    memset(g_ram, 0, sizeof(g_ram));
    g_master = true;
    g_irq = false;
    g_irq_edges = 0;
    g_ntx = 0;
    g_resumes = 0;
    if (opts_parse(&o, "--device", "e1000,netdev=n0,mac=52:54:00:ab:cd:ef", "driver") != VMM_OK ||
        e1000_device_type.create(&o, &dev) != VMM_OK) {
        printf("fixture: create failed: %s\n", g_last_log);
        exit(1);
    }
    opts_free(&o);
    d.type = &e1000_device_type;
    d.dev = dev;
    d.net = &g_net;
    e1000_device_type.attach(&d, &g_vm, NULL);
    S = dev;
}

static uint32_t rd(uint32_t off) { return e1000_reg_read(S, off); }
static void wr(uint32_t off, uint32_t v) { e1000_reg_write(S, off, v); }

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)be16(p) << 16 | be16(p + 2); }
static void wbe16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wbe32(uint8_t *p, uint32_t v) { wbe16(p, (uint16_t)(v >> 16)); wbe16(p + 2, (uint16_t)v); }

/* RFC 1071 反码和，独立于被测代码 */
static uint32_t sum16(uint32_t s, const uint8_t *p, uint32_t len)
{
    for (; len > 1; p += 2, len -= 2)
        s += (uint32_t)(p[0] << 8 | p[1]);
    if (len)
        s += (uint32_t)p[0] << 8;
    return s;
}

static uint16_t fold(uint32_t s)
{
    while (s >> 16)
        s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)s;
}

/* IPv4 头校验：连同校验和字段一起求和应为 0xFFFF */
static bool ip_ok(const uint8_t *ip)
{
    return fold(sum16(0, ip, (uint32_t)(ip[0] & 0xF) * 4)) == 0xFFFF;
}

/* TCP/UDP 校验：伪首部 + 整个 L4 段 */
static bool l4_ok(const uint8_t *ip, uint8_t proto)
{
    uint32_t ihl = (uint32_t)(ip[0] & 0xF) * 4, tot = be16(ip + 2), l4 = tot - ihl, s;

    s = sum16(0, ip + 12, 8);
    s += proto;
    s += l4;
    return fold(sum16(s, ip + ihl, l4)) == 0xFFFF;
}

/* 以太网 FCS：逐位 CRC-32（反射多项式 0xEDB88320），与被测的查表法独立 */
static uint32_t crc32_bitwise(const uint8_t *p, uint32_t len)
{
    uint32_t c = 0xFFFFFFFFu;
    int k;

    while (len--) {
        c ^= *p++;
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

/* ------------------------------------------------------------------ */
/* EEPROM                                                              */
/* ------------------------------------------------------------------ */

/* 照抄 Linux e1000_hw.c 的 Microwire 序列（e1000_acquire_eeprom、
 * e1000_shift_out_ee_bits、e1000_shift_in_ee_bits、e1000_standby_eeprom） */
static void ee_clk(uint32_t *e, bool high)
{
    *e = high ? (*e | E1000_EECD_SK) : (*e & ~E1000_EECD_SK);
    wr(E1000_EECD, *e);
}

static void ee_out(uint32_t *e, uint16_t data, int count)
{
    uint32_t mask = 1u << (count - 1);

    *e &= ~E1000_EECD_DO;
    do {
        *e &= ~E1000_EECD_DI;
        if (data & mask)
            *e |= E1000_EECD_DI;
        wr(E1000_EECD, *e);
        ee_clk(e, true);
        ee_clk(e, false);
        mask >>= 1;
    } while (mask);
    *e &= ~E1000_EECD_DI;
    wr(E1000_EECD, *e);
}

static uint16_t ee_in(uint32_t *e)
{
    uint16_t data = 0;
    int i;

    *e = rd(E1000_EECD) & ~(E1000_EECD_DO | E1000_EECD_DI);
    for (i = 0; i < 16; i++) {
        data = (uint16_t)(data << 1);
        ee_clk(e, true);
        *e = rd(E1000_EECD) & ~E1000_EECD_DI;
        if (*e & E1000_EECD_DO)
            data |= 1;
        ee_clk(e, false);
    }
    return data;
}

static bool ee_acquire(uint32_t *e)
{
    *e = rd(E1000_EECD) | E1000_EECD_REQ;
    wr(E1000_EECD, *e);
    if (!(rd(E1000_EECD) & E1000_EECD_GNT))
        return false;
    *e &= ~(E1000_EECD_DI | E1000_EECD_SK);
    wr(E1000_EECD, *e);
    *e |= E1000_EECD_CS;
    wr(E1000_EECD, *e);
    return true;
}

static void ee_standby(uint32_t *e)
{
    *e &= ~(E1000_EECD_CS | E1000_EECD_SK);
    wr(E1000_EECD, *e);
    ee_clk(e, true);
    *e |= E1000_EECD_CS;
    wr(E1000_EECD, *e);
    ee_clk(e, false);
}

static void ee_release(uint32_t *e)
{
    *e &= ~(E1000_EECD_CS | E1000_EECD_DI);
    wr(E1000_EECD, *e);
    ee_clk(e, true);
    ee_clk(e, false);
    *e &= ~E1000_EECD_REQ;
    wr(E1000_EECD, *e);
}

static void case_eeprom(void)
{
    uint16_t w[64], sum = 0;
    uint32_t e;
    int i;

    T_CASE("EEPROM：按 Linux 的 Microwire 序列读出 64 个字，MAC 和校验和正确，EERD 一致");
    fresh();
    CHECK(rd(E1000_EECD) & E1000_EECD_PRES, "EECD.PRES must report an EEPROM");
    CHECK(!(rd(E1000_EECD) & (1u << 9)), "EECD.SIZE must be 0 (64-word 93C46)");
    CHECK(ee_acquire(&e), "REQ must be granted");
    for (i = 0; i < 64; i++) {
        ee_out(&e, 0x6, 3);                  /* EEPROM_READ_OPCODE_MICROWIRE */
        ee_out(&e, (uint16_t)i, 6);
        w[i] = ee_in(&e);
        ee_standby(&e);
    }
    ee_release(&e);
    for (i = 0; i < 64; i++)
        sum = (uint16_t)(sum + w[i]);
    CHECK_U64(sum, 0xBABA, "sum of words 0..63");
    for (i = 0; i < 3; i++)
        CHECK_U64(w[i], (uint16_t)(MAC[2 * i] | MAC[2 * i + 1] << 8), "MAC word %d", i);
    CHECK_U64(w[0x0D], 0x100E, "device id word");
    for (i = 0; i < 64; i += 7) {
        wr(E1000_EERD, E1000_EERD_START | (uint32_t)i << 8);
        e = rd(E1000_EERD);
        CHECK(e & E1000_EERD_DONE, "EERD DONE for word %d", i);
        CHECK_U64(e >> 16, w[i], "EERD word %d", i);
    }

    T_CASE("EEPROM：顺序读跨字、写命令被忽略且不打乱后续读");
    fresh();
    ee_acquire(&e);
    ee_out(&e, 0x6, 3);
    ee_out(&e, 0x3E, 6);
    w[0] = ee_in(&e);
    w[1] = ee_in(&e);                        /* CS 一直高：顺序读到 0x3F */
    wr(E1000_EERD, E1000_EERD_START | 0x3F << 8);
    CHECK_U64(w[1], rd(E1000_EERD) >> 16, "sequential read continues to word 0x3F");
    ee_standby(&e);
    ee_out(&e, 0x5, 3);                      /* WRITE 01b */
    ee_out(&e, 0x00, 6);
    ee_out(&e, 0x1234, 16);
    ee_standby(&e);
    ee_out(&e, 0x6, 3);
    ee_out(&e, 0x00, 6);
    CHECK_U64(ee_in(&e), (uint16_t)(MAC[0] | MAC[1] << 8), "word 0 unchanged after WRITE");
    ee_release(&e);
}

/* ------------------------------------------------------------------ */
/* PHY 与中断                                                          */
/* ------------------------------------------------------------------ */

static uint32_t mdic(uint32_t op, uint32_t phy, uint32_t r, uint16_t data)
{
    wr(E1000_MDIC, op << 26 | phy << 21 | r << 16 | data);
    return rd(E1000_MDIC);
}

static void case_phy(void)
{
    uint32_t v;

    T_CASE("PHY：ID、link up、自协商完成；写入可读回；复位与重启自协商自清零");
    fresh();
    v = mdic(2, 1, PHY_ID1, 0);
    CHECK(v & E1000_MDIC_READY, "MDIC ready");
    CHECK(!(v & E1000_MDIC_ERROR), "MDIC no error");
    CHECK_U64((v & 0xFFFF) << 16 | (mdic(2, 1, PHY_ID2, 0) & 0xFFF0), 0x01410C20, "PHY id");
    v = mdic(2, 1, PHY_STATUS, 0) & 0xFFFF;
    CHECK((v & 0x0004) && (v & 0x0020), "link up and autoneg complete: 0x%x", v);
    mdic(1, 1, PHY_AUTONEG_ADV, 0x0DE1);
    CHECK_U64(mdic(2, 1, PHY_AUTONEG_ADV, 0) & 0xFFFF, 0x0DE1, "ANA readback");
    mdic(1, 1, PHY_STATUS, 0);
    CHECK_U64(mdic(2, 1, PHY_STATUS, 0) & 0xFFFF, v, "STATUS is read-only");
    mdic(1, 1, PHY_CTRL, 0x1340);             /* AN enable + restart */
    CHECK_U64(mdic(2, 1, PHY_CTRL, 0) & 0xFFFF, 0x1140, "restart AN self-clears");
    mdic(1, 1, PHY_CTRL, 0x8000);
    CHECK_U64(mdic(2, 1, PHY_CTRL, 0) & 0xFFFF, 0x1140, "PHY reset self-clears to the 88E1011 default");

    T_CASE("PHY：不存在的 PHY 地址置 ERROR；MDIC.I 产生 MDAC 中断");
    v = mdic(2, 2, PHY_ID1, 0);
    CHECK(v & E1000_MDIC_ERROR, "PHY address 2 must fail");
    wr(E1000_IMS, E1000_ICR_MDAC);
    wr(E1000_MDIC, 2u << 26 | 1u << 21 | E1000_MDIC_INT_EN);
    CHECK(g_irq, "MDAC raises INTx");
    CHECK_U64(rd(E1000_ICR), E1000_ICR_MDAC, "ICR");
    CHECK(!g_irq, "ICR read deasserts");
}

static void case_irq(void)
{
    T_CASE("中断：ICS 置位、IMS 屏蔽、ICR 读清零、写 1 清零、IMC");
    fresh();
    wr(E1000_ICS, E1000_ICR_LSC);
    CHECK(!g_irq, "masked cause must not assert INTx");
    wr(E1000_IMS, E1000_ICR_LSC | E1000_ICR_RXT0);
    CHECK(g_irq, "unmasking a pending cause asserts");
    CHECK_U64(rd(E1000_IMS), E1000_ICR_LSC | E1000_ICR_RXT0, "IMS readback");
    wr(E1000_IMC, E1000_ICR_LSC);
    CHECK(!g_irq, "IMC masks again");
    wr(E1000_IMS, E1000_ICR_LSC);
    wr(E1000_ICR, E1000_ICR_LSC);             /* 写 1 清零 */
    CHECK(!g_irq, "W1C clears the cause");
    CHECK_U64(rd(E1000_ICR), 0, "ICR empty");
    wr(E1000_ICS, E1000_ICR_RXT0 | E1000_ICR_TXDW);
    CHECK_U64(rd(E1000_ICR), E1000_ICR_RXT0 | E1000_ICR_TXDW, "read returns all causes");
    CHECK_U64(rd(E1000_ICR), 0, "and clears them");
    CHECK(!g_irq, "deasserted");
}

static void case_reset_and_access(void)
{
    uint8_t b[4];
    uint32_t v;

    T_CASE("CTRL.RST：寄存器回到复位值，RA0 由 EEPROM 重新装入，RST 自清零");
    fresh();
    wr(E1000_IMS, E1000_ICR_LSC);
    wr(E1000_ICS, E1000_ICR_LSC);
    wr(E1000_RCTL, E1000_RCTL_EN);
    wr(E1000_RA, 0);
    wr(E1000_CTRL, rd(E1000_CTRL) | E1000_CTRL_RST);
    CHECK(!g_irq, "reset deasserts INTx");
    CHECK_U64(rd(E1000_IMS), 0, "IMS");
    CHECK_U64(rd(E1000_RCTL), 0, "RCTL");
    CHECK(!(rd(E1000_CTRL) & E1000_CTRL_RST), "RST self-clears");
    CHECK_U64(rd(E1000_RA), 0xAB005452, "RAL0 from EEPROM");
    CHECK_U64(rd(E1000_RA + 4), 0x8000EFCD, "RAH0 with AV");
    v = rd(E1000_STATUS);
    CHECK((v & 3) == 3 && (v & 0xC0) == 0x80, "STATUS FD|LU|1000: 0x%x", v);

    T_CASE("访问宽度：窄写被忽略、窄读取对应字节；IO BAR 间接访问同一组寄存器");
    wr(E1000_RDTR, 0x11223344 & 0xFFFF);
    S->pci.bars[0].write(S, E1000_RDTR, 2, "\xff\xff");
    CHECK_U64(rd(E1000_RDTR), 0x3344, "16-bit write ignored");
    S->pci.bars[0].read(S, E1000_RDTR + 1, 1, b);
    CHECK_U64(b[0], 0x33, "byte read at +1");
    v = E1000_TIDV;
    S->pci.bars[2].write(S, 0, 4, &v);
    v = 0x1234;
    S->pci.bars[2].write(S, 4, 4, &v);
    CHECK_U64(rd(E1000_TIDV), 0x1234, "IODATA write lands in TIDV");
    v = 0;
    S->pci.bars[2].read(S, 4, 4, &v);
    CHECK_U64(v, 0x1234, "IODATA read");
    v = E1000_CTRL;
    S->pci.bars[2].write(S, 0, 4, &v);
    v = E1000_CTRL_RST;
    S->pci.bars[2].write(S, 4, 4, &v);
    CHECK_U64(rd(E1000_TIDV), 0, "CTRL.RST through the IO BAR resets (Linux 82540 path)");
}

/* ------------------------------------------------------------------ */
/* TX                                                                  */
/* ------------------------------------------------------------------ */

#define TX_RING  0x1000
#define TX_N     16
#define BUF      0x10000

static void tx_setup(void)
{
    wr(E1000_TDBAL, TX_RING);
    wr(E1000_TDBAH, 0);
    wr(E1000_TDLEN, TX_N * 16);
    wr(E1000_TDH, 0);
    wr(E1000_TDT, 0);
    wr(E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP);
    wr(E1000_IMS, E1000_ICR_TXDW | E1000_ICR_TXQE);
}

static uint8_t *txd(int i) { return g_ram + TX_RING + 16 * i; }

static void legacy(int i, uint64_t addr, uint16_t len, uint8_t cmd, uint8_t cso, uint8_t css)
{
    uint8_t *d = txd(i);

    memset(d, 0, 16);
    put64(d, addr);
    put16(d + 8, len);
    d[10] = cso;
    d[11] = cmd;
    d[13] = css;
}

static void ctxd(int i, uint8_t ipcss, uint8_t ipcso, uint16_t ipcse, uint8_t tucss,
                 uint8_t tucso, uint32_t paylen, uint8_t tucmd, uint8_t hdrlen, uint16_t mss)
{
    uint8_t *d = txd(i);

    memset(d, 0, 16);
    d[0] = ipcss; d[1] = ipcso; put16(d + 2, ipcse);
    d[4] = tucss; d[5] = tucso;
    put32(d + 8, paylen | (uint32_t)E1000_TXD_DTYP_CTX << 20);
    d[11] = tucmd | E1000_TXD_CMD_DEXT;
    d[13] = hdrlen;
    put16(d + 14, mss);
}

static void datad(int i, uint64_t addr, uint32_t len, uint8_t dcmd, uint8_t popts)
{
    uint8_t *d = txd(i);

    memset(d, 0, 16);
    put64(d, addr);
    put32(d + 8, len | (uint32_t)E1000_TXD_DTYP_DATA << 20);
    d[11] = dcmd | E1000_TXD_CMD_DEXT;
    d[13] = popts;
}

/* 一个以太网 + IPv4 + TCP 包，payload 是可辨认的递增字节 */
static uint32_t build_tcp(uint8_t *p, uint32_t payload, uint8_t flags)
{
    uint32_t i;

    memset(p, 0, 54);
    memcpy(p, "\x02\x00\x00\x00\x00\x01", 6);
    memcpy(p + 6, MAC, 6);
    wbe16(p + 12, 0x0800);
    p[14] = 0x45;
    wbe16(p + 16, (uint16_t)(40 + payload));
    wbe16(p + 18, 0x1000);                   /* IP ID */
    p[22] = 64; p[23] = 6;
    memcpy(p + 26, "\x0a\x00\x02\x0f\x0a\x00\x02\x01", 8);
    wbe16(p + 34, 40000); wbe16(p + 36, 9000);
    wbe32(p + 38, 0xFFFFF000);               /* 序号，故意让各段跨过 2^32 回绕 */
    p[46] = 5 << 4;
    p[47] = flags;
    wbe16(p + 48, 0xFFFF);
    for (i = 0; i < payload; i++)
        p[54 + i] = (uint8_t)(i * 7 + 3);
    return 54 + payload;
}

static void case_tx_legacy(void)
{
    uint8_t want[100];
    uint32_t s;
    int i;

    T_CASE("TX legacy：两段 SG 拼成一帧，IC 插入校验和，RS 回写 DD，TXDW|TXQE");
    fresh();
    tx_setup();
    for (i = 0; i < 100; i++)
        want[i] = (uint8_t)i;
    want[60] = want[61] = 0;                 /* 校验和字段，求和范围 [20, 末尾] */
    memcpy(g_ram + BUF, want, 30);
    memcpy(g_ram + BUF + 0x100, want + 30, 70);
    legacy(0, BUF, 30, 0, 0, 20);
    legacy(1, BUF + 0x100, 70, E1000_TXD_CMD_EOP | E1000_TXD_CMD_IC | E1000_TXD_CMD_RS, 60, 0);
    wr(E1000_TDT, 2);
    CHECK_U64(g_ntx, 1, "frames sent");
    CHECK_U64(g_tx_len[0], 100, "frame length");
    s = sum16(0, g_tx[0] + 20, 80);
    CHECK_U64(fold(s), 0xFFFF, "checksum over [20, end] verifies");
    CHECK(!memcmp(g_tx[0], want, 60) && !memcmp(g_tx[0] + 62, want + 62, 38), "payload intact");
    CHECK_U64(rd(E1000_TDH), 2, "TDH");
    CHECK(!(txd(0)[12] & 1), "no DD without RS");
    CHECK(txd(1)[12] & 1, "DD on the RS descriptor");
    CHECK_U64(rd(E1000_ICR), E1000_ICR_TXDW | E1000_ICR_TXQE, "ICR");

    T_CASE("TX：PSP 把短帧补到 60 字节；VLE 插入 802.1Q tag");
    memcpy(g_ram + BUF, want, 20);
    legacy(2, BUF, 20, E1000_TXD_CMD_EOP, 0, 0);
    wr(E1000_TDT, 3);
    CHECK_U64(g_tx_len[1], 60, "padded");
    CHECK(!memcmp(g_tx[1], want, 20) && g_tx[1][59] == 0, "pad is zero");
    wr(E1000_CTRL, rd(E1000_CTRL) | E1000_CTRL_VME);
    wr(E1000_VET, 0x8100);
    memcpy(g_ram + BUF, want, 100);
    legacy(3, BUF, 64, E1000_TXD_CMD_EOP | E1000_TXD_CMD_VLE, 0, 0);
    put16(txd(3) + 14, 0x2005);
    wr(E1000_TDT, 4);
    CHECK_U64(g_tx_len[2], 68, "tagged length");
    CHECK_U64(be32(g_tx[2] + 12), 0x81002005, "tag after the MACs");
    CHECK(!memcmp(g_tx[2] + 16, want + 12, 52), "rest shifted by 4");
}

static void case_tx_csum(void)
{
    uint8_t *p = g_ram + BUF;
    uint32_t len, s;

    T_CASE("TX 上下文 + 数据描述符：按 Linux e1000_tx_csum 的设置插入 IP 和 TCP 校验和");
    fresh();
    tx_setup();
    len = build_tcp(p, 333, 0x18);
    /* 协议栈的 CHECKSUM_PARTIAL：TCP 校验和字段里是含长度的伪首部和 */
    s = sum16(0, p + 26, 8) + 6 + (len - 34);
    wbe16(p + 50, fold(s));
    ctxd(0, 14, 24, 33, 34, 50, 0, E1000_TUCMD_TCP | E1000_TUCMD_IP, 0, 0);
    datad(1, BUF, 100, 0, E1000_TXD_POPTS_IXSM | E1000_TXD_POPTS_TXSM);
    datad(2, BUF + 100, len - 100, E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS, 0);
    wr(E1000_TDT, 3);
    CHECK_U64(g_ntx, 1, "one frame");
    CHECK_U64(g_tx_len[0], len, "length");
    CHECK(ip_ok(g_tx[0] + 14), "IP header checksum");
    CHECK(l4_ok(g_tx[0] + 14, 6), "TCP checksum");
    CHECK(!memcmp(g_tx[0] + 54, p + 54, 333), "payload intact");
}

static void case_tso(void)
{
    static uint8_t orig[8192];
    uint8_t *p = g_ram + BUF;
    uint32_t payload = 3000, mss = 1000, len, s, got = 0;
    int i, n;

    T_CASE("TSO：3000 字节按 MSS 1000 切 3 段，长度/ID/序号/FIN|PSH/校验和逐段正确");
    fresh();
    tx_setup();
    len = build_tcp(p, payload, 0x19);       /* FIN|PSH|ACK */
    /* Linux e1000_tso：tot_len、IP check 清零，TCP check 放长度为 0 的伪首部和 */
    wbe16(p + 16, 0);
    wbe16(p + 24, 0);
    s = sum16(0, p + 26, 8) + 6;
    wbe16(p + 50, fold(s));
    memcpy(orig, p, len);
    ctxd(0, 14, 24, 33, 34, 50, payload, E1000_TUCMD_TCP | E1000_TUCMD_IP | E1000_TUCMD_TSE, 54, (uint16_t)mss);
    /* 描述符边界故意不对齐段边界：头 54 + 载荷切成 700 字节一块 */
    datad(1, BUF, 54 + 700, E1000_TXD_CMD_TSE, E1000_TXD_POPTS_IXSM | E1000_TXD_POPTS_TXSM);
    n = 2;
    for (got = 700; got < payload; got += 700, n++)
        datad(n, BUF + 54 + got, payload - got < 700 ? payload - got : 700,
              E1000_TXD_CMD_TSE | (payload - got <= 700 ? E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS : 0), 0);
    wr(E1000_TDT, (uint32_t)n);
    CHECK_U64(g_ntx, 3, "segments");
    for (i = 0; i < g_ntx && i < 3; i++) {
        uint8_t *f = g_tx[i];

        CHECK_U64(g_tx_len[i], 54 + mss, "segment %d length", i);
        CHECK_U64(be16(f + 16), 40 + mss, "segment %d IP total length", i);
        CHECK_U64(be16(f + 18), 0x1000 + i, "segment %d IP ID", i);
        CHECK_U64(be32(f + 38), (uint32_t)(0xFFFFF000u + i * mss), "segment %d seq", i);
        CHECK_U64(f[47], i == 2 ? 0x19 : 0x10, "segment %d flags", i);
        CHECK(ip_ok(f + 14), "segment %d IP checksum", i);
        CHECK(l4_ok(f + 14, 6), "segment %d TCP checksum", i);
        CHECK(!memcmp(f + 54, orig + 54 + i * mss, mss), "segment %d payload", i);
    }
    CHECK(txd(n - 1)[12] & 1, "DD on the last descriptor");

    T_CASE("TSO：最后一段不足 MSS；其后的普通包不受分段上下文影响");
    fresh();
    tx_setup();
    payload = 2500;
    len = build_tcp(p, payload, 0x19);
    wbe16(p + 16, 0);
    s = sum16(0, p + 26, 8) + 6;
    wbe16(p + 50, fold(s));
    memcpy(orig, p, len);
    ctxd(0, 14, 24, 33, 34, 50, payload, E1000_TUCMD_TCP | E1000_TUCMD_IP | E1000_TUCMD_TSE, 54, (uint16_t)mss);
    datad(1, BUF, len, E1000_TXD_CMD_TSE | E1000_TXD_CMD_EOP, E1000_TXD_POPTS_IXSM | E1000_TXD_POPTS_TXSM);
    legacy(2, BUF, 80, E1000_TXD_CMD_EOP, 0, 0);
    wr(E1000_TDT, 3);
    CHECK_U64(g_ntx, 4, "3 segments + 1 plain frame");
    CHECK_U64(g_tx_len[2], 54 + 500, "short last segment");
    CHECK_U64(be16(g_tx[2] + 16), 40 + 500, "its IP length");
    CHECK(l4_ok(g_tx[2] + 14, 6), "its TCP checksum");
    CHECK_U64(g_tx_len[3], 80, "plain frame untouched by TSO");
}

static void case_tx_ring(void)
{
    T_CASE("TX 环：TDT 越界不处理；Bus Master 关闭时 DMA 失败、TDH 不动；EN=0 不发送");
    fresh();
    tx_setup();
    legacy(0, BUF, 60, E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS, 0, 0);
    wr(E1000_TDT, TX_N + 3);
    CHECK_U64(g_ntx, 0, "out-of-range TDT");
    CHECK_U64(rd(E1000_TDH), 0, "TDH");
    g_master = false;
    wr(E1000_TDT, 1);
    CHECK_U64(g_ntx, 0, "no DMA without bus master");
    CHECK_U64(rd(E1000_TDH), 0, "TDH stays");
    g_master = true;
    wr(E1000_TCTL, 0);
    wr(E1000_TDT, 1);
    CHECK_U64(g_ntx, 0, "TCTL.EN=0");
    wr(E1000_TCTL, E1000_TCTL_EN);
    CHECK_U64(g_ntx, 1, "enabling TX processes the pending descriptor");
    CHECK_U64(rd(E1000_TDH), 1, "TDH");

    T_CASE("TX 环：回绕");
    fresh();
    tx_setup();
    wr(E1000_TDH, TX_N - 1);
    wr(E1000_TDT, TX_N - 1);
    legacy(TX_N - 1, BUF, 60, E1000_TXD_CMD_EOP, 0, 0);
    legacy(0, BUF, 61, E1000_TXD_CMD_EOP, 0, 0);
    wr(E1000_TDT, 1);
    CHECK_U64(g_ntx, 2, "two frames across the wrap");
    CHECK_U64(g_tx_len[1], 61, "second frame");
    CHECK_U64(rd(E1000_TDH), 1, "TDH wrapped");
}

/* ------------------------------------------------------------------ */
/* RX                                                                  */
/* ------------------------------------------------------------------ */

#define RX_RING  0x2000
#define RX_N     8
#define RXBUF    0x40000

static uint8_t *rxd(int i) { return g_ram + RX_RING + 16 * i; }

/* 每个描述符的缓冲间隔 16 KiB，任何 BSIZE 都放得下 */
static void rx_setup(uint32_t extra_rctl)
{
    int i;

    for (i = 0; i < RX_N; i++) {
        memset(rxd(i), 0, 16);
        put64(rxd(i), RXBUF + 0x4000u * i);
    }
    wr(E1000_RDBAL, RX_RING);
    wr(E1000_RDLEN, RX_N * 16);
    wr(E1000_RDH, 0);
    wr(E1000_RDT, RX_N - 1);
    wr(E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | extra_rctl);
    wr(E1000_IMS, E1000_ICR_RXT0);
}

static enum netdev_rx_result rx(const uint8_t *f, uint32_t len)
{
    return g_rx_fn(g_rx_opaque, f, len);
}

static uint32_t frame(uint8_t *f, const uint8_t *dst, uint32_t len)
{
    uint32_t i;

    memcpy(f, dst, 6);
    memcpy(f + 6, "\x02\x11\x22\x33\x44\x55", 6);
    wbe16(f + 12, 0x0806);
    for (i = 14; i < len; i++)
        f[i] = (uint8_t)(i ^ 0x5A);
    return len;
}

static void case_rx_basic(void)
{
    uint8_t f[2048], *buf = g_ram + RXBUF;
    uint32_t fcs;

    T_CASE("RX：42 字节广播补到 60 + FCS，DD|EOP|IXSM，RDH 前进，RXT0");
    fresh();
    rx_setup(0);
    frame(f, (const uint8_t *)"\xff\xff\xff\xff\xff\xff", 42);
    CHECK_U64(rx(f, 42), NETDEV_RX_DONE, "accepted");
    CHECK_U64(g16(rxd(0) + 8), 64, "length includes padding and FCS");
    CHECK_U64(rxd(0)[12], E1000_RXD_STAT_DD | E1000_RXD_STAT_EOP | E1000_RXD_STAT_IXSM, "status");
    CHECK_U64(rxd(0)[13], 0, "errors");
    CHECK(!memcmp(buf, f, 42), "frame data");
    CHECK(buf[42] == 0 && buf[59] == 0, "zero padding");
    memcpy(&fcs, buf + 60, 4);
    CHECK_U64(fcs, crc32_bitwise(buf, 60), "FCS");
    CHECK_U64(rd(E1000_RDH), 1, "RDH");
    CHECK(g_irq, "RXT0 asserts INTx");
    CHECK(rd(E1000_ICR) & E1000_ICR_RXT0, "ICR.RXT0");

    T_CASE("RX：SECRC 剥 FCS；LPE=0 丢超长帧；RCTL.EN=0 丢弃");
    fresh();
    rx_setup(E1000_RCTL_SECRC);
    frame(f, MAC, 1514);
    rx(f, 1514);
    CHECK_U64(g16(rxd(0) + 8), 1514, "no FCS");
    frame(f, MAC, 1519);
    CHECK_U64(rx(f, 1519), NETDEV_RX_DONE, "oversize is dropped, not retried");
    CHECK_U64(rd(E1000_RDH), 1, "nothing written for oversize");
    wr(E1000_RCTL, 0);
    rx(f, 100);
    CHECK_U64(rd(E1000_RDH), 1, "RX disabled");
}

static void case_rx_filter(void)
{
    uint8_t f[128];
    static const uint8_t other[6] = { 0x02, 0, 0, 0, 0, 9 };
    static const uint8_t mdns[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb };

    T_CASE("RX 过滤：别人的单播丢弃、UPE 接收；组播按 MTA 哈希（MO=0 取位 47:36）");
    fresh();
    rx_setup(0);
    rx(f, frame(f, other, 64));
    CHECK_U64(rd(E1000_RDH), 0, "foreign unicast filtered");
    rx(f, frame(f, MAC, 64));
    CHECK_U64(rd(E1000_RDH), 1, "own unicast via RA0");
    rx(f, frame(f, mdns, 64));
    CHECK_U64(rd(E1000_RDH), 1, "multicast without MTA bit");
    /* Linux e1000_hash_mc_addr，mc_filter_type 0：(a[4] >> 4) | (a[5] << 4) = 0xFB0 */
    wr(E1000_MTA + 4 * (0xFB0 >> 5), 1u << (0xFB0 & 31));
    rx(f, frame(f, mdns, 64));
    CHECK_U64(rd(E1000_RDH), 2, "multicast via MTA");
    wr(E1000_RCTL, rd(E1000_RCTL) | E1000_RCTL_UPE);
    rx(f, frame(f, other, 64));
    CHECK_U64(rd(E1000_RDH), 3, "UPE");
    wr(E1000_RCTL, rd(E1000_RCTL) & ~(E1000_RCTL_UPE | E1000_RCTL_BAM));
    rx(f, frame(f, (const uint8_t *)"\xff\xff\xff\xff\xff\xff", 64));
    CHECK_U64(rd(E1000_RDH), 3, "broadcast needs BAM");
}

static void case_rx_ring(void)
{
    uint8_t f[2048];
    int i;

    T_CASE("RX 环满返回 FULL 且不写；写 RDT 通知后端；环回绕");
    fresh();
    rx_setup(E1000_RCTL_SECRC);
    for (i = 0; i < RX_N - 1; i++)
        CHECK_U64(rx(f, frame(f, MAC, 100 + i)), NETDEV_RX_DONE, "frame %d", i);
    CHECK_U64(rx(f, frame(f, MAC, 200)), NETDEV_RX_FULL, "ring full");
    CHECK_U64(rd(E1000_RDH), RX_N - 1, "RDH caught up with RDT");
    i = g_resumes;
    wr(E1000_RDT, 2);                        /* 驱动回收了 0..1，环上空出 RX_N-1、0、1 */
    CHECK_U64(g_resumes, i + 1, "RDT write resumes the backend");
    CHECK_U64(rx(f, frame(f, MAC, 201)), NETDEV_RX_DONE, "slot 7");
    CHECK_U64(rx(f, frame(f, MAC, 202)), NETDEV_RX_DONE, "slot 0 after wrap");
    CHECK_U64(g16(rxd(0) + 8), 202, "wrapped write");
    CHECK_U64(rd(E1000_RDH), 1, "RDH wrapped");

    T_CASE("RX 多描述符：256 字节缓冲装 600 字节帧，EOP 只在最后一个；null 缓冲只回写 DD");
    fresh();
    rx_setup(3u << E1000_RCTL_BSIZE_SHIFT | E1000_RCTL_SECRC);
    put64(rxd(1), 0);
    rx(f, frame(f, MAC, 600));
    CHECK_U64(g16(rxd(0) + 8), 256, "desc 0");
    CHECK_U64(rxd(0)[12], E1000_RXD_STAT_DD, "desc 0 not EOP");
    CHECK_U64(rxd(1)[12], E1000_RXD_STAT_DD, "null descriptor: DD only");
    CHECK_U64(g16(rxd(1) + 8), 0, "null descriptor: no length");
    CHECK_U64(g16(rxd(2) + 8), 256, "desc 2");
    CHECK_U64(g16(rxd(3) + 8), 88, "desc 3");
    CHECK(rxd(3)[12] & E1000_RXD_STAT_EOP, "EOP on the last");
    CHECK(!memcmp(g_ram + RXBUF + 0x4000 * 2, f + 256, 256), "data skips the null buffer");
    CHECK_U64(rd(E1000_RDH), 4, "RDH");
}

static void case_rx_vlan(void)
{
    uint8_t f[128];

    T_CASE("RX VLAN：VME 剥 tag、TCI 进 special、置 VP；VFE 按 VFTA 过滤");
    fresh();
    rx_setup(E1000_RCTL_SECRC | E1000_RCTL_VFE);
    wr(E1000_VET, 0x8100);
    wr(E1000_CTRL, rd(E1000_CTRL) | E1000_CTRL_VME);
    frame(f, MAC, 100);
    wbe16(f + 12, 0x8100);
    wbe16(f + 14, 0x6005);                   /* PCP 3、VID 5 */
    rx(f, 100);
    CHECK_U64(rd(E1000_RDH), 0, "VID 5 not in VFTA");
    wr(E1000_VFTA, 1u << 5);
    rx(f, 100);
    CHECK_U64(rd(E1000_RDH), 1, "VID 5 allowed");
    CHECK_U64(g16(rxd(0) + 8), 96, "tag stripped");
    CHECK(rxd(0)[12] & E1000_RXD_STAT_VP, "VP");
    CHECK_U64(g16(rxd(0) + 14), 0x6005, "special = TCI");
    CHECK(!memcmp(g_ram + RXBUF + 12, f + 16, 84), "payload after the tag");
}

int main(int argc, char **argv)
{
    T_MAIN_BEGIN();
    pthread_mutex_init(&g_vm.io_lock, NULL);
    strcpy(g_net.id, "n0");
    case_eeprom();
    case_phy();
    case_irq();
    case_reset_and_access();
    case_tx_legacy();
    case_tx_csum();
    case_tso();
    case_tx_ring();
    case_rx_basic();
    case_rx_filter();
    case_rx_ring();
    case_rx_vlan();
    free(S);
    return T_SUMMARY();
}
