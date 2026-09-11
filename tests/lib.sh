# tests/lib.sh — 验收脚本公共函数
#
# 被 tests/stepN.sh source。不直接执行。
#
# 约定：
#   - 每个 Step 一个 tests/stepN.sh，自成一体，可单独跑
#   - 能自动断言的做成 case_*，断言不了的（需要 root、需要看波形、
#     需要人眼判断串口输出）写进 guide_* 里，只打印指导命令和期望现象
#   - 退出码：0 = 全过，1 = 有 FAIL

set -u

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$TESTS_DIR")"
VMM="$ROOT/build/vmm"

if [ -t 1 ]; then
    C_RED=$'\e[31m'; C_GRN=$'\e[32m'; C_YEL=$'\e[33m'
    C_CYA=$'\e[36m'; C_BLD=$'\e[1m';  C_RST=$'\e[0m'
else
    C_RED=''; C_GRN=''; C_YEL=''; C_CYA=''; C_BLD=''; C_RST=''
fi

N_PASS=0
N_FAIL=0
N_SKIP=0
FAILED_CASES=""

# 当前 case 的上下文
CUR_NAME=""
CUR_OUT=""
CUR_RC=0
CUR_BAD=0

log()  { printf '%s\n' "$*"; }
head1(){ printf '\n%s=== %s ===%s\n' "$C_BLD" "$*" "$C_RST"; }

# 环境前置检查
need_kvm() {
    if [ ! -c /dev/kvm ]; then
        log "${C_RED}/dev/kvm 不存在${C_RST}：kvm_amd 模块没加载？"
        exit 1
    fi
    if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; then
        log "${C_RED}/dev/kvm 不可读写${C_RST}：当前用户在 kvm 组里吗？(id -nG)"
        exit 1
    fi
}

need_build() {
    if [ ! -x "$VMM" ]; then
        log "${C_YEL}build/vmm 不存在，先编译${C_RST}"
        ( cd "$ROOT" && make ) || exit 1
    fi
}

# ---- case 框架 ----
#
# 用法：
#   run_case "名字" <命令...>
#   expect_rc 0
#   expect_has "某段文本"
#   expect_not "不该出现的文本"
#   expect_any "候选 1" "候选 2"
#   end_case

run_case() {
    CUR_NAME="$1"; shift
    CUR_BAD=0
    printf '%s->%s %s\n' "$C_CYA" "$C_RST" "$CUR_NAME"
    printf '   $ %s\n' "$*"
    # stderr 合并进来一起断言：VMM 的日志走 stderr，
    # 调试端口的输出走 stdout，两边都要检查
    # stdin 接 /dev/null：被测程序不该碰终端。Step 2 起 VMM 会把
    # 终端切 raw 模式并读输入，而 timeout 会把它放进后台进程组，
    # 碰终端就会被 SIGTTOU/SIGTTIN 停住，整个测试挂死。
    CUR_OUT="$("$@" 2>&1 </dev/null)"
    CUR_RC=$?
}

expect_rc() {
    if [ "$CUR_RC" -ne "$1" ]; then
        printf '   %sFAIL%s 退出码 %d，期望 %d\n' "$C_RED" "$C_RST" "$CUR_RC" "$1"
        CUR_BAD=1
    fi
}

expect_has() {
    if ! printf '%s' "$CUR_OUT" | grep -qF -- "$1"; then
        printf '   %sFAIL%s 输出里没找到: %s\n' "$C_RED" "$C_RST" "$1"
        CUR_BAD=1
    fi
}

expect_not() {
    if printf '%s' "$CUR_OUT" | grep -qF -- "$1"; then
        printf '   %sFAIL%s 输出里不该出现: %s\n' "$C_RED" "$C_RST" "$1"
        CUR_BAD=1
    fi
}

# 候选里出现任意一个即可：同一件事有多条合法的日志路径时用。
#   expect_any "候选 1" "候选 2" ...
expect_any() {
    local s
    for s in "$@"; do
        if printf '%s' "$CUR_OUT" | grep -qF -- "$s"; then
            return 0
        fi
    done
    printf '   %sFAIL%s none of these found:' "$C_RED" "$C_RST"
    printf ' "%s"' "$@"
    printf '\n'
    CUR_BAD=1
}

end_case() {
    if [ "$CUR_BAD" -eq 0 ]; then
        printf '   %sPASS%s\n' "$C_GRN" "$C_RST"
        N_PASS=$((N_PASS + 1))
    else
        printf '   %s--- 实际输出 ---%s\n' "$C_YEL" "$C_RST"
        printf '%s\n' "$CUR_OUT" | sed 's/^/   | /'
        N_FAIL=$((N_FAIL + 1))
        FAILED_CASES="$FAILED_CASES\n  - $CUR_NAME"
    fi
}

skip_case() {
    printf '%s->%s %s\n   %sSKIP%s %s\n' \
        "$C_CYA" "$C_RST" "$1" "$C_YEL" "$C_RST" "${2:-}"
    N_SKIP=$((N_SKIP + 1))
}

# ---- 人工验收条目 ----
#
# guide "标题" "命令" "期望现象"
guide() {
    printf '\n%s[人工]%s %s\n' "$C_YEL" "$C_RST" "$1"
    printf '  运行:\n'
    printf '%s\n' "$2" | sed 's/^/    /'
    printf '  期望:\n'
    printf '%s\n' "$3" | sed 's/^/    /'
}

summary() {
    head1 "结果"
    printf '  PASS %d  FAIL %d  SKIP %d\n' "$N_PASS" "$N_FAIL" "$N_SKIP"
    if [ "$N_FAIL" -ne 0 ]; then
        printf '%s失败的用例:%s' "$C_RED" "$C_RST"
        printf "$FAILED_CASES\n"
        return 1
    fi
    printf '%s全部通过%s\n' "$C_GRN" "$C_RST"
    return 0
}