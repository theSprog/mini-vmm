#!/usr/bin/env bash
# tests/e2e/monitor.sh — 里程碑 monitor 的验收
#
#   --monitor PATH 在 unix socket 上提供 HMP 风格的按行文本命令：
#   info status、info cpus、info registers、info exit-stats、cpu、help、quit
#   错误命令、超长行、半行断开、超出连接数都只影响那一个连接，guest 照常运行
#   quit 让 VMM 以退出码 0 退出并删除 socket 文件
#
# 用法：
#   VMLINUX=/path/to/vmlinux ./tests/e2e/monitor.sh       全部
#   VMLINUX=/path/to/vmlinux ./tests/e2e/monitor.sh -a    只跑自动用例
#   ./tests/e2e/monitor.sh -m                             只看人工指引
#
# 判据的来源：
#   info cpus 的 thread_id 和 /proc/<pid>/task/<tid>/comm 对照，名字必须是
#   对应的 vcpuN（smp.c 给 vCPU 线程起的名字），不是只看格式。
#   info registers 的 rip 必须落在 vmlinux 的 _stext.._etext 之间：guest
#   停在 shell 提示符时所有 vCPU 都在内核的 idle 循环里；CR0.PG 和 EFER.LMA
#   置位说明读到的是 64 位内核运行中的真实状态，而不是上电复位值。
#   exit 计数在两次查询之间增长，中间 guest 往串口打印了一行（PIO exit），
#   证明 info exit-stats 读的是运行中的实时计数。
#   绑在 CPU 1 上空转的进程让 vCPU 1 线程在 host 上一直可运行，host 状态
#   必须是 R；空闲 vCPU 停在 KVM 内部的 HLT 里睡眠，状态是 S。
# 协议层面的边界（拆包、CRLF、超长行的边界、异步完成的顺序）由单元测试
# monitor_test 逐字节验证；这里验证真实 socket、真实 vCPU 线程两边对得上。

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

SERIAL64="$ROOT/build/serial64.bin"
INITRD="$ROOT/build/initramfs.cpio.gz"
VMLINUX="${VMLINUX:-}"
SHELL_PY="$ROOT/tests/guest_shell.py"
# 每条回复的兜底超时：正常回复是毫秒级，10 秒只在 VMM 卡死时触发
CLIENT="python3 $ROOT/tests/monitor_client.py --timeout 10"
SMP=2
MEM=512M

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SOCK="$WORK/mon.sock"

# 断言一个条件，失败时打印说明。expect_* 表达不了的判据用。
check() {
    local what="$1"; shift
    if ! "$@"; then
        printf '   %sFAIL%s %s\n' "$C_RED" "$C_RST" "$what"
        CUR_BAD=1
    fi
}

# 在 path 上留下一个没有进程监听的 socket 文件，模拟上一次没有正常退出
make_stale_socket() {
    python3 -c 'import socket, sys
s = socket.socket(socket.AF_UNIX); s.bind(sys.argv[1]); s.close()' "$1"
}

# ------------------------------------------------------------------
auto_tests() {

head1 "socket 路径"

: > "$WORK/regular"
run_case "路径上是普通文件：拒绝启动，文件保留" \
    "$VMM" --mode long64 --monitor "$WORK/regular" "$SERIAL64"
expect_rc 1
expect_has "--monitor $WORK/regular: exists and is not a socket, refusing to replace it"
check "the regular file is still there" test -f "$WORK/regular"
end_case

LONGPATH="$WORK/$(printf 'p%.0s' $(seq 1 120))"
run_case "路径超过 sun_path 的长度：拒绝启动" \
    "$VMM" --mode long64 --monitor "$LONGPATH" "$SERIAL64"
expect_rc 1
expect_has "path too long for a unix socket (max 107 bytes)"
end_case

make_stale_socket "$SOCK"
run_case "上次留下的 socket 文件被替换，VMM 退出时删除" \
    "$VMM" --mode long64 --monitor "$SOCK" "$SERIAL64"
expect_rc 0
expect_has "monitor listening on $SOCK"
check "socket file removed at exit" test ! -e "$SOCK"
end_case

if [ -z "$VMLINUX" ] || [ ! -f "$VMLINUX" ]; then
    skip_case "BusyBox guest 用例" "run tools/build_guest_kernel.sh or set VMLINUX=/path/to/vmlinux"
    return
fi
initrd_ready "$INITRD" || {
    skip_case "initramfs" "tools/build_initramfs.sh failed"
    return
}
local text_lo text_hi
text_lo=$(nm "$VMLINUX" | awk '$3 == "_stext" { print $1 }')
text_hi=$(nm "$VMLINUX" | awk '$3 == "_etext" { print $1 }')

head1 "guest 运行中查询（$SMP vCPU）"

run_case "状态、vCPU、寄存器、实时 exit 统计；错误输入不影响 guest" \
    python3 "$SHELL_PY" --timeout 120 \
        --host "$CLIENT $SOCK 'info status' @check-cpus help info 'cpu 1' 'info registers' 'cpu 9' 'cpu x' frob 'info frob' 'info cpus extra' @long:5000 'info status'" \
        --host "$CLIENT $SOCK @check-regs:$text_lo:$text_hi @repeat:200:'info registers -a'" \
        --host "$CLIENT $SOCK @stats-save:$WORK/stats" \
        --cmd 'echo exits-$((6*7))' \
        --host "$CLIENT $SOCK @stats-grew:$WORK/stats" \
        --cmd 'taskset 2 sh -c "while :; do :; done" & echo spin-started' \
        --host "$CLIENT $SOCK 'info cpus' @repeat:100:'info registers -a'" \
        --cmd 'kill $!; echo spin-stopped' \
        --host "$CLIENT $SOCK '@partial:info sta'" \
        --host "$CLIENT $SOCK @clients:5" \
        --cmd 'echo still-alive-$((40+2))' \
        --stop 'poweroff -f' \
        -- "$VMM" --mode linux --smp "$SMP" --mem "$MEM" --monitor "$SOCK" \
              --initrd "$INITRD" "$VMLINUX"
expect_rc 0
# info status
expect_re '^  VM status: running$'
expect_re "^  vcpus: $SMP, memory: 512 MiB, uptime: [0-9]+\.[0-9]+ s, resets: 0$"
# info cpus：thread_id 是真实的 vcpuN 线程；空闲 vCPU 在 KVM_RUN 里睡眠
expect_has "CHECK cpus ok $SMP CPUs, each thread_id is the matching vcpuN thread"
expect_re '^  \* CPU #0: thread_id=[0-9]+ state=guest host=[RS] exits=[0-9]+ last=[A-Z_]+$'
expect_re '^    CPU #1: thread_id=[0-9]+ state=guest host=[RS] exits=[0-9]+ last=[A-Z_]+$'
expect_re 'CPU #[01]: thread_id=[0-9]+ state=guest host=S '
# help 和 info 由注册表生成
expect_re '^  info registers \[-a\] +show the current CPU'"'"'s registers'
expect_re '^  cpu <index> +'
expect_re '^  quit +'
expect_re '^  info exit-stats +'
# cpu 切换当前 CPU，info registers 跟着变
expect_re '^mon> cpu 1$'
expect_re '^  === vCPU 1 state ===$'
expect_has "ERR no such CPU 9 (this VM has $SMP)"
expect_has "ERR invalid CPU index 'x'"
expect_has "ERR unknown command 'frob' (try 'help')"
expect_has "ERR unknown info command 'frob' (try 'info')"
expect_has "ERR info cpus: too many arguments (usage: info cpus)"
expect_has "ERR line too long (max 1024 bytes)"
# 寄存器：64 位内核里，rip 在内核代码段；反复读不影响 guest
expect_has "CHECK regs ok $SMP vCPUs in long mode, rip inside kernel text"
expect_has "info registers -a (x200): 200 OK, 0 ERR"
# 实时 exit 统计
expect_has "exits-42"
expect_re '^CHECK stats-grew ok [0-9]+ -> [0-9]+$'
# 空转的 vCPU 1 在 host 上一直可运行；读寄存器把它踢出 guest 再放回去
expect_re '^    CPU #1: thread_id=[0-9]+ state=(guest|vmm) host=R '
expect_has "info registers -a (x100): 100 OK, 0 ERR"
expect_has "spin-stopped"
# 半行断开、超出连接数之后 monitor 和 guest 都还在
expect_has "client 4: accepted"
expect_has "client 5: rejected (ERR too many monitor clients (max 4))"
expect_has "still-alive-42"
expect_not "TIMEOUT"
check "socket file removed at exit" test ! -e "$SOCK"
end_case

head1 "quit"

run_case "quit：VMM 退出码 0，连接被关闭，socket 文件删除，不打印 vCPU 现场" \
    python3 "$SHELL_PY" --timeout 60 \
        --cmd 'echo booted' \
        --stop-host "$CLIENT $SOCK 'info status' quit @wait-close" \
        -- "$VMM" --mode linux --smp "$SMP" --mem "$MEM" --monitor "$SOCK" \
              --initrd "$INITRD" "$VMLINUX"
expect_rc 0
expect_re '^mon> quit$'
expect_has "mon: connection closed by VMM"
expect_has "monitor: quit requested"
expect_not "=== vCPU"
expect_not "TIMEOUT"
check "socket file removed at exit" test ! -e "$SOCK"
end_case
}

manual_guide() {
guide "用 socat 手动连上 monitor" \
"./build/vmm --mode linux --smp 2 --mem 512M --monitor /tmp/mon.sock \\
    --initrd build/initramfs.cpio.gz guest-kernel/vmlinux
# 另一个终端
socat READLINE UNIX-CONNECT:/tmp/mon.sock" \
"输入 help 列出命令；info cpus 里空闲 vCPU 的 host 列是 S，guest 里跑
'while :; do :; done' 之后对应 vCPU 变成 R；info registers 的 rip 随
guest 在做的事变化"
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
