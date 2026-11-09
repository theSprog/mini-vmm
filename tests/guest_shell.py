#!/usr/bin/env python3
"""tests/guest_shell.py — 启动 VMM，在 guest 的 busybox shell 里依次执行命令

用法：
  guest_shell.py --timeout 60 [--cmd CMD | --host CMD]... --stop CMD -- VMM [VMM 参数...]

VMM 的 stdin 接一个管道，stdout/stderr 合并后原样转发到本进程的 stdout。
每条 --cmd 都等 shell 打印出提示符之后才发送；--stop（poweroff -f、
reboot -f、exit 等）发出后不再等提示符，等 VMM 进程自己退出。

退出码：VMM 的退出码；--timeout 到了仍未结束时杀掉 VMM 返回 124（与
coreutils timeout 相同，测试里的 expect_rc 据此报错）；用法错误返回 2。

为什么要等提示符而不是按固定延迟发送：guest 的串口驱动在 tty 打开时会
清空 RX FIFO，shell 起来之前送进去的字节全部丢失。

--host 在 host 上用 sh -c 执行一条命令，时机和 --cmd 一样是“出现提示符
之后”，用来在 guest 确实跑起来、停在 shell 里的时候观察 VMM 进程（线程、
fd、统计）。环境变量 VMM_PID 是 VMM 的 pid；输出夹在 "[host] $ CMD" 和
"[host] rc=N" 两行之间插进输出流。它不往 guest 发任何字节，所以不消耗
提示符，下一步仍然等同一个提示符。启动耗时随内核配置、
vCPU 数、host 负载变化，任何固定延迟都有猜错的时候，猜错时 shell 一直
等输入，用例以超时失败，看起来和 VMM 卡死一模一样。

约束：
- 提示符的格式来自 tools/build_initramfs.sh 写的 /etc/profile 里的 PS1
  （'[guest \\w]# '），两边要一起改。
- 单条命令（含换行）不能超过串口 RX 缓冲区能存的 255 字节：src/console.c
  逐字节往缓冲区里推，guest 读走之前满了就丢。超长命令在这里直接拒绝。
- 超时时在输出末尾写明卡在哪一步，这条诊断写进 stdout 而不是 stderr，
  因为调用方只断言和打印 stdout。
"""

import argparse
import os
import re
import select
import signal
import subprocess
import sys
import time

# 与 /etc/profile 的 PS1='[guest \w]# ' 对应；\w 是当前目录，不含 ']' 和换行
PROMPT = re.compile(rb"\[guest [^\]\r\n]*\]# ")

# include/devices/serial8250.h 的 SERIAL_RX_BUF 是 256 格的环形缓冲区，
# 留一格区分空和满，最多存 255 字节
RX_FIFO_BYTES = 255

TIMEOUT_RC = 124


def parse_args(argv):
    p = argparse.ArgumentParser(
        prog="guest_shell.py",
        description="Boot the VMM and run commands in the guest shell, "
                    "sending each one only after the shell prompt appears.")
    p.add_argument("--timeout", type=float, required=True,
                   help="seconds before the VMM is killed (exit 124)")
    # --cmd 和 --host 进同一个列表，保持命令行上的先后顺序
    p.add_argument("--cmd", dest="steps", action="append", default=[],
                   type=lambda c: ("cmd", c), metavar="CMD",
                   help="command to run in the guest at a prompt; repeatable, "
                        "in order")
    p.add_argument("--host", dest="steps", action="append",
                   type=lambda c: ("host", c), metavar="CMD",
                   help="shell command to run on the host at a prompt, with "
                        "VMM_PID set; sends nothing to the guest")
    p.add_argument("--stop", required=True,
                   help="last command, e.g. 'poweroff -f', 'reboot -f', 'exit'; "
                        "after it the VMM is expected to exit by itself")
    p.add_argument("vmm", nargs=argparse.REMAINDER,
                   help="-- followed by the VMM command line")
    a = p.parse_args(argv)
    if a.vmm and a.vmm[0] == "--":
        a.vmm = a.vmm[1:]
    if not a.vmm:
        p.error("missing VMM command line after --")
    for c in [c for kind, c in a.steps if kind == "cmd"] + [a.stop]:
        if len(c.encode()) + 1 > RX_FIFO_BYTES:
            p.error(f"command longer than {RX_FIFO_BYTES - 1} bytes would "
                    f"overflow the guest RX FIFO: {c[:40]!r}...")
        if "\n" in c:
            p.error(f"command must be a single line: {c[:40]!r}")
    return a


def emit(data):
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()


def note(msg):
    emit(f"\n[guest_shell] {msg}\n".encode())


def run_host(cmd, pid, deadline):
    # 超时只兜底 host 命令自己卡住，用整个用例剩下的时间
    emit(f"\n[host] $ {cmd}\n".encode())
    env = dict(os.environ, VMM_PID=str(pid))
    try:
        r = subprocess.run(["sh", "-c", cmd], env=env, stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=max(deadline - time.monotonic(), 0))
        emit(r.stdout)
        emit(f"[host] rc={r.returncode}\n".encode())
    except subprocess.TimeoutExpired:
        emit(b"[host] timed out\n")


def kill(proc):
    # 兜底路径：VMM 已经超时，先 SIGTERM 让它恢复终端、写完日志，
    # 不理会 SIGTERM 时 SIGKILL。这里的 5 秒只限制清理本身不会卡住。
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def main(argv):
    a = parse_args(argv)
    steps = a.steps + [("cmd", a.stop)]
    labels = [f"{kind} {i + 1}/{len(a.steps)} {c!r}"
              for i, (kind, c) in enumerate(a.steps)]
    labels.append(f"stop {a.stop!r}")

    try:
        proc = subprocess.Popen(a.vmm, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
    except OSError as e:
        print(f"guest_shell.py: cannot run {a.vmm[0]}: {e}", file=sys.stderr)
        return 2
    assert proc.stdin is not None and proc.stdout is not None

    deadline = time.monotonic() + a.timeout
    out = bytearray()
    scan_from = 0       # 下一个提示符只在这之后找，旧提示符不算数
    sent = 0            # 已完成的步骤数
    fd = proc.stdout.fileno()
    stdin_ok = True

    while True:
        # 能发就发：上一条命令之后出现了新的提示符
        while sent < len(steps) and stdin_ok:
            m = PROMPT.search(out, scan_from)
            if not m:
                break
            kind, c = steps[sent]
            if kind == "host":
                run_host(c, proc.pid, deadline)
                sent += 1
                continue
            scan_from = m.end()
            try:
                proc.stdin.write(c.encode() + b"\n")
                proc.stdin.flush()
            except BrokenPipeError:
                stdin_ok = False
                break
            sent += 1

        left = deadline - time.monotonic()
        if left <= 0:
            what = (f"waiting for prompt before {labels[sent]}"
                    if sent < len(steps) else
                    f"waiting for the VMM to exit after {labels[-1]}")
            note(f"timeout after {a.timeout:g}s while {what}")
            kill(proc)
            return TIMEOUT_RC

        r, _, _ = select.select([fd], [], [], left)
        if not r:
            continue
        chunk = os.read(fd, 65536)
        if not chunk:
            break       # EOF：VMM 退出（或关掉了输出）
        emit(chunk)
        out += chunk

    if sent < len(steps):
        note(f"VMM output ended before {labels[sent]} was sent")
    try:
        proc.stdin.close()
    except BrokenPipeError:
        pass
    try:
        rc = proc.wait(timeout=max(deadline - time.monotonic(), 0))
        # 被信号杀死时 Popen 给负数，换成 shell 的 128+N 约定
        return 128 - rc if rc < 0 else rc
    except subprocess.TimeoutExpired:
        note(f"timeout after {a.timeout:g}s: VMM closed its output "
             f"but did not exit")
        kill(proc)
        return TIMEOUT_RC


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
