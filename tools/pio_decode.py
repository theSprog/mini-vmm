#!/usr/bin/env python3
"""Slice and annotate a --trace-pio log.

The VMM writes one line per PIO access that reached user space:

    <sec>.<usec> <device> <port> R|W<size> <value> [rip=<guest RIP>]

This tool does the three things one always ends up doing by hand:

  --summary   how many accesses went to each port / device
  --console   rebuild the guest console text from the UART data writes,
              so a trace line number can be matched to a kernel message
  --syms      turn rip= into <symbol>+<offset> using a vmlinux, which is
              how one finds out who is poking an unregistered port
"""
import argparse
import bisect
import re
import subprocess
import sys

TRACE_RE = re.compile(
    r"^(?P<ts>\d+\.\d+)\s+(?P<dev>\S+)\s+(?P<port>[0-9a-f]+)\s+"
    r"(?P<dir>[RW])(?P<size>\d+)\s+(?P<val>[0-9a-f]+)"
    r"(?:\s+rip=(?P<rip>[0-9a-f]+))?\s*$"
)


class Record:
    __slots__ = ("lineno", "ts", "dev", "port", "dir", "size", "val", "rip")

    def __init__(self, lineno, m):
        self.lineno = lineno
        self.ts = float(m.group("ts"))
        self.dev = m.group("dev")
        self.port = int(m.group("port"), 16)
        self.dir = m.group("dir")
        self.size = int(m.group("size"))
        self.val = int(m.group("val"), 16)
        self.rip = int(m.group("rip"), 16) if m.group("rip") else None


def load(path):
    recs = []
    bad = 0
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            m = TRACE_RE.match(line)
            if m:
                recs.append(Record(lineno, m))
            elif line.strip():
                bad += 1
    if bad:
        print("warning: %d unparsable line(s) skipped" % bad, file=sys.stderr)
    return recs


class SymbolTable:
    """nm -n output, binary searched."""

    def __init__(self, vmlinux):
        out = subprocess.run(["nm", "-n", vmlinux], check=True,
                             capture_output=True, text=True).stdout
        syms = []
        for line in out.splitlines():
            f = line.split()
            if len(f) == 3:
                try:
                    syms.append((int(f[0], 16), f[2]))
                except ValueError:
                    pass
        syms.sort()
        self.addrs = [a for a, _ in syms]
        self.names = [n for _, n in syms]

    def lookup(self, addr):
        i = bisect.bisect_right(self.addrs, addr) - 1
        if i < 0:
            return "?"
        return "%s+0x%x" % (self.names[i], addr - self.addrs[i])


def parse_ports(spec):
    return {int(p, 16) for p in spec.replace(" ", "").split(",") if p}


def do_summary(recs):
    by_port = {}
    by_dev = {}
    for r in recs:
        key = (r.dev, r.port, r.dir, r.size)
        by_port[key] = by_port.get(key, 0) + 1
        by_dev[r.dev] = by_dev.get(r.dev, 0) + 1

    print("%-12s %-6s %-4s %8s" % ("device", "port", "dir", "count"))
    for (dev, port, d, size), n in sorted(by_port.items(),
                                          key=lambda kv: -kv[1]):
        print("%-12s %04x   %s%-3d %8d" % (dev, port, d, size, n))
    print()
    print("%-12s %8s" % ("device", "count"))
    for dev, n in sorted(by_dev.items(), key=lambda kv: -kv[1]):
        print("%-12s %8d" % (dev, n))
    print("%-12s %8d" % ("TOTAL", len(recs)))


def do_console(recs, data_port):
    """Rebuild console text from byte writes to the UART data register.

    Prints '<first>-<last>  <text>' so a trace line number found by other
    means can be located in the boot log, and the other way round.
    """
    cur = []
    start = None
    for r in recs:
        if r.port != data_port or r.dir != "W" or r.size != 1:
            continue
        if start is None:
            start = r.lineno
        if r.val == 0x0A:
            print("%d-%d\t%s" % (start, r.lineno, "".join(cur)))
            cur = []
            start = None
        elif r.val != 0x0D:
            cur.append(chr(r.val) if 0x20 <= r.val < 0x7F else ".")
    if cur:
        print("%d-\t%s" % (start, "".join(cur)))


def do_list(recs, syms, base_ts):
    for r in recs:
        line = "%5d %9.6f %-12s %04x %s%-2d %0*x" % (
            r.lineno, r.ts - base_ts, r.dev, r.port, r.dir, r.size,
            r.size * 2, r.val)
        if r.rip is not None:
            line += "  " + (syms.lookup(r.rip) if syms else "rip=%016x" % r.rip)
        print(line)


def main():
    ap = argparse.ArgumentParser(
        description="Slice and annotate a mini-vmm --trace-pio log.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""examples:
  %(prog)s pio.txt --summary
  %(prog)s pio.txt --port 70,71
  %(prog)s pio.txt --dev serial8250 --from 25700 --to 25800
  %(prog)s pio.txt --syms vmlinux --dev unhandled
  %(prog)s pio.txt --console
""")
    ap.add_argument("trace", help="file written by --trace-pio")
    ap.add_argument("--summary", action="store_true",
                    help="print per-port and per-device access counts")
    ap.add_argument("--console", action="store_true",
                    help="rebuild console text from writes to --data-port")
    ap.add_argument("--data-port", default="3f8",
                    help="UART data register for --console (hex, default 3f8)")
    ap.add_argument("--port", help="comma separated list of ports, hex")
    ap.add_argument("--dev", help="only records from this device name")
    ap.add_argument("--from", dest="first", type=int, default=0,
                    help="first trace line number to show")
    ap.add_argument("--to", dest="last", type=int, default=0,
                    help="last trace line number to show")
    ap.add_argument("--syms", metavar="VMLINUX",
                    help="resolve rip= against this vmlinux using nm -n")
    args = ap.parse_args()

    recs = load(args.trace)
    if not recs:
        print("no records", file=sys.stderr)
        return 1
    base_ts = recs[0].ts

    if args.console:
        do_console(recs, int(args.data_port, 16))
        return 0

    if args.port:
        want = parse_ports(args.port)
        recs = [r for r in recs if r.port in want]
    if args.dev:
        recs = [r for r in recs if r.dev == args.dev]
    if args.first:
        recs = [r for r in recs if r.lineno >= args.first]
    if args.last:
        recs = [r for r in recs if r.lineno <= args.last]

    if args.summary:
        do_summary(recs)
        return 0

    syms = SymbolTable(args.syms) if args.syms else None
    do_list(recs, syms, base_ts)
    return 0


if __name__ == "__main__":
    sys.exit(main())
