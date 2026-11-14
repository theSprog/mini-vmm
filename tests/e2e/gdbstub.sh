#!/usr/bin/env bash
# tests/e2e/gdbstub.sh — 里程碑 gdbstub 的验收
#
#   没改过的 gdb 经 TCP 连上 VMM，按 vmlinux 的符号在 start_kernel 停下
#   硬件断点、软件断点、单步、继续、Ctrl-C；读写寄存器和内核虚拟地址的内存
#   每个 vCPU 是一个线程；停下时所有 vCPU 线程都停在 gdbstub 里，没有谁还在 KVM_RUN
#   软件断点打开期间 guest 自己的 int3（启动时的 int3_selftest）照常交还给 guest
#   detach 后 guest 继续跑；guest 自己结束时 gdb 收到退出报告
#
# 用法：
#   ./tests/e2e/gdbstub.sh       全部
#   ./tests/e2e/gdbstub.sh -a    只跑自动用例
#   ./tests/e2e/gdbstub.sh -m    只看人工指引
#
# 包格式、寄存器布局、断点表、DR7 编码、KVM_EXIT_DEBUG 的归类由单元测试
# gdb_test 逐字节验证；这里验证真实的 gdb 和真实的 KVM 两边都对得上。
# gdb 一侧的检查写在 tests/gdb_checks.py，VMM 和 gdb 的启动与协调由
# tests/gdb_session.py 完成。

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

INITRD="$ROOT/build/initramfs.cpio.gz"
VMLINUX="${VMLINUX:-}"
SESSION="$ROOT/tests/gdb_session.py"
CHECKS="$ROOT/tests/gdb_checks.py"
SMP=2
MEM=512M

# tests/gdb_checks.py 的 full 场景依次打出的检查名
FULL_CHECKS="connect threads-per-vcpu hbreak-start_kernel stopped-thread-is-vcpu0
stepi-from-straight-line-insn stepi write-register restore-register
read-memory write-memory restore-memory unmapped-memory-errors
break-kernel_init read-other-vcpu interrupt threads-after-interrupt detach"

# ------------------------------------------------------------------
auto_tests() {

head1 "参数校验"

run_case "--gdb 只接受 0..65535 的端口号" \
    "$VMM" --mode linux --gdb 70000 "$ROOT/build/serial64.bin"
expect_rc 1
expect_has "--gdb must be a TCP port in 0..65535: 70000"
end_case

if ! command -v gdb >/dev/null 2>&1; then
    skip_case "gdb 用例" "gdb not installed"
    return
fi
if [ -z "$VMLINUX" ] || [ ! -f "$VMLINUX" ]; then
    skip_case "gdb 用例" "run tools/build_guest_kernel.sh or set VMLINUX=/path/to/vmlinux"
    return
fi
if [ ! -f "$INITRD" ]; then
    "$ROOT/tools/build_initramfs.sh" "$INITRD" >/dev/null || {
        skip_case "initramfs" "tools/build_initramfs.sh failed"
        return
    }
fi

head1 "gdb 调试 guest 内核"

# 断点地址、单步后的 pc 都和 vmlinux 的符号、反汇编比对，不依赖 DWARF。
# guest shell 打印出提示符后 gdb_session.py 先探测一次 vCPU 线程（running），
# 再让 gdb 发 Ctrl-C；停下之后再探测一次（stopped）
run_case "$SMP vCPU：hbreak、stepi、读写寄存器与内存、break、Ctrl-C、detach 后关机" \
    env GDBCHECK_VCPUS="$SMP" python3 "$SESSION" --timeout 120 \
        --script "$CHECKS" --symbols "$VMLINUX" --stop 'poweroff -f' \
        -- "$VMM" --mode linux --smp "$SMP" --mem "$MEM" --gdb 0 \
              --initrd "$INITRD" "$VMLINUX"
expect_rc 0
for c in $FULL_CHECKS; do
    expect_has "GDBCHECK ok $c"
done
expect_has "GDBCHECK done, 0 failed"
expect_not "GDBCHECK FAIL"
# 正对照：guest 在跑时 vCPU 线程在 KVM_RUN 里（ioctl）或正占着 CPU
expect_re '^\[probe running\] vcpu0 syscall=(ioctl|running)$'
expect_re '^\[probe running\] vcpu1 syscall=(ioctl|running)$'
# 停下时每个 vCPU 线程都阻塞在 gdbstub 的条件变量上
expect_re '^\[probe stopped\] vcpu0 syscall=futex$'
expect_re '^\[probe stopped\] vcpu1 syscall=futex$'
# 跑过 int3_selftest 时软件断点是打开的，那条 int3 被交还给了 guest；
# 没交还时内核在 alternative.c 里 BUG，到不了 shell
expect_re 'gdbstub: passed [1-9][0-9]* guest #BP'
expect_not "kernel BUG"
expect_has "gdbstub: gdb detached, guest resumed"
expect_has "guest requested poweroff via ACPI S5"
end_case

# rdinit=/nonexistent：内核找不到 init 而 panic，经 i8042 reset 结束。gdb 一直
# 在 continue 里等停止应答，stub 报告 W
run_case "guest 自己结束时 gdb 收到退出报告" \
    env GDBCHECK_SCENARIO=exit python3 "$SESSION" --timeout 60 \
        --script "$CHECKS" --symbols "$VMLINUX" \
        -- "$VMM" --mode linux --smp "$SMP" --mem "$MEM" --gdb 0 \
              --initrd "$INITRD" --append "rdinit=/nonexistent" "$VMLINUX"
expect_rc 0
expect_has "GDBCHECK ok connect"
expect_has "GDBCHECK ok exit-reported"
expect_has "guest requested reset via i8042"
end_case
}

manual_guide() {
guide "交互式调试 guest 内核" \
"build/vmm --mode linux --smp 2 --mem 512M --gdb 1234 --initrd build/initramfs.cpio.gz guest-kernel/vmlinux
# 另一个终端
gdb -iex 'add-auto-load-safe-path guest-kernel' guest-kernel/vmlinux
(gdb) target remote :1234
(gdb) hbreak start_kernel
(gdb) continue
(gdb) next                      # 源码级单步需要内核带 DWARF（tools/guest-kernel.config 已打开）
(gdb) lx-dmesg                  # guest-kernel/vmlinux-gdb.py 提供的内核辅助命令" \
"VMM 打印 'gdbstub: listening on 127.0.0.1:1234'，gdb 连上之前 guest 不跑；
连上时 guest 停在内核入口，页表还是 VMM 建的恒等映射，内核虚拟地址翻译不了，
这时插软件断点会报 Cannot insert breakpoint，用 hbreak"
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

main "$@"
