/* include/devices/acpi_pm.h — ACPI PM1 固定功能寄存器与 SCI                [acpi-events]
 *
 * 一块 8 字节的 I/O 寄存器文件（偏移相对 ACPI_PM1A_EVT_PORT）：
 *   0  PM1a_STS       16 位，写 1 清零（ACPI 6.5 4.8.3.1.1，Table 4.11）
 *   2  PM1a_EN        16 位（4.8.3.1.2，Table 4.12）
 *   4  PM1a_CNT       16 位（4.8.3.2.1，Table 4.13）
 *   6  SLEEP_CONTROL  8 位，只写（4.8.3.7，Table 4.19）
 *   7  SLEEP_STATUS   8 位，WAK_STS 写 1 清零
 *
 * 实现的固定事件是电源按钮（PWRBTN_STS / PWRBTN_EN，bit 8）和全局锁的
 * 使能位 GBL_EN（bit 5）。规范要求未实现的位读回 0、写入无效（4.8.3.1），
 * 而 ACPICA 正是靠“写使能位再读回”判断一个固定事件有没有硬件：读回 0 就
 * 打印 "Could not enable ... event" 并当它不存在。
 *   - 全局锁：FACS 里的锁字是真的（boot/acpi.c），GBL_EN 能读回，ACPICA 才
 *     认为锁存在。GBL_STS 表示“固件释放了 OSPM 等着的锁”，这台机器没有会
 *     持有锁的固件，它永远不会置位，所以只有使能位、没有状态位
 *   - RTC 闹钟（RTC_STS/RTC_EN）：规范说这对位可选（4.8.2.4），实现了就
 *     必须在闹钟响时置 RTC_STS。只有支持睡眠唤醒时它才有用，这台机器只有
 *     S5，所以不实现，guest 会打印一次 RealTimeClock 的 ACPI Error
 *   - 其余固定事件 FADT 都没声明：没有 PM_TMR_BLK，SLP_BUTTON 标志置位
 *
 * SCI 的电平由状态位和使能位组合得出，不是“置位时发一次中断”：
 *   SCI = (PM1_STS & PM1_EN) != 0
 * 规范的原话是“while PWRBTN_EN and PWRBTN_STS are both set, an interrupt
 * event is raised”。所以先按按钮、后打开使能也会拉起 SCI，guest 写 1 清
 * 状态位或关掉使能位时 SCI 落下。SCI 是电平触发的（FADT SCI_INT 的描述），
 * 状态位不清掉线就一直有效，guest 不处理就会被反复打断，这正是电平中断要的
 * 语义。SCI_EN 恒为 1：FADT 的 SMI_CMD 为 0，表示平台一直处于 ACPI 模式，
 * 这时由硬件置 SCI_EN（Table 4.13），所以它不参与上面的组合。
 *
 * 访问宽度：规范要求 PM1 寄存器支持字节和字访问（4.8.3.1.1、4.8.3.1.2、
 * 4.8.3.2.1），所以读写都按字节拆开处理，单独写 STS 或 EN 的高字节也要
 * 生效。Linux 按 PM1_EVT_LEN / 2 = 2 字节访问，别的 OSPM 不一定。
 *
 * 这个文件只有寄存器语义，不认识 VM：SCI 经回调送出，S5 请求作为返回值
 * 交给调用方，所以单元测试不需要 KVM。接线（PIO 注册、SCI 接到 GSI 9、
 * 复位、monitor 命令）在 power.h。并发由调用方保证：所有入口都要求持有
 * vm->io_lock。
 */
#ifndef DEVICES_ACPI_PM_H
#define DEVICES_ACPI_PM_H

#include <stdbool.h>
#include <stdint.h>

#define ACPI_PM_OFF_STS        0
#define ACPI_PM_OFF_EN         2
#define ACPI_PM_OFF_CNT        4
#define ACPI_PM_OFF_SLEEP_CTL  6
#define ACPI_PM_OFF_SLEEP_STS  7
#define ACPI_PM_REG_LEN        8

#define ACPI_PM1_PWRBTN        (1u << 8)    /* PWRBTN_STS 和 PWRBTN_EN 同一位 */
#define ACPI_PM1_GBL           (1u << 5)    /* GBL_EN（GBL_STS 不实现） */

#define ACPI_PM1_CNT_SCI_EN    (1u << 0)
#define ACPI_PM1_CNT_GBL_RLS   (1u << 2)    /* 只写 */
#define ACPI_PM1_CNT_SLP_TYP(v) (((v) >> 10) & 0x7u)
#define ACPI_PM1_CNT_SLP_EN    (1u << 13)   /* 只写，读回 0 */
#define ACPI_PM1_CNT_RESERVED  (3u << 14)   /* 读回 0 */

#define ACPI_SLEEP_CTL_SLP_TYP(v) (((v) >> 2) & 0x7u)
#define ACPI_SLEEP_CTL_SLP_EN  (1u << 5)
#define ACPI_SLEEP_STS_WAK_STS (1u << 7)

/* DSDT 的 \_S5 给出的 SLP_TYP，两处要一致 */
#define ACPI_S5_SLP_TYP        5

/* 写操作的结果。设备只声明了 S5（DSDT 里只有 \_S5），其余睡眠类型的请求
 * 报出来由调用方记日志，不执行：guest 走到这里说明它没按 DSDT 办事 */
enum acpi_pm_action {
    ACPI_PM_NONE = 0,
    ACPI_PM_POWEROFF,           /* guest 经 PM1_CNT 或 SLEEP_CONTROL 请求了 S5 */
    ACPI_PM_SLEEP_IGNORED,      /* SLP_EN 置位，但 SLP_TYP 不是 S5 */
};

struct acpi_pm {
    uint16_t sts;
    uint16_t en;
    uint16_t cnt;               /* 不含 SCI_EN 和只写位 */
    uint8_t  sleep_sts;

    bool     sci;               /* 当前送出的 SCI 电平 */
    uint64_t sci_raises;        /* SCI 0→1 的次数，monitor 回复里报出 */
    uint64_t pwrbtn_presses;

    void   (*set_sci)(void *opaque, int level);
    void    *opaque;
};

void acpi_pm_init(struct acpi_pm *pm, void (*set_sci)(void *, int), void *opaque);

/* 上电值：寄存器全 0；SCI 当前为高时把它拉低 */
void acpi_pm_reset(struct acpi_pm *pm);

/* off + size 必须落在 [0, ACPI_PM_REG_LEN) 内，size 为 1、2 或 4。
 * 读：小端拼成的值；写：val 的低 size 字节 */
uint32_t acpi_pm_read(struct acpi_pm *pm, uint32_t off, uint32_t size);
enum acpi_pm_action acpi_pm_write(struct acpi_pm *pm, uint32_t off,
                                  uint32_t size, uint32_t val);

/* 按一下电源按钮：PWRBTN_STS 无条件置位（4.8.2.2.1.1），SCI 按组合重新计算 */
void acpi_pm_press_power_button(struct acpi_pm *pm);

#endif /* DEVICES_ACPI_PM_H */
