#!/usr/bin/env bash
# tests/e2e/emu-net.sh — 里程碑 emu-net 的验收
#
#   --netdev tap 后端：命令行约束、TAP 的打开与错误提示
#
# 用法：
#   ./tests/e2e/emu-net.sh       全部
#   ./tests/e2e/emu-net.sh -a    只跑自动用例
#   ./tests/e2e/emu-net.sh -m    只看人工指引
#
# 后端的收发和流控由单元测试 netdev_test 在独立的 user + net namespace
# 里验证（见 tests/unit/netdev_test.c）；这里验证的是真实的 vmm 程序把
# --netdev 接对了：参数错误在创建 VM 之前报出，引用关系按约定检查。

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

INITRD="$ROOT/build/initramfs.cpio.gz"

# ------------------------------------------------------------------
auto_tests() {

head1 "命令行约束"

run_case "--netdev 只能和 --mode linux 一起用" \
    "$VMM" --mode long64 --netdev tap,id=n0,ifname=t0 "$ROOT/build/serial64.bin"
expect_rc 1
expect_has "--netdev requires --mode linux"
end_case

# 下面几条在 linux 模式下解析，参数检查发生在读内核和 initramfs 之前，
# 所以拿任意可读文件顶替就够了，不需要 guest 内核。
# 参数错误必须在创建 VM 之前报出：输出里不能出现 KVM 初始化的日志。
run_case "没有网卡引用的 --netdev 是错误" \
    "$VMM" --mode linux --netdev tap,id=n0,ifname=t0 \
    --initrd "$ROOT/Makefile" "$ROOT/Makefile"
expect_rc 1
expect_has "--netdev tap,id=n0,ifname=t0: not used by any --device (a NIC needs netdev=n0)"
expect_not "KVM API"
end_case

run_case "--netdev 的属性名拼错：启动前报错并列出合法属性" \
    "$VMM" --mode linux --netdev tap,id=n0,ifnmae=t0 \
    --initrd "$ROOT/Makefile" "$ROOT/Makefile"
expect_rc 1
expect_has "--netdev tap,id=n0,ifnmae=t0: unknown property 'ifnmae' (valid: type, id, ifname)"
expect_not "KVM API"
end_case

run_case "--netdev 的类型不存在" \
    "$VMM" --mode linux --netdev user,id=n0 \
    --initrd "$ROOT/Makefile" "$ROOT/Makefile"
expect_rc 1
expect_has "unknown type 'user' (see --netdev help)"
end_case

run_case "--netdev help 列出可用的类型" "$VMM" --netdev help
expect_rc 0
expect_re '^  tap +host TAP interface'
end_case

}

manual_guide() {
guide "预先建好一个普通用户可以打开的 TAP" \
"sudo ip tuntap add dev tap0 mode tap user \$(id -un)
sudo ip addr add 10.0.2.1/24 dev tap0
sudo ip link set tap0 up" \
"之后 --netdev tap,id=n0,ifname=tap0 不需要 root。没有预先建好时，
vmm 报 'Operation not permitted' 并给出同样的建议命令"
}

main() {
    local mode="${1:-all}"

    case "$mode" in
    -m|--manual) manual_guide; return 0 ;;
    -a|--auto)   ;;
    all|"")      ;;
    *) echo "用法: $0 [-a 只跑自动 | -m 只看人工指引]"; return 2 ;;
    esac

    need_kvm
    need_build
    auto_tests

    local rc=0
    summary || rc=1

    if [ "$mode" != "-a" ] && [ "$mode" != "--auto" ]; then
        manual_guide
    fi
    return $rc
}

main "${1:-all}"
