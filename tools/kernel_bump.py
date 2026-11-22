#!/usr/bin/env python3
"""tools/kernel_bump.py — 换 guest 内核（或换 host KVM 参照版本）时核对文档和测试里对内核的引用

blog、learning-note 和源码注释里有大量对内核源码的引用（`arch/x86/kernel/tsc.c:1234`、
附录里按版本分组的函数名清单），还有大段 guest 日志原文和 e2e 里匹配内核消息的
expect_not。换内核版本后它们会悄悄过期：行号漂到别的函数里、函数被改名、日志措辞变了，
文档照样能读，测试照样通过，没有任何信号。这个脚本把可以机械判断的部分查出来：

  refs        引用的文件、行号、版本分组里的标识符在新树里是否还成立
  remap       把仍指向旧树的 file:line 换算到新树（默认只打印，--apply 才改文件）
  logs        文档里引用的日志原文能否在新抓的日志里找到
  expect-not  e2e 里的 expect_not 字符串能否在内核源码里找到出处

判断一条行号引用是否还成立靠“上下文标识符”：引用所在分句（Markdown 不硬折行，按 。；| 切
分句；C 注释取前后两行）里写出的标识符，至少有一个落在被引行前后 WINDOW 行之内，或者就是
被引行所在函数的名字。只看行号是否越界不够，因为行号漂移后几乎总还落在文件里，只是指向了
别的代码。窗口放宽是为了少报误报，代价是漂移不到 WINDOW 行的错引 refs 看不出来，所以换内核
时先用 remap 按原文从旧树换算到新树，refs 只用来兜底。分句里没有任何能在目标文件里找到的
标识符时无法判断：refs 不报，remap 只给建议不改写。

remap 只改写“旧树里成立、新树里不成立、换算结果在新树里成立”的引用，所以重复运行不会
二次换算；换算按被引行的原文定位（唯一的原文、同名函数里的原文、上方最近的唯一行作锚点），
原文变了时给出的模糊结果只作为建议，不自动写入。

引用分 guest 和 host 两侧，由路径判断（HOST_PREFIXES）：mini-vmm 的文档同时引用 guest
内核和 host KVM，两者钉在不同版本上，必须各自对照各自的树。KVM 目录以外的 host 引用（例如
host 上 /dev/null 的实现）会被当成 guest 侧，在 guest 树里报 MISSING，需要人看一眼。
只给文件名的引用（`tsc.c:15`）在树里按文件名找候选，再用上下文标识符挑；仓库自己也有同名
文件（`acpi.c`）且上下文能在仓库文件里对上时，算作仓库内引用，不检查。
"""

from __future__ import annotations

import argparse
import bisect
import difflib
import os
import re
import subprocess
import sys
import tempfile
from collections import defaultdict
from dataclasses import dataclass, field
from enum import IntEnum
from pathlib import Path
from typing import Iterable, NoReturn


class Exit(IntEnum):
    OK = 0
    PROBLEMS = 1       # 查出了需要人看的引用
    USAGE = 2
    INTERRUPTED = 130  # 沿用 shell 对 SIGINT 的约定 128+2


ROOT = Path(__file__).resolve().parent.parent
WINDOW = 40          # 上下文标识符离被引行的最大距离
MAX_REPORT = 200     # 超过这么多条只给统计，避免淹没调用方
GREP_TIMEOUT = 600   # grep 扫整棵内核树的兜底超时，正常几十秒

# host 侧：KVM 本体和它的 UAPI。kvm_para.h 是 guest 也用的 PV 接口，两侧都算。
HOST_PREFIXES = ("arch/x86/kvm/", "virt/kvm/", "include/linux/kvm", "include/uapi/linux/kvm",
                 "arch/x86/include/asm/kvm", "arch/x86/include/uapi/asm/kvm",
                 "arch/x86/include/asm/svm.h", "arch/x86/include/asm/vmx.h")
SHARED_MARK = "kvm_para"
SOURCE_EXTS = (".c", ".h", ".S", ".rst", ".txt")
REPO_DIRS = ("src", "include", "tests", "tools")

# 引用：可选目录前缀 + 文件名 + 可选 :行[-行]。前面不能紧贴路径字符，避免从更长的词里截出
REF_RE = re.compile(r"(?<![\w/.+-])((?:[\w+-]+/)+[\w+-]+\.(?:c|h|S|rst|txt)|[\w+-]+\.[chS])(?::(\d+)(?:-(\d+))?)?(?![\w/])")
# 上下文标识符：反引号里的标识符，或正文里紧跟括号的函数名
TICK_RE = re.compile(r"`([^`]+)`")
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]{3,}")
WORD_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
CALL_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]{3,})\(")
# 附录里按版本分组的引用清单的标题，例如 **Linux 7.4-rc2（guest）**、**Linux 6.6（host KVM）**
BLOCK_HEAD_RE = re.compile(r"^\*\*[^*]*\bLinux (\d+\.\d+(?:\.\d+)?(?:-rc\d+)?)[^*]*\*\*\s*$")
EXPECT_NOT_RE = re.compile(r"""^\s*expect_not\s+(?:"((?:[^"\\]|\\.)*)"|'([^']*)')""")
CLAUSE_END = ("。", "；", "|")
FUNC_DEF_RE = re.compile(r"^[A-Za-z_].*?\b([A-Za-z_]\w*)\s*\(")


def die(msg: str, code: Exit = Exit.USAGE) -> NoReturn:
    hint = " (see --help)" if code == Exit.USAGE else ""
    print(f"kernel_bump: {msg}{hint}", file=sys.stderr)
    sys.exit(code)


class Parser(argparse.ArgumentParser):
    """argparse 默认的报错格式不带 --help 提示，统一走 die()。"""

    def error(self, message: str) -> NoReturn:
        die(f"{self.prog.split()[-1]}: {message}")


# ---------------------------------------------------------------- 内核树

class Tree:
    def __init__(self, path: str, what: str) -> None:
        p = Path(path).expanduser()
        mk = p / "Makefile"
        if not mk.is_file() or not (p / "arch" / "x86").is_dir():
            die(f"{what} {path} does not look like a Linux source tree (no Makefile or arch/x86)")
        self.path = p
        self.version = self._version(mk)
        self._files: list[str] | None = None
        self._by_name: dict[str, list[str]] | None = None

    @staticmethod
    def _version(mk: Path) -> str:
        v: dict[str, str] = {}
        for line in mk.read_text(errors="replace").splitlines()[:10]:
            m = re.match(r"^(VERSION|PATCHLEVEL|SUBLEVEL|EXTRAVERSION)\s*=\s*(\S*)", line)
            if m:
                v[m.group(1)] = m.group(2)
        s = f"{v.get('VERSION', '?')}.{v.get('PATCHLEVEL', '?')}"
        if v.get("SUBLEVEL", "0") not in ("", "0"):
            s += f".{v['SUBLEVEL']}"
        return s + v.get("EXTRAVERSION", "")

    @staticmethod
    def _nested_tree(d: Path) -> bool:
        return (d / "Makefile").is_file() and (d / "arch").is_dir() and (d / "include").is_dir()

    def files(self) -> list[str]:
        if self._files is None:
            out: list[str] = []
            for root, dirs, names in os.walk(self.path):
                # 跳过隐藏目录和树里嵌着的整棵源码副本（打包出的 linux-headers 之类），
                # 否则每个头文件都有两个同名候选，按文件名找时永远定不下来
                dirs[:] = [d for d in dirs if not d.startswith(".") and not self._nested_tree(Path(root, d))]
                rel = os.path.relpath(root, self.path)
                for n in names:
                    if n.endswith(SOURCE_EXTS):
                        out.append(n if rel == "." else f"{rel}/{n}")
            self._files = out
        return self._files

    def resolve(self, ref_path: str) -> list[str]:
        """树里路径等于 ref_path 或以 /ref_path 结尾的文件。"""
        if self._by_name is None:
            self._by_name = defaultdict(list)
            for f in self.files():
                self._by_name[f.rsplit("/", 1)[-1]].append(f)
        name = ref_path.rsplit("/", 1)[-1]
        hits = [f for f in self._by_name.get(name, []) if f == ref_path or f.endswith("/" + ref_path)]
        return [ref_path] if ref_path in hits else hits

    def src(self, rel: str) -> Src | None:
        return load(self.path / rel)

    def grep_fixed(self, patterns: list[str], words: bool, only_matching: bool) -> list[str]:
        """一次 grep 扫全树（只看 C 源码和 Kconfig），返回输出行（-n 时形如 ./path:line:text）。"""
        if not patterns:
            return []
        with tempfile.NamedTemporaryFile("w", suffix=".pat", delete=False) as f:
            f.write("\n".join(patterns) + "\n")
            patfile = f.name
        flags = ["-I", "-F", "-f", patfile]
        flags += ["-w"] if words else []
        flags += ["-o", "-h"] if only_matching else ["-n"]
        # 用 GNU grep 而不是 git grep：git grep -F 带几百个模式时不走多模式匹配，扫一遍内核
        # 树要十几分钟；GNU grep 的 Aho-Corasick 几十秒，时间主要花在读文件上
        cmd = ["grep", "-r", *flags, "--include=*.c", "--include=*.h", "--include=*.S",
               "--include=Kconfig*", "."]
        try:
            r = subprocess.run(cmd, cwd=self.path, capture_output=True, text=True,
                               errors="replace", timeout=GREP_TIMEOUT)
        except subprocess.TimeoutExpired:
            die(f"grep over {self.path} did not finish in {GREP_TIMEOUT}s", Exit.PROBLEMS)
        finally:
            os.unlink(patfile)
        if r.returncode not in (0, 1):
            die(f"{cmd[0]} failed in {self.path}: {r.stderr.strip()}", Exit.PROBLEMS)
        return r.stdout.splitlines()


def side_of(rel: str) -> str:
    if SHARED_MARK in rel:
        return "both"
    return "host" if rel.startswith(HOST_PREFIXES) else "guest"


def on_side(rel: str, side: str) -> bool:
    s = side_of(rel)
    return s == "both" or s == side


def version_matches(header: str, tree: str) -> bool:
    # 标题写 6.6 时 6.6.91 的树也算对上；写 7.4-rc2 时必须完全一致
    return tree == header or (tree.startswith(header + ".") and "-rc" not in header)


# ---------------------------------------------------------------- 文档里的引用

@dataclass
class Ref:
    doc: str
    line: int          # 文档行号
    start: int         # 在该行里的列，remap 改写用
    end: int
    path: str
    first: int | None  # 被引的起止行
    last: int | None
    context: set[str]

    def text(self) -> str:
        s = self.path
        if self.first is not None:
            s += f":{self.first}" + (f"-{self.last}" if self.last is not None else "")
        return s


def context_idents(text: str) -> set[str]:
    # 先抹掉路径本身，否则 `arch/x86/kernel/rtc.c` 会贡献 arch、kernel 这种处处都有的词
    text = REF_RE.sub(" ", text)
    ids: set[str] = set()
    for tok in TICK_RE.findall(text):
        ids.update(IDENT_RE.findall(tok))
    ids.update(CALL_RE.findall(text))
    return ids


def read_doc(doc: str) -> list[str]:
    try:
        return Path(doc).read_text(errors="replace").split("\n")
    except OSError as e:
        die(f"cannot read {doc}: {e.strerror}", Exit.PROBLEMS)


def collect_refs(doc: str, lines: list[str]) -> list[Ref]:
    refs: list[Ref] = []
    is_md = doc.endswith(".md")
    for i, line in enumerate(lines):
        if line.lstrip().startswith("#include"):
            continue
        matches = list(REF_RE.finditer(line))
        if not matches:
            continue
        for m in matches:
            # Markdown 一行就是一段，一段里常引好几个文件，只取引用所在的分句（表格取所在单元格）；
            # C 注释按行折过，取前后两行
            if is_md:
                lo = max(line.rfind(c, 0, m.start()) for c in CLAUSE_END) + 1
                his = [h for c in CLAUSE_END if (h := line.find(c, m.end())) >= 0]
                ctx = context_idents(line[lo:min(his, default=len(line))])
            else:
                ctx_text = REF_RE.sub(" ", "\n".join(lines[max(0, i - 2):i + 3]))
                ctx = context_idents(ctx_text) | set(IDENT_RE.findall(ctx_text))
            path = m.group(1)
            if "/" in path and is_repo_path(path):
                continue   # 仓库自己的文件（含相对 src/、include/ 的写法，如 devices/acpi_pm.h）
            first = int(m.group(2)) if m.group(2) else None
            last = int(m.group(3)) if m.group(3) else None
            refs.append(Ref(doc, i + 1, m.start(), m.end(), path, first, last, ctx))
    return refs


_repo_by_name: dict[str, list[Path]] | None = None


def repo_files_named(name: str) -> list[Path]:
    global _repo_by_name
    if _repo_by_name is None:
        _repo_by_name = defaultdict(list)
        for d in REPO_DIRS:
            for p in (ROOT / d).rglob("*"):
                if p.is_file() and p.suffix in SOURCE_EXTS:
                    _repo_by_name[p.name].append(p)
    return _repo_by_name.get(name, [])


def is_repo_path(path: str) -> bool:
    if (ROOT / path).exists():
        return True
    return any(p.as_posix().endswith("/" + path) for p in repo_files_named(path.rsplit("/", 1)[-1]))


class Src:
    """一个源文件的行和标识符索引：标识符 -> 出现的行号（升序）。每条引用都要查上下文，
    逐次拼接全文再跑正则在几万行的文件上太慢。"""

    def __init__(self, text: str) -> None:
        self.lines = text.split("\n")
        self.idx: dict[str, list[int]] = defaultdict(list)
        for n, line in enumerate(self.lines, 1):
            for w in set(WORD_RE.findall(line)):
                self.idx[w].append(n)

    def __len__(self) -> int:
        return len(self.lines)


_src_cache: dict[Path, Src | None] = {}


def load(path: Path) -> Src | None:
    if path not in _src_cache:
        try:
            _src_cache[path] = Src(path.read_text(errors="replace"))
        except OSError:
            _src_cache[path] = None
    return _src_cache[path]


def near(src: Src, lineno: int, ids: Iterable[str]) -> bool:
    for i in ids:
        hits = src.idx.get(i, ())
        k = bisect.bisect_left(hits, lineno - WINDOW)
        if k < len(hits) and hits[k] <= lineno + WINDOW:
            return True
    return False


def valid(src: Src, lineno: int, ids: Iterable[str]) -> bool:
    """被引行前后 WINDOW 行内出现上下文标识符，或者它所在的函数就叫这个名字（长函数中段的引用）。"""
    ids = list(ids)
    if near(src, lineno, ids):
        return True
    enc = enclosing_func(src.lines, lineno)
    return enc is not None and enc[0] in ids


def present_ids(src: Src, ids: Iterable[str]) -> list[str]:
    return sorted(i for i in ids if i in src.idx)


def occurrences(src: Src, ident: str, limit: int = 3) -> list[int]:
    return src.idx.get(ident, [])[:limit]


@dataclass
class Resolution:
    """一条引用在某棵树里的归属。rel 为 None 时 reason 说明为什么定不下来。"""
    rel: str | None
    reason: str = ""
    skip: bool = False   # 属于另一侧或仓库内，不报告


def resolve(ref: Ref, tree: Tree, side: str) -> Resolution:
    cands = tree.resolve(ref.path)
    mine = [c for c in cands if on_side(c, side)]
    if "/" not in ref.path and (repo := repo_files_named(ref.path)):
        # 仓库里也有同名文件（acpi.c、serial8250.c）：树里没有候选，或者上下文对得上仓库文件，
        # 就是仓库内的引用；两边都对得上或都对不上时定不下来，也不报
        if not mine:
            return Resolution(None, skip=True)
        if ref.first is not None and any((rs := load(p)) and ref.first <= len(rs)
                                         and valid(rs, ref.first, ref.context) for p in repo):
            return Resolution(None, skip=True)
    if not cands:
        if "/" not in ref.path:
            return Resolution(None, "NONAME")
        # 另一侧的文件，或者首段不是内核顶层目录（docs/specs/、util/ 这类别的项目的路径）
        if side_of(ref.path) not in (side, "both") or not (tree.path / ref.path.split("/")[0]).is_dir():
            return Resolution(None, skip=True)
        return Resolution(None, "MISSING")
    if not mine:
        return Resolution(None, skip=True)
    # 两侧都有同名文件（i8254.h）时，只有上下文对得上本侧才算本侧的引用
    if len(mine) == 1 and len(cands) == 1:
        return Resolution(mine[0])
    if ref.first is not None:
        fit = [c for c in mine if (cs := tree.src(c)) and ref.first <= len(cs) and valid(cs, ref.first, ref.context)]
    else:
        fit = [c for c in mine if (cs := tree.src(c)) and present_ids(cs, ref.context)]
    if len(fit) == 1:
        return Resolution(fit[0])
    return Resolution(None, skip=True)


@dataclass
class Report:
    items: list[tuple[str, int, str, str]] = field(default_factory=list)   # doc, line, kind, detail

    def add(self, doc: str, line: int, kind: str, detail: str) -> None:
        self.items.append((doc, line, kind, detail))

    def emit(self) -> None:
        if len(self.items) <= MAX_REPORT:
            for doc, line, kind, detail in self.items:
                print(f"{doc}:{line}: {kind} {detail}")
            return
        counts: dict[tuple[str, str], int] = defaultdict(int)
        for doc, _, kind, _ in self.items:
            counts[(doc, kind)] += 1
        print(f"{len(self.items)} problems, over the {MAX_REPORT}-line limit; counts per file and kind "
              f"(rerun on fewer files to list them):")
        for (doc, kind), n in sorted(counts.items(), key=lambda kv: -kv[1])[:40]:
            print(f"  {n:5d} {kind:8s} {doc}")


# ---------------------------------------------------------------- refs

def cmd_refs(args: argparse.Namespace) -> Exit:
    tree = Tree(args.tree, "--tree")
    rep = Report()
    nrefs = 0
    block_ids: dict[str, list[tuple[str, int]]] = defaultdict(list)
    noname: dict[str, int] = defaultdict(int)
    for doc in args.files:
        lines = read_doc(doc)
        for ref in collect_refs(doc, lines):
            r = resolve(ref, tree, args.side)
            if r.skip:
                continue
            nrefs += 1
            if r.reason == "NONAME":
                noname[ref.path] += 1
                continue
            if r.rel is None:
                rep.add(doc, ref.line, r.reason, ref.path)
                continue
            if ref.first is None:
                continue
            ls = tree.src(r.rel)
            if ls is None:
                continue
            for n in (ref.first, ref.last):
                if n is not None and n > len(ls):
                    rep.add(doc, ref.line, "PASTEOF", f"{ref.text()} ({r.rel} has {len(ls)} lines)")
                    break
            else:
                have = present_ids(ls, ref.context)
                if have and not valid(ls, ref.first, have):
                    where = "; ".join(f"{i} at {','.join(map(str, occurrences(ls, i)))}" for i in have[:3])
                    rep.add(doc, ref.line, "DRIFT", f"{ref.text()} [{r.rel}] context not within "
                            f"{WINDOW} lines: {where}")
        if doc.endswith(".md"):
            collect_block_idents(doc, lines, args.side, tree.version, rep, block_ids)
    if block_ids:
        found = set(tree.grep_fixed(sorted(block_ids), words=True, only_matching=True))
        for ident, places in sorted(block_ids.items()):
            if ident not in found:
                for doc, line in places:
                    rep.add(doc, line, "IDENT", ident)
    rep.items.sort(key=lambda t: (t[0], t[1]))
    rep.emit()
    print(f"kernel_bump refs: {args.side} side, tree Linux {tree.version}: {nrefs} file references and "
          f"{len(block_ids)} block identifiers checked in {len(args.files)} files, "
          f"{len(rep.items)} problems", file=sys.stderr)
    if noname:
        names = " ".join(f"{n}({c})" if c > 1 else n for n, c in sorted(noname.items()))
        print(f"bare file names found in neither the tree nor this repository (other projects, the "
              f"other side, or removed upstream): {names}", file=sys.stderr)
    return Exit.PROBLEMS if rep.items else Exit.OK


def collect_block_idents(doc: str, lines: list[str], side: str, tree_version: str, rep: Report,
                         out: dict[str, list[tuple[str, int]]]) -> None:
    """附录里 **Linux X（guest|host KVM）** 下面列出的标识符，和标题里的版本号。"""
    i = 0
    while i < len(lines):
        m = BLOCK_HEAD_RE.match(lines[i])
        if not m:
            i += 1
            continue
        head_side = "host" if "host" in lines[i] else "guest"
        if head_side == side and not version_matches(m.group(1), tree_version):
            rep.add(doc, i + 1, "VERSION", f"header names Linux {m.group(1)}, tree is {tree_version}")
        i += 1
        while i < len(lines) and not lines[i].startswith(("**", "#")):
            if head_side == side:
                for tok in TICK_RE.findall(lines[i]):
                    if REF_RE.search(tok):
                        continue
                    for t in re.split(r"[^A-Za-z0-9_]+", tok):
                        # 结尾是下划线的是文中写的名字前缀（`E1000_ICR_*`），不是完整标识符
                        if (len(t) >= 4 and re.match(r"[A-Za-z_]", t) and ("_" in t or t.islower())
                                and not t.endswith("_")):
                            out[t.removeprefix("CONFIG_")].append((doc, i + 1))
            i += 1


# ---------------------------------------------------------------- remap

def enclosing_func(lines: list[str], lineno: int) -> tuple[str, int] | None:
    """被引行往上最近的函数定义：(函数名, 被引行相对定义行的偏移)。"""
    for k in range(lineno, max(0, lineno - 1000), -1):
        line = lines[k - 1]
        if line.startswith("}") and k != lineno:
            return None   # 往上先碰到上一个函数的结尾：被引行不在任何函数里
        m = FUNC_DEF_RE.match(line)
        if m and not line.rstrip().endswith(";") and not line.startswith(("return", "else", "if")):
            return m.group(1), lineno - k
    return None


def func_def_lines(lines: list[str], name: str) -> list[int]:
    pat = re.compile(rf"^[A-Za-z_].*\b{re.escape(name)}\s*\(")
    return [n + 1 for n, l in enumerate(lines) if pat.match(l) and not l.rstrip().endswith(";")]


def func_span(lines: list[str], start: int) -> int:
    """从定义行往下找到第一个顶格的 }，返回函数最后一行的行号。"""
    for k in range(start, min(len(lines), start + 3000)):
        if lines[k].startswith("}"):
            return k + 1
    return len(lines)


def solid(text: str) -> bool:
    """有分量的一行：够长且含标识符。`}`、`yet. */` 这种行即使唯一也只是巧合。"""
    return len(text) >= 12 and IDENT_RE.search(text) is not None


def map_line(old: list[str], new: list[str], lineno: int) -> tuple[int | None, str, bool]:
    """把旧文件第 lineno 行换算到新文件，返回 (新行号, 依据, 是否精确)。

    精确：被引行的原文在新文件里唯一；或者在同名函数里、在上方最近一行唯一原文（锚点）
    附近找到原文。模糊：原文在函数里变了，取函数内最相似且离预期偏移最近的一行，只作为
    建议。原文找不到也不像任何一行时返回 None，并在依据里给出函数的新位置。
    """
    text = old[lineno - 1].strip()
    hits = [n + 1 for n, l in enumerate(new) if text and l.strip() == text]
    if len(hits) == 1 and solid(text):
        return hits[0], "exact", True
    enc = enclosing_func(old, lineno)
    if enc:
        defs = func_def_lines(new, enc[0])
        spans = [(d, func_span(new, d)) for d in defs]
        if not spans:
            return None, f"{enc[0]}() defined 0 times in the new file", False
        # 同名函数有多份（#ifdef 的几个版本）时，取含有原文的那一份
        with_hit = [(d, e) for d, e in spans if any(d <= h <= e for h in hits)]
        if len(with_hit) == 1 or (len(spans) == 1 and enc[1] == 0):
            d, e = (with_hit or spans)[0]
            if enc[1] == 0:
                return d, f"def {enc[0]}()", True
            inside = [h for h in hits if d <= h <= e]
            return min(inside, key=lambda h: abs(h - (d + enc[1]))), f"in {enc[0]}()", True
        if len(spans) != 1:
            return None, f"{enc[0]}() defined {len(spans)} times in the new file", False
        d, e = spans[0]
        best, score = None, 0.0
        for n in range(d, e + 1):
            r = difflib.SequenceMatcher(None, text, new[n - 1].strip()).ratio() if len(text) >= 3 else 0.0
            if r > score or (r == score and best is not None and abs(n - d - enc[1]) < abs(best - d - enc[1])):
                best, score = n, r
        if best is not None and score >= 0.6:
            return best, f"fuzzy {score:.2f} in {enc[0]}()", False
        return None, f"text changed; {enc[0]}() is now at {d}-{e}", False
    # 不在函数里（结构体、宏表、文件头注释）：找上方最近一行在新旧两边都唯一的原文作锚点
    for k in range(lineno - 1, max(0, lineno - 60), -1):
        t = old[k - 1].strip()
        if not solid(t) or sum(1 for l in old if l.strip() == t) != 1:
            continue
        anchors = [n + 1 for n, l in enumerate(new) if l.strip() == t]
        if len(anchors) != 1:
            continue
        want = anchors[0] + (lineno - k)
        near_hits = [h for h in hits if abs(h - want) <= 10]
        if near_hits:
            return min(near_hits, key=lambda h: abs(h - want)), f"anchored {lineno - k} lines below '{t[:30]}'", True
        return None, f"text changed below '{t[:30]}' (now at {anchors[0]})", False
    return None, f"{len(hits)} identical lines, no anchor above" if hits else "line text not found", False


def cmd_remap(args: argparse.Namespace) -> Exit:
    old, new = Tree(args.old, "--old"), Tree(args.new, "--new")
    rep = Report()
    changed: dict[str, list[tuple[int, int, int, str, str]]] = defaultdict(list)   # doc -> (行, 起, 止, 旧, 新)
    suggest: list[tuple[str, int, str]] = []
    moved: list[tuple[Ref, Src, str]] = []   # 同名函数在新文件里找不到：可能整段挪到了别的文件
    for doc in args.files:
        lines = read_doc(doc)
        for ref in collect_refs(doc, lines):
            if ref.first is None:
                continue
            rn = resolve(ref, new, args.side)
            if rn.skip or rn.reason == "NONAME":
                continue
            ro = resolve(ref, old, args.side)
            ol = old.src(ro.rel) if ro.rel else None
            if ol is None or any(n > len(ol) for n in (ref.first, ref.last) if n):
                if rn.rel is None:
                    rep.add(doc, ref.line, "UNRESOLVED", f"{ref.text()} in neither tree")
                continue   # 旧树里对不上而新树里有：多半已经换算过，不动
            have_old = present_ids(ol, ref.context)
            nl = new.src(rn.rel) if rn.rel else None
            if nl is not None:
                have_new = present_ids(nl, ref.context)
                if have_new and all(n <= len(nl) and valid(nl, n, have_new) for n in (ref.first, ref.last) if n):
                    continue   # 在新树里已经成立
                if not have_old and all(n <= len(nl) and ol.lines[n - 1] == nl.lines[n - 1]
                                        for n in (ref.first, ref.last) if n):
                    continue   # 无从验证，但被引行原文没变
            if have_old and not valid(ol, ref.first, have_old):
                rep.add(doc, ref.line, "STALE", f"{ref.text()} context not near it in the old tree either")
                continue
            if nl is None:
                enc = enclosing_func(ol.lines, ref.first)
                if enc:
                    moved.append((ref, ol, enc[0]))
                else:
                    rep.add(doc, ref.line, "UNRESOLVED", f"{ref.text()} file gone from the new tree")
                continue
            ends: list[int] = []
            hows: list[str] = []
            exact = True
            for n in (ref.first, ref.last):
                if n is None:
                    continue
                m, how, ex = map_line(ol.lines, nl.lines, n)
                if m is None:
                    break
                ends.append(m)
                hows.append(how)
                exact = exact and ex
            if len(ends) != (2 if ref.last else 1):
                enc = enclosing_func(ol.lines, ref.first)
                if enc and not func_def_lines(nl.lines, enc[0]):
                    moved.append((ref, ol, enc[0]))
                else:
                    rep.add(doc, ref.line, "UNRESOLVED", f"{ref.text()} [{rn.rel}] {how}")
                continue
            newtext = f"{ref.path}:{ends[0]}" + (f"-{ends[1]}" if len(ends) == 2 else "")
            how = "; ".join(dict.fromkeys(hows))
            if exact and have_old and valid(nl, ends[0], have_old):
                print(f"{doc}:{ref.line}: {ref.text()} -> {newtext} ({how})")
                changed[doc].append((ref.line, ref.start, ref.end, ref.text(), newtext))
            else:
                why = how if not exact else "no identifier on this doc line to verify it"
                suggest.append((doc, ref.line, f"{ref.text()} -> {newtext} ({why})"))
    locate_moved(moved, new, suggest, rep)
    for doc, line, text in sorted(suggest):
        print(f"{doc}:{line}: SUGGEST {text}")
    rep.items.sort(key=lambda t: (t[0], t[1]))
    rep.emit()
    refused = [doc for doc, edits in changed.items() if args.apply and not apply_edits(doc, edits)]
    n = sum(len(e) for d, e in changed.items() if d not in refused)
    verb = "rewritten" if args.apply else "would be rewritten (pass --apply)"
    print(f"kernel_bump remap: {args.side} side, Linux {old.version} -> {new.version}: {n} references {verb}, "
          f"{len(suggest)} suggestions to check by hand (never applied), {len(rep.items)} unresolved",
          file=sys.stderr)
    return Exit.PROBLEMS if suggest or rep.items or refused else Exit.OK


def locate_moved(moved: list[tuple[Ref, Src, str]], new: Tree, suggest: list[tuple[str, int, str]],
                 rep: Report) -> None:
    """被引函数在新文件里没有定义：一次 grep 在整棵新树里找它的定义，给出新位置作为建议。"""
    if not moved:
        return
    names = sorted({f for _, _, f in moved})
    defs: dict[str, list[tuple[str, int]]] = defaultdict(list)
    for line in new.grep_fixed([f"{n}(" for n in names], words=False, only_matching=False):
        path, num, text = line.split(":", 2)
        m = FUNC_DEF_RE.match(text)
        if m and m.group(1) in names and not text.rstrip().endswith(";"):
            defs[m.group(1)].append((path.removeprefix("./"), int(num)))
    for ref, ol, func in moved:
        where = defs.get(func, [])
        if len(where) != 1:
            rep.add(ref.doc, ref.line, "UNRESOLVED", f"{ref.text()} {func}() defined {len(where)} times "
                    f"in the new tree")
            continue
        path, _ = where[0]
        nl = new.src(path)
        ends = [map_line(ol.lines, nl.lines, n)[0] if nl else None for n in (ref.first, ref.last) if n]
        if None in ends:
            rep.add(ref.doc, ref.line, "UNRESOLVED", f"{ref.text()} {func}() moved to {path}, line not found")
            continue
        newtext = f"{path}:{ends[0]}" + (f"-{ends[1]}" if len(ends) == 2 else "")
        suggest.append((ref.doc, ref.line, f"{ref.text()} -> {newtext} ({func}() moved)"))


def apply_edits(doc: str, edits: list[tuple[int, int, int, str, str]]) -> bool:
    """改写一个文档。每处先核对原位置仍是旧引用：读取之后文档若被别人改过，整份放弃不写。"""
    lines = Path(doc).read_text().split("\n")
    for line, start, end, old, _ in edits:
        if line > len(lines) or lines[line - 1][start:end] != old:
            print(f"kernel_bump: {doc}:{line}: no longer reads {old!r}; file changed while running, "
                  f"nothing written to it, rerun remap", file=sys.stderr)
            return False
    for line, start, end, _, new in sorted(edits, key=lambda e: (e[0], -e[1])):
        s = lines[line - 1]
        lines[line - 1] = s[:start] + new + s[end:]
    # 写同目录临时文件再 rename：中途失败不留下改了一半的文档
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(os.path.abspath(doc)), prefix=".kernel_bump.")
    try:
        with os.fdopen(fd, "w") as f:
            f.write("\n".join(lines))
        os.chmod(tmp, os.stat(doc).st_mode & 0o7777)
        os.replace(tmp, doc)
    except BaseException:
        os.unlink(tmp)
        raise
    return True


# ---------------------------------------------------------------- logs

TS_RE = re.compile(r"^\s*\[\s*\d+\.\d+\]\s*")


def norm_log(s: str) -> str:
    """去掉时间戳，把数字和十六进制换成占位符：同一条消息每次启动的数值不同。"""
    s = TS_RE.sub("", s.strip())
    s = re.sub(r"0x[0-9a-fA-F]+", "H", s)
    s = re.sub(r"\b[0-9a-fA-F]*\d[0-9a-fA-F]*\b", "N", s)
    return re.sub(r"\s+", " ", s)


def cmd_logs(args: argparse.Namespace) -> Exit:
    log_lines = [norm_log(l) for log in args.log for l in read_doc(log)]
    rep = Report()
    quoted = 0
    for doc in args.files:
        in_code = False
        for i, line in enumerate(read_doc(doc), 1):
            if line.lstrip().startswith("```"):
                in_code = not in_code
                continue
            if in_code:
                cands = [line] if TS_RE.match(line) else []
            else:
                # 行内只看像内核日志的片段：有 "子系统: 消息" 的形状，不含中文和代码符号；
                # 命令（perf kvm stat）和函数名不在此列
                cands = [t for t in TICK_RE.findall(line)
                         if len(t) >= 12 and re.match(r"^[\w .-]{2,30}: \S", t)
                         and not re.search(r"[\u4e00-\u9fff]|[{};=|]|^\s*[-$#]|\.[ch]\b", t)]
            for c in cands:
                # 文中用 ... 省略的部分当通配符：各段按顺序出现在同一行日志里即可
                parts = [x.strip() for x in norm_log(c).split("...") if x.strip(" ()")]
                if sum(len(x) for x in parts) < 10:
                    continue
                quoted += 1
                if not any(in_order(l, parts) for l in log_lines):
                    rep.add(doc, i, "NOTINLOG", c.strip()[:120])
    rep.emit()
    print(f"kernel_bump logs: {quoted} quoted log fragments checked against {len(args.log)} logs, "
          f"{len(rep.items)} not found", file=sys.stderr)
    return Exit.PROBLEMS if rep.items else Exit.OK


def in_order(line: str, parts: list[str]) -> bool:
    pos = 0
    for x in parts:
        pos = line.find(x, pos)
        if pos < 0:
            return False
        pos += len(x)
    return True


# ---------------------------------------------------------------- expect-not

_repo_text: str | None = None


def repo_text() -> str:
    """mini-vmm 和测试工具自己的源码（e2e 脚本除外，expect_not 本身就写在里面）。"""
    global _repo_text
    if _repo_text is None:
        parts = []
        for d in ("src", "include", "tests", "tools"):
            for p in (ROOT / d).rglob("*"):
                # 本脚本的注释里拿这些模式举例，不能算作出处
                if (p.is_file() and p.suffix in (".c", ".h", ".py", ".sh") and p.parent.name != "e2e"
                        and p.resolve() != Path(__file__).resolve()):
                    parts.append(p.read_text(errors="replace"))
        _repo_text = "\n".join(parts)
    return _repo_text


def fmt_shape(s: str) -> str:
    """把 printf 格式串和它打印出的结果归到同一形状：格式说明符和数值都换成 %。
    "memslot %d: gpa=0x%llx" 与 "memslot 1: gpa=0x0" 都变成 "memslot %: gpa=%"。"""
    s = re.sub(r"(?:0x)?%[-#+ 0-9.*]*(?:hh|h|ll|l|z|j|t|PRI[a-z0-9]+)?[diouxXcsp]", "%", s)
    return re.sub(r"0x[0-9a-fA-F]+|\b\d+\b", "%", s)


def fragments(s: str) -> list[str]:
    """模式里的纯文字段，按长度降序。printf 的 %d、%s、%pR 打印出来是数字、名字、方括号区间，
    格式串里对应的只有这些纯文字段：连续的纯字母词连成一段；带数字的词按 - 拆开，取其中的
    纯字母部分（SERIRQ-0-END 得到 SERIRQ 和 END）。"""
    out: list[str] = []
    run: list[str] = []
    for tok in s.split():
        if re.fullmatch(r"[A-Za-z]+(?:[-'_,][A-Za-z]+)*,?", tok):
            run.append(tok)
            continue
        if run:
            out.append(" ".join(run))
            run = []
        out += [x for x in re.split(r"[^A-Za-z]+", tok) if len(x) >= 2]
    if run:
        out.append(" ".join(run))
    return sorted(set(out), key=len, reverse=True)


def cmd_expect_not(args: argparse.Namespace) -> Exit:
    tree = Tree(args.tree, "--tree")
    suites = args.files or sorted(str(p.relative_to(ROOT)) for p in (ROOT / "tests" / "e2e").glob("*.sh"))
    uses: dict[str, list[tuple[str, int]]] = defaultdict(list)
    script_rest: dict[str, str] = {}
    for s in suites:
        rest = []
        for i, line in enumerate(read_doc(s), 1):
            m = EXPECT_NOT_RE.match(line)
            if m:
                pat = m.group(1) if m.group(1) is not None else m.group(2)
                uses[pat.replace('\\"', '"')].append((s, i))
            elif not re.match(r"^\s*expect_", line):
                rest.append(line)
        script_rest[s] = "\n".join(rest)
    own = repo_text()
    own_shape = fmt_shape(own)
    kernel_pats = []
    for pat, where in uses.items():
        frags = fragments(pat)
        # mini-vmm 或测试工具打印的（含 printf 格式串）。片段太短（assigned、ttyS）到处都有，不算
        if pat in own or fmt_shape(pat) in own_shape or (frags and len(frags[0]) >= 10 and frags[0] in own):
            continue
        # 测试脚本自己拼出来的标记（echo SERIRQ-$n-END）：全大写，各段都出现在同一脚本的命令里
        if frags and all(f.isupper() and f in script_rest[where[0][0]] for f in frags):
            continue
        kernel_pats.append(pat)
    frag_of = {p: (fragments(p) or [""])[0] for p in kernel_pats}
    out = tree.grep_fixed(sorted(set(kernel_pats) | {f for f in frag_of.values() if len(f) >= 6}),
                          words=False, only_matching=False)
    rep = Report()
    for p in kernel_pats:
        if any(p in l.split(":", 2)[-1] for l in out):
            continue
        f = frag_of[p]
        # 同一行里含模式的纯文字段越多越像出处（"probe with driver" 到处都有，带上 "failed" 才是 dd.c）
        frags = fragments(p)
        cands = [l.split(":", 2) for l in out if len(f) >= 6 and f in l.split(":", 2)[-1]]
        fw = max(cands, key=lambda c: sum(x in c[2] for x in frags), default=None)
        s, i = uses[p][0]
        more = f" (+{len(uses[p]) - 1} more uses)" if len(uses[p]) > 1 else ""
        if fw:
            rep.add(s, i, "FRAGMENT", f'"{p}" only "{f}" found, at {fw[0].removeprefix("./")}:{fw[1]}{more}')
        else:
            rep.add(s, i, "NOTFOUND", f'"{p}"{more}')
    rep.items.sort(key=lambda t: (t[0], t[1]))
    rep.emit()
    print(f"kernel_bump expect-not: {len(uses)} distinct patterns, {len(uses) - len(kernel_pats)} from mini-vmm "
          f"or the test scripts, {len(kernel_pats) - len(rep.items)} found verbatim in Linux {tree.version}, "
          f"{len(rep.items)} need a human", file=sys.stderr)
    return Exit.PROBLEMS if rep.items else Exit.OK


# ---------------------------------------------------------------- main

HELP = f"""\
Check references to Linux sources in docs and tests after the pinned guest
kernel (or the host KVM reference version) changes.

subcommands:
  remap       move file:line references from --old to --new (dry run unless --apply)
  refs        files, line numbers and versioned identifier lists still valid in --tree
  logs        log text quoted in docs still appears in freshly captured logs
  expect-not  every e2e expect_not string still has a source in the kernel

output: one line per finding, "DOC:LINE: KIND detail"; a summary goes to stderr.
  remap:      "DOC:LINE: OLDREF -> NEWREF (how)" (rewritten by --apply),
              "DOC:LINE: SUGGEST OLDREF -> NEWREF (why)" (never applied),
              UNRESOLVED, STALE (already wrong in --old)
  refs:       MISSING, PASTEOF, DRIFT, IDENT, VERSION; bare file names found
              nowhere are summarized in one stderr line and do not fail the run
  logs:       NOTINLOG
  expect-not: FRAGMENT, NOTFOUND

EXAMPLE:
  tools/kernel_bump.py remap --old ~/linux-old --new ~/linux-guest learning-note/0[5-7]*.md
  tools/kernel_bump.py remap --old ~/linux-old --new ~/linux-guest --apply learning-note/0[5-7]*.md
  tools/kernel_bump.py refs --tree ~/linux-guest blog/*.md learning-note/0[4-7]*.md src/*.c
  tools/kernel_bump.py refs --side host --tree ~/linux-6.6 learning-note/04*.md src/arch/amd/*.c
  tools/kernel_bump.py logs --log /tmp/boot.log blog/12*.md
  tools/kernel_bump.py expect-not --tree ~/linux-guest

NOTE:
  Order for a kernel bump: remap from the old tree first, then refs as a
  backstop. A line reference counts as valid when an identifier written in the
  same clause (backticked, or name followed by "(") occurs within {WINDOW} lines of
  it or names its enclosing function, so drift shorter than that is invisible to
  refs. A clause with no such identifier cannot be checked: refs stays silent and
  remap only suggests.
  --side picks guest (default) or host references; host means paths under
  {', '.join(HOST_PREFIXES[:3])}, ... . Host references outside those
  directories are taken as guest ones. Files that cite a different version than
  the tree produce noise; pass only the files that cite the tree's version.
  remap rewrites only references that are invalid in --new, valid in --old and
  whose mapped line is valid in --new, so running it twice changes nothing. It
  refuses to write a file that changed while it ran.
  logs and expect-not are heuristics: a hit is a fact, a miss is a lead to check
  by hand (the message may be intentionally absent, split across printk calls
  or built from %s). Trees are only read; grep over a tree takes tens of seconds.
  Exit status: 0 nothing found, 1 findings listed, 2 usage error.
"""


def main() -> Exit:
    ap = Parser(prog="kernel_bump.py", description=HELP, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", metavar="SUBCOMMAND", parser_class=Parser)
    sub.required = True

    p = sub.add_parser("refs", help="check references against one tree")
    p.add_argument("--tree", required=True, help="Linux source tree the docs should cite")
    p.add_argument("--side", choices=("guest", "host"), default="guest")
    p.add_argument("files", nargs="+", metavar="FILE", help="Markdown or C files to scan")

    p = sub.add_parser("remap", help="rewrite line references from an old tree to a new one")
    p.add_argument("--old", required=True, help="tree the references were written against")
    p.add_argument("--new", required=True, help="tree they should point to")
    p.add_argument("--side", choices=("guest", "host"), default="guest")
    p.add_argument("--apply", action="store_true", help="rewrite the files instead of printing only")
    p.add_argument("files", nargs="+", metavar="FILE")

    p = sub.add_parser("logs", help="find quoted log text missing from captured logs")
    p.add_argument("--log", required=True, action="append", help="captured log (repeatable)")
    p.add_argument("files", nargs="+", metavar="FILE")

    p = sub.add_parser("expect-not", help="find expect_not strings with no source in the kernel")
    p.add_argument("--tree", required=True)
    p.add_argument("files", nargs="*", metavar="SUITE", help="e2e scripts (default tests/e2e/*.sh)")

    args = ap.parse_args()
    # 同一个文件给两次时 remap 会对同一位置改写两次，去重
    args.files = list(dict.fromkeys(args.files))
    for f in args.files + getattr(args, "log", []):
        if not Path(f).is_file():
            die(f"{f}: no such file")
    return {"refs": cmd_refs, "remap": cmd_remap, "logs": cmd_logs, "expect-not": cmd_expect_not}[args.cmd](args)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("kernel_bump: interrupted", file=sys.stderr)
        sys.exit(Exit.INTERRUPTED)
