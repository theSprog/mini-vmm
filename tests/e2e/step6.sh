#!/usr/bin/env bash
# tests/step6.sh — Step 6 验收
#
#   Step 6.1  SMP：每 vCPU 一个线程、CPUID 拓扑、AP 经 INIT/SIPI 启动、
#             任一 vCPU 触发的停机能让所有 vCPU 线程退出
#
# 用法：
#   VMLINUX=/path/to/vmlinux ./tests/step6.sh        全部
#   VMLINUX=/path/to/vmlinux ./tests/step6.sh -a     只跑自动用例
#   ./tests/step6.sh -m                              只看人工指引
#
# 可选环境变量：
#   SMP_MANY=16   多 vCPU 用例的 vCPU 数（默认 16，上限 VMM_MAX_VCPUS）
#
# 没设 VMLINUX 时，需要 guest 内核的用例 SKIP。

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

SERIAL64="$ROOT/build/serial64.bin"
INITRD="$ROOT/build/initramfs.cpio.gz"
VMLINUX="${VMLINUX:-}"
MEM=512M
SMP=4
# 并行启动（guest 的 CONFIG_HOTPLUG_PARALLEL）会把所有 AP 一起踢醒，它们
# 随后一起抢实模式跳板的那把锁（linux-6.6 arch/x86/realmode/rm/
# trampoline_64.S 的 tr_lock）。4 个 vCPU 压不到这条路径，用一个大一些的
# 数再跑一遍。VMM_MAX_VCPUS 是 32，host 物理核少于这个数也没关系，超卖
# 只是慢一点。
SMP_MANY="${SMP_MANY:-16}"
MEM_MANY=1024M

# 同 step2.sh：等 shell 起来后逐条发送，最后 poweroff。
# vCPU 多的时候启动慢一些，用 FEED_WAIT 把首次等待拉长。
feed_shell() {
    sleep "${FEED_WAIT:-6}"
    local c
    for c in "$@"; do
        printf '%s\n' "$c"
        sleep 1
    done
    printf 'poweroff -f\n'
    sleep 5
}

# ------------------------------------------------------------------
auto_tests() {

head1 "Step 6.1 — 参数校验"

run_case "--smp > 1 只能配合 --mode linux" \
    "$VMM" --mode long64 --smp 2 "$SERIAL64"
expect_rc 1
expect_has "--smp > 1 requires --mode linux"
end_case

run_case "--smp 越界给出明确报错" \
    "$VMM" --mode linux --smp 0 "$SERIAL64"
expect_rc 1
expect_has "--smp must be an integer in 1..32"
end_case

if [ -z "$VMLINUX" ] || [ ! -f "$VMLINUX" ]; then
    skip_case "Step 6.1 guest 用例" "set VMLINUX=/path/to/vmlinux to enable"
    return
fi

head1 "Step 6.1 — SMP 启动"

# Linux 模式必须带 --initrd
if [ ! -f "$INITRD" ]; then
    "$ROOT/tools/build_initramfs.sh" "$INITRD" >/dev/null || {
        skip_case "initramfs" "tools/build_initramfs.sh failed"
        return
    }
fi

# panic 发生在哪个 vCPU 上不确定，i8042 reset 由它触发，其余 vCPU
# 必须被 vm_request_stop() 踢出 KVM_RUN，否则 VMM 挂住直到 timeout。
# rdinit=/nonexistent 让内核在 initramfs 里找不到 init，从而 VFS panic
run_case "$SMP vCPU 启动到 VFS panic 并经 i8042 reset 全部退出" \
    timeout 60 "$VMM" --mode linux --smp "$SMP" --mem "$MEM" \
        --initrd "$INITRD" --append "rdinit=/nonexistent" "$VMLINUX"
expect_rc 0
expect_has "MP table @ GPA 0x9fc00: $SMP CPU(s), IOAPIC id $SMP"
expect_has "$SMP vCPU thread(s) running"
expect_has "smpboot: Allowing $SMP CPUs, 0 hotplug CPUs"
expect_has "smp: Brought up 1 node, $SMP CPUs"
expect_has "guest requested reset via i8042"
# AP 唤醒失败、APIC ID 和固件表对不上、TSC 不同步的典型报错
expect_not "failed to report alive state"
expect_not "do_boot_cpu failed"
expect_not "APIC id mismatch"
expect_not "TSC synchronization"
expect_not "stopped with error"
expect_not "SHUTDOWN"
end_case

CUR_NAME="$SMP vCPU initramfs：nproc + 拓扑 + poweroff"
printf '%s->%s %s\n' "$C_CYA" "$C_RST" "$CUR_NAME"
CUR_BAD=0
# 标记串都由 guest 的命令替换生成，回显的命令行里只有 $(...)，
# 不会误匹配
CUR_OUT="$(feed_shell \
    'echo NPROC-$(nproc)' \
    'echo APIC-$(sed -n "s/^apicid.*: //p" /proc/cpuinfo | tr "\n" -)' \
    'echo CORE-$(sed -n "s/^core id.*: //p" /proc/cpuinfo | tr "\n" -)' \
    'echo PKG-$(sed -n "s/^physical id.*: //p" /proc/cpuinfo | sort -u | tr "\n" -)' \
    'echo L3-$(cat /sys/devices/system/cpu/cpu0/cache/index3/shared_cpu_list)' \
    'echo L2-$(cat /sys/devices/system/cpu/cpu1/cache/index2/shared_cpu_list)' \
    | timeout 60 "$VMM" --mode linux --smp "$SMP" --mem "$MEM" \
          --initrd "$INITRD" "$VMLINUX" 2>&1)"
CUR_RC=$?
expect_rc 0
expect_has "mini-vmm initramfs: Linux 6."
expect_has "NPROC-$SMP"
# 拓扑：1 socket × 4 core × 1 thread，APIC ID = vcpu id
expect_has "APIC-0-1-2-3-"
expect_has "CORE-0-1-2-3-"
expect_has "PKG-0-"
expect_has "L3-0-3"
expect_has "L2-1"
expect_has "guest requested poweroff via ACPI S5"
expect_not "Kernel panic"
expect_not "stopped with error"
end_case

head1 "Step 6.1 — 多 vCPU（$SMP_MANY）"

CUR_NAME="$SMP_MANY vCPU：全部上线 + LLC 域 + poweroff"
printf '%s->%s %s\n' "$C_CYA" "$C_RST" "$CUR_NAME"
CUR_BAD=0
CUR_OUT="$(FEED_WAIT=10 feed_shell \
    'echo NPROC-$(nproc)' \
    'echo PKG-$(sed -n "s/^physical id.*: //p" /proc/cpuinfo | sort -u | tr "\n" -)' \
    'echo L3A-$(cat /sys/devices/system/cpu/cpu0/cache/index3/shared_cpu_list)' \
    "echo L3B-\$(cat /sys/devices/system/cpu/cpu$(( SMP_MANY - 1 ))/cache/index3/shared_cpu_list)" \
    | timeout 120 "$VMM" --mode linux --smp "$SMP_MANY" --mem "$MEM_MANY" \
          --initrd "$INITRD" "$VMLINUX" 2>&1)"
CUR_RC=$?
expect_rc 0
expect_has "$SMP_MANY vCPU thread(s) running"
expect_has "smpboot: Allowing $SMP_MANY CPUs, 0 hotplug CPUs"
expect_has "smp: Brought up 1 node, $SMP_MANY CPUs"
expect_has "NPROC-$SMP_MANY"
expect_has "PKG-0-"
# AP 争跳板锁失败、APIC ID 对不上、TSC 不同步的典型报错
expect_not "failed to report alive state"
expect_not "do_boot_cpu failed"
expect_not "APIC id mismatch"
expect_not "TSC synchronization"
expect_not "Kernel panic"
expect_not "stopped with error"
# LLC 域：VMM 在 CPUID 0x8000001D 里报的是所有 vCPU 共享 L3，但海光
# model < 5 的 guest 内核根本不看这个字段，而是直接
#   per_cpu(cpu_llc_id, cpu) = c->apicid >> 3   (cacheinfo_hygon_init_llc_id)
# 于是按 8 个 APIC ID 一组切开。两种结果都是已知且正确的行为，用
# expect_any 同时接受；VMM 侧对应 smp.c 的 smp_warn_llc_split()。
expect_any "L3A-0-$(( SMP_MANY - 1 ))" "L3A-0-7"
expect_has "guest requested poweroff via ACPI S5"
end_case

}

# ------------------------------------------------------------------
manual_guide() {

head1 "人工验收"

guide "多线程 workload 真的跑在多个 vCPU 上" \
"./build/vmm --mode linux --smp 4 --mem 512M --initrd build/initramfs.cpio.gz \$VMLINUX
# guest 里：
for i in 1 2 3 4; do (while :; do :; done) & done
# 另一个 host 终端：
top -H -p \$(pgrep -x vmm)" \
"host 上 vcpu0..vcpu3 四个线程各自接近 100%；guest 里
grep '^cpu[0-9]' /proc/stat 四个 CPU 的累计时间同步增长。"

guide "Ctrl-A x 能停下所有 vCPU" \
"./build/vmm --mode linux --smp 4 --mem 512M --initrd build/initramfs.cpio.gz \$VMLINUX
# shell 起来后按 Ctrl-A x" \
"VMM 立即退出（不会卡在某个 AP 的 HLT 里），依次打印 vCPU 0..3 的现场，
终端恢复 cooked 模式。"

guide "AP 在收到 SIPI 之前不占 CPU" \
"./build/vmm --mode linux --smp 4 --mem 512M --append 'maxcpus=1' \\
    --initrd build/initramfs.cpio.gz \$VMLINUX
top -H -p \$(pgrep -x vmm)" \
"guest 只启动 BSP，vcpu1..vcpu3 线程阻塞在 KVM 里（KVM_MP_STATE_UNINITIALIZED），
CPU 占用为 0；guest 里 nproc 为 1。"

}

# ------------------------------------------------------------------
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
