#!/usr/bin/env bash
# tests/e2e/event-loop.sh — 里程碑 event-loop 的验收
#
#   后端 fd 由一个 epoll I/O 线程负责，串口输入经它进入 guest；
#   vCPU 线程只跑 KVM_RUN，进程里没有别的阻塞读线程
#
# 用法：
#   VMLINUX=/path/to/vmlinux ./tests/e2e/event-loop.sh        全部
#   VMLINUX=/path/to/vmlinux ./tests/e2e/event-loop.sh -a     只跑自动用例
#   ./tests/e2e/event-loop.sh -m                              只看人工指引
#
# 没设 VMLINUX 时，需要 guest 内核的用例 SKIP。
# I/O 线程本身的派发与停机契约在 tests/unit/event_loop_test.c 里测。

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

INITRD="$ROOT/build/initramfs.cpio.gz"
VMLINUX="${VMLINUX:-}"
MEM=512M

# 在 host 上列出 VMM 的每个线程：线程名和它睡在哪个内核函数里
LIST_THREADS='for t in /proc/$VMM_PID/task/*; do echo "thread $(cat $t/comm) $(cat $t/wchan)"; done'

# ------------------------------------------------------------------
auto_tests() {

if [ -z "$VMLINUX" ] || [ ! -f "$VMLINUX" ]; then
    skip_case "event-loop guest 用例" "run tools/build_guest_kernel.sh or set VMLINUX=/path/to/vmlinux"
    return
fi
if [ ! -f "$INITRD" ]; then
    "$ROOT/tools/build_initramfs.sh" "$INITRD" >/dev/null || {
        skip_case "initramfs" "tools/build_initramfs.sh failed"
        return
    }
fi

head1 "I/O 线程"

# guest 停在 shell 提示符时看线程：主线程在 join，I/O 线程在 epoll_wait，
# 每个 vCPU 一个线程，除此之外没有别的线程。输入经 I/O 线程送进串口，
# 命令能执行（EVL-42 是 guest 里算出来的，回显的命令行里只有 $((6*7))），
# Ctrl-A x 也由 I/O 线程识别。
# wchan 是内核函数名：6.x 上 epoll_wait 睡在 do_epoll_wait，ep_poll 没被
# 内联的内核上显示 ep_poll，两种都接受。
run_case "threads at the shell prompt: vmm, io, one per vCPU; input and Ctrl-A x via io" \
    python3 "$GUEST_SHELL" --timeout 60 --cmd 'echo EVL-$((6*7))' \
        --host "$LIST_THREADS" --stop $'\x01x' \
        -- "$VMM" --mode linux --smp 2 --mem "$MEM" --initrd "$INITRD" "$VMLINUX"
expect_rc 0
expect_has "EVL-42"
names="$(printf '%s\n' "$CUR_OUT" | grep -a '^thread ' | awk '{print $2}' | sort | tr '\n' ' ')"
if [ "$names" != "io vcpu0 vcpu1 vmm " ]; then
    printf '   %sFAIL%s thread names are "%s", want "io vcpu0 vcpu1 vmm "\n' \
        "$C_RED" "$C_RST" "$names"
    CUR_BAD=1
fi
expect_any "thread io do_epoll_wait" "thread io ep_poll"
expect_has "Ctrl-A x, terminating"
expect_has "left guest (ok)"
end_case

head1 "stdin 不能提供输入时 guest 照常运行"

# run_case 把 stdin 接到 /dev/null，epoll 不能监听它（EPERM）。要明确说出
# 输入被关掉了，而不是悄悄没有输入。rdinit=/nonexistent 让内核 panic，
# panic=-1 立即经 i8042 reset 退出，整个过程不需要输入。
run_case "stdin is /dev/null: input disabled with a reason, guest still boots" \
    timeout 60 "$VMM" --mode linux --mem "$MEM" --initrd "$INITRD" \
        --append "rdinit=/nonexistent" "$VMLINUX"
expect_rc 0
expect_has "console: stdin cannot be polled (regular file or /dev/null), guest input is disabled"
expect_has "guest requested reset via i8042"
end_case

# 管道写端一开始就关掉：I/O 线程读到 EOF 后必须摘掉 stdin，否则水平触发
# 会让它一直报可读、一直读到 0，I/O 线程空转。EOF 日志恰好一条就是判据。
run_case "stdin EOF: handler removes stdin once, io thread stops cleanly" \
    sh -c 'true | timeout 60 "$@"' _ "$VMM" -v --mode linux --mem "$MEM" \
        --initrd "$INITRD" --append "rdinit=/nonexistent" "$VMLINUX"
expect_rc 0
n_eof="$(printf '%s\n' "$CUR_OUT" | grep -ac 'console: stdin EOF')"
if [ "$n_eof" != 1 ]; then
    printf '   %sFAIL%s "console: stdin EOF" logged %s times, want 1\n' \
        "$C_RED" "$C_RST" "$n_eof"
    CUR_BAD=1
fi
expect_has "io thread: stopped"
expect_has "guest requested reset via i8042"
end_case

}

manual_guide() {
head1 "人工验收"

guide "终端里交互：raw 模式、Ctrl-C 送给 guest、Ctrl-A x 退出" \
"./build/vmm --mode linux --mem 512M --initrd build/initramfs.cpio.gz \$VMLINUX
# 在 guest shell 里：sleep 100 然后按 Ctrl-C；最后按 Ctrl-A x" \
"sleep 被 Ctrl-C 打断，回到提示符，VMM 没有退出；
Ctrl-A x 后打印 '[vmm] Ctrl-A x, terminating'，终端恢复正常回显"
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
