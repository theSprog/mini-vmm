/* src/devices/acpi_pm.c — ACPI PM1 固定功能寄存器与 SCI            [acpi-events]
 *
 * 语义和出处见 include/devices/acpi_pm.h。
 */
#include <string.h>

#include "devices/acpi_pm.h"

/* 实现了的位；STS/EN 的其余位读回 0、写入无效（4.8.3.1）。GBL_EN 只在
 * 使能寄存器里：GBL_STS 由固件释放全局锁时置位，这台机器没有会持有全局锁
 * 的固件，它永远是 0 */
#define PM1_STS_IMPL  ACPI_PM1_PWRBTN
#define PM1_EN_IMPL   (ACPI_PM1_PWRBTN | ACPI_PM1_GBL)

static void update_sci(struct acpi_pm *pm)
{
    bool level = (pm->sts & pm->en) != 0;

    if (level == pm->sci)
        return;
    pm->sci = level;
    if (level)
        pm->sci_raises++;
    if (pm->set_sci)
        pm->set_sci(pm->opaque, level);
}

void acpi_pm_init(struct acpi_pm *pm, void (*set_sci)(void *, int), void *opaque)
{
    memset(pm, 0, sizeof(*pm));
    pm->set_sci = set_sci;
    pm->opaque  = opaque;
}

void acpi_pm_reset(struct acpi_pm *pm)
{
    pm->sts       = 0;
    pm->en        = 0;
    pm->cnt       = 0;
    pm->sleep_sts = 0;
    update_sci(pm);
}

static void image(const struct acpi_pm *pm, uint8_t r[ACPI_PM_REG_LEN])
{
    uint16_t cnt = pm->cnt | ACPI_PM1_CNT_SCI_EN;

    memset(r, 0, ACPI_PM_REG_LEN);
    r[ACPI_PM_OFF_STS]     = (uint8_t)pm->sts;
    r[ACPI_PM_OFF_STS + 1] = (uint8_t)(pm->sts >> 8);
    r[ACPI_PM_OFF_EN]      = (uint8_t)pm->en;
    r[ACPI_PM_OFF_EN + 1]  = (uint8_t)(pm->en >> 8);
    r[ACPI_PM_OFF_CNT]     = (uint8_t)cnt;
    r[ACPI_PM_OFF_CNT + 1] = (uint8_t)(cnt >> 8);
    /* SLEEP_CONTROL 只写，读回 0 */
    r[ACPI_PM_OFF_SLEEP_STS] = pm->sleep_sts;
}

uint32_t acpi_pm_read(struct acpi_pm *pm, uint32_t off, uint32_t size)
{
    uint8_t r[ACPI_PM_REG_LEN];
    uint32_t v = 0, i;

    image(pm, r);
    for (i = 0; i < size && off + i < ACPI_PM_REG_LEN; i++)
        v |= (uint32_t)r[off + i] << (8 * i);
    return v;
}

static bool sleep_request(uint32_t slp_en, uint32_t slp_typ,
                          enum acpi_pm_action *act)
{
    if (!slp_en)
        return false;
    *act = slp_typ == ACPI_S5_SLP_TYP ? ACPI_PM_POWEROFF : ACPI_PM_SLEEP_IGNORED;
    return true;
}

enum acpi_pm_action acpi_pm_write(struct acpi_pm *pm, uint32_t off,
                                  uint32_t size, uint32_t val)
{
    enum acpi_pm_action act = ACPI_PM_NONE;
    uint32_t i;

    for (i = 0; i < size && off + i < ACPI_PM_REG_LEN; i++) {
        uint32_t o = off + i;
        uint16_t b = (uint16_t)((val >> (8 * i)) & 0xFF);
        unsigned int sh = 8 * (o & 1);

        switch (o) {
        case ACPI_PM_OFF_STS:
        case ACPI_PM_OFF_STS + 1:
            pm->sts &= (uint16_t)~(b << sh);
            break;
        case ACPI_PM_OFF_EN:
        case ACPI_PM_OFF_EN + 1:
            pm->en = (uint16_t)((pm->en & ~(0xFFu << sh)) | (b << sh));
            pm->en &= PM1_EN_IMPL;
            break;
        case ACPI_PM_OFF_CNT:
        case ACPI_PM_OFF_CNT + 1: {
            uint16_t cnt = (uint16_t)((pm->cnt & ~(0xFFu << sh)) | (b << sh));

            /* SLP_TYP 和 SLP_EN 都在高字节，只有写到高字节才可能触发睡眠 */
            if (o == ACPI_PM_OFF_CNT + 1)
                sleep_request(cnt & ACPI_PM1_CNT_SLP_EN,
                              ACPI_PM1_CNT_SLP_TYP(cnt), &act);
            pm->cnt = cnt & (uint16_t)~(ACPI_PM1_CNT_SCI_EN | ACPI_PM1_CNT_GBL_RLS |
                                        ACPI_PM1_CNT_SLP_EN | ACPI_PM1_CNT_RESERVED);
            break;
        }
        case ACPI_PM_OFF_SLEEP_CTL:
            sleep_request(b & ACPI_SLEEP_CTL_SLP_EN, ACPI_SLEEP_CTL_SLP_TYP(b), &act);
            break;
        case ACPI_PM_OFF_SLEEP_STS:
            pm->sleep_sts &= (uint8_t)~(b & ACPI_SLEEP_STS_WAK_STS);
            break;
        }
    }
    update_sci(pm);
    return act;
}

void acpi_pm_press_power_button(struct acpi_pm *pm)
{
    pm->sts |= ACPI_PM1_PWRBTN;
    pm->pwrbtn_presses++;
    update_sci(pm);
}
