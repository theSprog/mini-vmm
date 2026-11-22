/* tests/unit/serial8250_test.c — 用 Linux 8250 的探测序列判定 UART 型号
 *
 * 判据（oracle）：Linux 6.6 drivers/tty/serial/8250/8250_port.c 里
 * autoconfig()、autoconfig_16550a()、size_fifo() 的寄存器访问顺序和判定条件，
 * 按原样转写在下面。我们要的结论只有一个：任何内核配置下都认成 16550A。
 *
 * 为什么要在单元测试里回放而不是只靠 e2e：e2e 只能覆盖手头那个 guest 内核
 * 的配置。CONFIG_SERIAL_8250_16550A_VARIANTS=n 时变体探测整段跳过，
 * VMM 在 LCR=0xBF、LCR=0xE0 等组合下的错误语义完全暴露不出来；
 * 换一个打开了这个选项的内核，端口就被认成 ST16650V2。这里两条路径都走。
 *
 * 转写时保留了 Linux 的函数结构和判定常量，改 8250_port.c 对照时按函数名找。
 * 与 Linux 的已知差异：mdelay()/锁/RSA 探测省略，它们不改变寄存器序列的判定。
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "vmm.h"
#include "devices/serial8250.h"
#include "harness.h"

/* ------------------------------------------------------------------ */
/* 桩：serial8250.c 引用的外部符号                                      */
/* ------------------------------------------------------------------ */

int vmm_log_level = -1;

void vmm_log(int level, const char *file, int line, const char *fmt, ...)
{
    (void)level; (void)file; (void)line; (void)fmt;
}

static struct vmm_io_dev g_dev;

int vmm_register_pio(struct vmm_vm *vm, const struct vmm_io_dev *dev)
{
    (void)vm;
    g_dev = *dev;
    return VMM_OK;
}

/* ------------------------------------------------------------------ */
/* serial_in / serial_out：经由真实的 PIO 回调访问设备                  */
/* ------------------------------------------------------------------ */

static unsigned serial_in(int off)
{
    uint8_t v = 0;
    g_dev.read(g_dev.opaque, (uint64_t)off, 1, &v);
    return v;
}

static void serial_out(int off, unsigned v)
{
    uint8_t b = (uint8_t)v;
    g_dev.write(g_dev.opaque, (uint64_t)off, 1, &b);
}

/* include/uapi/linux/serial_reg.h 里的常量 */
#define UART_RX   0
#define UART_TX   0
#define UART_IER  1
#define UART_IIR  2
#define UART_FCR  2
#define UART_EFR  2
#define UART_LCR  3
#define UART_MCR  4
#define UART_LSR  5
#define UART_MSR  6
#define UART_DLL  0
#define UART_DLM  1

#define UART_IER_ALL_INTR 0x0f
#define UART_IER_UUE      0x40
#define UART_FCR_ENABLE_FIFO 0x01
#define UART_FCR_CLEAR_RCVR  0x02
#define UART_FCR_CLEAR_XMIT  0x04
#define UART_FCR7_64BYTE     0x20
#define UART_IIR_FIFO_ENABLED        0xc0
#define UART_IIR_FIFO_ENABLED_8250   0x00
#define UART_IIR_FIFO_ENABLED_16550  0x80
#define UART_IIR_FIFO_ENABLED_16550A 0xc0
#define UART_IIR_64BYTE_FIFO         0x20
#define UART_LCR_CONF_MODE_A 0x80
#define UART_LCR_CONF_MODE_B 0xbf
#define UART_LCR_WLEN8       0x03
#define UART_MCR_RTS  0x02
#define UART_MCR_OUT2 0x08
#define UART_MCR_LOOP 0x10
#define UART_MSR_STATUS_BITS 0xf0
#define UART_MSR_DCD  0x80
#define UART_MSR_CTS  0x10
#define UART_LSR_DR   0x01

enum port_type {
    PORT_UNKNOWN, PORT_8250, PORT_16550, PORT_16550A, PORT_16650,
    PORT_16650V2, PORT_16550A_FSL64, PORT_NS16550A, PORT_16750,
    PORT_XSCALE, PORT_U6_16550A,
};

static const char *type_name(enum port_type t)
{
    static const char *n[] = {
        "unknown", "8250", "16550", "16550A", "ST16650", "ST16650V2",
        "16550A_FSL64", "NS16550A", "TI16750", "XScale", "U6_16550A",
    };
    return n[t];
}

/* 探测过程中值得单独断言的中间结果 */
static struct {
    unsigned efr_mode_a;   /* LCR=0x80 下读偏移 2 */
    unsigned efr_mode_b;   /* LCR=0xBF 下读偏移 2 */
    int      fifo_count;   /* size_fifo() 的返回值，-1 表示没执行 */
} g_obs;

static unsigned dl_read(void)
{
    return serial_in(UART_DLL) | (serial_in(UART_DLM) << 8);
}

static void dl_write(unsigned v)
{
    serial_out(UART_DLL, v & 0xff);
    serial_out(UART_DLM, (v >> 8) & 0xff);
}

static int size_fifo(void)
{
    unsigned old_fcr, old_mcr, old_lcr, old_dl;
    int count;

    old_lcr = serial_in(UART_LCR);
    serial_out(UART_LCR, 0);
    old_fcr = serial_in(UART_FCR);
    old_mcr = serial_in(UART_MCR);
    serial_out(UART_FCR, UART_FCR_ENABLE_FIFO |
               UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT);
    serial_out(UART_MCR, UART_MCR_LOOP);
    serial_out(UART_LCR, UART_LCR_CONF_MODE_A);
    old_dl = dl_read();
    dl_write(0x0001);
    serial_out(UART_LCR, UART_LCR_WLEN8);
    for (count = 0; count < 256; count++)
        serial_out(UART_TX, (unsigned)count);
    for (count = 0; (serial_in(UART_LSR) & UART_LSR_DR) && count < 256; count++)
        serial_in(UART_RX);
    serial_out(UART_FCR, old_fcr);
    serial_out(UART_MCR, old_mcr);
    serial_out(UART_LCR, UART_LCR_CONF_MODE_A);
    dl_write(old_dl);
    serial_out(UART_LCR, old_lcr);
    return count;
}

static enum port_type autoconfig_16550a(int variants)
{
    enum port_type type = PORT_16550A;
    unsigned status1, status2, iersave;

    if (!variants)
        return type;

    serial_out(UART_LCR, UART_LCR_CONF_MODE_A);
    g_obs.efr_mode_a = serial_in(UART_EFR);
    if (g_obs.efr_mode_a == 0) {
        serial_out(UART_EFR, 0xA8);
        if (serial_in(UART_EFR) != 0) {
            type = PORT_16650;
        } else {
            serial_out(UART_LCR, 0);
            serial_out(UART_FCR, UART_FCR_ENABLE_FIFO | UART_FCR7_64BYTE);
            status1 = serial_in(UART_IIR) &
                      (UART_IIR_64BYTE_FIFO | UART_IIR_FIFO_ENABLED);
            serial_out(UART_FCR, 0);
            serial_out(UART_LCR, 0);
            if (status1 == (UART_IIR_64BYTE_FIFO | UART_IIR_FIFO_ENABLED))
                type = PORT_16550A_FSL64;
        }
        serial_out(UART_EFR, 0);
        return type;
    }

    serial_out(UART_LCR, UART_LCR_CONF_MODE_B);
    g_obs.efr_mode_b = serial_in(UART_EFR);
    if (g_obs.efr_mode_b == 0)
        return PORT_16650V2;        /* autoconfig_has_efr()，没有芯片 ID 时的结论 */

    /* National Semiconductor SuperIO：bank 2 的 EXCR1 能反映 MCR.LOOP */
    serial_out(UART_LCR, 0);
    status1 = serial_in(UART_MCR);
    serial_out(UART_LCR, 0xE0);
    status2 = serial_in(0x02);
    if (!((status2 ^ status1) & UART_MCR_LOOP)) {
        serial_out(UART_LCR, 0);
        serial_out(UART_MCR, status1 ^ UART_MCR_LOOP);
        serial_out(UART_LCR, 0xE0);
        status2 = serial_in(0x02);
        serial_out(UART_LCR, 0);
        serial_out(UART_MCR, status1);
        if ((status2 ^ status1) & UART_MCR_LOOP)
            return PORT_NS16550A;
    }

    /* TI16750：只有 DLAB=1 时 64 字节 FIFO 位才会出现在 IIR bit5 */
    serial_out(UART_LCR, 0);
    serial_out(UART_FCR, UART_FCR_ENABLE_FIFO | UART_FCR7_64BYTE);
    status1 = serial_in(UART_IIR) & (UART_IIR_64BYTE_FIFO | UART_IIR_FIFO_ENABLED);
    serial_out(UART_FCR, UART_FCR_ENABLE_FIFO);
    serial_out(UART_LCR, UART_LCR_CONF_MODE_A);
    serial_out(UART_FCR, UART_FCR_ENABLE_FIFO | UART_FCR7_64BYTE);
    status2 = serial_in(UART_IIR) & (UART_IIR_64BYTE_FIFO | UART_IIR_FIFO_ENABLED);
    serial_out(UART_FCR, UART_FCR_ENABLE_FIFO);
    serial_out(UART_LCR, 0);
    if (status1 == UART_IIR_FIFO_ENABLED_16550A &&
        status2 == (UART_IIR_64BYTE_FIFO | UART_IIR_FIFO_ENABLED_16550A))
        return PORT_16750;

    /* XScale：IER bit6（UUE）可写 */
    iersave = serial_in(UART_IER);
    serial_out(UART_IER, iersave & ~UART_IER_UUE);
    if (!(serial_in(UART_IER) & UART_IER_UUE)) {
        serial_out(UART_IER, iersave | UART_IER_UUE);
        if (serial_in(UART_IER) & UART_IER_UUE)
            return PORT_XSCALE;
    }
    serial_out(UART_IER, iersave);

    g_obs.fifo_count = size_fifo();
    if (g_obs.fifo_count == 64)
        type = PORT_U6_16550A;
    return type;
}

/* skip_test 对应 UPF_SKIP_TEST（x86 COM1-COM3 带，COM4 不带）；
 * variants 对应 CONFIG_SERIAL_8250_16550A_VARIANTS */
static enum port_type autoconfig(int skip_test, int variants)
{
    unsigned scratch, scratch2, scratch3, status1, save_mcr, save_lcr;
    enum port_type type;

    memset(&g_obs, 0, sizeof(g_obs));
    g_obs.fifo_count = -1;

    scratch = serial_in(UART_IER);
    serial_out(UART_IER, 0);
    scratch2 = serial_in(UART_IER) & UART_IER_ALL_INTR;
    serial_out(UART_IER, UART_IER_ALL_INTR);
    scratch3 = serial_in(UART_IER) & UART_IER_ALL_INTR;
    serial_out(UART_IER, scratch);
    if (scratch2 != 0 || scratch3 != UART_IER_ALL_INTR)
        return PORT_UNKNOWN;

    save_mcr = serial_in(UART_MCR);
    save_lcr = serial_in(UART_LCR);

    if (!skip_test) {
        serial_out(UART_MCR, UART_MCR_LOOP | UART_MCR_OUT2 | UART_MCR_RTS);
        status1 = serial_in(UART_MSR) & UART_MSR_STATUS_BITS;
        serial_out(UART_MCR, save_mcr);
        if (status1 != (UART_MSR_DCD | UART_MSR_CTS))
            return PORT_UNKNOWN;
    }

    serial_out(UART_LCR, UART_LCR_CONF_MODE_B);
    serial_out(UART_EFR, 0);
    serial_out(UART_LCR, 0);
    serial_out(UART_FCR, UART_FCR_ENABLE_FIFO);

    switch (serial_in(UART_IIR) & UART_IIR_FIFO_ENABLED) {
    case UART_IIR_FIFO_ENABLED_8250:   type = PORT_8250;   break;
    case UART_IIR_FIFO_ENABLED_16550:  type = PORT_16550;  break;
    case UART_IIR_FIFO_ENABLED_16550A: type = autoconfig_16550a(variants); break;
    default:                           type = PORT_UNKNOWN; break;
    }

    serial_out(UART_LCR, save_lcr);
    return type;
}

/* ------------------------------------------------------------------ */

static struct serial8250 g_uart;

static void fresh_uart(void)
{
    /* 输出指向 /dev/null 的等价物：回环之外 THR 写出的字节没人关心 */
    serial8250_init(&g_uart, -1);
    serial8250_attach(NULL, &g_uart, SERIAL_COM1_BASE, SERIAL_COM1_IRQ);
}

static void check_probe(int skip_test, int variants)
{
    enum port_type t;

    fresh_uart();
    t = autoconfig(skip_test, variants);
    CHECK(t == PORT_16550A, "skip_test=%d variants=%d: probed as %s, want 16550A",
          skip_test, variants, type_name(t));
}

int main(int argc, char **argv)
{
    T_MAIN_BEGIN();

    T_CASE("VARIANTS=n, COM1 (UPF_SKIP_TEST): 16550A");
    check_probe(1, 0);

    T_CASE("VARIANTS=n, COM4 (LOOP test runs): 16550A");
    check_probe(0, 0);

    T_CASE("VARIANTS=y, COM1: full variant probe still ends at 16550A");
    check_probe(1, 1);

    T_CASE("VARIANTS=y, COM4: LOOP test + variant probe");
    check_probe(0, 1);

    /* 中间结果单独断言：型号碰巧对了但走错分支时也要报出来 */
    T_CASE("LCR=0xBF: offset 2 still reads IIR, never 0 (no EFR)");
    fresh_uart();
    autoconfig(1, 1);
    CHECK(g_obs.efr_mode_a != 0, "LCR=0x80 offset 2 read 0x%02x, want non-zero IIR",
          g_obs.efr_mode_a);
    CHECK(g_obs.efr_mode_b != 0, "LCR=0xBF offset 2 read 0x%02x, want non-zero IIR",
          g_obs.efr_mode_b);
    CHECK(g_obs.fifo_count >= 0 && g_obs.fifo_count != 64,
          "size_fifo() returned %d, want executed and != 64", g_obs.fifo_count);

    T_CASE("after the probe LOOP is off and LCR is restored");
    fresh_uart();
    autoconfig(1, 1);
    CHECK((serial_in(UART_MCR) & UART_MCR_LOOP) == 0, "MCR.LOOP left set: 0x%02x",
          serial_in(UART_MCR));
    CHECK(serial_in(UART_LCR) == 0, "LCR not restored: 0x%02x", serial_in(UART_LCR));

    return T_SUMMARY();
}
