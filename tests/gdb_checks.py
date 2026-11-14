"""tests/gdb_checks.py — 在 gdb 里执行的 gdbstub 验收脚本（gdb -batch -x）

由 tests/gdb_session.py 启动，环境变量：
  GDBSTUB_PORT        stub 的端口
  GDBCHECK_SCENARIO   full（默认）、exit 或 reset
  GDBCHECK_VCPUS      VMM 的 vCPU 数

每项检查打一行 "GDBCHECK ok NAME" 或 "GDBCHECK FAIL NAME: 细节"，
tests/e2e/gdbstub.sh 断言这些行。向 gdb_session.py 提请求的行以 "GDBREQ "
开头，协议见 gdb_session.py 的说明。

判据都取自 gdb 之外的来源，而不是“gdb 自己读回来的值和自己写进去的一样”：
断点地址来自 vmlinux 的符号，单步后的 pc 来自对停下那条指令的反汇编长度，
写寄存器之后先清掉 gdb 的寄存器缓存再读，内存读写关掉 gdb 的内存缓存。
"""

import os

import gdb

failed = 0


def check(name, ok, detail=""):
    global failed
    if ok:
        print("GDBCHECK ok %s" % name, flush=True)
    else:
        failed += 1
        print("GDBCHECK FAIL %s: %s" % (name, detail), flush=True)


def ex(cmd):
    return gdb.execute(cmd, to_string=True)


# 最近一次停止事件。gdb 对异步到来的停止（例如 Ctrl-C）打印的那一行不进
# execute(to_string=True) 的返回值，停止原因从事件里取
last_stop = None


def on_stop(ev):
    global last_stop
    last_stop = ev


gdb.events.stop.connect(on_stop)

# W 包对应的退出事件，道理同上
exit_code = None


def on_exit(ev):
    global exit_code
    exit_code = getattr(ev, "exit_code", "unknown")


gdb.events.exited.connect(on_exit)


def request(line, wait=False):
    print("GDBREQ " + line, flush=True)
    if wait:
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = os.read(0, 64)
            if not chunk:
                break
            buf += chunk


def u64(expr):
    return int(gdb.parse_and_eval(expr).cast(gdb.lookup_type("unsigned long"))) \
        & 0xffffffffffffffff


def pc():
    return u64("$pc")


def stop_at(symbol, how):
    """设断点（how 是 break 或 hbreak），continue，检查停在符号地址上"""
    want = u64("&" + symbol)
    ex("%s %s" % (how, symbol))
    out = ex("continue")
    got = pc()
    check("%s-%s" % (how, symbol), got == want,
          "pc 0x%x, %s is 0x%x; gdb said: %s" % (got, symbol, want, out.strip()))
    return got == want


def connect():
    ex("set pagination off")
    ex("set confirm off")
    # 内存读写的判据要来自 stub：关掉 gdb 对代码段和栈的读缓存
    ex("set code-cache off")
    ex("set stack-cache off")
    ex("target remote 127.0.0.1:%s" % os.environ["GDBSTUB_PORT"])


def scenario_full(nr_vcpus):
    inf = gdb.selected_inferior()

    check("threads-per-vcpu", len(inf.threads()) == nr_vcpus,
          "%d threads, expected %d" % (len(inf.threads()), nr_vcpus))

    # 启动入口只有 VMM 建的恒等映射，内核虚拟地址翻译不了：软件断点插不进去，
    # 硬件断点比较的是线性地址，不需要翻译
    if not stop_at("start_kernel", "hbreak"):
        return
    check("stopped-thread-is-vcpu0", gdb.selected_thread().num == 1,
          "thread %d" % gdb.selected_thread().num)

    # stepi：下一条 pc = 本条地址 + 指令长度。start_kernel 开头关着中断，
    # 单步不会走进中断处理函数
    start = pc()
    insn = gdb.selected_frame().architecture().disassemble(start)[0]
    mnem = insn["asm"].split()[0]
    branch = mnem.startswith(("j", "call", "ret", "loop"))
    check("stepi-from-straight-line-insn", not branch, "first insn is %s" % insn["asm"])
    ex("stepi")
    check("stepi", pc() == start + insn["length"],
          "pc 0x%x after stepping '%s' at 0x%x (len %d)"
          % (pc(), insn["asm"], start, insn["length"]))

    # 写寄存器：清掉 gdb 的寄存器缓存后读回，读到的是 stub 那边的值
    orig = u64("$rax")
    ex("set $rax = 0x1122334455667788")
    ex("maintenance flush register-cache")
    check("write-register", u64("$rax") == 0x1122334455667788,
          "rax reads back 0x%x" % u64("$rax"))
    ex("set $rax = %d" % orig)
    ex("maintenance flush register-cache")
    check("restore-register", u64("$rax") == orig, "rax 0x%x" % u64("$rax"))

    # 读写内存（内核虚拟地址，经 KVM_TRANSLATE）
    banner = u64("&linux_banner")
    data = bytes(inf.read_memory(banner, 13))
    check("read-memory", data == b"Linux version", "linux_banner starts %r" % data)
    inf.write_memory(banner, b"LINUX")
    data = bytes(inf.read_memory(banner, 5))
    check("write-memory", data == b"LINUX", "read back %r" % data)
    inf.write_memory(banner, b"Linux")
    data = bytes(inf.read_memory(banner, 5))
    check("restore-memory", data == b"Linux", "read back %r" % data)
    try:
        inf.read_memory(0xdead000000000000, 1)
        check("unmapped-memory-errors", False, "read of a non-canonical address succeeded")
    except gdb.MemoryError:
        check("unmapped-memory-errors", True)

    # 软件断点。这次 continue 会跑过 alternative_instructions() 里的
    # int3_selftest()：USE_SW_BP 打开时那条 int3 也退出到 VMM，必须还给
    # guest，否则内核 BUG。VMM 结束时打印还回去的个数，shell 那边断言它
    ex("delete")
    if not stop_at("kernel_init", "break"):
        return

    try:
        ex("thread 2")
        u64("$pc")
        ex("thread 1")
        check("read-other-vcpu", True)
    except gdb.error as e:
        check("read-other-vcpu", False, str(e))

    # 跑到 guest shell 空闲，Ctrl-C 停下：所有 vCPU 线程必须都停在 gdbstub
    # 的条件变量上（futex），没有谁还留在 KVM_RUN 里
    ex("delete")
    request("interrupt-at-prompt")
    ex("continue")
    sig = getattr(last_stop, "stop_signal", None)
    check("interrupt", sig == "SIGINT",
          "stop event %s, signal %s" % (type(last_stop).__name__, sig))
    request("probe stopped", wait=True)
    check("threads-after-interrupt", len(inf.threads()) == nr_vcpus,
          "%d threads" % len(inf.threads()))

    ex("detach")
    check("detach", True)


def scenario_exit():
    # guest 自己结束（rdinit=/nonexistent → panic → i8042 reset）时，gdb 在等
    # 停止应答，stub 报告 W
    ex("continue")
    check("exit-reported", exit_code == 0, "exit event code %r" % (exit_code,))


def scenario_reset():
    # guest 用 rdinit=/nonexistent 启动、不带 --no-reboot：每次启动都 panic 后
    # 整机复位。软件断点在第一次启动里插入，要在复位后的下一次启动里再次命中
    if not stop_at("start_kernel", "hbreak"):
        return
    ex("delete")
    if not stop_at("rest_init", "break"):
        return
    # 第一次启动已经走到 panic 时 panic_cpu 不再是 -1；下一次启动停在
    # rest_init 时它又是 -1，说明内核数据段确实被重新装载，停下的是新的一次
    # 启动，而不是同一次启动里的又一次调用
    out = ex("continue")
    want = u64("&rest_init")
    got = pc()
    check("break-after-reset", got == want,
          "pc 0x%x, rest_init is 0x%x; gdb said: %s" % (got, want, out.strip()))
    cpu = int(gdb.parse_and_eval("panic_cpu.counter"))
    check("image-reloaded", cpu == -1, "panic_cpu.counter is %d" % cpu)
    ex("kill")


def main():
    try:
        connect()
        check("connect", True)
    except gdb.error as e:
        check("connect", False, str(e))
        return
    scenario = os.environ.get("GDBCHECK_SCENARIO", "full")
    if scenario == "exit":
        scenario_exit()
    elif scenario == "reset":
        scenario_reset()
    else:
        scenario_full(int(os.environ.get("GDBCHECK_VCPUS", "1")))
    print("GDBCHECK done, %d failed" % failed, flush=True)


try:
    main()
except gdb.error as e:
    check("script", False, "gdb error: %s" % e)
