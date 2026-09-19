#!/usr/bin/env bash
# tests/offline/fuzz.sh — 内核加载器的 libFuzzer 跑批（不需要 /dev/kvm）
#
# 被测：src/boot/elf_loader.c、src/boot/bzimage.c —— 这个 VMM 里唯一
# 解析外部文件的两处，且解析的偏移与长度完全由文件内容决定。
#
# oracle 是 sanitizer + “必须有限时间内返回”，不需要知道正确解析结果，
# 因此不存在自产自销的问题。判据：libFuzzer 退出码为 0 且没有 crash
# artifact；发现问题时 artifact 会落在 build/fuzz/artifacts/。
#
# 预算（可用环境变量覆盖）：
#   FUZZ_TIME=300   每个 target 的墙钟秒数
#   FUZZ_JOBS=1     并行 worker 数，默认 1（不抢机器）
#   FUZZ_RSS=2048   单进程内存上限 MiB
#
# 这个 suite 不在 make test 的默认集合里，必须显式 make test fuzz。

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

FUZZ_DIR="$ROOT/build/fuzz"
CORPUS="$FUZZ_DIR/corpus"
ARTIFACTS="$FUZZ_DIR/artifacts"
FUZZ_TIME="${FUZZ_TIME:-300}"
FUZZ_JOBS="${FUZZ_JOBS:-1}"
FUZZ_RSS="${FUZZ_RSS:-2048}"

# target 名 -> 语料子目录
TARGETS="elf_loader_fuzz:elf bzimage_fuzz:bzimage"

export ASAN_OPTIONS="${ASAN_OPTIONS:-abort_on_error=0:print_stacktrace=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1}"

auto_tests() {

head1 "内核加载器 fuzz（libFuzzer + ASan + UBSan，每个 target ${FUZZ_TIME}s）"

mkdir -p "$ARTIFACTS"
run_case "生成种子语料" python3 "$ROOT/tests/fuzz/mkseeds.py" "$CORPUS"
expect_rc 0
expect_has "seeds:"
end_case

local spec name sub
for spec in $TARGETS; do
    name="${spec%%:*}"
    sub="${spec##*:}"

    if [ ! -x "$FUZZ_DIR/$name" ]; then
        skip_case "$name" "未构建（需要 clang），先跑 make $FUZZ_DIR/$name"
        continue
    fi

    mkdir -p "$CORPUS/$sub"
    # -print_final_stats 给出实际执行次数和覆盖率，写进报告才有意义；
    # 只说“跑了 5 分钟”不构成证据。
    run_case "$name (${FUZZ_TIME}s)" \
        "$FUZZ_DIR/$name" \
        "-max_total_time=$FUZZ_TIME" \
        "-jobs=$FUZZ_JOBS" \
        "-workers=$FUZZ_JOBS" \
        "-rss_limit_mb=$FUZZ_RSS" \
        "-max_len=131072" \
        "-print_final_stats=1" \
        "-artifact_prefix=$ARTIFACTS/$name-" \
        "$CORPUS/$sub"
    expect_rc 0
    expect_has "Done "
    expect_not "ERROR: AddressSanitizer"
    expect_not "ERROR: libFuzzer"
    expect_not "runtime error:"
    expect_not "SUMMARY: UndefinedBehaviorSanitizer"
    end_case
done

if [ -n "$(ls -A "$ARTIFACTS" 2>/dev/null)" ]; then
    log ""
    log "${C_RED}crash artifact 落在 $ARTIFACTS：${C_RST}"
    ls -la "$ARTIFACTS" | sed 's/^/  /'
    log "复现：$FUZZ_DIR/<target> <artifact 文件>"
fi

}

main() {
    case "${1:-all}" in
    -m|--manual) return 0 ;;
    esac
    auto_tests
    summary
}

main "${1:-all}"
