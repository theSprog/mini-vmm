#!/usr/bin/env bash
# tests/e2e/emu-net.sh — 里程碑 emu-net 的验收
#
#   --netdev tap 后端的命令行约束；e1000 在真实 guest 里探测、连通、
#   TCP 双向传输、INTx 计数，以及 --trace-mmio 记录的寄存器访问
#
# 用法：
#   ./tests/e2e/emu-net.sh       全部
#   ./tests/e2e/emu-net.sh -a    只跑自动用例
#   ./tests/e2e/emu-net.sh -m    只看人工指引
#
# 后端的收发和流控由单元测试 netdev_test、e1000 的寄存器语义和 offload
# 由 e1000_test 验证；这里验证真实的 vmm 程序和真实的 Linux e1000 驱动。
#
# guest 用例不需要 root：脚本用 `unshare -rn` 把自己再执行一次，进入一个
# 新的 user + net namespace，在里面建 TAP（10.0.2.1）、起对端
# tests/net_peer.py，再启动 VMM（guest 是 10.0.2.15）。namespace 随进程
# 退出消失，不碰 host 的网络配置。

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

INITRD="$ROOT/build/initramfs.cpio.gz"
NET_PEER="$ROOT/tests/net_peer.py"
TRACE_DECODE="$ROOT/tools/trace_decode.py"

HOST_IP=10.0.2.1
GUEST_IP=10.0.2.15
GUEST_MAC=52:54:00:ab:cd:ef
# e1000 BAR0 的地址由 VMM 分配（src/pci/pci.c，MMIO hole 起点），ICR 在偏移 0xC0
ICR_GPA=c00000c0
UPLOAD_MIB=16
# UDP 洪泛的包数。host 发得比 guest 收得快时，guest 的接收环会被用完，
# 前端返回 FULL、后端暂停读 TAP。驱动默认的 256 项环在 guest 跟得上时
# 可能一次都不满，所以 guest 启动时用 e1000.RxDescriptors=48（驱动允许的
# 最小值，E1000_MIN_RXD）把环缩小：两次补充缓冲之间 guest 往往要处理上百
# 帧，48 项的环必然被用完。小环也让每条用例都多次走到 RDH 的回绕
RX_DESCS=48
FLOOD_PKTS=200000
# host 侧发洪泛的脚本：每个包 18 字节负载，发往 guest 上没有人监听的端口。
# 发送缓冲调大，sendto 不会因为 socket 缓冲满而失败
FLOOD_PY='import socket, sys
n = int(sys.argv[2])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 << 20)
for i in range(n):
    s.sendto(b"x" * 18, (sys.argv[1], 9))
print(f"FLOOD-SENT-{n}")'
# 洪泛期间 host 内核自己丢掉、VMM 根本读不到的包数：TAP 的发送方向（发往
# guest）先经 qdisc，再进 tun 的 ptr_ring（长度是 txqueuelen，默认 1000）。
# VMM 因 guest 环满暂停读 TAP 时 ptr_ring 会满，tun_net_xmit 直接丢包并记进
# 接口的 tx dropped（SKB_DROP_REASON_FULL_RING）；qdisc 自己的丢包记在 tc 的
# drops 里。guest 慢的时候（host 负载高）两千来个包这样丢掉是正常的，它们
# 不是 VMM 丢的，判据里要扣掉
HOSTDROP_PY='import json, subprocess
link = json.loads(subprocess.check_output(["ip", "-j", "-s", "link", "show", "tap0"]))
qd = json.loads(subprocess.check_output(["tc", "-j", "-s", "qdisc", "show", "dev", "tap0"]))
print("HOSTDROP-%d" % (link[0]["stats64"]["tx"]["dropped"] + sum(q.get("drops", 0) for q in qd)))'

# 从 /proc/net/snmp 取 Tcp 的 InCsumErrors：第一行 Tcp: 是列名，第二行是值。
# host 和 guest（busybox awk）用同一段脚本。
SNMP_CSUM_AWK='/^Tcp:/ { if (!h) { for (i = 1; i <= NF; i++) if ($i == "InCsumErrors") c = i; h = 1 } else print $c }'

# ------------------------------------------------------------------
# 在新的 user + net namespace 里执行：建 TAP、起对端、跑 guest。
# 输出全部写到 stdout，由外层的用例断言；trace 写进 $1 目录。
in_netns() {
    local work="$1" ready up peer_out

    ip link set lo up &&
    ip tuntap add dev tap0 mode tap &&
    ip link set tap0 up &&
    ip addr add "$HOST_IP/24" dev tap0 || { echo "NETNS-SETUP-FAILED"; return 1; }

    # 对端在两个端口都 listen 之后才打印 ready，读到这一行再启动 guest
    coproc PEER { exec python3 -u "$NET_PEER" --addr "$HOST_IP" --timeout 120; }
    # coproc 一退出 bash 就清掉 PEER 数组，而对端恰好在打印完 upload 那一行后
    # 退出，所以先把读端复制到自己的 fd 上
    exec {peer_out}<&"${PEER[0]}"
    if ! read -r -t 60 -u "$peer_out" ready; then
        echo "PEER-NOT-READY"
        return 1
    fi
    echo "HOST $ready"

    # 链路 up 由驱动的 watchdog 异步上报，按 carrier 状态本身等待，上限 10 秒
    python3 "$GUEST_SHELL" --timeout 150 \
        --cmd 'dmesg | grep -a e1000' \
        --cmd "ip link set eth0 up; ip addr add $GUEST_IP/24 dev eth0; for i in \$(seq 100); do [ \"\$(cat /sys/class/net/eth0/carrier 2>/dev/null)\" = 1 ] && break; sleep 0.1; done; echo CARRIER-\$(cat /sys/class/net/eth0/carrier)" \
        --cmd 'dmesg | grep -a "Link is Up"' \
        --cmd "ping -c 3 -W 2 $HOST_IP" \
        --host "ping -c 3 -W 2 $GUEST_IP" \
        --cmd "echo DOWN-\$(wget -q -O - http://$HOST_IP:8080/blob | sha256sum | cut -c1-64)" \
        --cmd "dd if=/dev/urandom of=/tmp/up bs=1M count=$UPLOAD_MIB 2>/dev/null; echo UPSRC-\$(sha256sum /tmp/up | cut -c1-64)" \
        --cmd "nc $HOST_IP 9000 < /tmp/up; echo NC-RC-\$?" \
        --cmd "echo GUEST-TCP-CSUM-ERRORS-\$(awk '$SNMP_CSUM_AWK' /proc/net/snmp)" \
        --cmd 'B=$(cat /sys/class/net/eth0/statistics/rx_packets); echo FLOOD-RXB-$B' \
        --host "ip neigh replace $GUEST_IP lladdr $GUEST_MAC dev tap0 && python3 -I -c '$FLOOD_PY' $GUEST_IP $FLOOD_PKTS" \
        --cmd "i=0; while [ \$((\$(cat /sys/class/net/eth0/statistics/rx_packets) - B)) -lt $FLOOD_PKTS ] && [ \$i -lt 100 ]; do sleep 0.1; i=\$((i + 1)); done; echo FLOOD-RXA-\$(cat /sys/class/net/eth0/statistics/rx_packets)" \
        --host "python3 -I -c '$HOSTDROP_PY'" \
        --cmd 'grep -a eth0 /proc/interrupts; echo IRQ-$(awk "/eth0/{print \$2}" /proc/interrupts)' \
        --cmd 'cat /sys/class/net/eth0/statistics/rx_packets /sys/class/net/eth0/statistics/tx_packets | tr "\n" " " | sed "s/^/PKTS-/"; echo' \
        --stop 'poweroff -f' -- \
        "$VMM" --mode linux --mem 512M \
        --netdev tap,id=n0,ifname=tap0 --device "e1000,netdev=n0,mac=$GUEST_MAC" \
        --append "e1000.RxDescriptors=$RX_DESCS" \
        --trace-mmio "$work/mmio.txt" \
        --initrd "$INITRD" "$VMLINUX"
    echo "GUEST-RC-$?"
    echo "HOST-TCP-CSUM-ERRORS-$(awk "$SNMP_CSUM_AWK" /proc/net/snmp)"

    if read -r -t 30 -u "$peer_out" up; then
        echo "HOST $up"
    else
        echo "PEER-NO-UPLOAD"
    fi
}

# 同一次启动的输出给多个用例断言，每个用例只看自己关心的那几行
BOOT_OUT=""
BOOT_RC=0

check_case() {
    CUR_NAME="$1"
    printf '%s->%s %s\n' "$C_CYA" "$C_RST" "$CUR_NAME"
    CUR_BAD=0
    CUR_OUT="$BOOT_OUT"
    CUR_RC=$BOOT_RC
}

# 从输出里取出 "<前缀><值>" 的值，例如 field DOWN- 取 guest 算出的哈希
field() {
    printf '%s\n' "$CUR_OUT" | grep -ao -- "$1[0-9a-f]*" | tail -n1 | cut -c$((${#1} + 1))-
}

# 两个值必须非空且相等
expect_same() {
    if [ -z "$2" ] || [ "$2" != "$3" ]; then
        printf '   %sFAIL%s %s: "%s" != "%s"\n' "$C_RED" "$C_RST" "$1" "$2" "$3"
        CUR_BAD=1
    fi
}

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

guest_tests
}

guest_tests() {
if [ -z "$VMLINUX" ] || [ ! -f "$VMLINUX" ]; then
    skip_case "guest 内核用例" "run tools/build_guest_kernel.sh or set VMLINUX=/path/to/vmlinux"
    return
fi
if ! unshare -rn true 2>/dev/null; then
    skip_case "guest 网络用例" "unprivileged user namespaces are disabled (sysctl kernel.unprivileged_userns_clone / user.max_user_namespaces)"
    return
fi
if [ ! -f "$INITRD" ]; then
    "$ROOT/tools/build_initramfs.sh" "$INITRD" >/dev/null || {
        skip_case "initramfs" "tools/build_initramfs.sh failed"
        return
    }
fi

local work
work="$(mktemp -d)"
head1 "e1000 在真实 guest 里收发（unshare -rn，TAP $HOST_IP <-> guest $GUEST_IP）"
printf '   $ unshare -rn %s --in-netns %s\n' "$0" "$work"
BOOT_OUT="$(unshare -rn bash "$0" --in-netns "$work" 2>&1 </dev/null)"
BOOT_RC=$?

check_case "驱动探测：EEPROM 里的 MAC 就是 mac= 配置的，链路 1000M 全双工"
expect_has "GUEST-RC-0"
expect_has "e1000 0000:00:01.0 eth0: (PCI:33MHz:32-bit) $GUEST_MAC"
expect_has "e1000 0000:00:01.0 eth0: Intel(R) PRO/1000 Network Connection"
expect_has "CARRIER-1"
expect_has "NIC Link is Up 1000 Mbps Full Duplex"
expect_not "unimplemented register"
end_case

check_case "guest 和 host 互相 ping 通"
expect_has "3 packets transmitted, 3 packets received, 0% packet loss"
expect_re "^3 packets transmitted, 3 received, 0% packet loss"
end_case

check_case "host -> guest TCP：64 MiB 随机数据，guest 端哈希与 host 一致"
expect_same "sha256" "$(field DOWN-)" "$(field blob_sha256=)"
end_case

check_case "guest -> host TCP：${UPLOAD_MIB} MiB 随机数据（走 TSO 和校验和 offload），host 端哈希与 guest 一致"
expect_has "NC-RC-0"
expect_has "HOST upload bytes=$((UPLOAD_MIB << 20)) "
expect_same "sha256" "$(field UPSRC-)" "$(field "upload bytes=$((UPLOAD_MIB << 20)) sha256=")"
end_case

# 哈希一致还不够：校验和错的段会被对端丢弃，发送方超时后按 MSS 重传，
# 重传的单段不走 TSO，校验和由普通路径算对，数据最后照样完整到达，只是
# 慢了一个数量级。所以两端的 TCP 校验和错误计数都必须是 0（namespace
# 是新建的，host 侧计数只来自这一次传输）。
check_case "TCP 校验和错误计数：host 和 guest 都为 0（offload 算出的校验和被对端接受）"
expect_same "host InCsumErrors" "$(field HOST-TCP-CSUM-ERRORS-)" 0
expect_same "guest InCsumErrors" "$(field GUEST-TCP-CSUM-ERRORS-)" 0
end_case

# 洪泛之后等 guest 的 rx_packets 增量达到发出的包数，最多等 10 秒（guest 里
# 没有“收完了”的信号，计数器本身就是要等的状态）。host 内核丢了包时增量
# 永远到不了，这一等会用满 10 秒，之后 host 才读丢包计数（读早了，qdisc
# 里还排着的包之后仍可能被丢）。
# 判据是 增量 >= 发出数 - host 侧丢包数（见 HOSTDROP_PY）。增量可以略多，
# 多出来的是同时段的 ARP、IPv6 等流量。前端在环满时如果不返回 FULL 而是
# 悄悄丢帧，增量会少于这个下限，guest 和 host 里也都没有任何计数记下丢失。
# 后端的暂停次数必须大于 0：否则环从没满过，这条用例没有测到流控
local rxb rxa pauses hostdrop
rxb="$(printf '%s\n' "$BOOT_OUT" | grep -ao 'FLOOD-RXB-[0-9][0-9]*' | tail -n1 | cut -c11-)"
rxa="$(printf '%s\n' "$BOOT_OUT" | grep -ao 'FLOOD-RXA-[0-9][0-9]*' | tail -n1 | cut -c11-)"
pauses="$(printf '%s\n' "$BOOT_OUT" | grep -ao 'netdev n0: rx [0-9]* frames, [0-9]* pauses' | tail -n1 | awk '{print $6}')"
hostdrop="$(printf '%s\n' "$BOOT_OUT" | grep -ao 'HOSTDROP-[0-9][0-9]*' | tail -n1 | cut -c10-)"
check_case "host -> guest UDP 洪泛 $FLOOD_PKTS 个包：除 host 内核丢的 ${hostdrop:-?} 个外 guest 一个不少地收到（$rxb -> $rxa），后端因环满暂停过（$pauses 次）"
expect_has "FLOOD-SENT-$FLOOD_PKTS"
if ! [ -n "$rxb" ] || ! [ -n "$rxa" ] || ! [ -n "$hostdrop" ] ||
   [ $((rxa - rxb)) -lt $((FLOOD_PKTS - hostdrop)) ]; then
    printf '   %sFAIL%s want rx_packets to grow by at least %d - %s (dropped by the host kernel)\n' \
        "$C_RED" "$C_RST" "$FLOOD_PKTS" "${hostdrop:-?}"
    CUR_BAD=1
fi
if ! [ "${pauses:-0}" -gt 0 ] 2>/dev/null; then
    printf '   %sFAIL%s backend never paused: the RX ring did not fill, flow control was not exercised\n' "$C_RED" "$C_RST"
    CUR_BAD=1
fi
end_case

# e1000_intr() 每次被调用先读一次 ICR，所以 INTx 送达的次数 ≈ ICR 读的次数；
# 多出来的几次是驱动在复位和关中断路径上主动清 ICR。NAPI 把一批包合并到
# 一次中断里处理，所以中断数不超过收发包数之和。
local irqs icr_reads pkts
irqs="$(field IRQ-)"
icr_reads="$(python3 "$TRACE_DECODE" "$work/mmio.txt" --addr "$ICR_GPA" 2>/dev/null | grep -c ' R4 ')"
pkts="$(printf '%s\n' "$BOOT_OUT" | grep -ao 'PKTS-[0-9]* [0-9]*' | tail -n1 | cut -c6- | awk '{print $1 + $2}')"
check_case "INTx 经 IOAPIC 送达：/proc/interrupts 计数 ($irqs) 与 ICR 读次数 ($icr_reads)、收发包数 ($pkts) 对得上"
expect_re "^ *17: .*IO-APIC +17-fasteoi +eth0"
if ! [ "${irqs:-0}" -gt 0 ] 2>/dev/null || ! [ "${icr_reads:-0}" -ge "$irqs" ] ||
   [ "$icr_reads" -gt $((irqs + 16)) ] || ! [ "$irqs" -le "${pkts:-0}" ]; then
    printf '   %sFAIL%s want 0 < irqs <= icr_reads <= irqs + 16 and irqs <= packets\n' "$C_RED" "$C_RST"
    CUR_BAD=1
fi
end_case

run_case "--trace-mmio 记下了 e1000 BAR0 的寄存器访问" \
    python3 "$TRACE_DECODE" "$work/mmio.txt" --dev e1000.bar0 --summary
expect_rc 0
expect_re "^e1000.bar0 +$ICR_GPA +R4 +[0-9]+"
expect_re "^e1000.bar0 +c0003818 +W4 +[0-9]+"
end_case

rm -rf "$work"
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
    --in-netns) in_netns "$2"; return ;;
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
