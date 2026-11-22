#!/usr/bin/env python3
"""tests/monitor_client.py — 连接 VMM 的 monitor socket，依次发命令并检查回复

用法：
  monitor_client.py [--timeout S] SOCKET STEP...

每个 STEP 是一条 monitor 命令，或者以 @ 开头的测试动作：
  CMD                 发送一行命令，打印 "mon> CMD" 和完整回复
  @long:N             发送一行 N 个 'x'，打印回复（测超长行）
  @partial:TEXT       发送不带换行的 TEXT 后立刻断开（测半行断开），只能是最后一步
  @repeat:N:CMD       连续发送 N 次 CMD，打印 OK/ERR 计数和第一条 ERR
  @check-cpus         info cpus，核对每个 CPU 的 thread_id 是 VMM 里名为
                      vcpuN 的线程（需要环境变量 VMM_PID），打印 CHECK 行
  @check-regs:LO:HI   info registers -a，核对每个 vCPU 的 rip 落在 [LO, HI)
                      （十六进制，内核代码段），CR0.PG 和 EFER.LMA 置位
  @stats-save:FILE    info exit-stats，把用户态 exit 总数存进 FILE
  @stats-grew:FILE    info exit-stats，和 FILE 里存的总数比较，打印是否增长
  @clients:N          另开 N 个并发连接，逐个探测是被接受还是被拒绝
  @wait-close         等 VMM 关闭连接（quit 之后 VMM 退出时）

协议见 include/monitor.h：正文行以两个空格开头，最后一行是 OK 或 ERR。
CHECK 行的格式是 "CHECK <名字> ok ..." 或 "CHECK <名字> FAIL <原因>"，
e2e suite 断言这些行。

退出码：0 正常走完（CHECK 失败也是 0，由调用方断言输出）；2 用法错误；
3 连不上 socket 或连接意外断开；124 等回复超时。超时只兜底 VMM 卡死，
正常回复是毫秒级的。
"""

import argparse
import os
import re
import socket
import sys

TIMEOUT_RC = 124


class Conn:
    def __init__(self, path, timeout):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(timeout)
        self.s.connect(path)
        self.buf = b""

    def send(self, data):
        self.s.sendall(data)

    def line(self):
        """读一行（不含换行）；连接关闭返回 None"""
        while b"\n" not in self.buf:
            chunk = self.s.recv(65536)
            if not chunk:
                return None
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode(errors="replace")

    def reply(self):
        """读一条完整回复，返回 (正文行列表, 结束行)；连接关闭时结束行为 None"""
        body = []
        while True:
            line = self.line()
            if line is None:
                return body, None
            if line.startswith("  "):
                body.append(line[2:])
                continue
            return body, line

    def command(self, cmd):
        self.send(cmd.encode() + b"\n")
        return self.reply()

    def close(self):
        self.s.close()


def show(label, body, end):
    print(f"mon> {label}")
    for b in body:
        print(f"  {b}")
    print(end if end is not None else "mon: connection closed")


def check(name, ok, detail):
    print(f"CHECK {name} {'ok' if ok else 'FAIL'} {detail}")


def exit_total(body):
    """info exit-stats 第一段的 total 行，第一列数字是所有 vCPU 的和"""
    for b in body:
        m = re.match(r"\s*total\s+(\d+)", b)
        if m:
            return int(m.group(1))
    return None


def step_check_cpus(c):
    body, end = c.command("info cpus")
    show("info cpus", body, end)
    pid = os.environ.get("VMM_PID")
    if not pid:
        check("cpus", False, "VMM_PID not set")
        return
    cpus = []
    for b in body:
        m = re.match(r"[* ]?\s*CPU #(\d+): thread_id=(\d+)", b)
        if m:
            cpus.append((int(m.group(1)), m.group(2)))
    if not cpus:
        check("cpus", False, "no 'CPU #N: thread_id=T' lines")
        return
    bad = []
    for idx, tid in cpus:
        try:
            with open(f"/proc/{pid}/task/{tid}/comm") as f:
                comm = f.read().strip()
        except OSError as e:
            bad.append(f"CPU #{idx} tid {tid}: {e.strerror}")
            continue
        if comm != f"vcpu{idx}":
            bad.append(f"CPU #{idx} tid {tid} is thread '{comm}'")
    check("cpus", not bad, "; ".join(bad) if bad
          else f"{len(cpus)} CPUs, each thread_id is the matching vcpuN thread")


def step_check_regs(c, lo, hi):
    body, end = c.command("info registers -a")
    show("info registers -a", body, end)
    regs = {}
    cur = None
    for b in body:
        m = re.match(r"=== vCPU (\d+) state ===", b)
        if m:
            cur = int(m.group(1))
            regs[cur] = {}
            continue
        if cur is None:
            continue
        for name in ("rip", "cr0", "efer"):
            m = re.search(rf"\b{name}=([0-9a-f]+)", b)
            if m:
                regs[cur][name] = int(m.group(1), 16)
    if end != "OK" or not regs:
        check("regs", False, f"reply ended with {end!r}, {len(regs)} vCPU blocks")
        return
    bad = []
    for cpu, r in sorted(regs.items()):
        if not lo <= r.get("rip", -1) < hi:
            bad.append(f"vCPU {cpu} rip {r.get('rip', -1):#x} outside kernel text")
        if not r.get("cr0", 0) >> 31 & 1:
            bad.append(f"vCPU {cpu} CR0.PG clear")
        if not r.get("efer", 0) >> 10 & 1:
            bad.append(f"vCPU {cpu} EFER.LMA clear")
    check("regs", not bad, "; ".join(bad) if bad
          else f"{len(regs)} vCPUs in long mode, rip inside kernel text")


def step_clients(path, timeout, n):
    conns = []
    for _ in range(n):
        conns.append(Conn(path, timeout))
    for i, c in enumerate(conns, 1):
        try:
            _, end = c.command("info status")
        except (BrokenPipeError, ConnectionResetError):
            end = None
            # 被拒绝的连接可能在我们发送之前就被关掉了，原因行仍在接收缓冲里
            try:
                end = c.line()
            except OSError:
                pass
        if end == "OK":
            print(f"client {i}: accepted")
        else:
            print(f"client {i}: rejected ({end})")
    for c in conns:
        c.close()


def main(argv):
    p = argparse.ArgumentParser(
        description="Send commands to the VMM monitor socket and check replies.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Steps starting with @ are test actions, see the module docstring.")
    p.add_argument("--timeout", type=float, default=30,
                   help="seconds to wait for each reply before exiting 124")
    p.add_argument("socket")
    p.add_argument("steps", nargs="+")
    a = p.parse_args(argv)

    for i, st in enumerate(a.steps):
        if st.startswith("@partial:") and i != len(a.steps) - 1:
            p.error("@partial must be the last step")

    conn = None
    st = ""
    try:
        for st in a.steps:
            if st.startswith("@clients:"):
                step_clients(a.socket, a.timeout, int(st.split(":", 1)[1]))
                continue
            if conn is None:
                conn = Conn(a.socket, a.timeout)
            if st.startswith("@long:"):
                n = int(st.split(":", 1)[1])
                conn.send(b"x" * n + b"\n")
                show(f"<{n}-byte line>", *conn.reply())
            elif st.startswith("@partial:"):
                conn.send(st.split(":", 1)[1].encode())
                print(f"mon> {st.split(':', 1)[1]!r} (no newline), disconnecting")
            elif st.startswith("@repeat:"):
                _, n, cmd = st.split(":", 2)
                ok = err = 0
                first_err = None
                for _ in range(int(n)):
                    _, end = conn.command(cmd)
                    if end == "OK":
                        ok += 1
                    else:
                        err += 1
                        first_err = first_err or end
                print(f"mon> {cmd} (x{n}): {ok} OK, {err} ERR"
                      + (f", first: {first_err}" if first_err else ""))
            elif st == "@check-cpus":
                step_check_cpus(conn)
            elif st.startswith("@check-regs:"):
                _, lo, hi = st.split(":")
                step_check_regs(conn, int(lo, 16), int(hi, 16))
            elif st.startswith("@stats-save:") or st.startswith("@stats-grew:"):
                body, end = conn.command("info exit-stats")
                total = exit_total(body)
                path = st.split(":", 1)[1]
                if st.startswith("@stats-save:"):
                    with open(path, "w") as f:
                        f.write(f"{total}\n")
                    print(f"mon> info exit-stats: total {total}, saved")
                else:
                    with open(path) as f:
                        before = int(f.read())
                    check("stats-grew", total is not None and total > before,
                          f"{before} -> {total}")
            elif st == "@wait-close":
                line = conn.line()
                print("mon: connection closed by VMM" if line is None
                      else f"mon: unexpected line {line!r}")
            elif st.startswith("@"):
                p.error(f"unknown test action {st!r}")
            else:
                show(st, *conn.command(st))
    except socket.timeout:
        print(f"TIMEOUT waiting for the monitor after step {st!r}")
        return TIMEOUT_RC
    except OSError as e:
        print(f"monitor_client: {a.socket}: {e}")
        return 3
    finally:
        if conn is not None:
            conn.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
