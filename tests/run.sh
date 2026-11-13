#!/usr/bin/env bash
# tests/run.sh — 验收总入口，被 `make test [suite...]` 调用
#
# 一个 suite 一个脚本，自成一体，也可以单独跑。这里只负责三件事：
# 选 suite、按顺序跑、把各自的结果汇总成一张表和一个退出码。
#
# 用法：
#   ./tests/run.sh                 默认集合（offline 里除 fuzz 外的全部 + 所有 e2e）
#   ./tests/run.sh unit io         只跑指定 suite
#   ./tests/run.sh all             默认集合 + fuzz
#   ./tests/run.sh --list          列出所有 suite
#   ./tests/run.sh --manual linux-boot  只打印某个 suite 的人工验收指引
#
# 环境变量：
#   VMLINUX=  BZIMAGE=   guest 内核，缺省取 guest-kernel/ 下 tools/build_guest_kernel.sh 的产物，
#                        那里也没有时相关用例自行 SKIP
#   FUZZ_TIME=300        fuzz suite 每个 target 的秒数
#
# 退出码：0 全过，1 有 suite 失败，2 用法错误。
#
# 为什么 fuzz 不在默认集合里：它按墙钟时间计费（默认每 target 5 分钟），
# 而 make test 应该是随手就能跑的。要跑就显式写出来：make test fuzz。

set -u

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$TESTS_DIR")"

if [ -t 1 ]; then
    C_RED=$'\e[31m'; C_GRN=$'\e[32m'; C_YEL=$'\e[33m'
    C_BLD=$'\e[1m';  C_RST=$'\e[0m'
else
    C_RED=''; C_GRN=''; C_YEL=''; C_BLD=''; C_RST=''
fi

# suite 名 -> 脚本路径 -> 是否需要 /dev/kvm
suite_script() {
    case "$1" in
    unit)   echo "offline/unit.sh" ;;
    negctl) echo "offline/negctl.sh" ;;
    tables) echo "offline/boot_tables.sh" ;;
    fuzz)   echo "offline/fuzz.sh" ;;
    gshell) echo "offline/guest_shell.sh" ;;
    milestones) echo "offline/milestones.sh" ;;
    kvm-basics|linux-boot|smp|legacy-devices|guest-memory|event-loop|exit-stats|pci-bus|emu-net|emu-blk|pci-msi) echo "e2e/$1.sh" ;;
    *)      echo "" ;;
    esac
}

suite_needs_kvm() {
    case "$1" in
    kvm-basics|linux-boot|smp|legacy-devices|guest-memory|event-loop|exit-stats|pci-bus|emu-net|emu-blk|pci-msi) return 0 ;;
    *) return 1 ;;
    esac
}

suite_desc() {
    case "$1" in
    unit)   echo "C 单元测试：内存模型、IO 总线、固件表字段、事件循环、网络后端（ASan+UBSan）" ;;
    negctl) echo "negative control：每个 mutant 都必须让单元测试变红" ;;
    tables) echo "ACPI 表交给 iasl 反汇编（外部 oracle）" ;;
    fuzz)   echo "vmlinux / bzImage 加载器的 libFuzzer 跑批（按时间计费）" ;;
    gshell) echo "e2e 驱动脚本 guest_shell.py 自测（假 guest）" ;;
    milestones) echo "MILESTONES.md 依赖关系与层级表一致" ;;
    kvm-basics)     echo "实模式、长模式页表、错误路径诊断" ;;
    linux-boot)     echo "8250 UART、vmlinux/bzImage 直启、initramfs shell" ;;
    smp)            echo "SMP：多 vCPU 启动、CPUID 拓扑、停机" ;;
    legacy-devices) echo "传统设备探测、PIO trace 结构、退出路径" ;;
    guest-memory)   echo "3GiB 以上 RAM：memslot 拆分、E820、MMIO hole" ;;
    event-loop)     echo "epoll I/O 线程：线程布局、串口输入、stdin EOF 与不可 poll" ;;
    exit-stats)     echo "按 exit_reason、按设备的 exit 计数，KVM 二进制统计" ;;
    pci-bus)        echo "conf1 配置空间、BAR 探测与路由、PCI0 窗口、INTx 路由" ;;
    emu-net)        echo "--netdev tap 后端的命令行约束（e1000 前端见里程碑 emu-net）" ;;
    emu-blk)        echo "--drive 磁盘后端，AHCI 控制器的探测与端口枚举" ;;
    pci-msi)        echo "AHCI 的 MSI：PCI-MSI、运行中回退 INTx、非法消息的诊断" ;;
    esac
}

# e2e suite 以 MILESTONES.md 里的里程碑 ID 命名
E2E_SUITES="kvm-basics linux-boot smp legacy-devices guest-memory event-loop exit-stats pci-bus emu-net emu-blk pci-msi"
ALL_SUITES="unit negctl tables gshell milestones fuzz $E2E_SUITES"
DEFAULT_SUITES="unit negctl tables gshell milestones $E2E_SUITES"

usage() {
    echo "用法: $0 [--list|--manual] [suite...]"
    echo
    echo "suite:"
    local s
    for s in $ALL_SUITES; do
        printf '  %-15s %s\n' "$s" "$(suite_desc "$s")"
    done
    echo
    echo "  all      = $DEFAULT_SUITES fuzz"
    echo "  (默认)   = $DEFAULT_SUITES"
}

MODE="-a"
SELECTED=""

while [ $# -gt 0 ]; do
    case "$1" in
    --list|-l)   usage; exit 0 ;;
    --manual|-m) MODE="-m" ;;
    --auto|-a)   MODE="-a" ;;
    -h|--help)   usage; exit 0 ;;
    all)         SELECTED="$SELECTED $ALL_SUITES" ;;
    offline)     SELECTED="$SELECTED unit negctl tables gshell milestones fuzz" ;;
    e2e)         SELECTED="$SELECTED $E2E_SUITES" ;;
    -*)          echo "未知选项: $1" >&2; usage >&2; exit 2 ;;
    *)
        if [ -z "$(suite_script "$1")" ]; then
            echo "未知 suite: $1" >&2
            usage >&2
            exit 2
        fi
        SELECTED="$SELECTED $1"
        ;;
    esac
    shift
done

[ -n "${SELECTED// /}" ] || SELECTED="$DEFAULT_SUITES"

# 去重并保持 ALL_SUITES 的顺序
ORDERED=""
for s in $ALL_SUITES; do
    case " $SELECTED " in
    *" $s "*) ORDERED="$ORDERED $s" ;;
    esac
done

HAVE_KVM=0
[ -c /dev/kvm ] && [ -r /dev/kvm ] && [ -w /dev/kvm ] && HAVE_KVM=1

results=""
rc_all=0

for s in $ORDERED; do
    script="$TESTS_DIR/$(suite_script "$s")"

    printf '\n%s================================================================%s\n' \
        "$C_BLD" "$C_RST"
    printf '%s suite: %-15s %s%s\n' "$C_BLD" "$s" "$(suite_desc "$s")" "$C_RST"
    printf '%s================================================================%s\n' \
        "$C_BLD" "$C_RST"

    if suite_needs_kvm "$s" && [ "$HAVE_KVM" -eq 0 ]; then
        printf '%sSKIP%s /dev/kvm 不可读写，跳过（kvm_amd 没加载？当前用户不在 kvm 组？）\n' \
            "$C_YEL" "$C_RST"
        results="$results\n  $(printf '%-15s' "$s") ${C_YEL}SKIP${C_RST}  no /dev/kvm"
        continue
    fi

    start=$SECONDS
    bash "$script" "$MODE"
    rc=$?
    dur=$((SECONDS - start))

    if [ "$rc" -eq 0 ]; then
        results="$results\n  $(printf '%-15s' "$s") ${C_GRN}PASS${C_RST}  ${dur}s"
    else
        results="$results\n  $(printf '%-15s' "$s") ${C_RED}FAIL${C_RST}  ${dur}s (rc=$rc)"
        rc_all=1
    fi
done

printf '\n%s================================================================%s\n' \
    "$C_BLD" "$C_RST"
printf '%s 汇总%s' "$C_BLD" "$C_RST"
printf "$results\n"
printf '%s================================================================%s\n' \
    "$C_BLD" "$C_RST"

if [ "$rc_all" -eq 0 ]; then
    printf '%s全部 suite 通过%s\n' "$C_GRN" "$C_RST"
else
    printf '%s有 suite 失败%s\n' "$C_RED" "$C_RST"
fi
exit $rc_all
