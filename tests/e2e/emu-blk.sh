#!/usr/bin/env bash
# tests/e2e/emu-blk.sh — 里程碑 emu-blk 的验收
#
#   --drive 磁盘后端的命令行约束
#
# 用法：
#   ./tests/e2e/emu-blk.sh       全部
#   ./tests/e2e/emu-blk.sh -a    只跑自动用例
#   ./tests/e2e/emu-blk.sh -m    只看人工指引
#
# 后端的参数、引用、加锁和读写由单元测试 blockdev_test 验证；这里验证真实
# 的 vmm 程序把 --drive 接进了命令行。

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

# ------------------------------------------------------------------
auto_tests() {

head1 "命令行约束"

run_case "--drive 只能和 --mode linux 一起用" \
    "$VMM" --mode long64 --drive id=d0,file=x.img "$ROOT/build/serial64.bin"
expect_rc 1
expect_has "--drive requires --mode linux"
end_case

# 下面几条在 linux 模式下解析，参数检查发生在读内核和 initramfs 之前，
# 所以拿任意可读文件顶替就够了，不需要 guest 内核。镜像文件在启动阶段
# 才打开，这里写一个不存在的名字也不影响。
# 参数错误必须在创建 VM 之前报出：输出里不能出现 KVM 初始化的日志。
run_case "没有控制器引用的 --drive 是错误" \
    "$VMM" --mode linux --drive id=d0,file=x.img \
    --initrd "$ROOT/Makefile" "$ROOT/Makefile"
expect_rc 1
expect_has "--drive id=d0,file=x.img: not used by any --device (a disk controller needs drive=d0)"
expect_not "KVM API"
end_case

run_case "--drive 的属性名拼错：启动前报错并列出合法属性" \
    "$VMM" --mode linux --drive id=d0,flie=x.img \
    --initrd "$ROOT/Makefile" "$ROOT/Makefile"
expect_rc 1
expect_has "--drive id=d0,flie=x.img: unknown property 'flie' (valid: id, file, format, readonly)"
expect_not "KVM API"
end_case

run_case "--drive 只支持 raw 格式" \
    "$VMM" --mode linux --drive id=d0,file=x.img,format=qcow2 \
    --initrd "$ROOT/Makefile" "$ROOT/Makefile"
expect_rc 1
expect_has "unsupported format 'qcow2' (only raw)"
end_case

run_case "两个 --drive 用了同一个 id" \
    "$VMM" --mode linux --drive id=d0,file=a.img --drive id=d0,file=b.img \
    --initrd "$ROOT/Makefile" "$ROOT/Makefile"
expect_rc 1
expect_has "--drive id=d0,file=b.img: duplicate id 'd0' (also used by --drive id=d0,file=a.img)"
end_case
}

manual_guide() {
guide "准备一个 raw 镜像" \
"truncate -s 64M disk.img          # 全零的空盘
mke2fs -q -t ext2 -d DIR disk.img   # 或者直接带上 DIR 里的文件，不需要 root" \
"--drive id=d0,file=disk.img 就能用；镜像在 VM 运行期间加锁，
另一个 vmm 同时打开会报 'is locked by another process'"
}

main() {
    local mode="${1:-all}"

    case "$mode" in
    -m|--manual) manual_guide; return 0 ;;
    -a|--auto)   ;;
    all|"")      ;;
    *) echo "用法: $0 [-a 只跑自动 | -m 只看人工指引]"; return 2 ;;
    esac

    need_build
    auto_tests

    local rc=0
    summary || rc=1

    if [ "$mode" != "-a" ] && [ "$mode" != "--auto" ]; then
        manual_guide
    fi
    return $rc
}

main "$@"
