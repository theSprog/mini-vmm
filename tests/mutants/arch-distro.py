# tests/mutants/arch-distro.py — 里程碑 arch-distro 的源码变异体，由 tests/mutate.py 读取
#
# 这些错误只有完整发行版走一遍启动、重启、关机才看得到，全是 e2e mutant
# （suite=），默认的 negctl 只检查 old 文字有没有过期。每次运行会在副本树的
# build/ 下复制一份 1 GiB 以上的盘，副本树在 $TMPDIR 里，/tmp 是 tmpfs 时
# 把 TMPDIR 指到磁盘上：
#   TMPDIR=$PWD/build tests/mutate.py arch-distro
# 每一项都要让 arch-distro 的某个用例 FAIL，判据见 tests/TEST_DESIGN.md 的 E31。
#
# 不收录“复位时不调设备的复位回调”：arch-distro 只用 AHCI 和 e1000，Linux
# 的 ahci 驱动在 probe 时写 GHC.HR（libahci.c 的 ahci_reset_controller），
# e1000 驱动写 CTRL.RST（e1000_hw.c 的 e1000_reset_hw），guest 自己把设备
# 复位了一遍，VMM 漏掉的复位在这里看不出来。这类错误由 system-reset suite
# 里 guest 不会自己复位的 pci-test 寄存器挡住（tests/mutants/system-reset.py
# 的 no-device-reset）。

RESET = "src/reset.c"

MUTANTS = [
    dict(
        name="reboot-exits",
        breaks="reset.h: a guest reboot resets the machine in place; "
               "systemctl reboot must come back to a second boot",
        file=RESET,
        old="    if (vm->cfg.no_reboot)\n"
            "        vmm_info(\"--no-reboot: stopping instead of resetting\");\n"
            "    else\n"
            "        __atomic_store_n(&vm->reset_requested, true, __ATOMIC_SEQ_CST);\n",
        new="    vmm_info(\"--no-reboot: stopping instead of resetting\");\n",
        suite="arch-distro",
    ),
]
