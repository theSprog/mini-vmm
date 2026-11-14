#!/usr/bin/env bash
# tests/e2e/arch-distro.sh — 里程碑 arch-distro 的验收：未经裁剪的 Arch Linux
#
#   官方 linux 包的 vmlinuz + initramfs 直启，根文件系统在 AHCI 盘上；
#   systemd 进入 running、没有失败的 unit、dmesg 没有探测超时；
#   e1000 经 DHCP 拿到地址，pacman -Syu 从本地仓库装包并校验签名；
#   systemctl reboot 原地复位后再次进入 running；systemctl poweroff 经
#   ACPI S5 退出，host 上 e2fsck 确认文件系统干净。
#
# 用法：
#   ./tests/e2e/arch-distro.sh       全部
#   ./tests/e2e/arch-distro.sh -a    只跑自动用例
#   ./tests/e2e/arch-distro.sh -m    只看人工指引
#
# 镜像由 tools/build_arch_image.sh 生成（要联网，几分钟），默认在 arch-image/，
# ARCH_IMAGE 可以指向别处。没有镜像时 guest 用例 SKIP。每次运行都在 build/
# 下用 cp --sparse=always 复制一份盘来跑，guest 的写入不会留在原镜像里。
# 副本不放 /tmp：常见发行版的 /tmp 是 tmpfs，1 GiB 以上的盘会吃掉内存。
#
# 设置 ARCH_DISTRO_BASELINE=1 时再跑基线：同样配置启动三次，分别空载、
# fio 读盘、iperf3 收发，用 --exit-stats 差分出每次 I/O、每帧的 exit 数。
# 没设置时不跑也不计 SKIP，人工指引里给出开启方式。
#
# 网络全部离线：和 emu-net 一样用 `unshare -rn` 把脚本在新的 user + net
# namespace 里再执行一次，建 TAP（10.0.2.2），起 dnsmasq 作 DHCP 服务器、
# python3 http.server 提供 arch-image/repo 里的本地仓库。guest 里 pacman
# 用一份只含这个仓库的临时配置，不碰真实镜像源，所以结果不随外网和上游
# 版本变化。
#
# 设置 ARCH_DISTRO_ONLINE=1 时再跑一次真实外网的 pacman -Syu（需要 host 装
# passt 包里的 pasta）：guest 用镜像自带的 mirrorlist，即构建时的软件源。
# 结果取决于外网和软件源是否可用，所以不进默认用例；没设置时不跑也不计
# SKIP。

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

IMG="${ARCH_IMAGE:-$ROOT/arch-image}"
HOST_IP=10.0.2.2
GUEST_IP=10.0.2.15
SMP=4
MEM=2G
# MemTotal 不到 --mem：内核镜像、保留区和 crashkernel 之外的部分才算进去。
# 下限只需要证明 2 GiB 的布局被 guest 看到，而不是退回到某个更小的默认值
MIN_MEMKB=1900000
TREE_VERSION="tree v2.3.2"
# 从发出 systemctl reboot/poweroff 到内核打印 "reboot: ..." 的上限（guest
# uptime 秒）。正常是一两秒；systemd 停 unit、给残留进程发 SIGTERM 后的
# 等待上限都是 90 秒，超过 30 秒说明有一步是等到超时才过去的
SHUTDOWN_MAX=30

# 基线负载（ARCH_DISTRO_BASELINE=1 时运行）。fio 用 O_DIRECT 读整块盘，
# 每个 4 KiB 读请求都落到 AHCI 上，不经过页缓存也不需要先铺数据文件；
# --readonly 让 fio 拒绝任何写，读的是 guest 正在用的根盘。fio 默认
# randrepeat=1，每次的偏移序列相同
BASELINE_RUNS=(idle blk net)
FIO_IOS=16384
FIO_CMD="fio --name=randread --filename=/dev/sda --readonly --rw=randread --bs=4k --direct=1 --ioengine=psync --size=1G --io_size=$((FIO_IOS * 4))k"
IPERF_BYTES=64M

# guest dmesg 里允许出现的 err/warn（扩展正则）。每一条都查明了与 VMM 无关，
# 只是允许而不要求出现，换一台 host 时其中几条会消失。不在这里的
# err/warn 一律算失败：VMM 的设备或固件表出问题时，最先表现为驱动或
# ACPI 的一条警告。
DMESG_ALLOWED=(
    # bsp_init_amd() 在 CONSTANT_TSC 的 AMD CPU 上要求 MSR_K7_HWCR 的
    # TscFreqSel（bit 24）为 1。这个 MSR 由 KVM 模拟：6.6 的
    # kvm_set_msr_common() 只接受 0 或 McStatusWrEn（bit 18），VMM 写 bit 24
    # 会被拒绝（KVM_SET_MSRS 返回 0）。换 host 内核后用 KVM_SET_MSRS 写一次
    # 0xC0010015 = 1<<24 看返回值即可重新确认
    "\[Firmware Bug\]: TSC doesn't count with P0 frequency!"
    # srso_select_mitigation() 要 CPUID 0x80000021 的 IBPB_BRTYPE，KVM 只在
    # host 自己有这一位时才提供。host 的
    # /sys/devices/system/cpu/vulnerabilities/spec_rstack_overflow 报
    # "no microcode" 时 guest 必然也报
    "Speculative Return Stack Overflow: IBPB-extending microcode not applied!"
    "Speculative Return Stack Overflow: WARNING: See https://kernel.org/doc/html/latest/admin-guide/hw-vuln/srso.html"
    # Arch 内核开了 CONFIG_INTEL_TDX_HOST，arch/x86/virt/vmx/tdx/tdx.c 的
    # tdx_enable() 在任何没有 TDX 的平台上都打印这一行，不分 CPU 厂商
    "virt/tdx: TDX not supported by the host platform"
    # systemd-networkd 查询 nl80211 genetlink family，内核按别名
    # net-pf-16-proto-16-family-nl80211 自动加载 cfg80211，cfg80211 去读
    # wireless-regdb 包提供的 regulatory.db，镜像里没装这个包
    "regulatory: Direct firmware load for regulatory.db failed with error -2"
)

# ignore_loglevel：内核和 systemd-shutdown 的消息不论级别都打到串口。
# 发行版默认的控制台级别会滤掉它们，关机慢或者卡住时串口上就只剩一串
# 终端转义序列，看不出停在哪一步

# guest 里 pacman 的临时配置：只有本地仓库 mvmm。SigLevel 和镜像里的
# /etc/pacman.conf 相同，包签名照样按 archlinux-keyring 校验
LOCAL_CONF="printf '[options]\\nArchitecture = auto\\nSigLevel = Required DatabaseOptional\\n[mvmm]\\nServer = http://$HOST_IP:8080\\n' > /tmp/local.conf"

# 联网用例 guest 里执行的安装：镜像自带的 /etc/pacman.conf 和 mirrorlist
ONLINE_PACMAN="pacman -Syu --noconfirm tree"

# 每次启动都做的检查，输出带上启动序号 $1：
#   状态、失败的 unit、dmesg 里的超时、systemd-fsck 对根文件系统的结论、boot_id
boot_checks() {
    local n=$1
    BOOT_STEPS+=(
        --cmd "echo STATE$n-\$(systemctl is-system-running --wait)"
        --cmd "systemctl --failed --no-legend --plain | sed 's/^/FAILED$n /'; echo FAILED$n-COUNT-\$(systemctl --failed --no-legend | wc -l)"
        --cmd "dmesg | grep -iE 'timed? ?out' | sed 's/^/TIMEOUT$n /'; echo TIMEOUT$n-COUNT-\$(dmesg | grep -ciE 'timed? ?out')"
        --cmd "journalctl -b -o cat -t systemd-fsck | sed 's/^/FSCK$n /'"
        --cmd "echo BOOTID$n-\$(cat /proc/sys/kernel/random/boot_id)"
    )
}

# ------------------------------------------------------------------
# 在新的 user + net namespace 里建 TAP，起 DHCP 和本地仓库；$2 为 iperf3 时
# 再起 iperf3 服务端（只有基线需要，默认用例不依赖 host 装 iperf3）。
# 服务在 namespace 的 shell 退出时由 EXIT trap 收掉。
netns_services() {
    local work="$1" iperf="${2:-}" pids="" i

    ip link set lo up &&
    ip tuntap add dev tap0 mode tap &&
    ip link set tap0 up &&
    ip addr add "$HOST_IP/24" dev tap0 || { echo "NETNS-SETUP-FAILED"; return 1; }

    # namespace 里的 root 就是当前用户，dnsmasq 不用再降权
    dnsmasq --no-daemon --conf-file=/dev/null --interface=tap0 --bind-interfaces \
        --port=0 --user=root --dhcp-range="$GUEST_IP,$GUEST_IP,255.255.255.0,1h" \
        --dhcp-leasefile="$work/leases" --pid-file= --log-dhcp \
        > "$work/dnsmasq.log" 2>&1 &
    pids="$pids $!"
    (cd "$IMG/repo" && exec python3 -m http.server --bind "$HOST_IP" 8080) \
        > "$work/http.log" 2>&1 &
    pids="$pids $!"
    : > "$work/iperf3.log"
    if [ "$iperf" = iperf3 ]; then
        iperf3 --server --bind "$HOST_IP" > "$work/iperf3.log" 2>&1 &
        pids="$pids $!"
    fi
    # 双引号：pid 在设置时展开，EXIT 时这个局部变量已经不在作用域里
    trap "kill $pids 2>/dev/null" EXIT

    # 这些服务就绪时都不打印固定的一行，直接看 socket 是否已在监听；
    # 上限 10 秒只兜底服务起不来（端口被占、参数错误）
    for i in $(seq 100); do
        [ -n "$(ss -Hlun 'sport = :67')" ] && [ -n "$(ss -Hltn 'sport = :8080')" ] &&
        { [ "$iperf" != iperf3 ] || [ -n "$(ss -Hltn 'sport = :5201')" ]; } && return 0
        sleep 0.1
    done
    echo "SERVICES-NOT-LISTENING"
    cat "$work/dnsmasq.log" "$work/http.log" "$work/iperf3.log"
    return 1
}

# guest 的公共参数：$1 盘，$2 根文件系统 UUID，其余追加给 VMM
vmm_args() {
    local disk="$1" uuid="$2"
    shift 2
    VMM_ARGS=("$VMM" --mode linux --smp "$SMP" --mem "$MEM"
        --device ahci,drive=d0 --drive id=d0,file="$disk"
        --netdev tap,id=n0,ifname=tap0 --device e1000,netdev=n0
        --initrd "$IMG/initramfs-linux.img"
        --append "root=UUID=$uuid rw ignore_loglevel" "$@" "$IMG/vmlinuz-linux")
}

# 默认用例：一次 VMM 运行里两次启动，输出全部写到 stdout，由外层断言
in_netns_distro() {
    local work="$1" disk="$2" uuid="$3"

    netns_services "$work" || return 1

    BOOT_STEPS=()
    boot_checks 1
    BOOT_STEPS+=(
        --cmd "echo ROOT-\$(findmnt -no SOURCE /) CPUS-\$(nproc) MEMKB-\$(awk '/MemTotal/{print \$2}' /proc/meminfo)"
        --cmd "dmesg --level=err,warn | sed 's/^/WARN /'; echo WARN-END"
        --cmd "/usr/lib/systemd/systemd-networkd-wait-online --any -4 --timeout=60; echo ONLINE-RC-\$?; ip -br -4 addr show scope global | sed 's/^/ADDR /'"
        --cmd "$LOCAL_CONF"
        --cmd "pacman --config /tmp/local.conf -Syu --noconfirm tree; echo PACMAN-RC-\$?"
        --cmd "tree --version | sed 's/^/INSTALLED /'"
        --reboot "echo UPTIME1-\$(cut -d' ' -f1 /proc/uptime); systemctl reboot"
    )
    boot_checks 2
    BOOT_STEPS+=(--cmd "tree --version | sed 's/^/AFTER-REBOOT /'")

    vmm_args "$disk" "$uuid"
    python3 "$GUEST_SHELL" --timeout 300 --login root "${BOOT_STEPS[@]}" \
        --stop "echo UPTIME2-\$(cut -d' ' -f1 /proc/uptime); systemctl poweroff" -- \
        "${VMM_ARGS[@]}"
    echo "GUEST-RC-$?"
    echo "DHCP-LOG:"
    grep -a "DHCPACK" "$work/dnsmasq.log"
    echo "HTTP-LOG:"
    cat "$work/http.log"
}

# 基线：同样的配置启动三次，只差 guest 里跑不跑负载。--exit-stats 的计数
# 从 VMM 启动累积到退出，包含启动、DHCP、关机这些每次都有的部分，两次
# 运行相减后剩下的才是负载本身引起的。三次而不是两次，是为了让 KVM 层
# 不分设备的计数器（总 exit 数、中断注入数）也能分别归到块和网络上。
# 每次都从原镜像复制一份新盘，三次启动看到的文件系统状态相同。
in_netns_baseline() {
    local work="$1" uuid="$2" run disk

    netns_services "$work" iperf3 || return 1

    for run in "${BASELINE_RUNS[@]}"; do
        disk="$work/bl-$run.img"
        cp --sparse=always "$IMG/disk.img" "$disk"
        BOOT_STEPS=(--cmd "/usr/lib/systemd/systemd-networkd-wait-online --any -4 --timeout=60; echo ONLINE-RC-\$?")
        case "$run" in
        blk) BOOT_STEPS+=(--cmd "$FIO_CMD | sed 's/^/FIO /'; echo FIO-RC-\${PIPESTATUS[0]}") ;;
        net) BOOT_STEPS+=(
                --cmd "iperf3 -c $HOST_IP -n $IPERF_BYTES | sed 's/^/IPERF-TX /'; echo IPERF-TX-RC-\${PIPESTATUS[0]}"
                --cmd "iperf3 -c $HOST_IP -n $IPERF_BYTES -R | sed 's/^/IPERF-RX /'; echo IPERF-RX-RC-\${PIPESTATUS[0]}") ;;
        esac
        vmm_args "$disk" "$uuid" --exit-stats "$work/bl-$run.stats"
        python3 "$GUEST_SHELL" --timeout 300 --login root "${BOOT_STEPS[@]}" \
            --stop "systemctl poweroff" -- "${VMM_ARGS[@]}" > "$work/bl-$run.out" 2>&1
        echo "BL-$run-RC-$?"
        rm -f "$disk"
    done
}

# 联网：在 pasta 建的 user + net namespace 里运行（pasta 是外层 namespace 的
# 创建者，脚本由它再执行一次）。namespace 里只有 pasta 的 tap 接口 pst0，
# 另一端是 pasta 进程：它把收到的帧翻译成 host 上普通的 TCP/UDP socket 收发，
# 并且自己应答 DHCP，把 host 默认路由接口的地址、网关和 DNS 分给对端。
# VMM 的 tap0 和 pst0 挂进同一个 bridge，guest 发出的帧就到了 pasta，guest
# 拿到的地址和 host 相同。不需要 dnsmasq，也不需要在 namespace 里做 NAT。
in_pasta_online() {
    local disk="$1" uuid="$2"

    ip link set lo up &&
    ip tuntap add dev tap0 mode tap &&
    ip link add br0 type bridge &&
    ip link set pst0 master br0 &&
    ip link set tap0 master br0 &&
    ip link set br0 up &&
    ip link set pst0 up &&
    ip link set tap0 up || { echo "BRIDGE-SETUP-FAILED"; ip -br link; return 1; }

    vmm_args "$disk" "$uuid"
    python3 "$GUEST_SHELL" --timeout 300 --login root \
        --cmd "/usr/lib/systemd/systemd-networkd-wait-online --any -4 --timeout=60; echo ONLINE-RC-\$?; ip -br -4 addr show scope global | sed 's/^/ADDR /'" \
        --cmd "grep -h '^Server' /etc/pacman.d/mirrorlist | sed 's/^/MIRROR /'" \
        --cmd "$ONLINE_PACMAN; echo PACMAN-RC-\$?" \
        --cmd "tree --version | sed 's/^/INSTALLED /'" \
        --stop "systemctl poweroff" -- "${VMM_ARGS[@]}"
    echo "GUEST-RC-$?"
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

# 内核打印 "$1" 那一行的时间戳（秒），取第一个
klog_time() {
    printf '%s\n' "$CUR_OUT" | grep -ao "\[ *[0-9]*\.[0-9]*\] $1" | head -n1 | grep -o '[0-9]*\.[0-9]*'
}

# 关机耗时：发命令时的 uptime 字段 $1 到内核打印 $2 的时间
expect_shutdown_quick() {
    local t0 t1
    t0="$(field "$1")"
    t1="$(klog_time "$2")"
    if [ -z "$t0" ] || [ -z "$t1" ] ||
       ! awk -v a="$t0" -v b="$t1" -v m="$SHUTDOWN_MAX" 'BEGIN { exit !(b - a >= 0 && b - a < m) }'; then
        printf '   %sFAIL%s "%s" at %ss, command sent at %ss: not within %ss\n' \
            "$C_RED" "$C_RST" "$2" "$t1" "$t0" "$SHUTDOWN_MAX"
        CUR_BAD=1
    else
        printf '   shutdown took %.1fs\n' "$(awk -v a="$t0" -v b="$t1" 'BEGIN { print b - a }')"
    fi
}

# 从输出里取出 "<前缀><值>" 的值（到空白为止），取最后一个
field() {
    printf '%s\n' "$CUR_OUT" | grep -ao -- "$1[^[:space:]]*" | tail -n1 | cut -c$((${#1} + 1))-
}

# ------------------------------------------------------------------
auto_tests() {

local f
for f in disk.img vmlinuz-linux initramfs-linux.img repo/mvmm.db; do
    if [ ! -f "$IMG/$f" ]; then
        skip_case "Arch guest 用例" "$IMG/$f missing: run tools/build_arch_image.sh (needs network) or set ARCH_IMAGE=/path"
        return
    fi
done
if ! unshare -rn true 2>/dev/null; then
    skip_case "Arch guest 用例" "unprivileged user namespaces are disabled (sysctl kernel.unprivileged_userns_clone / user.max_user_namespaces)"
    return
fi
if ! command -v dnsmasq >/dev/null; then
    skip_case "Arch guest 用例" "dnsmasq not installed (pacman -S dnsmasq)"
    return
fi

local work disk uuid
mkdir -p "$ROOT/build"
work="$(mktemp -d "$ROOT/build/arch-distro.XXXXXX")"
WORK="$work"
trap 'rm -rf "$WORK"' EXIT
disk="$work/disk.img"
cp --sparse=always "$IMG/disk.img" "$disk"
# 根文件系统的 UUID 从镜像本身读，不和构建脚本各写一份
uuid="$(blkid -p -o value -s UUID "$disk")"

head1 "Arch Linux：启动、网络装包、reboot、poweroff（unshare -rn，TAP $HOST_IP <-> guest $GUEST_IP）"
printf '   $ unshare -rn %s --in-netns %s %s %s\n' "$0" "$work" "$disk" "$uuid"
BOOT_OUT="$(unshare -rn bash "$0" --in-netns "$work" "$disk" "$uuid" 2>&1 </dev/null)"
BOOT_RC=$?

check_case "直启官方内核和 initramfs，根文件系统在 AHCI 盘上，systemd 进入 running"
expect_has "GUEST-RC-0"
expect_has "STATE1-running"
expect_has "ROOT-/dev/sda "
expect_re "^Arch Linux [^ ]+ \(ttyS0\)"
end_case

check_case "没有失败的 unit（两次启动）"
expect_has "FAILED1-COUNT-0"
expect_has "FAILED2-COUNT-0"
end_case

check_case "dmesg 里没有超时（两次启动）"
expect_has "TIMEOUT1-COUNT-0"
expect_has "TIMEOUT2-COUNT-0"
end_case

check_case "dmesg 的 err/warn 都是已查明与 VMM 无关的几条"
local warns w pat ok
# 串口输出的行尾是 \r\n，先去掉 \r 再按整行匹配
warns="$(printf '%s\n' "$CUR_OUT" | tr -d '\r' | grep -a '^WARN \[' | sed 's/^WARN \[ *[0-9.]*\] //')"
if ! printf '%s\n' "$CUR_OUT" | tr -d '\r' | grep -qx 'WARN-END'; then
    printf '   %sFAIL%s the dmesg err/warn listing did not run\n' "$C_RED" "$C_RST"
    CUR_BAD=1
fi
while IFS= read -r w; do
    [ -n "$w" ] || continue
    ok=0
    for pat in "${DMESG_ALLOWED[@]}"; do
        if printf '%s\n' "$w" | grep -qE -- "$pat"; then ok=1; break; fi
    done
    if [ "$ok" -eq 0 ]; then
        printf '   %sFAIL%s unexpected dmesg err/warn: %s\n' "$C_RED" "$C_RST" "$w"
        CUR_BAD=1
    fi
done <<< "$warns"
end_case

check_case "--smp $SMP --mem $MEM：guest 看到 $SMP 个 CPU、MemTotal 不少于 $MIN_MEMKB kB"
expect_has "CPUS-$SMP "
local memkb
memkb="$(field MEMKB-)"
if [ -z "$memkb" ] || [ "$memkb" -lt "$MIN_MEMKB" ]; then
    printf '   %sFAIL%s MemTotal "%s" kB < %d\n' "$C_RED" "$C_RST" "$memkb" "$MIN_MEMKB"
    CUR_BAD=1
fi
end_case

check_case "e1000 经 DHCP 拿到 $GUEST_IP，pacman -Syu 从本地仓库装上 tree，签名校验通过"
expect_has "ONLINE-RC-0"
expect_re "^ADDR +[^ ]+ +UP +$GUEST_IP/24"
expect_re "DHCPACK\(tap0\) $GUEST_IP "
expect_has "PACMAN-RC-0"
expect_has "INSTALLED $TREE_VERSION"
expect_re "\"GET /mvmm.db HTTP/1.1\" 200"
expect_re "\"GET /tree-[^ ]+\.pkg\.tar\.zst HTTP/1.1\" 200"
expect_not "signature from"
expect_not "invalid or corrupted package"
end_case

check_case "systemctl reboot：$SHUTDOWN_MAX 秒内关完，原地复位后第二次启动同样 running，boot_id 变了，装的包还在"
local b1 b2
b1="$(field BOOTID1-)"
b2="$(field BOOTID2-)"
if [ -z "$b1" ] || [ -z "$b2" ] || [ "$b1" = "$b2" ]; then
    printf '   %sFAIL%s boot_id "%s" -> "%s": the second boot did not happen\n' \
        "$C_RED" "$C_RST" "$b1" "$b2"
    CUR_BAD=1
fi
expect_has "STATE2-running"
expect_has "AFTER-REBOOT $TREE_VERSION"
expect_shutdown_quick UPTIME1- "reboot: Restarting system"
end_case

# systemd-fsck-root 在 initrd 里先于挂载运行 e2fsck 的 preen 模式，输出
# 以卷标（arch）开头；上次没有干净卸载时它会回放日志或修复，输出就不再
# 是 clean
check_case "reboot 后 systemd-fsck 报告根文件系统 clean，不需要修复"
expect_re "^FSCK2 [^ ]+: clean, "
expect_not "recovering journal"
end_case

check_case "systemctl poweroff：$SHUTDOWN_MAX 秒内关完，经 ACPI S5 退出，退出码 0"
expect_shutdown_quick UPTIME2- "reboot: Power down"
expect_has "guest requested poweroff via ACPI S5"
expect_has "GUEST-RC-0"
end_case

# -n 只读检查，-f 即使超级块标着 clean 也完整扫一遍。没有干净卸载时
# 超级块带 needs_recovery，e2fsck -n 不回放日志而是报错
run_case "关机后 host 上 e2fsck -fn：文件系统没有错误" e2fsck -fn "$disk"
expect_rc 0
end_case

rm -f "$disk"
if [ "${ARCH_DISTRO_BASELINE:-0}" = 1 ]; then
    if command -v iperf3 >/dev/null; then
        baseline_tests "$uuid"
    else
        skip_case "基线" "iperf3 not installed on the host (pacman -S iperf3)"
    fi
fi
if [ "${ARCH_DISTRO_ONLINE:-0}" = 1 ]; then
    if command -v pasta >/dev/null; then
        online_tests "$work" "$uuid"
    else
        skip_case "真实外网" "pasta not installed on the host (pacman -S passt)"
    fi
fi

}

# ------------------------------------------------------------------
# 基线报告的取数：$1 键，$2 运行名
# --exit-stats 文件：设备行（第二列是 PIO/MMIO）取读加写，其余行取 total 列
bl_stat() {
    awk -v k="$1" '$1 == k { if ($2 == "PIO" || $2 == "MMIO") print $3 + $4; else print $2; exit }' \
        "$WORK/bl-$2.stats"
}
# VMM 退出时打印的后端计数：$1 是 grep -P 的模式，\K 之后的数字
bl_vmm() {
    tr -d '\r' < "$WORK/bl-$2.out" | grep -aoP -- "$1" | tail -n1
}
bl_secs() {
    awk '/^exit-stats:/ { print $4; exit }' "$WORK/bl-$1.stats"
}

# 一行差分：$1 名称，$2 空载运行，$3 负载运行，$4 负载的操作数，其余是取数命令
bl_row() {
    local label="$1" a="$2" b="$3" ops="$4" va vb
    shift 4
    va="$("$@" "$a")"
    vb="$("$@" "$b")"
    awk -v l="$label" -v a="${va:-0}" -v b="${vb:-0}" -v o="$ops" 'BEGIN {
        printf "   %-26s %12d %12d %12d %10.2f\n", l, a, b, b - a, (o > 0 ? (b - a) / o : 0) }'
}

bl_header() {
    printf '   %-26s %12s %12s %12s %10s\n' "" idle "$1" delta "per $2"
}

online_tests() {
    local work="$1" uuid="$2" disk="$1/online.img"

    head1 "真实外网：pasta 把 namespace 接到 host 网络，guest 从镜像自带的软件源 pacman -Syu（ARCH_DISTRO_ONLINE=1）"
    cp --sparse=always "$IMG/disk.img" "$disk"
    printf '   $ pasta -f -q -4 -I pst0 -- %s --in-pasta %s %s\n' "$0" "$disk" "$uuid"
    # -4：镜像的 networkd 只开 DHCPv4。-I pst0：namespace 里接口名固定，
    # 不随 host 的接口名变化
    BOOT_OUT="$(pasta -f -q -4 -I pst0 -- bash "$0" --in-pasta "$disk" "$uuid" 2>&1 </dev/null)"
    BOOT_RC=$?
    rm -f "$disk"

    check_case "pasta 的 DHCP 经 bridge 到达 e1000，guest 拿到地址"
    expect_has "ONLINE-RC-0"
    expect_re "^ADDR +[^ ]+ +UP +[0-9.]+/[0-9]+"
    end_case

    check_case "$ONLINE_PACMAN 从镜像自带的软件源装上 tree，签名校验通过，poweroff 后 VMM 退出码 0"
    expect_has "PACMAN-RC-0"
    expect_has "INSTALLED $TREE_VERSION"
    expect_not "signature from"
    expect_not "invalid or corrupted package"
    expect_has "guest requested poweroff via ACPI S5"
    expect_has "GUEST-RC-0"
    end_case
}

baseline_tests() {
    local uuid="$1" run ops tx rx min_frames

    head1 "基线：fio 和 iperf3 的固定负载，--exit-stats 三次运行差分（ARCH_DISTRO_BASELINE=1）"
    printf '   $ unshare -rn %s --in-netns-baseline %s %s\n' "$0" "$WORK" "$uuid"
    BOOT_OUT="$(unshare -rn bash "$0" --in-netns-baseline "$WORK" "$uuid" 2>&1 </dev/null)"
    BOOT_RC=$?
    for run in "${BASELINE_RUNS[@]}"; do
        [ -f "$WORK/bl-$run.out" ] && BOOT_OUT="$BOOT_OUT
$(sed "s/^/$run| /" "$WORK/bl-$run.out")"
    done

    check_case "三次启动都正常关机，fio 读完 $FIO_IOS 次，iperf3 两个方向各传完 $IPERF_BYTES"
    for run in "${BASELINE_RUNS[@]}"; do
        expect_has "BL-$run-RC-0"
    done
    expect_has "FIO-RC-0"
    expect_has "issued rwts: total=$FIO_IOS,0,0,0"
    expect_has "IPERF-TX-RC-0"
    expect_has "IPERF-RX-RC-0"
    expect_not "ONLINE-RC-1"
    end_case

    # 读请求数由 VMM 的块后端自己计数（每条 AHCI 命令一次），不是 fio 报的
    # 数字。fio 每个 4 KiB 读是一条命令，所以差分应当等于 FIO_IOS；容差 2%
    # 留给两次启动之间 systemd 读盘的差异
    check_case "块：VMM 统计的读请求比空载多出 fio 的 $FIO_IOS 次（容差 2%），列出每次 I/O 的 exit"
    bl_header blk I/O
    bl_row "drive d0 read ops" idle blk "$FIO_IOS" bl_vmm 'drive d0: read \K[0-9]+'
    bl_row "ahci.bar5 MMIO exits" idle blk "$FIO_IOS" bl_stat ahci.bar5
    bl_row "user-space exits" idle blk "$FIO_IOS" bl_stat total
    bl_row "KVM exits" idle blk "$FIO_IOS" bl_stat exits
    bl_row "KVM irq_injections" idle blk "$FIO_IOS" bl_stat irq_injections
    printf '   guest seconds: idle %s, blk %s\n' "$(bl_secs idle)" "$(bl_secs blk)"
    ops=$(( $(bl_vmm 'drive d0: read \K[0-9]+' blk || echo 0) - $(bl_vmm 'drive d0: read \K[0-9]+' idle || echo 0) ))
    if [ "$ops" -lt $((FIO_IOS * 98 / 100)) ] || [ "$ops" -gt $((FIO_IOS * 102 / 100)) ]; then
        printf '   %sFAIL%s read ops delta %d is not within 2%% of %d\n' "$C_RED" "$C_RST" "$ops" "$FIO_IOS"
        CUR_BAD=1
    fi
    end_case

    # 一帧最多 1514 字节（MTU 1500 加以太网头），传完 IPERF_BYTES 至少要
    # 这么多帧；两个方向各有数据帧和对端的 ACK，所以 tx、rx 各自都要达到
    min_frames=$(( $(numfmt --from=iec "$IPERF_BYTES") / 1514 ))
    check_case "网络：tx、rx 各比空载多出至少 $min_frames 帧（$IPERF_BYTES / 1514 字节），列出每帧的 exit"
    tx=$(( $(bl_vmm 'netdev n0: rx \d+ frames, \d+ pauses, \d+ errors; tx \K[0-9]+' net || echo 0) -
           $(bl_vmm 'netdev n0: rx \d+ frames, \d+ pauses, \d+ errors; tx \K[0-9]+' idle || echo 0) ))
    rx=$(( $(bl_vmm 'netdev n0: rx \K[0-9]+' net || echo 0) - $(bl_vmm 'netdev n0: rx \K[0-9]+' idle || echo 0) ))
    bl_header net frame
    bl_row "netdev n0 tx frames" idle net $((tx + rx)) bl_vmm 'netdev n0: rx \d+ frames, \d+ pauses, \d+ errors; tx \K[0-9]+'
    bl_row "netdev n0 rx frames" idle net $((tx + rx)) bl_vmm 'netdev n0: rx \K[0-9]+'
    bl_row "netdev n0 rx pauses" idle net $((tx + rx)) bl_vmm 'netdev n0: rx \d+ frames, \K[0-9]+'
    bl_row "e1000.bar0 MMIO exits" idle net $((tx + rx)) bl_stat e1000.bar0
    bl_row "user-space exits" idle net $((tx + rx)) bl_stat total
    bl_row "KVM exits" idle net $((tx + rx)) bl_stat exits
    bl_row "KVM irq_injections" idle net $((tx + rx)) bl_stat irq_injections
    # iperf3 自己报的速率不打印：iperf3 3.22 在 -n 模式下把耗时取整到整秒
    # （host 回环上 iperf3 -c 127.0.0.1 -n 64M 也报 0.00-1.00 sec），
    # 速率只是 IPERF_BYTES 除以这个整数
    printf '   guest seconds: idle %s, net %s\n' "$(bl_secs idle)" "$(bl_secs net)"
    if [ "$tx" -lt "$min_frames" ] || [ "$rx" -lt "$min_frames" ]; then
        printf '   %sFAIL%s frames delta tx %d, rx %d: below %d\n' "$C_RED" "$C_RST" "$tx" "$rx" "$min_frames"
        CUR_BAD=1
    fi
    end_case
}

manual_guide() {
guide "生成镜像（需要联网，不需要 root）" \
"tools/build_arch_image.sh                    # 默认用 Arch Linux Archive 当天的快照
tools/build_arch_image.sh -d 2026/11/14      # 指定快照日期
tools/build_arch_image.sh -m 'https://mirrors.tuna.tsinghua.edu.cn/archlinux/\$repo/os/\$arch'" \
"产物在 arch-image/：disk.img、vmlinuz-linux、initramfs-linux.img、packages.txt、repo/"

guide "可复现：同一个快照日期再构建一次，包集合和版本逐字一致" \
"mkdir -p build/repro && cp -al arch-image/cache build/repro/cache   # 复用已下载的包
tools/build_arch_image.sh -o build/repro       # 和 arch-image/ 用同一个 -d（默认值相同即可）
diff arch-image/packages.txt build/repro/packages.txt && cmp arch-image/vmlinuz-linux build/repro/vmlinuz-linux
ARCH_IMAGE=\$PWD/build/repro ./tests/e2e/arch-distro.sh -a
unshare --map-auto --map-root-user --setuid 0 --setgid 0 rm -rf build/repro" \
"diff、cmp 没有输出，新镜像上的用例全过。disk.img 本身不逐字节相同（ext4 时间戳和
目录哈希种子每次随机），等价性以 packages.txt 和用例结果为准。最后一步经 user
namespace 删除，因为 rootfs 里属于 guest 系统用户的文件在 host 上属于 subuid"

guide "基线：fio 和 iperf3 固定负载下每次 I/O、每帧的 exit 数（多三次启动，约 1.5 分钟，需要 host 装 iperf3）" \
"ARCH_DISTRO_BASELINE=1 ./tests/e2e/arch-distro.sh -a" \
"默认用例之后多三个基线用例，各打印一张 idle / 负载 / delta / 每次操作的表"

guide "真实外网：guest 经 pasta 从镜像自带的软件源 pacman -Syu（需要 host 装 passt，结果取决于外网）" \
"ARCH_DISTRO_ONLINE=1 ./tests/e2e/arch-distro.sh -a" \
"默认用例之后多两个联网用例；guest 拿到的地址和 host 默认路由接口的地址相同（pasta 的分配方式）"

guide "交互式启动（串口登录 root，空密码）" \
"cp --sparse=always arch-image/disk.img /path/to/copy.img
build/vmm --mode linux --smp 4 --mem 2G --device ahci,drive=d0 --drive id=d0,file=/path/to/copy.img \\
    --initrd arch-image/initramfs-linux.img \\
    --append \"root=UUID=\$(blkid -p -o value -s UUID /path/to/copy.img) rw\" arch-image/vmlinuz-linux" \
"出现 'mini-vmm-arch login:'，输入 root 进入 bash；systemctl poweroff 后 VMM 退出"
}

main() {
    local mode="${1:-all}"

    case "$mode" in
    --in-netns) in_netns_distro "$2" "$3" "$4"; return ;;
    --in-netns-baseline) in_netns_baseline "$2" "$3"; return ;;
    --in-pasta) in_pasta_online "$2" "$3"; return ;;
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
