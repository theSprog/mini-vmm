# tests/mutants/pci-msi.py — 里程碑 pci-msi 的源码变异体，由 tests/mutate.py 读取
#
# 每一项把真实源码里的一段文字（old）换成故意写错的版本（new），然后构建
# tests 里列出的单元测试，要求其中至少一个因为断言失败而退出非零。
# name 说的是“坏在哪”，breaks 说的是它违反哪一条规范或契约，判据见
# tests/TEST_DESIGN.md 的 E26。
#
# 这里的错误点在 static 函数内部（irq_sync、intx_output 等）或一条条件
# 表达式里，换不掉整个对外函数，所以不能做成 tests/unit/*_mutants.c 那种
# 接口层 mutant。old 按文字定位而不按行号，源码改动让 old 不再唯一出现时，
# mutate.py 报 STALE 并让 suite 失败，这时照新的源码改写这一项。

MUTANTS = [
    # ---- AHCI 什么时候发消息：AHCI 1.3.1 第 10.7.2.1 节图 22 ----
    dict(
        name="ahci-new-bit-only",
        breaks="AHCI 10.7.2.1: a partial clear by software that leaves bits set must send",
        file="src/devices/ahci.c",
        old="(sw_change || (s->is & ~before))",
        new="(s->is & ~before)",
        tests=["ahci_test"],
    ),
    # 按 INTx 电平的上升沿发：真实 guest 上挡不住（E27），只能靠这里
    dict(
        name="ahci-intx-rising-edge",
        breaks="AHCI 10.7.2.1: every change of IS that leaves it non-zero sends, not only the line's rising edge",
        file="src/devices/ahci.c",
        old="    if (active && pci_msi_enabled(&s->pci) && (sw_change || (s->is & ~before)))\n"
            "        pci_msi_notify(&s->pci);",
        new="    { static bool last; (void)before; (void)sw_change;\n"
            "      if (active && !last && pci_msi_enabled(&s->pci)) pci_msi_notify(&s->pci);\n"
            "      last = active; }",
        tests=["ahci_test"],
    ),
    dict(
        name="ahci-send-every-sync",
        breaks="AHCI 10.7.2.1: no message when IS did not change",
        file="src/devices/ahci.c",
        old="(sw_change || (s->is & ~before))",
        new="(1)",
        tests=["ahci_test"],
    ),
    dict(
        name="ahci-send-with-ie-off",
        breaks="AHCI 3.1.2: GHC.IE=0 masks all interrupts, MSI included",
        file="src/devices/ahci.c",
        old="if (active && pci_msi_enabled",
        new="if (pci_msi_enabled",
        tests=["ahci_test"],
    ),
    dict(
        name="ahci-ie-rewrite-sends",
        breaks="rewriting GHC.IE=1 is not an enable edge and must not send",
        file="src/devices/ahci.c",
        old="ie_on = (val & AHCI_GHC_IE) && !(s->ghc & AHCI_GHC_IE);",
        new="ie_on = (val & AHCI_GHC_IE);",
        tests=["ahci_test"],
    ),
    dict(
        name="ahci-ie-on-no-resend",
        breaks="GHC.IE 0->1 with pending IS bits must send one message",
        file="src/devices/ahci.c",
        old="irq_sync(s, ie_on);",
        new="irq_sync(s, false);",
        tests=["ahci_test"],
    ),
    dict(
        name="ahci-is-write-always-sends",
        breaks="AHCI 10.7.2.1: an IS write that clears nothing is not a change",
        file="src/devices/ahci.c",
        old="irq_sync(s, cleared != 0);",
        new="irq_sync(s, true);",
        tests=["ahci_test"],
    ),
    dict(
        name="ahci-no-msi-cap",
        breaks="the AHCI controller exposes an MSI capability",
        file="src/devices/ahci.c",
        old="p->msi        = true;",
        new=";",
        tests=["ahci_test"],
    ),

    # ---- PCI 层：PCI 3.0 第 6.8.1、6.8.3.3、6.2.2 节 ----
    dict(
        name="pci-intx-while-msi",
        breaks="PCI 3.0 6.8.3.3: INTx is not used while MSI Enable is set",
        file="src/pci/pci.c",
        old="&&\n           !pci_msi_enabled(d);",
        new=";",
        tests=["pci_bus_test"],
    ),
    dict(
        name="pci-send-without-bus-master",
        breaks="PCI 3.0 6.2.2: an MSI is a memory write and needs Bus Master",
        file="src/pci/pci.c",
        old="if (!(cfg_get(d, PCI_COMMAND, 2) & PCI_COMMAND_MASTER))\n        why",
        new="if (0)\n        why",
        tests=["pci_bus_test"],
    ),
    dict(
        name="pci-enable-keeps-intx",
        breaks="changing MSI Enable must re-evaluate the INTx line",
        file="src/pci/pci.c",
        old="msi_note_enable(d);\n        if (d->intx_pin)\n            intx_line_update(bus, d);",
        new="msi_note_enable(d);",
        tests=["pci_bus_test"],
    ),
    dict(
        name="pci-enable-read-only",
        breaks="PCI 3.0 6.8.1.3: MSI Enable is read/write",
        file="src/pci/pci.c",
        old="wmask_set16(d, MSI_CTRL, PCI_MSI_FLAGS_ENABLE | PCI_MSI_FLAGS_QSIZE);",
        new="wmask_set16(d, MSI_CTRL, PCI_MSI_FLAGS_QSIZE);",
        tests=["pci_bus_test"],
    ),
    dict(
        name="pci-drop-log-every",
        breaks="repeated drops of the same address/data are logged at runs 1, 2, 4, 8...",
        file="src/pci/pci.c",
        old="if ((d->msi_drop_run & (d->msi_drop_run - 1)) == 0)",
        new="if (1)",
        tests=["pci_bus_test"],
    ),
    dict(
        name="pci-drop-log-once",
        breaks="a new address/data pair and later powers of two are logged again",
        file="src/pci/pci.c",
        old="if ((d->msi_drop_run & (d->msi_drop_run - 1)) == 0)",
        new="if (d->msi_drop_run == 1 && d->msi_dropped == 1)",
        tests=["pci_bus_test"],
    ),

    # ---- 消息格式：Intel SDM 第 13.11 节，KVM 不查这些 ----
    dict(
        name="msi-high-dword-ignored",
        breaks="SDM 13.11: a non-zero upper address dword is outside the interrupt window",
        file="include/devices/irqchip.h",
        old="if ((addr >> 32) || ",
        new="if (",
        tests=["pci_bus_test"],
    ),
    dict(
        name="msi-window-ignored",
        breaks="SDM 13.11.1: the address must be in 0xFEE00000-0xFEEFFFFF",
        file="include/devices/irqchip.h",
        old="|| ((uint32_t)addr & MSI_ADDR_WINDOW_MASK) != MSI_ADDR_WINDOW)",
        new=")",
        tests=["pci_bus_test"],
    ),
    dict(
        name="msi-reserved-mode-ok",
        breaks="SDM 13.11.2: delivery modes 011b and 110b are reserved",
        file="include/devices/irqchip.h",
        old="if (dm == 3 || dm == 6)",
        new="if (0)",
        tests=["pci_bus_test"],
    ),
    dict(
        name="msi-low-vector-ok",
        breaks="SDM 13.11.2: fixed and lowest-priority vectors are 0x10-0xFE",
        file="include/devices/irqchip.h",
        old="vec < 0x10 || ",
        new="",
        tests=["pci_bus_test"],
    ),
]
