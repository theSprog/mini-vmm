/* src/devices/serial8250.c — 16550A UART 模拟                    [Step 2]
 *
 * 寄存器布局（offset 相对 base，DLAB = LCR bit7）：
 *   0  R: RBR  W: THR          DLAB=1: DLL
 *   1  IER                     DLAB=1: DLM
 *   2  R: IIR  W: FCR
 *   3  LCR
 *   4  MCR
 *   5  LSR（只读）
 *   6  MSR（只读）
 *   7  SCR
 */
#define _GNU_SOURCE
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "vmm.h"
#include "devices/serial8250.h"

#define UART_RX   0
#define UART_TX   0
#define UART_DLL  0
#define UART_IER  1
#define UART_DLM  1
#define UART_IIR  2
#define UART_FCR  2
#define UART_LCR  3
#define UART_MCR  4
#define UART_LSR  5
#define UART_MSR  6
#define UART_SCR  7

#define UART_IER_RDI   0x01   /* 接收数据可用中断 */
#define UART_IER_THRI  0x02   /* 发送保持寄存器空中断 */

#define UART_IIR_NO_INT 0x01
#define UART_IIR_THRI   0x02
#define UART_IIR_RDI    0x04
#define UART_IIR_FIFO   0xc0  /* bit7:6 = 11 -> 16550A，FIFO 已开启 */

#define UART_FCR_ENABLE 0x01
#define UART_FCR_CLR_RX 0x02
#define UART_FCR_CLR_TX 0x04

#define UART_LCR_DLAB  0x80
/* LCR 写成 0xBF 时，扩展 16550 兼容芯片把偏移 2 变成 EFR。我们不是那种
 * 芯片，但也不能让这些访问落到 IIR/FCR 上——autoconfig_16550a() 的变体
 * 探测会往 EFR 写 0xA8，落到 FCR 上正好会把 FIFO 使能位清掉。读回 0、
 * 写丢弃，等价于“这颗芯片没有 EFR”。 */
#define UART_LCR_CONF_B 0xbf

#define UART_MCR_DTR   0x01
#define UART_MCR_RTS   0x02
#define UART_MCR_OUT1  0x04
#define UART_MCR_OUT2  0x08
#define UART_MCR_LOOP  0x10

#define UART_LSR_DR    0x01
#define UART_LSR_THRE  0x20
#define UART_LSR_TEMT  0x40

#define UART_MSR_CTS   0x10
#define UART_MSR_DSR   0x20
#define UART_MSR_RI    0x40
#define UART_MSR_DCD   0x80

static bool rx_empty(const struct serial8250 *s)
{
    return s->rx_head == s->rx_tail;
}

/* 根据当前状态重算 IIR 并驱动中断线。调用者持锁。
 *
 * 16550 的中断优先级：接收线状态 > 接收数据 > THR 空 > modem 状态。
 * 我们只产生后两种里的前两个。IIR 只报告最高优先级的那个。 */
static void update_irq(struct serial8250 *s)
{
    uint8_t iir = UART_IIR_NO_INT;
    int level;

    if ((s->ier & UART_IER_RDI) && !rx_empty(s))
        iir = UART_IIR_RDI;
    else if ((s->ier & UART_IER_THRI) && s->thri_pending)
        iir = UART_IIR_THRI;

    if (s->fcr & UART_FCR_ENABLE)
        iir |= UART_IIR_FIFO;
    s->iir = iir;

    /* PC 上 COM 口的中断要经过 MCR.OUT2 这个门控才会到 PIC。
     * Linux 在打开端口时会置 OUT2，我们也照这个规矩来，否则 autoconfig
     * 探测 IRQ 时会误判。回环模式下中断不往外送。 */
    level = !(iir & UART_IIR_NO_INT) && (s->mcr & UART_MCR_OUT2)
            && !(s->mcr & UART_MCR_LOOP);

    if (level != s->irq_level) {
        s->irq_level = level;
        if (s->irq_set)
            s->irq_set(s->irq_opaque, s->irq, level);
    }
}

static void tx_byte(struct serial8250 *s, uint8_t c)
{
    ssize_t n;

    /* 回环模式：发出去的字节直接回到自己的接收端，不输出到 host */
    if (s->mcr & UART_MCR_LOOP) {
        uint32_t next = (s->rx_head + 1) % SERIAL_RX_BUF;
        if (next != s->rx_tail) {
            s->rx_buf[s->rx_head] = c;
            s->rx_head = next;
        }
        return;
    }

    do {
        n = write(s->out_fd, &c, 1);
    } while (n < 0 && errno == EINTR);
}

static uint8_t msr_value(const struct serial8250 *s)
{
    uint8_t m;

    if (!(s->mcr & UART_MCR_LOOP))
        return s->msr;

    /* 回环时 modem 输入线由 MCR 输出线直接驱动（16550 datasheet 规定）：
     *   RTS -> CTS, DTR -> DSR, OUT1 -> RI, OUT2 -> DCD
     * 谁会用到：autoconfig() 的 LOOP 测试写 MCR=LOOP|OUT2|RTS 后要读到
     * MSR 高 4 位 = DCD|CTS = 0x90；size_fifo() 也靠回环数 FIFO 深度。
     * 但在当前这套 guest 上两者都不会执行——x86 的 SERIAL_PORT_DFNS 给
     * COM1-COM3 带了 UPF_SKIP_TEST 跳过 LOOP 测试，size_fifo() 则被
     * CONFIG_SERIAL_8250_16550A_VARIANTS=n 挡在 autoconfig_16550a() 门口。
     * 把串口挪到 0x2e8（COM4 没有 UPF_SKIP_TEST）就会走到这里。 */
    m = 0;
    if (s->mcr & UART_MCR_RTS)  m |= UART_MSR_CTS;
    if (s->mcr & UART_MCR_DTR)  m |= UART_MSR_DSR;
    if (s->mcr & UART_MCR_OUT1) m |= UART_MSR_RI;
    if (s->mcr & UART_MCR_OUT2) m |= UART_MSR_DCD;
    return m;
}

static int serial_read(void *opaque, uint64_t off, uint32_t size, void *data)
{
    struct serial8250 *s = opaque;
    uint8_t v = 0;

    if (size != 1) {
        memset(data, 0xff, size);
        return VMM_OK;
    }

    pthread_mutex_lock(&s->lock);
    switch (off) {
    case UART_RX:
        if (s->lcr & UART_LCR_DLAB) {
            v = s->dll;
        } else if (!rx_empty(s)) {
            v = s->rx_buf[s->rx_tail];
            s->rx_tail = (s->rx_tail + 1) % SERIAL_RX_BUF;
        }
        break;
    case UART_IER:
        v = (s->lcr & UART_LCR_DLAB) ? s->dlm : s->ier;
        break;
    case UART_IIR:
        if (s->lcr == UART_LCR_CONF_B)
            break;              /* EFR 不存在，读回 0 */
        v = s->iir;
        /* 读 IIR 拿到 THRI 即视为已应答，这是 16550 的语义 */
        if ((v & 0x0f) == UART_IIR_THRI)
            s->thri_pending = false;
        break;
    case UART_LCR:
        v = s->lcr;
        break;
    case UART_MCR:
        v = s->mcr;
        break;
    case UART_LSR:
        v = UART_LSR_THRE | UART_LSR_TEMT;
        if (!rx_empty(s))
            v |= UART_LSR_DR;
        break;
    case UART_MSR:
        v = msr_value(s);
        break;
    case UART_SCR:
        v = s->scr;
        break;
    }
    update_irq(s);
    pthread_mutex_unlock(&s->lock);

    *(uint8_t *)data = v;
    return VMM_OK;
}

static int serial_write(void *opaque, uint64_t off, uint32_t size,
                        const void *data)
{
    struct serial8250 *s = opaque;
    uint8_t v = *(const uint8_t *)data;

    if (size != 1)
        return VMM_OK;

    pthread_mutex_lock(&s->lock);
    switch (off) {
    case UART_TX:
        if (s->lcr & UART_LCR_DLAB) {
            s->dll = v;
        } else {
            tx_byte(s, v);
            /* 我们是瞬间发完的，THR 立刻又空了 */
            s->thri_pending = true;
        }
        break;
    case UART_IER:
        if (s->lcr & UART_LCR_DLAB) {
            s->dlm = v;
        } else {
            /* 打开 THRI 的瞬间如果 THR 是空的，要马上来一次中断。
             * Linux 的 serial8250_start_tx 就是靠这个启动发送的。 */
            if (!(s->ier & UART_IER_THRI) && (v & UART_IER_THRI))
                s->thri_pending = true;
            s->ier = v & 0x0f;
        }
        break;
    case UART_FCR:
        if (s->lcr == UART_LCR_CONF_B)
            break;              /* EFR 不存在，写丢弃 */
        if (v & UART_FCR_CLR_RX)
            s->rx_head = s->rx_tail = 0;
        s->fcr = v & UART_FCR_ENABLE;
        break;
    case UART_LCR:
        s->lcr = v;
        break;
    case UART_MCR:
        s->mcr = v & 0x1f;
        break;
    case UART_SCR:
        s->scr = v;
        break;
    default:
        /* LSR/MSR 只读，写忽略 */
        break;
    }
    update_irq(s);
    pthread_mutex_unlock(&s->lock);
    return VMM_OK;
}

int serial8250_init(struct serial8250 *s, int out_fd)
{
    memset(s, 0, sizeof(*s));
    s->out_fd = out_fd;
    s->iir    = UART_IIR_NO_INT;
    s->lsr    = UART_LSR_THRE | UART_LSR_TEMT;
    /* modem 线常态：DCD/DSR/CTS 有效，像一根接好的线 */
    s->msr    = UART_MSR_DCD | UART_MSR_DSR | UART_MSR_CTS;
    s->irq    = -1;
    pthread_mutex_init(&s->lock, NULL);
    return VMM_OK;
}

int serial8250_attach(struct vmm_vm *vm, struct serial8250 *s,
                      uint16_t base, int irq)
{
    struct vmm_io_dev dev;

    memset(&dev, 0, sizeof(dev));
    dev.name   = "serial8250";
    dev.base   = base;
    dev.len    = SERIAL_NR_REGS;
    dev.opaque = s;
    dev.read   = serial_read;
    dev.write  = serial_write;

    s->irq = irq;
    vmm_info("serial8250 @ PIO 0x%x, IRQ %d", base, irq);
    return vmm_register_pio(vm, &dev);
}

bool serial8250_push_rx(struct serial8250 *s, uint8_t c)
{
    uint32_t next;
    bool ok = false;

    pthread_mutex_lock(&s->lock);
    next = (s->rx_head + 1) % SERIAL_RX_BUF;
    if (next != s->rx_tail) {
        s->rx_buf[s->rx_head] = c;
        s->rx_head = next;
        ok = true;
    }
    update_irq(s);
    pthread_mutex_unlock(&s->lock);
    return ok;
}
