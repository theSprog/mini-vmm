#!/usr/bin/env python3
"""Source-level mutation runner for the C unit tests and the e2e suites.

Each mutant in tests/mutants/<set>.py replaces one exact piece of source text
with a deliberately wrong version. For every mutant this script copies the
build inputs into a scratch tree and applies the replacement. A unit mutant
builds the listed unit tests with the Makefile's own rules and runs them; it
is CAUGHT when at least one exits non-zero with an assertion FAIL line. An
e2e mutant builds build/vmm and runs one tests/e2e/<suite>.sh; it is CAUGHT
when the suite reports at least one FAIL case.
"""

# 为什么要有它：tests/unit/*_mutants.c 的接口层 mutant 只能整份替换对外
# 函数，而 MSI 这类逻辑的错误点在 static 函数或一条条件表达式里，只能改
# 源码文字。按文字（不是行号）定位，源码变了让 old 不再唯一出现时报
# STALE，suite 失败，不会悄悄变成“没跑”。
#
# e2e mutant 用来留住只有整机运行才看得到的错误（例如整机复位漏恢复 AP
# 的状态），它们每个要跑一整个 suite、启动多次 guest，所以只在点名运行
# （写出 set 或 mutant 名）或带 --e2e 时才跑；不带时仍检查 old 文字是否
# STALE，让 negctl 在源码改动后照样发现清单过期。e2e 的 suite 按顺序一个
# 一个跑，不并行：suite 里的 guest 有超时，几个 --smp 4 的 guest 抢 CPU 时
# 超时会被当成 FAIL，mutant 就成了“被抓住”，结论不可信。
#
# 判据和 tests/offline/negctl.sh 一致：退出非零且输出里有断言的 FAIL 行。
# 只看退出码不够，编译错、桩函数缺失、sanitizer 崩溃都会非零，但那不说明
# 断言有能力区分对错。先在没打补丁的副本上跑一遍同一批测试（baseline），
# baseline 不绿时所有 mutant 都会“被抓住”，结论没有意义，直接报错退出。

import argparse
import concurrent.futures
import os
import re
import runpy
import shutil
import subprocess
import sys
import tempfile
import threading
from dataclasses import dataclass, field
from enum import IntEnum
from pathlib import Path
from typing import NoReturn


class Exit(IntEnum):
    OK = 0
    FAIL = 1
    USAGE = 2


ROOT = Path(__file__).resolve().parent.parent
SETS_DIR = ROOT / "tests" / "mutants"
# 单元测试的构建只读这些路径（Makefile 直接编 src/ 下的 .c，头文件来自
# include/ 和 tests/unit/），副本里只放它们，复制很快
COPY_PATHS = ["Makefile", "include", "src", "tests/unit"]
# e2e suite 还要 tests/ 下的公共脚本和 tools/ 里的 initramfs 构建脚本
E2E_COPY_PATHS = ["Makefile", "include", "src", "tests", "tools"]
# suite 用 build/ 下的 initramfs，复制过去免得每个副本都重新打包
E2E_COPY_FILES = ["build/initramfs.cpio.gz"]
FAIL_LINE = re.compile(r"^  FAIL ", re.M)
SITE_LINE = re.compile(r"^\s+(\S+\.c:\d+): (.*)$", re.M)
# tests/lib.sh 的 summary() 打印的汇总行和失败用例列表
SUMMARY_LINE = re.compile(r"^  PASS (\d+)  FAIL (\d+)  SKIP (\d+)$", re.M)
FAILED_CASE = re.compile(r"^  - (.+)$", re.M)
ANSI = re.compile(r"\x1b\[[0-9;]*m")

HELP_EPILOG = """\
EXAMPLE
  tests/mutate.py                          run every mutant of every set
  tests/mutate.py pci-msi                  run one set
  tests/mutate.py pci-msi/ahci-no-msi-cap  run one mutant
  tests/mutate.py --list                   list sets, mutants and what each breaks
  tests/mutate.py system-reset             run one set, e2e mutants included
  tests/mutate.py --e2e                    run every mutant, e2e ones included
  tests/mutate.py --keep pci-msi/ahci-no-msi-cap
                                           keep the scratch tree to inspect the patch

NOTE
  Output is one tab-separated line per mutant, in set order:
    <STATUS>\\t<set>/<name>\\t<breaks>\\t<detail>
  STATUS is CAUGHT, ESCAPED (all listed tests passed), STALE (the old text
  is not found exactly once in the source), BUILD-FAIL (the mutant does not
  compile) or BAD-RED (a test failed without an assertion FAIL line, or an
  e2e suite exited non-zero without its PASS/FAIL/SKIP summary).
  Only CAUGHT counts as success; any other status makes the exit code 1.
  A mutant with `suite` is an e2e mutant. E2e mutants run only when their
  set or name is given on the command line, or with --e2e; otherwise only
  their old text is checked, and a STALE one is still reported and fails.
  A baseline of the unpatched tree runs first: every listed unit test must
  pass, and every e2e suite must report FAIL 0 and SKIP 0 (a skipped case
  would let a mutant escape untested); otherwise the script stops with
  exit code 1.
  E2e suites run one at a time and need /dev/kvm, the guest kernel (VMLINUX
  and BZIMAGE, defaulting to the ones in guest-kernel/) and whatever else
  the suite needs, such as gdb for gdbstub. One mutant takes as long as one
  `make test <suite>` run, longer when the mutant makes a guest hang until
  the suite's timeout.
  Scratch trees live under $TMPDIR and are removed unless --keep is given.
  The working tree is copied as is, so uncommitted changes are included.
  Requires make and the C toolchain used by `make test unit`.
"""


@dataclass
class Mutant:
    set: str
    name: str
    breaks: str
    file: str
    old: str
    new: str
    tests: list[str] = field(default_factory=list)  # 单元测试
    suite: str = ""                                 # 或者一个 e2e suite

    @property
    def id(self) -> str:
        return f"{self.set}/{self.name}"

    @property
    def e2e(self) -> bool:
        return bool(self.suite)


@dataclass
class Result:
    status: str
    detail: str


def die(msg: str, code: Exit = Exit.FAIL) -> NoReturn:
    print(f"mutate.py: {msg}", file=sys.stderr)
    sys.exit(code)


def load_sets() -> dict[str, list[Mutant]]:
    sets: dict[str, list[Mutant]] = {}
    for path in sorted(SETS_DIR.glob("*.py")):
        name = path.stem
        entries = runpy.run_path(str(path)).get("MUTANTS")
        if not isinstance(entries, list):
            die(f"{path}: MUTANTS must be a list")
        seen: set[str] = set()
        for e in entries:
            try:
                m = Mutant(set=name, **e)
            except TypeError as err:
                die(f"{path}: bad mutant entry {e.get('name', '?')}: {err}")
            if m.name in seen:
                die(f"{path}: duplicate mutant name {m.name}")
            if bool(m.tests) == bool(m.suite):
                die(f"{path}: mutant {m.name} needs exactly one of tests= "
                    f"(unit tests) or suite= (an e2e suite)")
            if m.suite and not (ROOT / "tests" / "e2e" / f"{m.suite}.sh").is_file():
                die(f"{path}: mutant {m.name}: no suite tests/e2e/{m.suite}.sh")
            seen.add(m.name)
            sets.setdefault(name, []).append(m)
    return sets


def select(sets: dict[str, list[Mutant]], wanted: list[str]) -> list[Mutant]:
    if not wanted:
        return [m for ms in sets.values() for m in ms]
    out: list[Mutant] = []
    for w in wanted:
        if w in sets:
            out += sets[w]
            continue
        hit = [m for ms in sets.values() for m in ms if m.id == w]
        if not hit:
            die(f"unknown set or mutant: {w} (see --list)", Exit.USAGE)
        out += hit
    # 同一个 mutant 写了两次只跑一次
    return list({m.id: m for m in out}.values())


def make_tree(scratch: Path, label: str, e2e: bool = False) -> Path:
    tree = scratch / label
    for rel in E2E_COPY_PATHS if e2e else COPY_PATHS:
        src = ROOT / rel
        dst = tree / rel
        if src.is_dir():
            shutil.copytree(src, dst)
        else:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
    for rel in E2E_COPY_FILES if e2e else []:
        if (ROOT / rel).is_file():
            (tree / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / rel, tree / rel)
    return tree


def check_anchor(m: Mutant, tree: Path) -> Result | None:
    """Apply the replacement in tree; return a STALE result if it cannot."""
    path = tree / m.file
    if not path.is_file():
        return Result("STALE", f"{m.file} does not exist")
    text = path.read_text()
    count = text.count(m.old)
    if count != 1:
        return Result("STALE", f"old text found {count} times in {m.file}")
    path.write_text(text.replace(m.old, m.new))
    return None


def anchor_only(m: Mutant) -> Result | None:
    """STALE result for an e2e mutant that is not run, None if its text matches."""
    path = ROOT / m.file
    if not path.is_file():
        return Result("STALE", f"{m.file} does not exist")
    count = path.read_text().count(m.old)
    if count != 1:
        return Result("STALE", f"old text found {count} times in {m.file}")
    return None


def e2e_env() -> dict[str, str]:
    # 副本里没有 guest-kernel/，lib.sh 的默认值找不到内核，这里按同样的
    # 规则指到工作区里的产物
    env = dict(os.environ)
    kdir = ROOT / "guest-kernel"
    for var, rel in (("VMLINUX", "vmlinux"),
                     ("BZIMAGE", "arch/x86/boot/bzImage")):
        if not env.get(var) and (kdir / rel).is_file():
            env[var] = str(kdir / rel)
    return env


def build_vmm(tree: Path) -> tuple[int, str]:
    b = subprocess.run(["make", "-s", "build/vmm"], cwd=tree,
                       capture_output=True, text=True)
    return b.returncode, b.stdout + b.stderr


def run_suite(tree: Path, suite: str) -> tuple[int, str]:
    # guest 串口上可能出现任意字节（例如复位瞬间截断的转义序列），按
    # UTF-8 严格解码会让整个 mutate.py 抛异常退出
    r = subprocess.run(["bash", str(tree / "tests" / "e2e" / f"{suite}.sh")],
                       cwd=tree, capture_output=True, text=True,
                       errors="replace", env=e2e_env())
    return r.returncode, ANSI.sub("", r.stdout + r.stderr)


def build_and_run(tree: Path, test: str) -> tuple[str, int, str]:
    """Return (phase, returncode, output); phase is 'build' or 'run'."""
    b = subprocess.run(["make", "-s", f"build/tests/{test}"], cwd=tree,
                       capture_output=True, text=True)
    if b.returncode:
        return "build", b.returncode, b.stdout + b.stderr
    env = dict(os.environ)
    env.setdefault("ASAN_OPTIONS", "detect_leaks=1:print_stacktrace=1")
    env.setdefault("UBSAN_OPTIONS", "print_stacktrace=1")
    r = subprocess.run([str(tree / "build" / "tests" / test)], cwd=tree,
                       capture_output=True, text=True, env=env)
    return "run", r.returncode, r.stdout + r.stderr


def first_lines(text: str, n: int) -> str:
    lines = [l.strip() for l in text.strip().splitlines() if l.strip()]
    return " | ".join(lines[:n])


def run_e2e_mutant(m: Mutant, scratch: Path,
                   suite_lock: threading.Lock) -> Result:
    tree = make_tree(scratch, m.id.replace("/", "__"), e2e=True)
    stale = check_anchor(m, tree)
    if stale:
        return stale
    rc, out = build_vmm(tree)
    if rc:
        return Result("BUILD-FAIL", f"build/vmm: {first_lines(out, 3)}")
    with suite_lock:
        rc, out = run_suite(tree, m.suite)
    summ = SUMMARY_LINE.findall(out)
    if not summ:
        if rc == 0:
            return Result("ESCAPED", f"{m.suite} exited 0 without a summary")
        return Result("BAD-RED", f"{m.suite}: exit {rc} without a summary, "
                      f"{first_lines(out[-2000:], 2)}")
    _, n_fail, _ = (int(x) for x in summ[-1])
    if rc and n_fail:
        cases = FAILED_CASE.findall(out[out.rfind("  PASS "):])
        return Result("CAUGHT", f"{m.suite}: {n_fail} failed"
                      + (f" (first: {cases[0]})" if cases else ""))
    if rc:
        return Result("BAD-RED", f"{m.suite}: exit {rc} with FAIL 0")
    return Result("ESCAPED", f"every case of {m.suite} passed")


def run_mutant(m: Mutant, scratch: Path) -> Result:
    tree = make_tree(scratch, m.id.replace("/", "__"))
    stale = check_anchor(m, tree)
    if stale:
        return stale

    caught: list[str] = []
    bad: list[str] = []
    for t in m.tests:
        phase, rc, out = build_and_run(tree, t)
        if phase == "build":
            return Result("BUILD-FAIL", f"{t}: {first_lines(out, 3)}")
        if rc == 0:
            continue
        if FAIL_LINE.search(out):
            sites = [f"{f} {msg}" for f, msg in SITE_LINE.findall(out)]
            caught.append(f"{t}: {len(FAIL_LINE.findall(out))} failed"
                          + (f" (first: {sites[0]})" if sites else ""))
        else:
            bad.append(f"{t}: exit {rc}, {first_lines(out, 2)}")
    if caught:
        return Result("CAUGHT", "; ".join(caught))
    if bad:
        return Result("BAD-RED", "; ".join(bad))
    return Result("ESCAPED", "all of " + ", ".join(m.tests) + " passed")


def baseline(tests: list[str], suites: list[str], scratch: Path) -> None:
    if tests:
        tree = make_tree(scratch, "baseline")
        for t in sorted(set(tests)):
            phase, rc, out = build_and_run(tree, t)
            if rc:
                die(f"baseline {t} is not green ({phase} exit {rc}); "
                    f"fix it before judging mutants:\n{first_lines(out, 10)}")
    if suites:
        tree = make_tree(scratch, "baseline-e2e", e2e=True)
        rc, out = build_vmm(tree)
        if rc:
            die(f"baseline build/vmm failed:\n{first_lines(out, 10)}")
        for s in sorted(set(suites)):
            rc, out = run_suite(tree, s)
            summ = SUMMARY_LINE.findall(out)
            if rc or not summ or summ[-1][1:] != ("0", "0"):
                got = "PASS %s  FAIL %s  SKIP %s" % summ[-1] if summ else "no summary"
                die(f"baseline suite {s} is not green (exit {rc}, {got}); "
                    f"every case must pass, none may SKIP, before judging "
                    f"mutants:\n{first_lines(out[-3000:], 12)}")


def main() -> Exit:
    ap = argparse.ArgumentParser(
        description=__doc__,
        epilog=HELP_EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("targets", nargs="*", metavar="SET[/MUTANT]",
                    help="sets or single mutants to run (default: all)")
    ap.add_argument("--list", action="store_true",
                    help="list mutants as <set>/<name>\\t<tests>\\t<breaks> and exit")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1,
                    help="mutants built in parallel (default: CPU count)")
    ap.add_argument("--keep", action="store_true",
                    help="keep the scratch trees and print their location")
    ap.add_argument("--e2e", action="store_true",
                    help="also run e2e mutants when no target is named")
    args = ap.parse_args()
    if args.jobs < 1:
        die("--jobs must be at least 1", Exit.USAGE)

    sets = load_sets()
    if not sets:
        die(f"no mutant sets in {SETS_DIR}")
    mutants = select(sets, args.targets)

    if args.list:
        for m in mutants:
            what = f"e2e:{m.suite}" if m.e2e else ",".join(m.tests)
            print(f"{m.id}\t{what}\t{m.breaks}")
        return Exit.OK

    # 没点名又没带 --e2e 时 e2e mutant 只查 old 文字，过期的照样报 STALE
    run_e2e = args.e2e or bool(args.targets)
    unrun = [m for m in mutants if m.e2e and not run_e2e]
    stale = {m.id: r for m in unrun if (r := anchor_only(m))}
    unrun_ids = {m.id for m in unrun}
    # 输出里留下要跑的和过期的，按清单顺序
    mutants = [m for m in mutants if m.id not in unrun_ids or m.id in stale]
    to_run = [m for m in mutants if m.id not in stale]
    scratch = Path(tempfile.mkdtemp(prefix="mini-vmm-mutate-"))
    suite_lock = threading.Lock()
    try:
        baseline([t for m in to_run for t in m.tests],
                 [m.suite for m in to_run if m.e2e], scratch)
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            futures = {m.id: pool.submit(run_e2e_mutant, m, scratch, suite_lock)
                       if m.e2e else pool.submit(run_mutant, m, scratch)
                       for m in to_run}
            # 按清单顺序输出，结果稳定，方便和 --list 对照
            results = [stale[m.id] if m.id in stale else futures[m.id].result()
                       for m in mutants]
    finally:
        if args.keep:
            print(f"scratch trees kept in {scratch}", file=sys.stderr)
        else:
            shutil.rmtree(scratch, ignore_errors=True)

    for m, r in zip(mutants, results):
        print(f"{r.status}\t{m.id}\t{m.breaks}\t{r.detail}")
    n_bad = sum(r.status != "CAUGHT" for r in results)
    sys.stdout.flush()
    note = ""
    if len(unrun) > len(stale):
        note = (f"; {len(unrun) - len(stale)} e2e mutants not run, "
                f"old text checked (name their set or pass --e2e to run them)")
    print(f"{len(results) - n_bad}/{len(results)} caught{note}", file=sys.stderr)
    return Exit.FAIL if n_bad else Exit.OK


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        die("interrupted")
