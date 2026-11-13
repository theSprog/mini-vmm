#!/usr/bin/env python3
"""Source-level mutation runner for the C unit tests.

Each mutant in tests/mutants/<set>.py replaces one exact piece of source text
with a deliberately wrong version. For every mutant this script copies the
build inputs into a scratch tree, applies the replacement, builds the listed
unit tests with the Makefile's own rules and runs them. A mutant is CAUGHT
when at least one listed test exits non-zero with an assertion FAIL line.
"""

# 为什么要有它：tests/unit/*_mutants.c 的接口层 mutant 只能整份替换对外
# 函数，而 MSI 这类逻辑的错误点在 static 函数或一条条件表达式里，只能改
# 源码文字。按文字（不是行号）定位，源码变了让 old 不再唯一出现时报
# STALE，suite 失败，不会悄悄变成“没跑”。
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
from dataclasses import dataclass
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
FAIL_LINE = re.compile(r"^  FAIL ", re.M)
SITE_LINE = re.compile(r"^\s+(\S+\.c:\d+): (.*)$", re.M)

HELP_EPILOG = """\
EXAMPLE
  tests/mutate.py                          run every mutant of every set
  tests/mutate.py pci-msi                  run one set
  tests/mutate.py pci-msi/ahci-no-msi-cap  run one mutant
  tests/mutate.py --list                   list sets, mutants and what each breaks
  tests/mutate.py --keep pci-msi/ahci-no-msi-cap
                                           keep the scratch tree to inspect the patch

NOTE
  Output is one tab-separated line per mutant, in set order:
    <STATUS>\\t<set>/<name>\\t<breaks>\\t<detail>
  STATUS is CAUGHT, ESCAPED (all listed tests passed), STALE (the old text
  is not found exactly once in the source), BUILD-FAIL (the mutant does not
  compile) or BAD-RED (a test failed without an assertion FAIL line).
  Only CAUGHT counts as success; any other status makes the exit code 1.
  A baseline build of the unpatched tree runs first; if any listed test is
  not green there, the script stops with exit code 1.
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
    tests: list[str]

    @property
    def id(self) -> str:
        return f"{self.set}/{self.name}"


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


def make_tree(scratch: Path, label: str) -> Path:
    tree = scratch / label
    for rel in COPY_PATHS:
        src = ROOT / rel
        dst = tree / rel
        if src.is_dir():
            shutil.copytree(src, dst)
        else:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
    return tree


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


def run_mutant(m: Mutant, scratch: Path) -> Result:
    tree = make_tree(scratch, m.id.replace("/", "__"))
    path = tree / m.file
    if not path.is_file():
        return Result("STALE", f"{m.file} does not exist")
    text = path.read_text()
    count = text.count(m.old)
    if count != 1:
        return Result("STALE", f"old text found {count} times in {m.file}")
    path.write_text(text.replace(m.old, m.new))

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


def baseline(tests: list[str], scratch: Path) -> None:
    tree = make_tree(scratch, "baseline")
    for t in sorted(set(tests)):
        phase, rc, out = build_and_run(tree, t)
        if rc:
            die(f"baseline {t} is not green ({phase} exit {rc}); "
                f"fix it before judging mutants:\n{first_lines(out, 10)}")


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
    args = ap.parse_args()
    if args.jobs < 1:
        die("--jobs must be at least 1", Exit.USAGE)

    sets = load_sets()
    if not sets:
        die(f"no mutant sets in {SETS_DIR}")
    mutants = select(sets, args.targets)

    if args.list:
        for m in mutants:
            print(f"{m.id}\t{','.join(m.tests)}\t{m.breaks}")
        return Exit.OK

    scratch = Path(tempfile.mkdtemp(prefix="mini-vmm-mutate-"))
    try:
        baseline([t for m in mutants for t in m.tests], scratch)
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            futures = [pool.submit(run_mutant, m, scratch) for m in mutants]
            # 按清单顺序输出，结果稳定，方便和 --list 对照
            results = [f.result() for f in futures]
    finally:
        if args.keep:
            print(f"scratch trees kept in {scratch}", file=sys.stderr)
        else:
            shutil.rmtree(scratch, ignore_errors=True)

    for m, r in zip(mutants, results):
        print(f"{r.status}\t{m.id}\t{m.breaks}\t{r.detail}")
    n_bad = sum(r.status != "CAUGHT" for r in results)
    sys.stdout.flush()
    print(f"{len(results) - n_bad}/{len(results)} caught", file=sys.stderr)
    return Exit.FAIL if n_bad else Exit.OK


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        die("interrupted")
