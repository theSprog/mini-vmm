#!/usr/bin/env python3
"""tests/guest_shell.py — 启动 VMM，在 guest 的 shell 里依次执行命令

用法：
  guest_shell.py --timeout 60 [--login USER] [--cmd CMD | --host CMD | --reboot CMD]... --stop CMD -- VMM [VMM 参数...]

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

--login 用于跑完整发行版的 guest（getty + bash，而不是 initramfs 里直接
起的 busybox shell）。每次输出里出现 getty 的 "HOST login: " 就发用户名，
所以 guest 重启后再次出现登录提示时会自动重新登录；用户必须是空密码，
pam_unix 对空密码不再问 Password。拿到 bash 提示符后发一行 LOGIN_SETUP，
把 shell 规整成和 busybox 一样：提示符改成同一个格式，后面的步骤因此
完全不区分两种 guest；关掉 systemd 往 PROMPT_COMMAND 和 PS0 里挂的
OSC 3008 上下文转义（/etc/profile.d/80-systemd-osc-context.sh），否则每条
命令前后都夹着一段转义序列；TERM=dumb 和 SYSTEMD_PAGER= 让 systemctl、
journalctl 不开分页器、不输出颜色，分页器会停在那里等按键。

--reboot（要求 --login）发出一条让 guest 重启的命令（systemctl reboot），
之后的步骤只认重新登录之后的提示符。不能用 --cmd 代替：systemctl reboot
只是把请求交给 systemd 就返回，旧 shell 马上又打出提示符，下一条命令会
发给正在关机的 shell 而丢失。

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

# getty 的登录提示：agetty 在新的一行打印 "主机名 login: "
LOGIN = re.compile(rb"\n\S+ login: ")

# 登录后 bash 的第一个提示符，来自发行版 /etc/bash.bashrc 的
# PS1='[\u@\h \W]\$ '；只用来知道 bash 已经在读输入
LOGIN_SHELL = re.compile(rb"\]# ")

# PS1 里的 '[guest'' \w]# ' 拆成两段引号：这一行会被 tty 回显，写成连续的
# '[guest \w]# ' 时回显本身就匹配 PROMPT，下一步会在 bash 执行这一行之前
# 发出去
LOGIN_SETUP = ("unset PROMPT_COMMAND PS0; "
               "export TERM=dumb SYSTEMD_COLORS=0 SYSTEMD_PAGER=; "
               "PS1='[guest'' \\w]# '")

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
    p.add_argument("--reboot", dest="steps", action="append",
                   type=lambda c: ("reboot", c), metavar="CMD",
                   help="command that reboots the guest, e.g. 'systemctl "
                        "reboot'; later steps wait for the prompt after the "
                        "next login (requires --login)")
    p.add_argument("--login", metavar="USER",
                   help="log in as USER (empty password) whenever a getty "
                        "'login:' prompt appears, then switch the shell to "
                        "the standard prompt")
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
    if not a.login and any(kind == "reboot" for kind, _ in a.steps):
        p.error("--reboot requires --login: the prompt after the reboot "
                "comes from a new login")
    lines = [c for kind, c in a.steps if kind != "host"] + [a.stop]
    if a.login:
        lines.append(a.login)
    for c in lines:
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
    login_from = 0      # 下一个登录提示只在这之后找
    login_at = None     # 已发用户名、等 bash 提示符时，从这里找
    relogin = False     # 发过 --reboot，下一步要等重新登录

    def send(line):
        nonlocal stdin_ok
        try:
            proc.stdin.write(line.encode() + b"\n")
            proc.stdin.flush()
        except BrokenPipeError:
            stdin_ok = False

    while True:
        # 登录：先于步骤处理。登录完成前 PROMPT 不会出现，步骤自然停着
        while a.login and stdin_ok:
            if login_at is None:
                m = LOGIN.search(out, login_from)
                if not m:
                    break
                send(a.login)
                login_at = login_from = m.end()
            else:
                m = LOGIN_SHELL.search(out, login_at)
                if not m:
                    break
                send(LOGIN_SETUP)
                login_at = None
                login_from = m.end()
                if relogin:
                    # 之前出现的提示符都属于重启前的 shell
                    scan_from = max(scan_from, login_from)
                    relogin = False

        # 能发就发：上一条命令之后出现了新的提示符
        while sent < len(steps) and stdin_ok and not relogin:
            m = PROMPT.search(out, scan_from)
            if not m:
                break
            kind, c = steps[sent]
            if kind == "host":
                run_host(c, proc.pid, deadline)
                sent += 1
                continue
            scan_from = m.end()
            send(c)
            if not stdin_ok:
                break
            sent += 1
            relogin = kind == "reboot"

        left = deadline - time.monotonic()
        if left <= 0:
            if login_at is not None:
                what = f"waiting for the shell after logging in as {a.login!r}"
            elif relogin:
                what = f"waiting for the login after {labels[sent - 1]}"
            elif sent < len(steps):
                what = f"waiting for prompt before {labels[sent]}"
            else:
                what = f"waiting for the VMM to exit after {labels[-1]}"
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
