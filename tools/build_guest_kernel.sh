#!/usr/bin/env bash
# tools/build_guest_kernel.sh — 构建 mini-vmm 测试用的 guest 内核
#
# 配置 = x86_64_defconfig + tools/guest-kernel.config，树外构建（O=），产出
# vmlinux 和 bzImage。每次运行都从 defconfig 重新生成 .config，所以同一份
# 源码加同一份片段总是得到同一份配置；kbuild 只重编配置变化涉及的部分，
# 配置没变时第二次运行几乎不花时间。
#
# 源码树只读：kbuild 在源码树里有构建产物（.config、include/config）时拒绝
# O= 构建，所以这里先检查并给出明确提示，而不是让 make 半路报错。同一棵
# 源码树可以同时服务多个 O= 目录，不影响其他用途的构建。
set -euo pipefail

readonly EXIT_OK=0
readonly EXIT_FAIL=1
readonly EXIT_USAGE=2
readonly EXIT_INTERRUPTED=130   # shell 对 SIGINT 的约定 128+2

# 片段按这个版本系列编写和验证；换版本时改这里，并重新跑一遍 make test
readonly KERNEL_SERIES="6.6"
readonly BASE_DEFCONFIG="x86_64_defconfig"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly ROOT
readonly FRAGMENT="$ROOT/tools/guest-kernel.config"

usage() {
    cat <<EOF
Usage: tools/build_guest_kernel.sh --src DIR [--out DIR] [-j N]

Build the guest kernel used by mini-vmm's e2e tests: $BASE_DEFCONFIG plus
tools/guest-kernel.config, built out of tree. Prints the paths of vmlinux and
bzImage on stdout when done; progress goes to stderr.

Options:
  --src DIR   Linux $KERNEL_SERIES.x source tree (or set LINUX_SRC); it is never written to
  --out DIR   build directory (default: guest-kernel/ in the repository)
  -j N        parallel jobs (default: number of CPUs, at most 8)
  -h, --help  show this help

EXAMPLE:
  tools/build_guest_kernel.sh --src ~/linux-6.6
  LINUX_SRC=~/linux-6.6 tools/build_guest_kernel.sh -j 4
  VMLINUX=guest-kernel/vmlinux make test linux-boot

NOTE:
  The e2e tests pick up guest-kernel/vmlinux and guest-kernel/arch/x86/boot/bzImage
  automatically when VMLINUX / BZIMAGE are not set.
  The default build directory is outside build/ so that 'make clean' does not
  throw away a kernel build.
  .config is regenerated from scratch on every run; options listed in the
  fragment are checked afterwards and the build stops if Kconfig changed any.
  The source tree must have no in-tree build artifacts (kbuild refuses O= builds
  otherwise); run 'make mrproper' in it if needed.
  Exit codes: 0 success, 1 build or configuration failure, 2 usage error,
              130 interrupted.
EOF
}

die() {
    printf 'build_guest_kernel: %s\n' "$1" >&2
    exit "${2:-$EXIT_FAIL}"
}

usage_error() {
    printf 'build_guest_kernel: %s\n' "$1" >&2
    printf "Try 'tools/build_guest_kernel.sh --help'.\n" >&2
    exit "$EXIT_USAGE"
}

log() { printf '==> %s\n' "$*" >&2; }

trap 'printf "build_guest_kernel: interrupted\n" >&2; exit $EXIT_INTERRUPTED' INT

src="${LINUX_SRC:-}"
out="$ROOT/guest-kernel"
ncpu="$(nproc)"
jobs=$(( ncpu < 8 ? ncpu : 8 ))

while [ $# -gt 0 ]; do
    case "$1" in
    --src)  [ $# -ge 2 ] || usage_error "--src needs a directory"; src="$2"; shift 2 ;;
    --out)  [ $# -ge 2 ] || usage_error "--out needs a directory"; out="$2"; shift 2 ;;
    -j)     [ $# -ge 2 ] || usage_error "-j needs a number"; jobs="$2"; shift 2 ;;
    -h|--help) usage; exit "$EXIT_OK" ;;
    *)      usage_error "unknown argument: $1" ;;
    esac
done

[ -n "$src" ] || usage_error "no source tree given: use --src DIR or set LINUX_SRC"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || usage_error "-j expects a positive integer, got '$jobs'"
[ -f "$src/Makefile" ] && [ -f "$src/Kconfig" ] || die "$src does not look like a Linux source tree"
[ -f "$FRAGMENT" ] || die "config fragment missing: $FRAGMENT"
src="$(cd "$src" && pwd)"

version="$(make -s -C "$src" kernelversion)"
case "$version" in
"$KERNEL_SERIES" | "$KERNEL_SERIES".*) ;;
*) die "$src is Linux $version, but the fragment is written for $KERNEL_SERIES.x (see KERNEL_SERIES in this script)" ;;
esac

if [ -e "$src/.config" ] || [ -d "$src/include/config" ]; then
    die "$src has in-tree build artifacts and kbuild will refuse an O= build; run 'make mrproper' there first"
fi

mkdir -p "$out"
out="$(cd "$out" && pwd)"
[ "$out" != "$src" ] || usage_error "--out must differ from the source tree"

log "Linux $version from $src, building in $out"

log "configure: $BASE_DEFCONFIG + ${FRAGMENT#"$ROOT"/}"
make -s -C "$src" O="$out" "$BASE_DEFCONFIG" >&2
# -m：只合并不调用 make，否则 merge_config.sh 会以 alldefconfig 而不是上面的
# defconfig 为底重新生成
( cd "$src" && scripts/kconfig/merge_config.sh -m -Q -O "$out" "$out/.config" "$FRAGMENT" ) >&2
make -s -C "$src" O="$out" olddefconfig >&2

# merge_config.sh 只对被覆盖的值告警、olddefconfig 会静默改掉依赖不满足的选项，
# 两者都不失败。这里逐条核对片段里的每个值都原样留在了最终 .config 里。
bad=0
while IFS= read -r line; do
    case "$line" in
    CONFIG_*=*) want="$line" ;;
    "# CONFIG_"*" is not set") want="$line" ;;
    *) continue ;;
    esac
    if ! grep -qxF -- "$want" "$out/.config"; then
        name="${want#\# }"; name="${name%%[= ]*}"
        got="$(grep -E "^(# )?${name}[= ]" "$out/.config" || echo "(absent)")"
        printf 'build_guest_kernel: fragment wants "%s", .config has "%s"\n' "$want" "$got" >&2
        bad=1
    fi
done < "$FRAGMENT"
[ "$bad" -eq 0 ] || die "Kconfig did not accept the fragment as written (unmet dependencies?)"

log "build: make -j$jobs vmlinux bzImage"
make -s -C "$src" O="$out" -j"$jobs" vmlinux bzImage >&2

release="$(make -s -C "$src" O="$out" kernelrelease)"
log "done: Linux $release"
printf '%s\n' "$out/vmlinux" "$out/arch/x86/boot/bzImage"
exit "$EXIT_OK"
