# tests/mutants/monitor.py — 里程碑 monitor 的源码变异体，由 tests/mutate.py 读取
#
# 协议核心的错误由单元测试 monitor_test 挡住（tests=），默认的 negctl 会跑；
# vCPU 工作队列、统计 fd、线程号、quit 这些只有真实 vCPU 线程才看得出来，
# 是 e2e mutant（suite=），要跑它们：
#   tests/mutate.py monitor
# 判据见 tests/TEST_DESIGN.md 的 E32。

MON = "src/monitor.c"
CMDS = "src/monitor_cmds.c"

MUTANTS = [
    dict(
        name="body-not-indented",
        breaks="monitor.h: body lines start with two spaces, so a body line "
               "can never look like the OK/ERR end marker",
        file=MON,
        old='        out_str(c, "  ");\n',
        new="",
        tests=["monitor_test"],
    ),
    dict(
        name="line-limit-off-by-one",
        breaks="monitor.h: a line of exactly MON_LINE_MAX bytes is accepted",
        file=MON,
        old="    if (len > MON_LINE_MAX) {",
        new="    if (len >= MON_LINE_MAX) {",
        tests=["monitor_test"],
    ),
    dict(
        name="discard-never-ends",
        breaks="monitor.h: an over-long line is dropped up to its newline, "
               "the next line is parsed normally",
        file=MON,
        old="            c->discarding = false;\n",
        new="",
        tests=["monitor_test"],
    ),
    dict(
        name="no-wait-for-pending",
        breaks="monitor.h: later commands on a connection wait for its async "
               "command, replies stay in request order",
        file=MON,
        old="    while (!c->pending && off < c->in_len) {",
        new="    while (off < c->in_len) {",
        tests=["monitor_test"],
    ),
    dict(
        name="pending-not-orphaned",
        breaks="monitor.h: a request still pending when its connection goes "
               "away is finished as an orphan, not replied to a dead connection",
        file=MON,
        old="        c->pending->conn = NULL;\n",
        new="",
        tests=["monitor_test"],
    ),
    dict(
        name="crlf-kept",
        breaks="monitor.h: a trailing \\r is ignored",
        file=MON,
        old="        if (len && start[len - 1] == '\\r')\n            len--;\n",
        new="",
        tests=["monitor_test"],
    ),
    dict(
        name="err-newline-kept",
        breaks="monitor.h: the end marker is one line even when the reason "
               "contains a newline",
        file=MON,
        old="            *p = ' ';\n",
        new="            ;\n",
        tests=["monitor_test"],
    ),
    # 以下是 e2e mutant
    dict(
        name="work-not-run-on-kick",
        breaks="smp.h: a kicked vCPU runs its queued work before going back "
               "into the guest; an idle vCPU sits in HLT and never would",
        file="src/vcpu.c",
        old="            vcpu_run_work(vcpu);\n",
        new="",
        suite="monitor",
    ),
    dict(
        name="immediate-exit-not-cleared",
        breaks="smp.h: after a work kick the vCPU returns to the guest; left "
               "set, immediate_exit keeps it out of KVM_RUN for good",
        file="src/vcpu.c",
        old="                __atomic_store_n(&vcpu->run->immediate_exit, 0,\n"
            "                                 __ATOMIC_SEQ_CST);\n",
        new="                ;\n",
        suite="monitor",
    ),
    dict(
        name="pid-not-tid",
        breaks="info cpus: thread_id is the host thread of that vCPU",
        file="src/smp.c",
        old="(int)gettid()",
        new="(int)getpid()",
        suite="monitor",
    ),
    dict(
        name="host-state-constant",
        breaks="info cpus: host= is the scheduler state of the vCPU thread "
               "(S while the guest halts, R while it spins)",
        file=CMDS,
        old="    return (p && p[1] == ' ' && p[2]) ? p[2] : '-';",
        new="    (void)p; return 'S';",
        suite="monitor",
    ),
    dict(
        name="stats-fd-opened-while-running",
        breaks="vmm.h stats_fd: KVM_GET_STATS_FD takes vcpu->mutex, which a "
               "vCPU holds for the whole of KVM_RUN; opened from the I/O "
               "thread at query time it blocks until the vCPU leaves the guest",
        file="src/exit_stats.c",
        old="read_kvm_stats(out, vm->vcpus[n].stats_fd,",
        new="read_kvm_stats(out, kvm_get_stats_fd(vm->vcpus[n].fd),",
        suite="monitor",
    ),
    dict(
        name="quit-does-not-stop",
        breaks="quit: the VMM exits with status 0",
        file=CMDS,
        old="    vm_request_stop(vm);\n    return MON_OK;",
        new="    return MON_OK;",
        suite="monitor",
    ),
]
