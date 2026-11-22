# tests/mutants/pci-msix.py — 里程碑 pci-msix 的源码变异体，由 tests/mutate.py
# 读取
#
# PCI 核心的 MSI-X 规则（capability 与 table 的读写属性、屏蔽、pending、补发、
# 与 INTx 互斥、复位）由单元测试 pci_bus_test 挡住，edu 按字节分项和清
# pending 由 edu_test 挡住（tests=），默认的 negctl 会跑。屏蔽语义在真实
# Linux guest 里能不能被观察到，由 e2e mutant（suite=）证明，要跑它们：
#   tests/mutate.py pci-msix
# 判据见 tests/TEST_DESIGN.md 的 E39。

PCI = "src/pci/pci.c"
DEV = "src/devices/edu.c"

MUTANTS = [
    # ---- PCI 核心：PCI 3.0 第 6.8.2、6.8.3 节 ----
    dict(
        name="entry-mask-ignored",
        breaks="6.8.3.5: a masked entry must not send, it sets its pending bit",
        file=PCI,
        old="    return msix_func_masked(d) || (d->msix_table[n].ctrl & PCI_MSIX_ENTRY_CTRL_MASKBIT);\n",
        new="    return msix_func_masked(d);\n",
        tests=["pci_bus_test"],
    ),
    dict(
        name="function-mask-ignored",
        breaks="6.8.3.5: Function Mask masks every entry regardless of its own Mask bit",
        file=PCI,
        old="    return msix_func_masked(d) || (d->msix_table[n].ctrl & PCI_MSIX_ENTRY_CTRL_MASKBIT);\n",
        new="    return (d->msix_table[n].ctrl & PCI_MSIX_ENTRY_CTRL_MASKBIT);\n",
        tests=["pci_bus_test"],
    ),
    dict(
        name="no-replay-on-entry-unmask",
        breaks="6.8.3.5: unmasking an entry with its pending bit set sends the message",
        file=PCI,
        old="        e->ctrl = v & PCI_MSIX_ENTRY_CTRL_MASKBIT;\n        msix_flush(d);\n",
        new="        e->ctrl = v & PCI_MSIX_ENTRY_CTRL_MASKBIT;\n",
        tests=["pci_bus_test"],
    ),
    dict(
        name="no-replay-on-function-unmask",
        breaks="6.8.3.5: clearing Function Mask may send many pending messages",
        file=PCI,
        old="            intx_line_update(bus, d);\n        msix_flush(d);\n",
        new="            intx_line_update(bus, d);\n",
        tests=["pci_bus_test"],
    ),
    dict(
        name="pending-kept-after-replay",
        breaks="6.8.3.5: the pending bit clears once the message has been sent",
        file=PCI,
        old="            msix_send(d, n);\n            d->msix_pba &= ~bit;\n",
        new="            msix_send(d, n);\n",
        tests=["pci_bus_test"],
    ),
    dict(
        name="clear-pending-ignored",
        breaks="6.8.3.5: the function clears a pending bit once the interrupt event is satisfied",
        file=PCI,
        old="    d->msix_pba &= ~(1ULL << n);\n}\n",
        new="    (void)n;\n}\n",
        tests=["pci_bus_test"],
    ),
    dict(
        name="msix-keeps-intx",
        breaks="6.8.3.3: with MSI-X enabled the function must not use INTx",
        file=PCI,
        old="           !pci_msi_enabled(d) && !pci_msix_enabled(d);\n",
        new="           !pci_msi_enabled(d);\n",
        tests=["pci_bus_test"],
    ),
    dict(
        name="data-truncated",
        breaks="6.8.2.8: MSI-X message data is a full 32 bits, sent unmodified",
        file=PCI,
        old="    msi_deliver(d, what, (uint64_t)e->addr_hi << 32 | e->addr_lo, e->data);\n",
        new="    msi_deliver(d, what, (uint64_t)e->addr_hi << 32 | e->addr_lo, e->data & 0xFFFF);\n",
        tests=["pci_bus_test"],
    ),
    dict(
        name="ctrl-reserved-writable",
        breaks="6.8.2.9: only the Mask bit of Vector Control is implemented",
        file=PCI,
        old="        e->ctrl = v & PCI_MSIX_ENTRY_CTRL_MASKBIT;\n",
        new="        e->ctrl = v;\n",
        tests=["pci_bus_test"],
    ),
    dict(
        name="reset-keeps-table",
        breaks="6.8.2.9: after reset every entry is masked and nothing is pending",
        file=PCI,
        old="        d->msi_drop_run       = 0;\n        msix_reset(d);\n",
        new="        d->msi_drop_run       = 0;\n",
        tests=["pci_bus_test"],
    ),
    dict(
        name="qword-high-first",
        breaks="6.8.2 note: in a QWORD write of Data + Vector Control the data takes effect first",
        file=PCI,
        old="    for (i = 0; i < size; i += 4) {\n"
            "        memcpy(&v, (const uint8_t *)data + i, 4);\n"
            "        msix_dword_write(d, (uint32_t)off + i, v);\n",
        new="    for (i = size; i >= 4 && (i -= 4, 1); ) {\n"
            "        memcpy(&v, (const uint8_t *)data + i, 4);\n"
            "        msix_dword_write(d, (uint32_t)off + i, v);\n",
        tests=["pci_bus_test"],
    ),
    # ---- edu 的分项：edu_regs.h ----
    dict(
        name="edu-all-on-entry-0",
        breaks="edu_regs.h: byte n of INT_STATUS is signalled on MSI-X entry n",
        file=DEV,
        old="                pci_msix_notify(&e->pci, n);\n",
        new="                pci_msix_notify(&e->pci, 0);\n",
        tests=["edu_test"],
    ),
    dict(
        name="edu-pending-never-cleared",
        breaks="edu_regs.h: acknowledging a byte to 0 clears that entry's pending bit",
        file=DEV,
        old="        if (!(e->int_status & EDU_INT_VEC_MASK(n)))\n"
            "            pci_msix_clear_pending(&e->pci, n);\n",
        new="        (void)0;\n",
        tests=["edu_test"],
    ),
    # ---- 真实 guest 里看得见的屏蔽语义 ----
    dict(
        name="guest-entry-mask-ignored",
        breaks="6.8.3.5: Linux disable_irq writes the Mask bit; the device must stop sending",
        file=PCI,
        old="    return msix_func_masked(d) || (d->msix_table[n].ctrl & PCI_MSIX_ENTRY_CTRL_MASKBIT);\n",
        new="    return msix_func_masked(d);\n",
        suite="pci-msix",
    ),
    dict(
        name="guest-no-replay",
        breaks="6.8.3.5: enable_irq clears the Mask bit and the pending message must follow",
        file=PCI,
        old="        e->ctrl = v & PCI_MSIX_ENTRY_CTRL_MASKBIT;\n        msix_flush(d);\n",
        new="        e->ctrl = v & PCI_MSIX_ENTRY_CTRL_MASKBIT;\n",
        suite="pci-msix",
    ),
    dict(
        name="guest-function-mask-ignored",
        breaks="6.8.3.5: Function Mask written through config space masks every entry",
        file=PCI,
        old="    return msix_func_masked(d) || (d->msix_table[n].ctrl & PCI_MSIX_ENTRY_CTRL_MASKBIT);\n",
        new="    return (d->msix_table[n].ctrl & PCI_MSIX_ENTRY_CTRL_MASKBIT);\n",
        suite="pci-msix",
    ),
    dict(
        name="guest-pending-never-cleared",
        breaks="edu_regs.h: a cause acknowledged while masked leaves no pending bit behind",
        file=DEV,
        old="        if (!(e->int_status & EDU_INT_VEC_MASK(n)))\n"
            "            pci_msix_clear_pending(&e->pci, n);\n",
        new="        (void)0;\n",
        suite="pci-msix",
    ),
    dict(
        name="guest-wrong-entry",
        breaks="edu_regs.h: each byte of INT_STATUS has its own vector, counted on its own",
        file=DEV,
        old="                pci_msix_notify(&e->pci, n);\n",
        new="                pci_msix_notify(&e->pci, 0);\n",
        suite="pci-msix",
    ),
]
