#!/usr/bin/env python3
"""tests/gdb_session.py — 启动带 --gdb 的 VMM，再让真实的 gdb 按脚本连上去调试它

用法：
  gdb_session.py --timeout 120 --script tests/gdb_checks.py --symbols vmlinux \\
                 [--stop 'poweroff -f'] -- VMM [VMM 参数...]

VMM 的参数里要带 --gdb 0。流程：
  1. 启动 VMM，stdout/stderr 合并后原样转发到本进程的 stdout；
  2. 等 VMM 打印 "gdbstub: listening on 127.0.0.1:PORT"，拿到内核分配的端口；
  3. 启动 gdb -batch -x SCRIPT SYMBOLS，环境变量 GDBSTUB_PORT 是端口；gdb 的
     输出每行加 "[gdb] " 前缀插进同一个输出流；
  4. gdb 退出后，给了 --stop 就等 guest shell 出现过提示符，把命令写进 VMM
     的 stdin（串口）；然后等 VMM 退出。

gdb 脚本经 stdout 打出以 "GDBREQ " 开头的行向这里提请求：
  GDBREQ interrupt-at-prompt
      等 guest shell 打印出提示符，先探测一次 vCPU 线程（标签 running），再给
      gdb 进程发 SIGINT，gdb 据此向 stub 发 Ctrl-C。脚本打出这行之后接着执行
      continue，不等应答。
  GDBREQ probe NAME
      探测一次 vCPU 线程，完成后往 gdb 的 stdin 写一行 "done"，脚本读到它再
      往下走。

探测读 /proc/VMM_PID/task/*/syscall，每个名为 vcpuN 的线程打一行
  [probe NAME] vcpuN syscall=futex|ioctl|running|<号码>
被 gdb 暂停的 vCPU 线程阻塞在 gdbstub 的条件变量上（futex），在跑的 vCPU
线程在 KVM_RUN 里（ioctl）或正占着 CPU（running）。/proc/PID/syscall 要求
ptrace 权限，Yama ptrace_scope=1 时只有祖先进程能读，所以由 VMM 的父进程，
也就是本脚本来读，不交给 gdb。

退出码：VMM 的退出码；--timeout 到了仍未结束时杀掉 VMM 和 gdb，返回 124；
用法错误返回 2。等待都挂在具体的事件上（端口行、提示符、进程退出），
--timeout 只兜底卡死，超时时在输出末尾写明卡在哪一步。
"""

import argparse
import os
import re
import signal
import subprocess
import sys
import threading
import time

from guest_shell import PROMPT

PORT_RE = re.compile(rb"gdbstub: listening on 127\.0\.0\.1:(\d+)")
TIMEOUT_RC = 124

# x86_64 系统调用号，arch/x86/entry/syscalls/syscall_64.tbl
SYSCALL_NAMES = {"202": "futex", "16": "ioctl"}

out_lock = threading.Lock()
at_line_start = True


def emit(data, own_line=False):
    """own_line：本脚本自己的行（探测结果、诊断）要从行首开始。guest shell
    的提示符后面没有换行，直接接在后面会让按行匹配的断言对不上"""
    global at_line_start
    with out_lock:
        if own_line and not at_line_start:
            data = b"\n" + data
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
        at_line_start = data.endswith(b"\n")


def note(msg):
    emit(("[gdb_session] %s\n" % msg).encode(), own_line=True)


def probe(pid, name):
    task_dir = "/proc/%d/task" % pid
    try:
        tids = sorted(os.listdir(task_dir), key=int)
    except OSError as e:
        emit(("[probe %s] cannot list %s: %s\n" % (name, task_dir, e.strerror)).encode(),
             own_line=True)
        return
    for tid in tids:
        base = "%s/%s/" % (task_dir, tid)
        comm = "tid %s" % tid
        try:
            with open(base + "comm") as f:
                comm = f.read().strip()
            if not comm.startswith("vcpu"):
                continue
            with open(base + "syscall") as f:
                nr = f.read().split()[0]
        except OSError as e:
            nr = "unreadable(%s)" % e.strerror
        emit(("[probe %s] %s syscall=%s\n" % (name, comm, SYSCALL_NAMES.get(nr, nr))).encode(),
             own_line=True)


class Vmm:
    """VMM 进程和它的输出里我们关心的两个事件"""

    def __init__(self, argv):
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
        self.port = None
        self.port_seen = threading.Event()
        self.prompt_seen = threading.Event()
        self.exited = threading.Event()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        buf = b""
        while True:
            chunk = self.proc.stdout.read1(4096)
            if not chunk:
                break
            emit(chunk)
            # 只保留尾部，足够匹配跨 read 边界的端口行和提示符
            buf = (buf + chunk)[-4096:]
            if self.port is None:
                m = PORT_RE.search(buf)
                if m:
                    self.port = int(m.group(1))
                    self.port_seen.set()
            if PROMPT.search(buf):
                self.prompt_seen.set()
        self.proc.wait()
        self.exited.set()
        self.port_seen.set()        # 让等端口的一方也醒来，再看 exited


class Gdb:
    def __init__(self, gdb, script, symbols, port, vmm):
        env = dict(os.environ, GDBSTUB_PORT=str(port))
        # -nx：不读用户的 .gdbinit；auto-load 关掉是因为内核构建目录里的
        # vmlinux-gdb.py 不在默认 safe-path 里，会打一大段与测试无关的警告
        self.proc = subprocess.Popen(
            [gdb, "-nx", "-batch", "-iex", "set auto-load python-scripts off",
             "-x", script, symbols],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, env=env)
        self.vmm = vmm
        self.exited = threading.Event()
        self.failure = None         # 处理请求时卡住的原因，供超时诊断
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        for line in iter(self.proc.stdout.readline, b""):
            emit(b"[gdb] " + line, own_line=True)
            text = line.decode(errors="replace").strip()
            if text.startswith("GDBREQ "):
                threading.Thread(target=self._request, args=(text[7:],),
                                 daemon=True).start()
        self.proc.wait()
        emit(("[gdb] rc=%d\n" % self.proc.returncode).encode(), own_line=True)
        self.exited.set()

    def _request(self, req):
        pid = self.vmm.proc.pid
        if req == "interrupt-at-prompt":
            self.failure = "waiting for the guest shell prompt before interrupting"
            self.vmm.prompt_seen.wait()
            probe(pid, "running")
            self.failure = None
            os.kill(self.proc.pid, signal.SIGINT)
        elif req.startswith("probe "):
            probe(pid, req[6:])
            try:
                self.proc.stdin.write(b"done\n")
                self.proc.stdin.flush()
            except OSError:
                pass
        else:
            note("unknown request from gdb script: %s" % req)


def kill(proc):
    if proc and proc.poll() is None:
        proc.kill()
        proc.wait()


def wait_event(ev, deadline):
    return ev.wait(max(0.0, deadline - time.monotonic()))


def main(argv):
    p = argparse.ArgumentParser(
        prog="gdb_session.py",
        description="Boot the VMM with --gdb 0 and run a gdb script against it.")
    p.add_argument("--timeout", type=float, required=True,
                   help="seconds before both processes are killed (exit 124)")
    p.add_argument("--script", required=True, help="gdb script (-x)")
    p.add_argument("--symbols", required=True, help="vmlinux for gdb")
    p.add_argument("--gdb", default="gdb", help="gdb binary (default: gdb)")
    p.add_argument("--stop", help="command for the guest shell after gdb exits")
    p.add_argument("vmm", nargs=argparse.REMAINDER,
                   help="-- VMM [args...], must include --gdb 0")
    a = p.parse_args(argv)
    if a.vmm and a.vmm[0] == "--":
        a.vmm = a.vmm[1:]
    if not a.vmm or "--gdb" not in a.vmm:
        p.error("give the VMM command after --, including --gdb PORT")

    deadline = time.monotonic() + a.timeout
    vmm = Vmm(a.vmm)
    gdb = None
    step = "waiting for the gdbstub port line"
    try:
        if not wait_event(vmm.port_seen, deadline):
            raise TimeoutError(step)
        if vmm.port is None:
            note("VMM exited before the gdbstub was listening")
            return vmm.proc.wait()

        gdb = Gdb(a.gdb, a.script, a.symbols, vmm.port, vmm)
        step = "waiting for gdb to finish its script"
        if not wait_event(gdb.exited, deadline):
            raise TimeoutError(gdb.failure or step)

        if a.stop:
            step = "waiting for the guest shell prompt to send --stop"
            if not wait_event(vmm.prompt_seen, deadline):
                raise TimeoutError(step)
            vmm.proc.stdin.write(a.stop.encode() + b"\n")
            vmm.proc.stdin.flush()

        step = "waiting for the VMM to exit"
        if not wait_event(vmm.exited, deadline):
            raise TimeoutError(step)
        return vmm.proc.returncode
    except TimeoutError as e:
        note("timeout after %.0fs: %s" % (a.timeout, e))
        return TIMEOUT_RC
    finally:
        kill(gdb.proc if gdb else None)
        kill(vmm.proc)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
