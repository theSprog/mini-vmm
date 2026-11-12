#!/usr/bin/env bash
# tests/e2e/emu-blk.sh — 里程碑 emu-blk 的验收
#
#   --drive 磁盘后端的命令行约束
#   AHCI 控制器：Linux ahci 驱动探测、端口枚举、链路建立、INTx
#
# 用法：
#   ./tests/e2e/emu-blk.sh       全部
#   ./tests/e2e/emu-blk.sh -a    只跑自动用例
#   ./tests/e2e/emu-blk.sh -m    只看人工指引
#
# 后端的参数、引用、加锁和读写由单元测试 blockdev_test 验证，HBA 寄存器
# 语义由 ahci_test 验证；这里验证真实的 vmm 程序把它们接进了命令行，以及
# guest 里没改过的 Linux ahci 驱动认得这个控制器。

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

INITRD="$ROOT/build/initramfs.cpio.gz"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

BOOT_OUT=""
BOOT_RC=0

boot() {
    BOOT_OUT="$(python3 "$GUEST_SHELL" --timeout 90 "$@" 2>&1)"
    BOOT_RC=$?
}

check_case() {
    CUR_NAME="$1"
    printf '%s->%s %s\n' "$C_CYA" "$C_RST" "$CUR_NAME"
    CUR_BAD=0
    CUR_OUT="$BOOT_OUT"
    CUR_RC=$BOOT_RC
}

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

guest_tests
}

guest_tests() {
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

head1 "AHCI 控制器：探测与端口枚举"

truncate -s 64M "$WORK/disk.img"
# 中断计数的标记由 guest 的命令替换展开生成，回显的命令行里只有 $(...)
boot \
    --cmd "dmesg | grep -aE 'ahci|ata[0-9]'" \
    --cmd "echo AHCI-IRQ-\$(awk '/ahci/ {print \$2}' /proc/interrupts)" \
    --stop 'poweroff -f' \
    -- "$VMM" --mode linux --mem 512M --drive id=d0,file="$WORK/disk.img" \
          --device ahci,drive=d0 --exit-stats - --initrd "$INITRD" "$VMLINUX"

check_case "guest 启动并正常关机"
expect_rc 0
expect_has "guest requested poweroff via ACPI S5"
expect_not "Kernel panic"
expect_not "stopped with error"
end_case

# 驱动按 class 010601 绑定，打印的是它从 VS、CAP、PI 读出的值
check_case "ahci 驱动绑定，CAP/PI/VS 被如实读出"
expect_has "pci 00:01.0 ahci [1234:0003] class 010601"
expect_has "ahci 0000:00:01.0: AHCI 0001.0301 32 slots 6 ports 6 Gbps 0x3f impl SATA mode"
expect_has "ahci 0000:00:01.0: flags: 64bit clo only pio"
expect_has "ata6: SATA max UDMA/133 abar m8192@0xc0000000 port 0xc0000380 irq 17"
end_case

# COMRESET 之后 libata 读 SStatus：有盘的端口 DET=3、Gen3、active，
# 空端口收不到 COMINIT，停在 0
check_case "端口枚举：端口 0 链路建立，端口 1-5 没有设备"
expect_has "ata1: SATA link up 6.0 Gbps (SStatus 133 SControl 300)"
for n in 2 3 4 5 6; do
    expect_has "ata$n: SATA link down (SStatus 0 SControl 300)"
done
end_case

# 命令完成经 PxIS -> IS -> INTx 到 IOAPIC 17 号脚
check_case "命令完成产生 INTx 中断，寄存器访问经 BAR5"
expect_re 'AHCI-IRQ-[1-9][0-9]*'
expect_re '^  ahci\.bar5 +MMIO +[1-9][0-9]* +[1-9][0-9]*$'
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
