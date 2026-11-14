# tests/mutants/system-reset.py — 里程碑 system-reset 的源码变异体，由 tests/mutate.py 读取
#
# 整机复位的错误只有让 guest 真的重启一次才看得到，所以这里全是 e2e
# mutant（suite=），默认的 negctl 只检查 old 文字有没有过期，要跑它们：
#   tests/mutate.py system-reset
# 每一项都要让对应 suite 的某个用例 FAIL，判据见 tests/TEST_DESIGN.md 的 E30。
#
# 不收录的错误实现：不恢复 MSR、复位前不拉低中断线、串口复位后不重算中断线。
# 它们让 system-reset suite 照样通过，原因见 TEST_DESIGN.md 的剩余风险，
# 放进来只会是永远 ESCAPED 的条目。

RESET = "src/reset.c"
GDB = "src/gdb/gdbstub.c"

VCPU_LOOP = ("    for (i = 0; i < vm->nr_vcpus && r == VMM_OK; i++)\n"
             "        r = vcpu_restore(po, &vm->vcpus[i]);\n")

MUTANTS = [
    dict(
        name="no-vcpu-restore",
        breaks="reset.h: every vCPU returns to its saved power-on state; "
               "an AP reset while running keeps executing the old kernel",
        file=RESET,
        old=VCPU_LOOP,
        new="",
        suite="system-reset",
    ),
    dict(
        name="bsp-only-restore",
        breaks="reset.h: APs need the restore too, sysrq-b resets them "
               "without stopping them first",
        file=RESET,
        old=VCPU_LOOP,
        new="    r = vcpu_restore(po, &vm->vcpus[0]);\n",
        suite="system-reset",
    ),
    dict(
        name="no-irqchip-restore",
        breaks="reset.h: in-kernel PIC/IOAPIC/PIT go back to power-on state",
        file=RESET,
        old="    if (po->has_irqchip)\n        r = irqchip_restore(vm, po);\n",
        new="",
        suite="system-reset",
    ),
    dict(
        name="no-device-reset",
        breaks="reset.h: every registered device reset handler runs",
        file=RESET,
        old="            vm->reset_handlers[i].fn(vm->reset_handlers[i].opaque);\n",
        new="",
        suite="system-reset",
    ),
    dict(
        name="gdb-bp-not-rewritten",
        breaks="gdbstub.h: software breakpoints are written again into the "
               "reloaded image and hit in the next boot",
        file=GDB,
        old="        g->bps.sw[i].orig = *hva;\n        *hva = GDB_X86_INT3;\n",
        new="        g->bps.sw[i].orig = *hva;\n",
        suite="gdbstub",
    ),
]
