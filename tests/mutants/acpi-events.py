# tests/mutants/acpi-events.py — 里程碑 acpi-events 的源码变异体，由
# tests/mutate.py 读取
#
# PM1 寄存器的语义错误由单元测试挡住（tests=），默认的 negctl 会跑，判据见
# tests/unit/acpi_pm_test.c 开头列出的 ACPI 6.5 章节。ACPI 表的声明由
# boot_tables_test 挡住。SCI 的接线和 guest 的处理只有真实 guest 看得出来，
# 是 e2e mutant（suite=），要跑它们：
#   tests/mutate.py acpi-events
# 判据见 tests/TEST_DESIGN.md 的 E34。

PM = "src/devices/acpi_pm.c"
POWER = "src/power.c"
ACPI = "src/boot/acpi.c"
MPTABLE = "src/boot/mptable.c"

MUTANTS = [
    dict(
        name="pm1-sts-w1c-ignored",
        breaks="ACPI 6.5 Table 4.11: PWRBTN_STS is cleared by writing 1",
        file=PM,
        old="            pm->sts &= (uint16_t)~(b << sh);\n",
        new="",
        tests=["acpi_pm_test"],
    ),
    dict(
        name="pm1-sts-write-sets",
        breaks="status bits are set by hardware only; writing 1 clears",
        file=PM,
        old="            pm->sts &= (uint16_t)~(b << sh);\n",
        new="            pm->sts ^= (uint16_t)((b << sh) & PM1_STS_IMPL);\n",
        tests=["acpi_pm_test"],
    ),
    dict(
        name="pm1-en-ignored",
        breaks="4.8.2.2.1.1: SCI only while PWRBTN_EN and PWRBTN_STS are both set",
        file=PM,
        old="    bool level = (pm->sts & pm->en) != 0;\n",
        new="    bool level = pm->sts != 0;\n",
        tests=["acpi_pm_test"],
    ),
    dict(
        name="pm1-byte-access-high-byte",
        breaks="PM1 registers accept byte accesses (Tables 4.11-4.13)",
        file=PM,
        old="        unsigned int sh = 8 * (o & 1);\n",
        new="        unsigned int sh = 0;\n",
        tests=["acpi_pm_test"],
    ),
    dict(
        name="pm1-en-unimplemented-bits",
        breaks="4.8.3.1: unimplemented enable bits read 0",
        file=PM,
        old="            pm->en &= PM1_EN_IMPL;\n",
        new="",
        tests=["acpi_pm_test"],
    ),
    dict(
        name="pm1-gbl-en-not-sticking",
        breaks="ACPICA decides the global lock exists only if GBL_EN reads back 1",
        file=PM,
        old="#define PM1_EN_IMPL   (ACPI_PM1_PWRBTN | ACPI_PM1_GBL)\n",
        new="#define PM1_EN_IMPL   ACPI_PM1_PWRBTN\n",
        tests=["acpi_pm_test"],
    ),
    dict(
        name="facs-not-referenced",
        breaks="5.2.10: a non-HW-reduced platform has a FACS, found via FADT "
               "FIRMWARE_CTRL",
        file=ACPI,
        old="    fadt->x_firmware_ctrl = facs_gpa;\n",
        new="",
        tests=["boot_tables_test"],
    ),
    dict(
        name="facs-signature-stale",
        breaks="Table 5.13: the hardware signature changes when the tables change",
        file=ACPI,
        old="               tables_signature(base, at.facs));\n",
        new="               tables_signature(base, 16));\n",
        tests=["boot_tables_test"],
    ),
    dict(
        name="pm1-cnt-sci-en-zero",
        breaks="Table 4.13: SCI_EN reads 1 on a platform without SMI_CMD",
        file=PM,
        old="    uint16_t cnt = pm->cnt | ACPI_PM1_CNT_SCI_EN;\n",
        new="    uint16_t cnt = pm->cnt;\n",
        tests=["acpi_pm_test"],
    ),
    dict(
        name="pm-reset-keeps-state",
        breaks="reset returns PM1 to power-on values and lowers SCI",
        file=PM,
        old="    pm->sts       = 0;\n    pm->en        = 0;\n",
        new="",
        tests=["acpi_pm_test"],
    ),
    dict(
        name="fadt-pwr-button-control-method",
        breaks="Table 5.10: PWR_BUTTON clear declares the fixed power button",
        file=ACPI,
        old="    fadt->flags          = FADT_F_WBINVD | FADT_F_SLP_BUTTON | FADT_F_RESET_REG_SUP;\n",
        new="    fadt->flags          = FADT_F_WBINVD | FADT_F_PWR_BUTTON | FADT_F_SLP_BUTTON | FADT_F_RESET_REG_SUP;\n",
        tests=["boot_tables_test"],
    ),
    dict(
        name="mptable-sci-edge",
        breaks="the MP table must agree with ACPI: SCI is level, active low",
        file=MPTABLE,
        old="            s->irqflag = MP_IRQFLAG_ACTIVE_LOW | MP_IRQFLAG_LEVEL;\n",
        new="            s->irqflag = MP_IRQFLAG_ACTIVE_LOW;\n",
        tests=["boot_tables_test"],
    ),
    # ---- e2e：SCI 接线和 guest 的处理 ----
    dict(
        name="sci-wrong-gsi",
        breaks="FADT SCI_INT: the SCI is wired to GSI 9",
        file=POWER,
        old="    if (irqchip_set_irq(vm, ACPI_SCI_IRQ, level) != VMM_OK)\n",
        new="    if (irqchip_set_irq(vm, ACPI_SCI_IRQ + 1, level) != VMM_OK)\n",
        suite="acpi-events",
    ),
    dict(
        name="sci-stuck-after-w1c",
        breaks="writing 1 to PWRBTN_STS deasserts the SCI (Table 4.11)",
        file=POWER,
        old="    if (irqchip_set_irq(vm, ACPI_SCI_IRQ, level) != VMM_OK)\n",
        new="    if (!level) return;\n"
            "    if (irqchip_set_irq(vm, ACPI_SCI_IRQ, level) != VMM_OK)\n",
        suite="acpi-events",
    ),
    dict(
        name="monitor-press-without-sci",
        breaks="system_powerdown must go through the PM1 model so SCI follows",
        file=POWER,
        old="    acpi_pm_press_power_button(pm);\n",
        new="    pm->sts |= ACPI_PM1_PWRBTN;\n",
        suite="acpi-events",
    ),
    dict(
        name="s5-ignored",
        breaks="SLP_TYP=5 with SLP_EN set powers the machine off",
        file=POWER,
        old="    case ACPI_PM_POWEROFF:\n",
        new="    case ACPI_PM_POWEROFF: break;\n",
        suite="acpi-events",
    ),
]
