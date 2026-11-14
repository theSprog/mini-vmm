# tests/mutants/gdbstub.py — 里程碑 gdbstub 的源码变异体，由 tests/mutate.py 读取
#
# 每一项把 src/gdb/ 里的一段文字（old）换成故意写错的版本（new），然后构建
# gdb_test，要求它因为断言失败而退出非零。name 说的是“坏在哪”，breaks 说的
# 是它违反哪一条规范或契约，判据见 tests/TEST_DESIGN.md 的 E28。
#
# 错误点大多在 static 函数（send_packet、reply_stop、handle_vcont 等）或
# 一条条件表达式里，所以用源码层而不是 tests/unit/*_mutants.c 的接口层。
# old 按文字定位，源码改动让 old 不再唯一出现时 mutate.py 报 STALE，这时
# 照新的源码改写这一项。
#
# “gdb 手册附录 E”指 GDB 手册的 Remote Serial Protocol 附录。

RSP = "src/gdb/rsp.c"
X86 = "src/gdb/x86.c"
T = ["gdb_test"]

MUTANTS = [
    # ---- 组包与解包：附录 E.1 Overview ----
    dict(
        name="tx-star-unescaped",
        breaks="gdb manual E.1: a stub must escape '*' or gdb reads it as run-length encoding",
        file=RSP,
        old="c == '#' || c == '$' || c == '}' || c == '*'",
        new="c == '#' || c == '$' || c == '}'",
        tests=T,
    ),
    dict(
        name="tx-checksum-skips-escape",
        breaks="gdb manual E.1: the checksum covers the bytes actually sent, including the '}' escape",
        file=RSP,
        old="            sum += '}';\n",
        new="",
        tests=T,
    ),
    dict(
        name="rx-escape-not-xored",
        breaks="gdb manual E.1: an escaped byte is the next byte XOR 0x20",
        file=RSP,
        old="pkt_put(s, (char)(c ^ 0x20));",
        new="pkt_put(s, (char)c);",
        tests=T,
    ),
    dict(
        name="rx-checksum-skips-escaped-byte",
        breaks="gdb manual E.1: the checksum covers the escaped byte as received",
        file=RSP,
        old="        case RSP_RX_ESC:\n            s->sum += c;\n",
        new="        case RSP_RX_ESC:\n",
        tests=T,
    ),
    dict(
        name="rx-bad-checksum-accepted",
        breaks="gdb manual E.1: a packet with a wrong checksum is answered with '-' and not executed",
        file=RSP,
        old="(uint8_t)(s->cs_hi << 4 | d) != s->sum || ",
        new="",
        tests=T,
    ),
    dict(
        name="rx-overflow-accepted",
        breaks="rsp.h contract: a packet longer than PacketSize is NAKed instead of executed truncated",
        file=RSP,
        old=" || s->pkt_overflow) {",
        new=") {",
        tests=T,
    ),
    dict(
        name="rx-nak-not-resent",
        breaks="gdb manual E.1: on '-' the stub retransmits its last packet",
        file=RSP,
        old="            } else if (c == '-' && s->last_len) {\n"
            "                raw_write(s, s->last, s->last_len);\n",
        new="            } else if (c == '-' && s->last_len) {\n",
        tests=T,
    ),
    dict(
        name="rx-packet-run-while-running",
        breaks="rsp.h contract: target callbacks run only while the target is stopped",
        file=RSP,
        old="            if (s->running)\n                break;\n            s->pkt[s->pkt_len] = '\\0';",
        new="            s->pkt[s->pkt_len] = '\\0';",
        tests=T,
    ),
    dict(
        name="rx-ctrl-c-while-stopped",
        breaks="rsp.h contract: 0x03 interrupts only a running target; a stopped one has nothing to stop",
        file=RSP,
        old="c == 0x03 && s->running",
        new="c == 0x03",
        tests=T,
    ),
    # ---- 停止应答：附录 E.3 Stop Reply Packets ----
    dict(
        name="stop-swbreak-unadvertised",
        breaks="gdb manual E.3: swbreak is sent only when gdb advertised swbreak+ in qSupported",
        file=RSP,
        old="        if (s->gdb_swbreak)\n            reason = \"swbreak:;\";",
        new="        reason = \"swbreak:;\";",
        tests=T,
    ),
    dict(
        name="stop-thread-zero-based",
        breaks="gdbstub contract: thread id is vCPU index + 1 because gdb reserves 0 for any thread",
        file=RSP,
        old="st->cpu + 1, reason);",
        new="st->cpu, reason);",
        tests=T,
    ),
    dict(
        name="stop-no-thread-select",
        breaks="gdb manual E.3: after a stop the reported thread becomes the one g/m act on",
        file=RSP,
        old="        s->g_cpu = st->cpu;\n",
        new="",
        tests=T,
    ),
    # ---- 寄存器：target.xml 的顺序和 g/p/P ----
    dict(
        name="reg-unavailable-as-zero",
        breaks="gdb manual E 'g' packet: a register the stub cannot read is 'xx', not a fake value",
        file=RSP,
        old="memset(*o, 'x', 2 * d->size);",
        new="memset(*o, '0', 2 * d->size);",
        tests=T,
    ),
    dict(
        name="reg-P-segment-writable",
        breaks="rsp.c contract: only gpr/rip/eflags are writable; a selector without its hidden descriptor is inconsistent",
        file=RSP,
        old="    if (n >= RSP_NR_WRITABLE) {",
        new="    if (0) {",
        tests=T,
    ),
    dict(
        name="reg-rbp-rsp-swapped",
        breaks="gdb 64bit-core.xml: register 6 is rbp and 7 is rsp, unlike the kvm_regs field order",
        file=X86,
        old="r->gpr[6]  = kr->rbp;  r->gpr[7]  = kr->rsp;",
        new="r->gpr[6]  = kr->rsp;  r->gpr[7]  = kr->rbp;",
        tests=T,
    ),
    # ---- 内存与 target.xml ----
    dict(
        name="hex-trailing-ignored",
        breaks="gdb manual E 'M' packet: the data length must match LENGTH exactly",
        file=RSP,
        old="    return src[2 * len] == '\\0';",
        new="    return true;",
        tests=T,
    ),
    dict(
        name="m-partial-read-is-error",
        breaks="gdb manual E 'm' packet: the reply may be shorter than requested; only a zero-byte read is an error",
        file=RSP,
        old="    if (!n && len) {",
        new="    if (n < len) {",
        tests=T,
    ),
    dict(
        name="qxfer-never-last",
        breaks="gdb manual E qXfer: the final chunk starts with 'l', otherwise gdb keeps reading",
        file=RSP,
        old="buf[0] = (off + n < total) ? 'm' : 'l';",
        new="buf[0] = 'm';",
        tests=T,
    ),
    # ---- 线程与 vCont：附录 E 'H' 和 'vCont' ----
    dict(
        name="vcont-rightmost-wins",
        breaks="gdb manual E vCont: each thread takes the leftmost action that matches it",
        file=RSP,
        old="(tid == -1 || tid == i + 1) && act[i] == RSP_ACT_NONE",
        new="(tid == -1 || tid == i + 1)",
        tests=T,
    ),
    dict(
        name="H-zero-selects-cpu",
        breaks="gdb manual E 'H' packet: thread 0 means any thread and must not change the selection",
        file=RSP,
        old="if (op == 'g' && tid > 0)",
        new="if (op == 'g' && tid >= 0)",
        tests=T,
    ),
    # ---- 软件断点：int3 的隐藏与影子写 ----
    dict(
        name="sw-int3-not-hidden",
        breaks="gdbstub contract: memory reads return the original byte, not the planted int3",
        file=X86,
        old="            buf[off] = b->sw[i].orig;",
        new="            (void)off;",
        tests=T,
    ),
    dict(
        name="sw-write-removes-int3",
        breaks="gdbstub contract: writing over a planted breakpoint keeps the int3 in guest memory",
        file=X86,
        old="            buf[off] = GDB_X86_INT3;\n",
        new="",
        tests=T,
    ),
    dict(
        name="sw-write-orig-stale",
        breaks="gdbstub contract: writing over a breakpoint updates the byte restored on removal",
        file=X86,
        old="            b->sw[i].orig = buf[off];\n",
        new="",
        tests=T,
    ),
    dict(
        name="sw-del-drops-last",
        breaks="x86.h contract: removing a breakpoint removes that entry, not the last one",
        file=X86,
        old="b->sw[i] = b->sw[--b->nr_sw];",
        new="b->nr_sw--;",
        tests=T,
    ),
    dict(
        name="sw-add-duplicate",
        breaks="gdb manual E 'Z' packets: inserting the same breakpoint twice must not record it twice",
        file=X86,
        old="    if (gdb_x86_sw_find(b, gva) >= 0)\n        return 0;\n",
        new="",
        tests=T,
    ),
    # ---- 硬件断点、KVM_SET_GUEST_DEBUG、KVM_EXIT_DEBUG 归类 ----
    dict(
        name="dr7-enable-bit-wrong",
        breaks="asm/debugreg.h DR_LOCAL_ENABLE/DR_ENABLE_SIZE: Ln of breakpoint n is DR7 bit 2n",
        file=X86,
        old="dr7 |= 1ull << (2 * i);",
        new="dr7 |= 1ull << i;",
        tests=T,
    ),
    dict(
        name="hw-add-duplicate-slot",
        breaks="gdb manual E 'Z' packets: inserting the same hardware breakpoint twice reuses its slot",
        file=X86,
        old="        if (b->hw_used[i] && b->hw[i] == addr)\n            return i;\n",
        new="",
        tests=T,
    ),
    dict(
        name="guestdbg-no-enable",
        breaks="linux/kvm.h: KVM_GUESTDBG_USE_SW_BP has no effect without KVM_GUESTDBG_ENABLE",
        file=X86,
        old="dbg->control |= KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_SW_BP;",
        new="dbg->control |= KVM_GUESTDBG_USE_SW_BP;",
        tests=T,
    ),
    dict(
        name="step-extra-dropped",
        breaks="gdbstub contract: single-step passes KVM_GUESTDBG_BLOCKIRQ when KVM has it",
        file=X86,
        old="KVM_GUESTDBG_SINGLESTEP |\n                        step_extra;",
        new="KVM_GUESTDBG_SINGLESTEP;",
        tests=T,
    ),
    dict(
        name="classify-foreign-bp-swallowed",
        breaks="x86.c: a guest's own int3 (e.g. Linux int3_selftest) must be reinjected, not reported to gdb",
        file=X86,
        old=": GDB_X86_EV_REINJECT_BP;",
        new=": GDB_X86_EV_SWBREAK;",
        tests=T,
    ),
    dict(
        name="classify-step-without-bs",
        breaks="asm/debugreg.h DR_STEP: a single-step #DB sets DR6.BS; other #DBs while stepping belong to the guest",
        file=X86,
        old="if (stepping && (ex->dr6 & GDB_X86_DR6_BS))",
        new="if (stepping)",
        tests=T,
    ),
    dict(
        name="classify-hw-unused-slot",
        breaks="x86.c: a DR6 Bn hit for a slot gdb did not set is the guest's own #DB",
        file=X86,
        old="if (b->hw_used[i] && (ex->dr6 & (1u << i)))",
        new="if (ex->dr6 & (1u << i))",
        tests=T,
    ),
]
