# tests/mutants/split-irqchip.py — 里程碑 split-irqchip 的源码变异体，由
# tests/mutate.py 读取
#
# 芯片语义的错误由单元测试挡住（tests=），默认的 negctl 会跑。判据见各
# 单元测试文件开头列出的数据手册章节。芯片和 KVM 之间的接线（路由镜像、
# EOI 退出、ExtINT 注入、IRQ0 的形状、补发策略）只有真实 guest 看得出来，
# 是 e2e mutant（suite=），要跑它们：
#   tests/mutate.py split-irqchip
# 判据见 tests/TEST_DESIGN.md 的 E33。

IOAPIC = "src/devices/ioapic.c"
PIC = "src/devices/i8259.c"
PIT = "src/devices/i8254.c"
BOARD = "src/devices/split_irqchip.c"

MUTANTS = [
    dict(
        name="ioapic-eoi-keeps-remote-irr",
        breaks="82093AA 3.2.4 bit 14: an EOI with a matching vector clears "
               "remote IRR",
        file=IOAPIC,
        old="        io->redtbl[pin] = rte & ~IOAPIC_RTE_REMOTE_IRR;\n",
        new="",
        tests=["ioapic_test"],
    ),
    dict(
        name="ioapic-eoi-no-resend",
        breaks="a level line still asserted at EOI is sent again",
        file=IOAPIC,
        old="        io->redtbl[pin] = rte & ~IOAPIC_RTE_REMOTE_IRR;\n"
            "        service_level(io, pin);\n",
        new="        io->redtbl[pin] = rte & ~IOAPIC_RTE_REMOTE_IRR;\n",
        tests=["ioapic_test"],
    ),
    dict(
        name="ioapic-remote-irr-ignored",
        breaks="remote IRR blocks a second level message until EOI",
        file=IOAPIC,
        old="        (rte & IOAPIC_RTE_REMOTE_IRR) || !(io->line & (1u << pin)))",
        new="        !(io->line & (1u << pin)))",
        tests=["ioapic_test"],
    ),
    dict(
        name="ioapic-remote-irr-without-accept",
        breaks="remote IRR is set only when a LAPIC accepted the message",
        file=IOAPIC,
        old="    if (n <= 0) {\n        io->lost[pin]++;\n        return;\n    }\n",
        new="    if (n <= 0)\n        io->lost[pin]++;\n",
        tests=["ioapic_test"],
    ),
    dict(
        name="ioapic-edge-switch-keeps-remote-irr",
        breaks="switching an entry to edge clears remote IRR, which Linux "
               "__eoi_ioapic_pin relies on for version 11h",
        file=IOAPIC,
        old="        rte &= ~IOAPIC_RTE_REMOTE_IRR;\n",
        new="        (void)0;\n",
        tests=["ioapic_test"],
    ),
    dict(
        name="ioapic-remote-irr-writable",
        breaks="82093AA 3.2.4: remote IRR and delivery status are read-only",
        file=IOAPIC,
        old="#define RTE_RW_LO  (0xFFFull | IOAPIC_RTE_INTPOL | IOAPIC_RTE_LEVEL | \\\n",
        new="#define RTE_RW_LO  (0x7FFFull | IOAPIC_RTE_INTPOL | IOAPIC_RTE_LEVEL | \\\n",
        tests=["ioapic_test"],
    ),
    dict(
        name="ioapic-masked-edge-held",
        breaks="82093AA 3.2.4 bit 16: an edge on a masked pin is dropped, "
               "not delivered on unmask",
        file=IOAPIC,
        old="    if (!ioapic_rte_is_level(rte) || (rte & IOAPIC_RTE_MASK) ||\n",
        new="    if ((rte & IOAPIC_RTE_MASK) ||\n",
        tests=["ioapic_test"],
    ),
    dict(
        name="ioapic-unmask-no-service",
        breaks="unmasking a level entry whose line is asserted sends it",
        file=IOAPIC,
        old="        io->ops->rte_changed(io->opaque, pin);\n"
            "    service_level(io, pin);\n",
        new="        io->ops->rte_changed(io->opaque, pin);\n",
        tests=["ioapic_test"],
    ),
    dict(
        name="ioapic-edge-every-assert",
        breaks="an edge entry sends once per rising edge, not per assert",
        file=IOAPIC,
        old="    if (level && !was && !(rte & IOAPIC_RTE_MASK))\n",
        new="    if (level && !(rte & IOAPIC_RTE_MASK))\n",
        tests=["ioapic_test"],
    ),
    dict(
        name="ioapic-rte-changed-always",
        breaks="rte_changed fires only when the entry really changed, so the "
               "KVM route table is not rewritten on every identical write",
        file=IOAPIC,
        old="    if (rte != old && io->ops->rte_changed)\n",
        new="    if (io->ops->rte_changed)\n",
        tests=["ioapic_test"],
    ),
    dict(
        name="ioapic-reset-keeps-line",
        breaks="reset forgets the pin levels; devices drive them again",
        file=IOAPIC,
        old="    io->line = 0;\n",
        new="",
        tests=["ioapic_test"],
    ),
    dict(
        name="ioapic-iowin-any-width",
        breaks="82093AA 3.0: IOWIN is accessed as a dword only",
        file=IOAPIC,
        old="    } else if (offset == 0x10 && size == 4) {\n        write_indirect(io, v);\n",
        new="    } else if (offset == 0x10) {\n        write_indirect(io, v);\n",
        tests=["ioapic_test"],
    ),
    dict(
        name="pic-ignores-elcr",
        breaks="PIIX4 8.6.5: an IRQ whose ELCR bit is 1 is level triggered",
        file=PIC,
        old="    return c->line & (c->elcr | c->armed);\n",
        new="    return c->line & c->armed;\n",
        tests=["pic_test"],
    ),
    dict(
        name="pic-elcr-reserved-writable",
        breaks="PIIX4 4.2.2.9/10: IRQ0-2, 8 and 13 are always edge triggered",
        file=PIC,
        old="        c->elcr = d[i] & c->elcr_mask;\n",
        new="        c->elcr = d[i];\n",
        tests=["pic_test"],
    ),
    dict(
        name="pic-inta-keeps-armed",
        breaks="8259A figure 9: setting ISR clears the edge sense latch, so a "
               "held edge interrupts once",
        file=PIC,
        old="    c->isr |= bit;\n    c->armed &= ~bit;\n",
        new="    c->isr |= bit;\n",
        tests=["pic_test"],
    ),
    dict(
        name="pic-icw1-keeps-armed",
        breaks="8259A ICW1 a: after initialization an IR must make a "
               "low-to-high transition",
        file=PIC,
        old="    c->armed = ~c->line;\n",
        new="    c->armed = 0xFF;\n",
        tests=["pic_test"],
    ),
    dict(
        name="pic-icw1-clears-isr",
        breaks="8259A figure 9: master clear does not reach the in-service latch",
        file=PIC,
        old="    c->imr = 0;\n    c->armed = ~c->line;\n",
        new="    c->imr = 0;\n    c->isr = 0;\n    c->armed = ~c->line;\n",
        tests=["pic_test"],
    ),
    dict(
        name="pic-imr-gates-irr",
        breaks="8259A: IRR is not affected by IMR (Linux startup_ioapic_irq "
               "reads a masked request)",
        file=PIC,
        old="    return c->read_isr ? c->isr : irr(c);\n",
        new="    return c->read_isr ? c->isr : irr(c) & ~c->imr;\n",
        tests=["pic_test"],
    ),
    dict(
        name="pic-no-nesting-block",
        breaks="fully nested mode: a level in service blocks equal and lower "
               "priority requests",
        file=PIC,
        old="    if (svc >= 0 && prio(c, req) >= prio(c, svc))\n",
        new="    if (svc >= 0 && prio(c, req) > prio(c, svc))\n",
        tests=["pic_test"],
    ),
    dict(
        name="pic-no-rotation",
        breaks="8259A OCW2: rotation commands change the bottom priority level",
        file=PIC,
        old="static int prio(const struct pic8259 *c, int irq)\n{\n"
            "    return (irq - c->priority_add) & 7;\n",
        new="static int prio(const struct pic8259 *c, int irq)\n{\n"
            "    return irq;\n",
        tests=["pic_test"],
    ),
    dict(
        name="pic-no-aeoi",
        breaks="8259A ICW4 AEOI: ISR is cleared at the end of INTA",
        file=PIC,
        old="        c->isr &= ~bit;\n    }\n}\n",
        new="    }\n}\n",
        tests=["pic_test"],
    ),
    dict(
        name="pic-smm-eoi-clears-masked",
        breaks="8259A: in special mask mode a non-specific EOI skips a masked "
               "in-service level",
        file=PIC,
        old="    int irq = highest(c, c->special_mask ? (c->isr & ~c->imr) : c->isr);\n",
        new="    int irq = highest(c, c->isr);\n",
        tests=["pic_test"],
    ),
    dict(
        name="pic-smm-ignored",
        breaks="8259A special mask mode: a masked in-service level stops "
               "blocking lower levels",
        file=PIC,
        old="    if (c->special_mask)\n        in_service &= ~c->imr;\n",
        new="",
        tests=["pic_test"],
    ),
    dict(
        name="pic-sfnm-ignored",
        breaks="8259A special fully nested mode: the master does not block a "
               "slave whose request is in service",
        file=PIC,
        old="    if (!c->is_slave && c->sfnm)\n        in_service &= ~c->icw3;\n",
        new="",
        tests=["pic_test"],
    ),
    dict(
        name="pic-cascade-ignores-icw3",
        breaks="8259A ICW3: only IR inputs marked as slaves hand the vector to "
               "the slave",
        file=PIC,
        old="        if (!m->single && (m->icw3 & (1u << irq))) {\n",
        new="        if (irq == I8259_CASCADE_IR) {\n",
        tests=["pic_test"],
    ),
    dict(
        name="pic-spurious-sets-isr",
        breaks="8259A p.7: a default IR7 does not set the ISR bit",
        file=PIC,
        old="        p->spurious[0]++;\n        vec = m->vector_base | 7;\n",
        new="        p->spurious[0]++;\n        m->isr |= 0x80;\n        vec = m->vector_base | 7;\n",
        tests=["pic_test"],
    ),
    dict(
        name="pic-poll-sticky",
        breaks="8259A poll command applies to the next read only",
        file=PIC,
        old="    if (c->poll) {\n        c->poll = false;\n",
        new="    if (c->poll) {\n",
        tests=["pic_test"],
    ),
    dict(
        name="pic-esmm-ignored",
        breaks="8259A OCW3: SMM takes effect only together with ESMM",
        file=PIC,
        old="    if (val & 0x40)\n        c->special_mask = val & 0x20;\n",
        new="    c->special_mask = val & 0x20;\n",
        tests=["pic_test"],
    ),
    # ---- 8254 + port 0x61 ----
    dict(
        name="pit-load-same-pulse",
        breaks="82C54 page 9-12: a written count is loaded on the next CLK "
               "pulse, not the current one",
        file=PIT,
        old="    ce->start = t + 1;\n",
        new="    ce->start = t;\n",
        tests=["pit_test"],
    ),
    dict(
        name="pit-mode2-low-off-by-one",
        breaks="82C54 page 11: mode 2 OUT goes low when the count reaches 1, "
               "N pulses after the write",
        file=PIT,
        old="        return d % n != n - 1;\n",
        new="        return d % n != 0;\n",
        tests=["pit_test"],
    ),
    dict(
        name="pit-mode0-terminal-late",
        breaks="82C54 page 9: mode 0 OUT goes high N+1 pulses after the write",
        file=PIT,
        old="        return d >= n;\n",
        new="        return d > n;\n",
        tests=["pit_test"],
    ),
    dict(
        name="pit-mode4-strobe-early",
        breaks="82C54 page 12: mode 4 strobes N+1 pulses after the write",
        file=PIT,
        old="        return d != n;\n",
        new="        return d != n - 1;\n",
        tests=["pit_test"],
    ),
    dict(
        name="pit-mode3-odd-symmetric",
        breaks="82C54 page 11-12: an odd mode 3 count is high (N+1)/2 and "
               "low (N-1)/2 pulses",
        file=PIT,
        old="    return (n + 1) / 2;\n",
        new="    return n / 2;\n",
        tests=["pit_test"],
    ),
    dict(
        name="pit-rewrite-immediate",
        breaks="82C54 page 11: a new mode 2/3 count waits for the end of the "
               "current cycle",
        file=PIT,
        old="        if (!ce->active || t < ce->start) {\n",
        new="        if (1) {\n",
        tests=["pit_test"],
    ),
    dict(
        name="pit-next-rise-ignores-pending",
        breaks="the timer must follow a pending mode 2/3 count",
        file=PIT,
        old="    if (c->ce.active && c->ce.pend && (r == I8254_NO_EVENT ||\n",
        new="    if (0 && c->ce.active && c->ce.pend && (r == I8254_NO_EVENT ||\n",
        tests=["pit_test"],
    ),
    dict(
        name="pit-relatch-overrides",
        breaks="82C54 page 7: a second latch before the read is ignored",
        file=PIT,
        old="    if (c->ol_left)\n        return;\n    c->ol = read_value(c, t);\n",
        new="    c->ol = read_value(c, t);\n",
        tests=["pit_test"],
    ),
    dict(
        name="pit-status-after-count",
        breaks="82C54 page 8: with count and status latched the first read "
               "returns status",
        file=PIT,
        old="    if (c->status_latched) {\n        c->status_latched = false;\n",
        new="    if (c->status_latched && !c->ol_left) {\n        c->status_latched = false;\n",
        tests=["pit_test"],
    ),
    dict(
        name="pit-gate-low-out-unforced",
        breaks="82C54 Figure 21: GATE low in mode 2/3 sets OUT high at once",
        file=PIT,
        old="            c->ce.forced_out = 1;\n",
        new="            (void)0;\n",
        tests=["pit_test"],
    ),
    dict(
        name="pit-gate-resume-restarts",
        breaks="82C54 page 9: GATE low in mode 0 only pauses counting",
        file=PIT,
        old="    if (t - done > ce->start)\n        ce->start = t - done;\n",
        new="    ce->start = t;\n",
        tests=["pit_test"],
    ),
    dict(
        name="pit-missed-periods-merged",
        breaks="a late poll reports every OUT edge that elapsed",
        file=PIT,
        old="        return cnt + db / n - da / n;\n",
        new="        return cnt + (db / n > da / n);\n",
        tests=["pit_test"],
    ),
    dict(
        name="pit-control-word-edge-lost",
        breaks="82C54 page 14: a control word sets OUT to its initial level, "
               "which can be a rising edge on IRQ0",
        file=PIT,
        old="    c->idle_out = m != 0;\n    note_edge(c, before, t);\n",
        new="    c->idle_out = m != 0;\n",
        tests=["pit_test"],
    ),
    dict(
        name="pit-mode0-lsb-keeps-counting",
        breaks="82C54 page 9 item 3: the LSB of a new mode 0 count stops the "
               "counter before rollover",
        file=PIT,
        old="    c->hold = !(t >= ce->start && seg_d(ce, t) > ce->n);\n",
        new="    c->hold = false;\n",
        tests=["pit_test"],
    ),
    dict(
        name="pit-bcd-ignored",
        breaks="82C54 Figure 7: BCD=1 counts four decades",
        file=PIT,
        old="    uint32_t n = (c->bcd ? from_bcd(c->cr) : c->cr) % m;\n",
        new="    uint32_t n = c->cr % m;\n",
        tests=["pit_test"],
    ),
    dict(
        name="pit-nmisc-out2-constant",
        breaks="PIIX4 4.2.4.1 bit 5 reflects Counter 2 OUT (Linux "
               "pit_calibrate_tsc polls it)",
        file=PIT,
        old="        d[i] = (uint8_t)(out_at(&pit->c[2], t) << 5 |\n",
        new="        d[i] = (uint8_t)(0 << 5 |\n",
        tests=["pit_test"],
    ),
    dict(
        name="pit-nmisc-refresh-constant",
        breaks="PIIX4 4.2.4.1 bit 4 toggles every refresh cycle (Counter 1)",
        file=PIT,
        old="                         (pit->c[1].rises & 1) << 4 | pit->nmisc);\n",
        new="                         0 << 4 | pit->nmisc);\n",
        tests=["pit_test"],
    ),
    # 以下是 e2e mutant
    dict(
        name="route-not-synced",
        breaks="split_irqchip.h: RTE changes are mirrored into KVM's GSI "
               "routes, otherwise KVM never exits on the EOI of a level vector "
               "and remote IRR stays set after the first interrupt",
        file=BOARD,
        old="    s->routes[pin] = e;\n    routes_commit(s);\n",
        new="    s->routes[pin] = e;\n    (void)0;\n",
        suite="split-irqchip",
    ),
    dict(
        name="eoi-exit-ignored",
        breaks="split_irqchip.h: KVM_EXIT_IOAPIC_EOI reaches the user-space "
               "IOAPIC and clears remote IRR",
        file=BOARD,
        old="    ioapic_eoi(&s->ioapic, vector);\n",
        new="    (void)0;\n",
        suite="split-irqchip",
    ),
    dict(
        name="irq0-pulse-ends-low",
        breaks="split_irqchip.c pit_pulse: an 8259A edge request must stay "
               "high until INTA, a 1-then-0 pulse is lost under noapic",
        file=BOARD,
        old="    set_line_locked(s, 0, 0);\n    set_line_locked(s, 0, 1);\n}",
        new="    set_line_locked(s, 0, 1);\n    set_line_locked(s, 0, 0);\n}",
        suite="split-irqchip",
    ),
    dict(
        name="pin0-route-edge",
        breaks="split_irqchip.h: with reinject on, pin 0's route is marked "
               "level so the EOI of the timer vector exits and acks the tick",
        file=BOARD,
        old="        data |= 1u << 15;\n",
        new="        (void)0;\n",
        suite="split-irqchip",
    ),
    dict(
        name="backlog-discarded",
        breaks="split_irqchip.h: reinject on queues every tick that expired "
               "before the previous one was acked",
        file=BOARD,
        old="        s->pit_backlog += edges;\n",
        new="        s->pit_backlog += !!edges;\n",
        suite="split-irqchip",
    ),
    dict(
        name="policy-ignored",
        breaks="split_irqchip.h: reinject off merges the ticks of one wakeup "
               "into a single interrupt",
        file=BOARD,
        old="    s->pit_ticks += edges;\n    if (s->pit_reinject) {\n",
        new="    s->pit_ticks += edges;\n    if (1) {\n",
        suite="split-irqchip",
    ),
    dict(
        name="unmask-keeps-backlog",
        breaks="split_irqchip.c pit_reinject_reset: unmasking IRQ0 clears the "
               "backlog, a tick sent while masked is never acked",
        file=BOARD,
        old="            pit_reinject_reset(s);\n    }\n    s->pit_seen_isr0",
        new="            (void)0;\n    }\n    s->pit_seen_isr0",
        suite="split-irqchip",
    ),
    dict(
        name="kernel-reinject-always-on",
        breaks="irqchip.c: --pit-reinject off reaches the in-kernel PIT via "
               "KVM_REINJECT_CONTROL",
        file="src/devices/irqchip.c",
        old="    reinject.pit_reinject = vm->cfg.pit_reinject;\n",
        new="    reinject.pit_reinject = 1;\n",
        suite="split-irqchip",
    ),
]
