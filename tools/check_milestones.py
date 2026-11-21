#!/usr/bin/env python3
"""tools/check_milestones.py — 校验 MILESTONES.md 的依赖关系并维护层级表

MILESTONES.md 里每个里程碑写成：

    ### <id> — <标题>
    依赖: a, b · 参考: ...
    - [x] / - [ ] 验收项

本脚本检查三件事：
  1. 每个依赖都是已定义的 ID，且排在当前里程碑之前。只要这一条成立，
     文档顺序就是合法的拓扑序，依赖图里也不可能有环（A 在 B 前、B 又在
     A 前不能同时成立）。
  2. 排在闸门里程碑（GATE_ID）之后的里程碑隐含依赖它，这是文档「约定」
     一节写明的规则，计算层级时自动补上。
  3. 「层级总览」表与按依赖算出的层级一致。层级 = 所有依赖的最大层级 + 1，
     无依赖为 0；所有勾选项都勾上的里程碑在表里标 ✓。

层级表是派生数据：增删依赖会连带改变下游一串里程碑的层级，手工维护容易
漏改，所以由 --write 重新生成，--check 挂在 make test 里防止它和依赖脱节。
"""

from __future__ import annotations

import argparse
import difflib
import re
import sys
from dataclasses import dataclass, field
from enum import IntEnum
from pathlib import Path
from typing import NoReturn


class Exit(IntEnum):
    OK = 0
    INCONSISTENT = 1   # 依赖有错，或层级表与依赖不一致
    USAGE = 2
    INTERRUPTED = 130  # 沿用 shell 对 SIGINT 的约定 128+2


# 闸门里程碑：排在它后面的里程碑都隐含依赖它（见 MILESTONES.md「约定」）
GATE_ID = "arch-distro"

DEFAULT_PATH = Path(__file__).resolve().parent.parent / "MILESTONES.md"

HEADING_RE = re.compile(r"^### ([a-z0-9][a-z0-9-]*) — ")
DEPS_RE = re.compile(r"^依赖: ([^·]+)")
TABLE_HEADER = "| 层级 | 里程碑 |\n|---|---|\n"
DONE_MARK = " ✓"


@dataclass
class Milestone:
    ident: str
    line: int
    deps: list[str] | None = None      # None 表示缺少「依赖:」行
    checked: int = 0
    unchecked: int = 0
    implicit: list[str] = field(default_factory=list)

    @property
    def done(self) -> bool:
        return self.checked > 0 and self.unchecked == 0


def die(msg: str, code: Exit = Exit.USAGE) -> NoReturn:
    print(f"check_milestones: {msg}", file=sys.stderr)
    if code == Exit.USAGE:
        print("Try 'tools/check_milestones.py --help'.", file=sys.stderr)
    sys.exit(code)


def parse(text: str) -> tuple[list[Milestone], list[str]]:
    milestones: list[Milestone] = []
    problems: list[str] = []
    cur: Milestone | None = None
    for no, line in enumerate(text.splitlines(), 1):
        if line.startswith("## "):          # 进入新章节，后面的勾选项不再归属上一个里程碑
            cur = None
            continue
        m = HEADING_RE.match(line)
        if m:
            cur = Milestone(m.group(1), no)
            milestones.append(cur)
            continue
        if line.startswith("### "):
            problems.append(f"line {no}: heading does not match '### <id> — <title>': {line}")
            cur = None
            continue
        if cur is None:
            continue
        m = DEPS_RE.match(line)
        if m and cur.deps is None:
            raw = m.group(1).strip()
            cur.deps = [] if raw == "无" else [d.strip() for d in raw.split(",") if d.strip()]
        elif line.startswith("- [x]"):
            cur.checked += 1
        elif line.startswith("- [ ]"):
            cur.unchecked += 1
    return milestones, problems


def validate(milestones: list[Milestone]) -> list[str]:
    problems: list[str] = []
    seen: dict[str, Milestone] = {}
    known = {m.ident for m in milestones}
    if GATE_ID not in known:
        problems.append(f"gate milestone '{GATE_ID}' is not defined")
    past_gate = False
    for m in milestones:
        if m.ident in seen:
            problems.append(f"{m.ident} (line {m.line}): duplicate id, first defined at line {seen[m.ident].line}")
        if m.deps is None:
            problems.append(f"{m.ident} (line {m.line}): missing '依赖:' line")
            m.deps = []
        for d in m.deps:
            if d == m.ident:
                problems.append(f"{m.ident} (line {m.line}): depends on itself")
            elif d not in known:
                problems.append(f"{m.ident} (line {m.line}): unknown dependency '{d}'")
            elif d not in seen:
                problems.append(f"{m.ident} (line {m.line}): dependency '{d}' is defined later; move it above or fix the dependency")
        if m.checked + m.unchecked == 0:
            problems.append(f"{m.ident} (line {m.line}): no acceptance checkbox")
        if past_gate and GATE_ID not in m.deps:
            m.implicit.append(GATE_ID)
        if m.ident == GATE_ID:
            past_gate = True
        seen.setdefault(m.ident, m)
    return problems


def layers(milestones: list[Milestone]) -> dict[str, int]:
    # 调用前 validate 已保证依赖都排在前面，所以按文档顺序一遍就能算完
    result: dict[str, int] = {}
    for m in milestones:
        deps = (m.deps or []) + m.implicit
        result[m.ident] = 1 + max((result[d] for d in deps if d in result), default=-1)
    return result


def render_table(milestones: list[Milestone], layer: dict[str, int]) -> str:
    rows: dict[int, list[str]] = {}
    for m in milestones:
        rows.setdefault(layer[m.ident], []).append(m.ident + (DONE_MARK if m.done else ""))
    body = "".join(f"| {lv} | {' · '.join(names)} |\n" for lv, names in sorted(rows.items()))
    return TABLE_HEADER + body


def find_table(text: str) -> tuple[int, int]:
    start = text.find(TABLE_HEADER)
    if start < 0:
        die("layer table not found: expected a table starting with '| 层级 | 里程碑 |'", Exit.INCONSISTENT)
    end = start + len(TABLE_HEADER)
    while text.startswith("| ", end):
        end = text.index("\n", end) + 1
    return start, end


def main() -> Exit:
    ap = argparse.ArgumentParser(
        prog="tools/check_milestones.py",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description="Validate milestone dependencies in MILESTONES.md and keep its layer table in sync.",
        epilog=f"""\
EXAMPLE:
  tools/check_milestones.py              check dependencies and the layer table
  tools/check_milestones.py --write      regenerate the layer table in place
  tools/check_milestones.py --print      print the computed layer table to stdout

NOTE:
  Default file: MILESTONES.md at the repository root; pass a path to check another copy.
  A milestone's layer is 1 + the highest layer among its dependencies (0 if none).
  Milestones after '{GATE_ID}' implicitly depend on it.
  A milestone whose checkboxes are all ticked is marked with '{DONE_MARK.strip()}' in the table.
  --write refuses to touch the file while dependency errors remain.
  Exit codes: 0 consistent, 1 dependency errors or stale table, 2 usage error,
              130 interrupted.
""")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true", help="rewrite the layer table in place")
    mode.add_argument("--print", action="store_true", help="print the computed layer table and exit")
    ap.add_argument("path", nargs="?", type=Path, default=DEFAULT_PATH, help="milestone file (default: %(default)s)")
    args = ap.parse_args()

    if not args.path.is_file():
        die(f"no such file: {args.path}")
    text = args.path.read_text(encoding="utf-8")

    milestones, problems = parse(text)
    if not milestones:
        die(f"no milestones found in {args.path}", Exit.INCONSISTENT)
    problems += validate(milestones)
    if problems:
        print(f"{len(problems)} dependency problem(s) in {args.path}:", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return Exit.INCONSISTENT

    table = render_table(milestones, layers(milestones))
    if args.print:
        sys.stdout.write(table)
        return Exit.OK

    start, end = find_table(text)
    current = text[start:end]
    if current == table:
        print(f"OK: {len(milestones)} milestones, layer table is up to date")
        return Exit.OK
    if args.write:
        args.path.write_text(text[:start] + table + text[end:], encoding="utf-8")
        print(f"updated layer table in {args.path}")
        return Exit.OK

    print(f"layer table in {args.path} is stale; run 'tools/check_milestones.py --write'. Diff (current -> expected):", file=sys.stderr)
    sys.stderr.writelines(difflib.unified_diff(
        current.splitlines(keepends=True), table.splitlines(keepends=True), "current", "expected"))
    return Exit.INCONSISTENT


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("check_milestones: interrupted", file=sys.stderr)
        sys.exit(Exit.INTERRUPTED)
