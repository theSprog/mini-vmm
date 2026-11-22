/* tests/unit/acpi_pm_test.c — PM1 固定功能寄存器与 SCI 的语义
 *
 * 判据（oracle）都出自 ref/ACPI_Spec_6_5_Aug29.pdf：
 *   - 寄存器默认值 0，未实现的位读回 0、写入无效：4.8.3.1、Table 4.11/4.12。
 *     实现的使能位是 PWRBTN_EN（bit 8）和 GBL_EN（bit 5，FACS 里有全局锁，
 *     5.2.10.1）；状态位只有 PWRBTN_STS，GBL_STS 只在固件释放锁时置位，
 *     这台机器没有这样的固件
 *   - PWRBTN_STS 只由硬件置位、软件写 1 清零；“PWRBTN_EN 和 PWRBTN_STS 同时
 *     置位期间”产生中断：Table 4.11 bit 8、4.8.2.2.1.1
 *   - PM1 寄存器接受字节和字访问：4.8.3.1.1、4.8.3.1.2、4.8.3.2.1
 *   - SCI_EN 由硬件维护，SLP_EN 只写、读回 0，bit 15:14 读回 0：Table 4.13
 *   - 扩展睡眠寄存器 SLP_TYP 在 bit 4:2、SLP_EN 在 bit 5，WAK_STS 在
 *     SLEEP_STATUS bit 7、写 1 清零：4.8.3.7
 * 期望值在用例里按位写出，不调用被测代码里的宏去算。
 *
 * SCI 回调是一个记录器：记下每次电平变化，用来断言“只在变化时调用”和
 * “最终电平”，前者保证 irqchip 不会收到重复的同电平请求。
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "devices/acpi_pm.h"
#include "harness.h"

/* ------------------------------------------------------------------ */
/* SCI 记录器                                                          */
/* ------------------------------------------------------------------ */

static struct {
    int calls;
    int level;
} sci;

static void rec_sci(void *opaque, int level)
{
    (void)opaque;
    sci.calls++;
    sci.level = level;
}

static void fresh(struct acpi_pm *pm)
{
    memset(&sci, 0, sizeof(sci));
    acpi_pm_init(pm, rec_sci, NULL);
}

#define STS 0
#define EN  2
#define CNT 4
#define SLEEP_CTL 6
#define SLEEP_STS 7

/* ------------------------------------------------------------------ */

static void test_reset_values(void)
{
    struct acpi_pm pm;

    T_CASE("power-on values: STS/EN 0, PM1_CNT reads SCI_EN=1, SCI low");
    fresh(&pm);
    CHECK(acpi_pm_read(&pm, STS, 2) == 0, "PM1_STS 0x%x", acpi_pm_read(&pm, STS, 2));
    CHECK(acpi_pm_read(&pm, EN, 2) == 0, "PM1_EN 0x%x", acpi_pm_read(&pm, EN, 2));
    CHECK(acpi_pm_read(&pm, CNT, 2) == 0x0001, "PM1_CNT 0x%x (SCI_EN must "
          "read 1: SMI_CMD is 0, platform is always in ACPI mode)",
          acpi_pm_read(&pm, CNT, 2));
    CHECK(acpi_pm_read(&pm, SLEEP_STS, 1) == 0, "SLEEP_STATUS");
    CHECK(sci.calls == 0, "SCI touched %d times at init", sci.calls);
}

static void test_press_enabled(void)
{
    struct acpi_pm pm;

    T_CASE("press with PWRBTN_EN set raises SCI; W1C of PWRBTN_STS drops it");
    fresh(&pm);
    acpi_pm_write(&pm, EN, 2, 0x0100);
    CHECK(sci.calls == 0, "enabling alone raised SCI");
    acpi_pm_press_power_button(&pm);
    CHECK(acpi_pm_read(&pm, STS, 2) == 0x0100, "PM1_STS 0x%x", acpi_pm_read(&pm, STS, 2));
    CHECK(sci.calls == 1 && sci.level == 1, "SCI calls %d level %d", sci.calls, sci.level);
    CHECK(pm.sci_raises == 1, "sci_raises %llu", (unsigned long long)pm.sci_raises);

    /* 写 0 不清；写别的位不清 */
    acpi_pm_write(&pm, STS, 2, 0x0000);
    acpi_pm_write(&pm, STS, 2, 0xFEFF);
    CHECK(acpi_pm_read(&pm, STS, 2) == 0x0100, "PWRBTN_STS cleared by a write "
          "that has bit 8 = 0: 0x%x", acpi_pm_read(&pm, STS, 2));
    CHECK(sci.calls == 1 && sci.level == 1, "SCI changed: calls %d level %d",
          sci.calls, sci.level);

    acpi_pm_write(&pm, STS, 2, 0x0100);
    CHECK(acpi_pm_read(&pm, STS, 2) == 0, "PM1_STS after W1C 0x%x",
          acpi_pm_read(&pm, STS, 2));
    CHECK(sci.calls == 2 && sci.level == 0, "SCI after W1C: calls %d level %d",
          sci.calls, sci.level);

    /* 第二次按下：重新拉起，计数各加一 */
    acpi_pm_press_power_button(&pm);
    CHECK(sci.calls == 3 && sci.level == 1, "second press: calls %d level %d",
          sci.calls, sci.level);
    CHECK(pm.sci_raises == 2 && pm.pwrbtn_presses == 2, "raises %llu presses %llu",
          (unsigned long long)pm.sci_raises, (unsigned long long)pm.pwrbtn_presses);
}

static void test_press_disabled_then_enable(void)
{
    struct acpi_pm pm;

    T_CASE("press with PWRBTN_EN clear latches STS only; enabling later raises SCI");
    fresh(&pm);
    acpi_pm_press_power_button(&pm);
    CHECK(acpi_pm_read(&pm, STS, 2) == 0x0100, "STS is not set unconditionally");
    CHECK(sci.calls == 0, "SCI raised with PWRBTN_EN clear");

    acpi_pm_write(&pm, EN, 2, 0x0100);
    CHECK(sci.calls == 1 && sci.level == 1, "SCI not raised when EN set over a "
          "latched STS: calls %d level %d", sci.calls, sci.level);

    /* 关掉使能，状态位还在，SCI 落下 */
    acpi_pm_write(&pm, EN, 2, 0x0000);
    CHECK(sci.calls == 2 && sci.level == 0, "SCI after disabling: calls %d level %d",
          sci.calls, sci.level);
    CHECK(acpi_pm_read(&pm, STS, 2) == 0x0100, "disabling cleared the status bit");
}

static void test_repeated_press_no_retrigger(void)
{
    struct acpi_pm pm;

    T_CASE("a press while STS is still set keeps SCI high without a new edge");
    fresh(&pm);
    acpi_pm_write(&pm, EN, 2, 0x0100);
    acpi_pm_press_power_button(&pm);
    acpi_pm_press_power_button(&pm);
    CHECK(sci.calls == 1 && sci.level == 1, "calls %d level %d", sci.calls, sci.level);
    CHECK(pm.sci_raises == 1 && pm.pwrbtn_presses == 2, "raises %llu presses %llu",
          (unsigned long long)pm.sci_raises, (unsigned long long)pm.pwrbtn_presses);
}

static void test_unimplemented_bits(void)
{
    struct acpi_pm pm;

    T_CASE("unimplemented STS/EN bits read 0 and writes have no effect");
    fresh(&pm);
    acpi_pm_write(&pm, EN, 2, 0xFFFF);
    CHECK(acpi_pm_read(&pm, EN, 2) == 0x0120, "PM1_EN reads 0x%x, want only "
          "PWRBTN_EN and GBL_EN", acpi_pm_read(&pm, EN, 2));
    CHECK(sci.calls == 0, "enabling everything raised SCI with no status set");
    acpi_pm_write(&pm, EN, 2, 0xFEDF);
    CHECK(acpi_pm_read(&pm, EN, 2) == 0, "PM1_EN 0x%x", acpi_pm_read(&pm, EN, 2));
    /* 软件不能经写入置位状态位 */
    acpi_pm_write(&pm, EN, 2, 0x0100);
    acpi_pm_write(&pm, STS, 2, 0xFFFF);
    CHECK(acpi_pm_read(&pm, STS, 2) == 0 && sci.calls == 0,
          "writing 1s set a status bit: STS 0x%x SCI calls %d",
          acpi_pm_read(&pm, STS, 2), sci.calls);
}

static void test_gbl_en(void)
{
    struct acpi_pm pm;

    T_CASE("GBL_EN sticks (ACPICA's global lock probe) and never raises SCI");
    fresh(&pm);
    /* ACPICA acpi_enable_event：读 EN、置位、写回、再读回，不是 1 就判定没有
     * 全局锁硬件（evxfevnt.c、evglock.c） */
    acpi_pm_write(&pm, EN, 2, acpi_pm_read(&pm, EN, 2) | 0x0020);
    CHECK(acpi_pm_read(&pm, EN, 2) == 0x0020, "GBL_EN did not stick: 0x%x",
          acpi_pm_read(&pm, EN, 2));
    /* 软件写 STS 的 bit 5 不能造出 GBL_STS */
    acpi_pm_write(&pm, STS, 2, 0x0020);
    CHECK(acpi_pm_read(&pm, STS, 2) == 0 && sci.calls == 0,
          "GBL_STS 0x%x, SCI calls %d", acpi_pm_read(&pm, STS, 2), sci.calls);
    /* 电源按钮照常工作，GBL_EN 不影响它 */
    acpi_pm_write(&pm, EN, 2, 0x0120);
    acpi_pm_press_power_button(&pm);
    CHECK(sci.calls == 1 && sci.level == 1, "press with GBL_EN set: calls %d level %d",
          sci.calls, sci.level);
}

static void test_byte_access(void)
{
    struct acpi_pm pm;

    T_CASE("byte accesses: high bytes of STS and EN work on their own");
    fresh(&pm);
    /* 只写 EN 的高字节（端口 0x603） */
    acpi_pm_write(&pm, EN + 1, 1, 0x01);
    CHECK(acpi_pm_read(&pm, EN, 2) == 0x0100, "PM1_EN after byte write to its "
          "high byte: 0x%x", acpi_pm_read(&pm, EN, 2));
    /* 写 EN 的低字节不能把高字节冲掉 */
    acpi_pm_write(&pm, EN, 1, 0x00);
    CHECK(acpi_pm_read(&pm, EN, 2) == 0x0100, "low-byte write clobbered "
          "PWRBTN_EN: 0x%x", acpi_pm_read(&pm, EN, 2));
    CHECK(acpi_pm_read(&pm, EN + 1, 1) == 0x01, "byte read of EN high byte 0x%x",
          acpi_pm_read(&pm, EN + 1, 1));

    acpi_pm_press_power_button(&pm);
    CHECK(acpi_pm_read(&pm, STS + 1, 1) == 0x01, "byte read of STS high byte 0x%x",
          acpi_pm_read(&pm, STS + 1, 1));
    CHECK(acpi_pm_read(&pm, STS, 1) == 0x00, "byte read of STS low byte 0x%x",
          acpi_pm_read(&pm, STS, 1));
    /* W1C 写到低字节的 bit 0 不碰 bit 8 */
    acpi_pm_write(&pm, STS, 1, 0x01);
    CHECK(acpi_pm_read(&pm, STS, 2) == 0x0100 && sci.level == 1,
          "low-byte W1C cleared PWRBTN_STS");
    /* W1C 写到高字节 */
    acpi_pm_write(&pm, STS + 1, 1, 0x01);
    CHECK(acpi_pm_read(&pm, STS, 2) == 0 && sci.level == 0,
          "byte W1C at the high byte: STS 0x%x SCI %d",
          acpi_pm_read(&pm, STS, 2), sci.level);

    /* 一次 4 字节读跨 STS 和 EN：小端拼接 */
    acpi_pm_press_power_button(&pm);
    CHECK(acpi_pm_read(&pm, STS, 4) == 0x01000100u, "dword read 0x%08x",
          acpi_pm_read(&pm, STS, 4));
}

static void test_pm1_cnt(void)
{
    struct acpi_pm pm;

    T_CASE("PM1_CNT: SCI_EN read-only 1, SLP_EN/GBL_RLS write-only, reserved 0");
    fresh(&pm);
    acpi_pm_write(&pm, CNT, 2, 0x0000);
    CHECK(acpi_pm_read(&pm, CNT, 2) == 0x0001, "SCI_EN cleared by software: 0x%x",
          acpi_pm_read(&pm, CNT, 2));
    /* BM_RLD(1) 和 SLP_TYP=3 可以读回；GBL_RLS(2)、SLP_EN(13)、bit 15:14 不行 */
    CHECK(acpi_pm_write(&pm, CNT, 2, 0xC000 | (3u << 10) | 0x0004 | 0x0002) ==
          ACPI_PM_NONE, "plain write reported an action");
    CHECK(acpi_pm_read(&pm, CNT, 2) == ((3u << 10) | 0x0002 | 0x0001),
          "PM1_CNT reads 0x%x", acpi_pm_read(&pm, CNT, 2));
}

static void test_sleep_requests(void)
{
    struct acpi_pm pm;

    T_CASE("S5 via PM1_CNT word, PM1_CNT high byte and SLEEP_CONTROL; others ignored");
    fresh(&pm);
    /* Linux acpi_hw_legacy_sleep：先写 SLP_TYP，再写 SLP_TYP|SLP_EN */
    CHECK(acpi_pm_write(&pm, CNT, 2, 5u << 10) == ACPI_PM_NONE, "SLP_TYP alone");
    CHECK(acpi_pm_write(&pm, CNT, 2, (5u << 10) | (1u << 13)) == ACPI_PM_POWEROFF,
          "PM1_CNT word SLP_TYP=5|SLP_EN is not a poweroff");
    CHECK((acpi_pm_read(&pm, CNT, 2) & (1u << 13)) == 0, "SLP_EN reads back 1");

    fresh(&pm);
    /* SLP_TYP 和 SLP_EN 都在高字节：单写 0x605 一个字节 */
    CHECK(acpi_pm_write(&pm, CNT + 1, 1, (5u << 2) | (1u << 5)) == ACPI_PM_POWEROFF,
          "byte write to PM1_CNT high byte is not a poweroff");

    fresh(&pm);
    CHECK(acpi_pm_write(&pm, CNT, 2, (1u << 10) | (1u << 13)) == ACPI_PM_SLEEP_IGNORED,
          "SLP_TYP=1|SLP_EN not reported as ignored");

    fresh(&pm);
    CHECK(acpi_pm_write(&pm, SLEEP_CTL, 1, (5u << 2) | (1u << 5)) == ACPI_PM_POWEROFF,
          "SLEEP_CONTROL SLP_TYP=5|SLP_EN is not a poweroff");
    CHECK(acpi_pm_write(&pm, SLEEP_CTL, 1, 5u << 2) == ACPI_PM_NONE,
          "SLEEP_CONTROL without SLP_EN acted");
    CHECK(acpi_pm_read(&pm, SLEEP_CTL, 1) == 0, "SLEEP_CONTROL reads back");
}

static void test_sleep_status(void)
{
    struct acpi_pm pm;

    T_CASE("SLEEP_STATUS: only WAK_STS is W1C");
    fresh(&pm);
    pm.sleep_sts = 0x80;
    acpi_pm_write(&pm, SLEEP_STS, 1, 0x7F);
    CHECK(acpi_pm_read(&pm, SLEEP_STS, 1) == 0x80, "WAK_STS cleared by 0x7f");
    acpi_pm_write(&pm, SLEEP_STS, 1, 0x80);
    CHECK(acpi_pm_read(&pm, SLEEP_STS, 1) == 0, "WAK_STS not cleared by 0x80");
}

static void test_reset_lowers_sci(void)
{
    struct acpi_pm pm;

    T_CASE("reset clears the registers and lowers an asserted SCI");
    fresh(&pm);
    acpi_pm_write(&pm, EN, 2, 0x0100);
    acpi_pm_press_power_button(&pm);
    CHECK(sci.level == 1, "setup: SCI not high");
    acpi_pm_reset(&pm);
    CHECK(sci.calls == 2 && sci.level == 0, "after reset: calls %d level %d",
          sci.calls, sci.level);
    CHECK(acpi_pm_read(&pm, STS, 4) == 0, "STS/EN after reset 0x%x",
          acpi_pm_read(&pm, STS, 4));
    /* 复位后旧的使能不再生效：按下只锁存状态位 */
    acpi_pm_press_power_button(&pm);
    CHECK(sci.calls == 2, "SCI raised after reset with EN clear");
}

int main(int argc, char **argv)
{
    T_MAIN_BEGIN();
    test_reset_values();
    test_press_enabled();
    test_press_disabled_then_enable();
    test_repeated_press_no_retrigger();
    test_unimplemented_bits();
    test_gbl_en();
    test_byte_access();
    test_pm1_cnt();
    test_sleep_requests();
    test_sleep_status();
    test_reset_lowers_sci();
    return T_SUMMARY();
}
