# tests/mutants/pvpanic.py — 里程碑 pvpanic 的源码变异体，由 tests/mutate.py
# 读取
#
# DSDT 里的设备描述和 AML 字符串编码由单元测试挡住（tests=），默认的 negctl
# 会跑。端口语义和退出码只有真实 guest 驱动看得出来，是 e2e mutant
# （suite=），要跑它们：
#   tests/mutate.py pvpanic
# 判据见 tests/TEST_DESIGN.md 的 E35。

DEV = "src/devices/pvpanic.c"
ACPI = "src/boot/acpi.c"
AML = "src/boot/aml.c"

MUTANTS = [
    dict(
        name="dsdt-device-missing",
        breaks="the guest driver binds through _HID QEMU0001 in the DSDT",
        file=ACPI,
        old="    if (devs->pvpanic)\n        build_pvpanic(&a);\n",
        new="",
        tests=["boot_tables_test"],
    ),
    dict(
        name="dsdt-device-always",
        breaks="without --device pvpanic nothing answers port 0x505",
        file=ACPI,
        old="    if (devs->pvpanic)\n        build_pvpanic(&a);\n",
        new="    build_pvpanic(&a);\n",
        tests=["boot_tables_test"],
    ),
    dict(
        name="dsdt-io-decode10",
        breaks="ACPI 6.5 Table 6.33: _DEC = 1 for a 16-bit decoded port",
        file=ACPI,
        old="#define RES_IO_DECODE16      0x01",
        new="#define RES_IO_DECODE16      0x00",
        tests=["boot_tables_test"],
    ),
    dict(
        name="aml-string-no-nul",
        breaks="ACPI 6.5 20.2.3: String := StringPrefix AsciiCharList NullChar",
        file=AML,
        old="    aml_bytes(a, s, strlen(s) + 1);\n",
        new="    aml_bytes(a, s, strlen(s));\n",
        tests=["aml_test"],
    ),
    dict(
        name="attach-flag-not-set",
        breaks="acpi.c describes the device only when acpi_devices.pvpanic is set",
        file=DEV,
        old="    if (r == VMM_OK)\n        ctx->acpi->pvpanic = true;\n",
        new="",
        suite="pvpanic",
    ),
    dict(
        name="read-returns-zero",
        breaks="pvpanic.rst: a read returns the bits the device recognizes",
        file=DEV,
        old="    *(uint8_t *)data = PVPANIC_FEATURES;\n",
        new="    *(uint8_t *)data = 0;\n",
        suite="pvpanic",
    ),
    dict(
        name="write-unknown-bits-kept",
        breaks="pvpanic.rst: bits the device does not recognize are ignored",
        file=DEV,
        old="    unsigned ev = v & PVPANIC_FEATURES;\n",
        new="    unsigned ev = v;\n",
        suite="pvpanic",
    ),
    dict(
        name="panic-exit-code-0",
        breaks="milestone: the exit code tells a panic from a normal reset",
        file=DEV,
        old="        vm->exit_code = VMM_EXIT_GUEST_PANIC;\n",
        new="        vm->exit_code = 0;\n",
        suite="pvpanic",
    ),
    dict(
        name="crash-loaded-stops-guest",
        breaks="pvpanic.rst bit 1: the host must not affect the guest's execution",
        file=DEV,
        old="             \"guest keeps running\", ev, names);\n    return VMM_OK;\n",
        new="             \"guest keeps running\", ev, names);\n    return VMM_ERR_EXIT;\n",
        suite="pvpanic",
    ),
    dict(
        name="shutdown-before-panicked",
        breaks="PANICKED takes precedence over SHUTDOWN (devices/pvpanic.h)",
        file=DEV,
        old="    if (ev & PVPANIC_PANICKED) {\n",
        new="    if ((ev & PVPANIC_PANICKED) && !(ev & PVPANIC_SHUTDOWN)) {\n",
        suite="pvpanic",
    ),
]
