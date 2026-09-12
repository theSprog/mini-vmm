/* src/devices/rtc.c — MC146818 CMOS RTC（只读时钟 + 128 字节 CMOS）
 *
 * 不模拟它的话，端口 0x70/0x71 读回 0xFF，寄存器 A 的 UIP 位
 * （update in progress）永远为 1，mc146818_avoid_UIP() 的等待循环只能
 * 跑满超时：10000 圈、每圈 udelay(100) 加两次 CMOS_READ。两个调用者
 * （mach_get_cmos_time 和 rtc-cmos 探测里的 mc146818_does_rtc_work）
 * 各超时一次，实测整次启动多出 8 万次 PIO exit、约 2 秒（--trace-pio
 * 对比：30449 -> 110187 条，Run /init 从 0.75s 推到 2.70s），
 * 日志里还会留下 "Unable to read current time from RTC" 和
 * "rtc_cmos rtc_cmos: broken or not accessible"。
 *
 * 这里只做读时间：每次读时间寄存器都现取 host 的 UTC 时间，
 * 不跑内部时钟、不产生中断（IRQ 8），写时间寄存器被当作普通 CMOS 字节。
 */
#include <string.h>
#include <time.h>

#include "vmm.h"
#include "devices/rtc.h"

#define RTC_PORT_INDEX 0x70
#define RTC_NR_PORTS   2

#define RTC_SECONDS    0x00
#define RTC_MINUTES    0x02
#define RTC_HOURS      0x04
#define RTC_WDAY       0x06
#define RTC_MDAY       0x07
#define RTC_MONTH      0x08
#define RTC_YEAR       0x09
#define RTC_REG_A      0x0a
#define RTC_REG_B      0x0b
#define RTC_REG_C      0x0c
#define RTC_REG_D      0x0d
#define RTC_CENTURY    0x32

#define RTC_REG_A_DEFAULT 0x26   /* 32.768kHz 时基，1024Hz 周期，UIP=0 */
#define RTC_REG_B_24H     0x02   /* 24 小时制，BCD 编码，无中断 */
#define RTC_REG_D_VRT     0x80   /* 电池正常，CMOS 内容有效 */

static struct {
    uint8_t index;
    uint8_t cmos[128];
} g_rtc;

static uint8_t bin2bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static uint8_t rtc_read_reg(uint8_t reg)
{
    time_t now;
    struct tm tm;

    switch (reg) {
    case RTC_SECONDS: case RTC_MINUTES: case RTC_HOURS: case RTC_WDAY:
    case RTC_MDAY: case RTC_MONTH: case RTC_YEAR: case RTC_CENTURY:
        break;
    case RTC_REG_C:
        return 0;               /* 没有挂起的中断 */
    default:
        return g_rtc.cmos[reg];
    }

    now = time(NULL);
    gmtime_r(&now, &tm);
    switch (reg) {
    case RTC_SECONDS: return bin2bcd(tm.tm_sec);
    case RTC_MINUTES: return bin2bcd(tm.tm_min);
    case RTC_HOURS:   return bin2bcd(tm.tm_hour);
    case RTC_WDAY:    return bin2bcd(tm.tm_wday + 1);
    case RTC_MDAY:    return bin2bcd(tm.tm_mday);
    case RTC_MONTH:   return bin2bcd(tm.tm_mon + 1);
    case RTC_YEAR:    return bin2bcd(tm.tm_year % 100);
    default:          return bin2bcd((tm.tm_year + 1900) / 100);
    }
}

static int rtc_read(void *opaque, uint64_t off, uint32_t size, void *data)
{
    (void)opaque;
    memset(data, 0xff, size);
    if (off == 1)
        *(uint8_t *)data = rtc_read_reg(g_rtc.index);
    return VMM_OK;
}

static int rtc_write(void *opaque, uint64_t off, uint32_t size,
                     const void *data)
{
    uint8_t v = *(const uint8_t *)data;

    (void)opaque; (void)size;
    if (off == 0) {
        /* bit7 是 NMI 屏蔽位，和寄存器选择无关 */
        g_rtc.index = v & 0x7f;
    } else if (g_rtc.index != RTC_REG_C && g_rtc.index != RTC_REG_D) {
        g_rtc.cmos[g_rtc.index] = v;
    }
    return VMM_OK;
}

int rtc_attach(struct vmm_vm *vm)
{
    struct vmm_io_dev dev;

    memset(&g_rtc, 0, sizeof(g_rtc));
    g_rtc.cmos[RTC_REG_A] = RTC_REG_A_DEFAULT;
    g_rtc.cmos[RTC_REG_B] = RTC_REG_B_24H;
    g_rtc.cmos[RTC_REG_D] = RTC_REG_D_VRT;

    memset(&dev, 0, sizeof(dev));
    dev.name  = "cmos-rtc";
    dev.base  = RTC_PORT_INDEX;
    dev.len   = RTC_NR_PORTS;
    dev.read  = rtc_read;
    dev.write = rtc_write;
    return vmm_register_pio(vm, &dev);
}
