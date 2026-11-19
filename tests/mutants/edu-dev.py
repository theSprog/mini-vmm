# tests/mutants/edu-dev.py — 里程碑 edu-dev 的源码变异体，由 tests/mutate.py
# 读取
#
# 寄存器语义、DMA 范围检查、MSI 何时发、复位丢弃命令由单元测试挡住
# （tests=），默认的 negctl 会跑。中断闭环、DMA 方向和驱动的资源归还只有
# 真实 guest 驱动看得出来，是 e2e mutant（suite=），要跑它们：
#   tests/mutate.py edu-dev
# 判据见 tests/TEST_DESIGN.md 的 E38。

DEV = "src/devices/edu.c"
DRV = "tools/guest/edu/edu.c"

MUTANTS = [
    dict(
        name="msi-on-every-update",
        breaks="edu_regs.h: one MSI per INT_STATUS bit that goes from 0 to 1, none on ACK",
        file=DEV,
        old="    } else if (rise && pci_msi_enabled(&e->pci)) {\n",
        new="    } else if (pci_msi_enabled(&e->pci)) {\n",
        tests=["edu_test"],
    ),
    dict(
        name="raise-not-masked",
        breaks="edu_regs.h: INT_RAISE only accepts EDU_INT_SOFT_MASK bits",
        file=DEV,
        old="        edu_set_int(e, v & EDU_INT_SOFT_MASK);\n",
        new="        edu_set_int(e, v);\n",
        tests=["edu_test"],
    ),
    dict(
        name="busy-writes-accepted",
        breaks="edu_regs.h: DMA register writes while START is set are dropped",
        file=DEV,
        old="        if (busy) {\n",
        new="        if (0 && busy) {\n",
        tests=["edu_test"],
    ),
    dict(
        name="range-check-wraps",
        breaks="DMA_OFF + DMA_LEN computed in 32 bits wraps and passes the buffer bound",
        file=DEV,
        old="    if (e->run_len == 0 || e->run_off > EDU_BUF_SIZE ||\n"
            "        e->run_len > EDU_BUF_SIZE - e->run_off) {\n",
        new="    if (e->run_len == 0 || e->run_off + e->run_len > EDU_BUF_SIZE) {\n",
        tests=["edu_test"],
    ),
    dict(
        name="reset-keeps-command",
        breaks="edu_regs.h: reset discards the running command without an interrupt",
        file=DEV,
        old="    e->dma_cmd = 0;\n    e->dma_result = 0;\n",
        new="    e->dma_result = 0;\n",
        tests=["edu_test"],
    ),
    dict(
        name="buf-past-bar-end",
        breaks="MMIO dispatch only bounds the start address; the device must stop accesses that run past BAR0",
        file=DEV,
        old="    if (size <= EDU_BAR0_SIZE - off)\n",
        new="    if (1)\n",
        tests=["edu_test"],
    ),
    dict(
        name="msi-never-sent",
        breaks="with MSI enabled the device must signal completions by message",
        file=DEV,
        old="    } else if (rise && pci_msi_enabled(&e->pci)) {\n"
            "        pci_msi_notify(&e->pci);\n"
            "    }\n",
        new="    }\n",
        suite="edu-dev",
    ),
    dict(
        name="intx-never-raised",
        breaks="edu_regs.h: the INTA level equals INT_STATUS != 0",
        file=DEV,
        old="    pci_set_irq(&e->pci, e->int_status != 0);\n",
        new="    pci_set_irq(&e->pci, false);\n",
        suite="edu-dev",
    ),
    dict(
        name="dma-direction-swapped",
        breaks="edu_regs.h: EDU_DMA_CMD_TO_RAM = 1 moves buffer -> guest memory",
        file=DEV,
        old="        if (e->run_cmd & EDU_DMA_CMD_TO_RAM)\n",
        new="        if (!(e->run_cmd & EDU_DMA_CMD_TO_RAM))\n",
        suite="edu-dev",
    ),
    dict(
        name="done-count-missing",
        breaks="edu_regs.h: DMA_DONE_COUNT grows by one per completed command",
        file=DEV,
        old="    e->dma_done_count++;\n",
        new="",
        suite="edu-dev",
    ),
    dict(
        name="driver-keeps-enabled",
        breaks="every probe step is undone in remove: pci_enable_device pairs with pci_disable_device",
        file=DRV,
        old="\tpci_release_regions(pdev);\n\tpci_disable_device(pdev);\n\tkfree(e);\n",
        new="\tpci_release_regions(pdev);\n\tkfree(e);\n",
        suite="edu-dev",
    ),
]
