/* src/power.c — 整机电源事件，设计见 include/power.h          [acpi-events] */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "vmm.h"
#include "power.h"
#include "reset.h"
#include "monitor.h"
#include "boot/acpi.h"
#include "devices/acpi_pm.h"
#include "devices/irqchip.h"

_Static_assert(ACPI_PM_IO_LEN == ACPI_PM_REG_LEN,
               "FADT port block and the PM1 register file differ in size");
_Static_assert(ACPI_PM1A_CNT_PORT - ACPI_PM1A_EVT_PORT == ACPI_PM_OFF_CNT &&
               ACPI_SLEEP_CTL_PORT - ACPI_PM1A_EVT_PORT == ACPI_PM_OFF_SLEEP_CTL &&
               ACPI_SLEEP_STS_PORT - ACPI_PM1A_EVT_PORT == ACPI_PM_OFF_SLEEP_STS,
               "FADT register addresses do not match the PM1 register file");

static struct {
    struct vmm_vm *vm;
    struct acpi_pm pm;
} g_power;

static void set_sci(void *opaque, int level)
{
    struct vmm_vm *vm = opaque;

    if (irqchip_set_irq(vm, ACPI_SCI_IRQ, level) != VMM_OK)
        vmm_err("acpi: failed to set SCI (GSI %d) to %d", ACPI_SCI_IRQ, level);
}

static int pm_read(void *opaque, uint64_t off, uint32_t size, void *data)
{
    uint32_t v;

    (void)opaque;
    v = acpi_pm_read(&g_power.pm, (uint32_t)off, size);
    memset(data, 0, size);
    memcpy(data, &v, size > sizeof(v) ? sizeof(v) : size);
    return VMM_OK;
}

static int pm_write(void *opaque, uint64_t off, uint32_t size, const void *data)
{
    struct vmm_vm *vm = opaque;
    uint32_t v = 0;

    memcpy(&v, data, size > sizeof(v) ? sizeof(v) : size);
    switch (acpi_pm_write(&g_power.pm, (uint32_t)off, size, v)) {
    case ACPI_PM_POWEROFF:
        vmm_info("guest requested poweroff via ACPI S5 (port 0x%llx, value 0x%x)",
                 (unsigned long long)(ACPI_PM1A_EVT_PORT + off), v);
        vm->exit_code = 0;
        return VMM_ERR_EXIT;
    case ACPI_PM_SLEEP_IGNORED:
        vmm_warn("guest set SLP_EN with a sleep type other than S5 (port 0x%llx, "
                 "value 0x%x); only \\_S5 is declared, ignored",
                 (unsigned long long)(ACPI_PM1A_EVT_PORT + off), v);
        break;
    case ACPI_PM_NONE:
        break;
    }
    return VMM_OK;
}

static void power_reset(void *opaque)
{
    (void)opaque;
    acpi_pm_reset(&g_power.pm);
}

int power_attach(struct vmm_vm *vm)
{
    struct vmm_io_dev dev;
    int r;

    g_power.vm = vm;
    acpi_pm_init(&g_power.pm, set_sci, vm);
    memset(&dev, 0, sizeof(dev));
    dev.name   = "acpi-pm1a";
    dev.base   = ACPI_PM1A_EVT_PORT;
    dev.len    = ACPI_PM_IO_LEN;
    dev.opaque = vm;
    dev.read   = pm_read;
    dev.write  = pm_write;
    r = vmm_register_pio(vm, &dev);
    if (r == VMM_OK)
        r = vmm_register_reset(vm, "acpi-pm", power_reset, NULL);
    return r;
}

/* ---- monitor ---- */

/* 和 QEMU 的同名命令一样，只是按一下按钮，不等 guest 关机：guest 怎么
 * 处理（关机、忽略、弹对话框）是它自己的策略。回复里带上寄存器状态，
 * guest 没有打开 PWRBTN_EN 时明确说出来，否则“按了没反应”无从查起 */
static int cmd_system_powerdown(struct mon_req *req, int argc, char **argv)
{
    struct acpi_pm *pm = &g_power.pm;
    uint16_t sts, en;
    unsigned long long presses, raises;
    bool sci;

    (void)argc; (void)argv;
    if (!g_power.vm)
        return mon_err(req, "no ACPI power button (bare payload mode)");
    pthread_mutex_lock(&g_power.vm->io_lock);
    acpi_pm_press_power_button(pm);
    sts = pm->sts;
    en  = pm->en;
    sci = pm->sci;
    presses = pm->pwrbtn_presses;
    raises  = pm->sci_raises;
    pthread_mutex_unlock(&g_power.vm->io_lock);

    vmm_info("acpi: power button pressed (PM1_STS=0x%04x PM1_EN=0x%04x SCI=%d)",
             sts, en, sci);
    fprintf(mon_out(req), "power button pressed: PM1_STS=0x%04x PM1_EN=0x%04x SCI=%d\n",
            sts, en, sci);
    fprintf(mon_out(req), "presses since reset: %llu, SCI raised %llu times\n",
            presses, raises);
    if (!(en & ACPI_PM1_PWRBTN))
        fprintf(mon_out(req), "PWRBTN_EN is clear: the guest has not enabled the "
                "fixed power button, no SCI raised\n");
    return MON_OK;
}

int power_monitor_register(struct monitor *m)
{
    const struct mon_cmd c = {
        "system_powerdown", "", "press the ACPI power button",
        0, 0, cmd_system_powerdown, NULL,
    };

    return monitor_register(m, MON_TOP, &c);
}
