#!/usr/bin/env bash
# tests/e2e/guest-memory.sh — 里程碑 guest-memory 的验收
#
#   guest RAM 超过 3GiB：低段 [0, 3GiB) + 高段 [4GiB, ...) 两个 memslot，
#   [3GiB, 4GiB) 留给 MMIO，E820 与 memslot 一一对应，guest 能用上全部 RAM
#
# 用法：
#   VMLINUX=/path/to/vmlinux ./tests/e2e/guest-memory.sh       全部
#   VMLINUX=/path/to/vmlinux ./tests/e2e/guest-memory.sh -a    只跑自动用例
#   ./tests/e2e/guest-memory.sh -m                             只看人工指引
#
# 没设 VMLINUX 时只跑不需要 guest 内核的布局用例。
# 5GiB 的 guest 只按需占用 host 内存（MAP_NORESERVE），启动到 shell 实际
# 碰到的页在百 MiB 量级。

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

SERIAL64="$ROOT/build/serial64.bin"
INITRD="$ROOT/build/initramfs.cpio.gz"
VMLINUX="${VMLINUX:-}"
MEM=5G

# ------------------------------------------------------------------
auto_tests() {

head1 "memslot 布局（裸 payload，不需要 guest 内核）"

# 长模式 payload 只用低端几 MiB，这里看的是 mem_init() 的切分本身
run_case "--mem 3G 正好填满低段，只有一个 memslot" \
    "$VMM" --mode long64 --mem 3G "$SERIAL64"
expect_rc 0
expect_has "memslot 0: GPA [0x0, 0xc0000000)"
expect_not "memslot 1:"
end_case

run_case "--mem $MEM 拆成低段和 4GiB 以上的高段" \
    "$VMM" --mode long64 --mem "$MEM" "$SERIAL64"
expect_rc 0
expect_has "memslot 0: GPA [0x0, 0xc0000000)"
expect_has "memslot 1: GPA [0x100000000, 0x180000000)"
expect_has "guest wrote port 0x10: 0x53"
end_case

if [ -z "$VMLINUX" ] || [ ! -f "$VMLINUX" ]; then
    skip_case "guest 内核用例" "run tools/build_guest_kernel.sh or set VMLINUX=/path/to/vmlinux"
    return
fi

if [ ! -f "$INITRD" ]; then
    "$ROOT/tools/build_initramfs.sh" "$INITRD" >/dev/null || {
        skip_case "initramfs" "tools/build_initramfs.sh failed"
        return
    }
fi

head1 "Linux guest 使用 3GiB 以上的 RAM"

# 判据分三层：
#   VMM 交给内核的 E820 和 memslot 一致（VMM 日志）；
#   内核原样收下了这张表，并把 hole 交给 PCI（dmesg）；
#   高段 RAM 真的进了伙伴系统：zone Normal 从 pfn 0x100000（4GiB）开始，
#   present 正好是高段的 2GiB = 524288 页，MemTotal 超过 4GiB。
# 标记串由 guest 的算术展开生成，回显的命令行里只有 $((...))，不会误匹配。
CUR_NAME="$MEM guest：E820、PCI gap、zone Normal、MemTotal"
printf '%s->%s %s\n' "$C_CYA" "$C_RST" "$CUR_NAME"
CUR_BAD=0
CUR_OUT="$(python3 "$GUEST_SHELL" --timeout 90 \
    --cmd 'grep -A12 "zone   Normal" /proc/zoneinfo | grep -E "spanned|present|managed"' \
    --cmd 'grep start_pfn /proc/zoneinfo' \
    --cmd 'echo MEMTOTAL-$(( $(sed -n "s/^MemTotal: *\([0-9]*\) kB/\1/p" /proc/meminfo) / 1048576 ))GiB' \
    --stop 'poweroff -f' \
    -- "$VMM" --mode linux --mem "$MEM" --initrd "$INITRD" "$VMLINUX" 2>&1)"
CUR_RC=$?
expect_rc 0
expect_has "e820: [0x000000100000, 0x0000c0000000) RAM"
expect_has "e820: [0x000100000000, 0x000180000000) RAM"
expect_has "BIOS-e820: [mem 0x0000000000100000-0x00000000bfffffff] usable"
expect_has "BIOS-e820: [mem 0x0000000100000000-0x000000017fffffff] usable"
expect_has "[mem 0xc0000000-0xffffffff] available for PCI devices"
expect_has "present  524288"
expect_has "start_pfn:           1048576"
expect_has "MEMTOTAL-4GiB"
expect_has "guest requested poweroff via ACPI S5"
expect_not "Kernel panic"
expect_not "stopped with error"
end_case

}

manual_guide() {
guide "看 guest 把哪些早期结构放进了高段" \
"./build/vmm --mode linux --no-reboot --mem $MEM --initrd $INITRD \\
    --append rdinit=/nonexistent \$VMLINUX 2>&1 | grep -a NODE_DATA" \
"NODE_DATA(0) allocated [mem 0x17ffc1000-0x17fffffff] 之类：memblock 自顶向下分配，
最早的分配就落在高段末尾，说明内核启动时已经在读写 4GiB 以上的 memslot"
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
