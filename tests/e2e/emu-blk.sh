#!/usr/bin/env bash
# tests/e2e/emu-blk.sh — 里程碑 emu-blk 的验收
#
#   --drive 磁盘后端的命令行约束
#   AHCI 控制器：Linux ahci 驱动探测、端口枚举、链路建立、INTx
#   ATA 命令：IDENTIFY、DMA 读写、FLUSH，host 侧核对镜像；从盘启动
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
# 控制器也有 MSI capability，pci=nomsi 让 guest 不申请 MSI，这次启动验证的
# 是 INTx 这条路（MSI 由 pci-msi suite 验证）
boot \
    --cmd "dmesg | grep -aE 'ahci|ata[0-9]'" \
    --cmd "echo AHCI-IRQ-\$(awk '/ahci/ {print \$2}' /proc/interrupts)" \
    --stop 'poweroff -f' \
    -- "$VMM" --mode linux --mem 512M --drive id=d0,file="$WORK/disk.img" \
          --device ahci,drive=d0 --exit-stats - --append pci=nomsi \
          --initrd "$INITRD" "$VMLINUX"

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
expect_has "ahci 0000:00:01.0: flags: 64bit ncq clo only pio"
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
expect_not "MSI enabled"
expect_re '^  ahci\.bar5 +MMIO +[1-9][0-9]* +[1-9][0-9]*$'
end_case

data_tests
noncq_tests
ncq_error_tests
root_tests
}

# 两个控制器各接一块盘：d0 是 host 用 mke2fs -d 做好的 ext2 镜像，guest
# 只读挂载后读；d1 是空盘，guest 自己 mkfs、写文件、卸载，关机后 host 用
# debugfs 从镜像里取出文件核对。两边的判据都是 sha256，数据的生产者和
# 校验者分处 VMM 两侧，中间任何一处 LBA、PRD 或方向弄错都对不上。
data_tests() {
head1 "AHCI 控制器：guest 读写磁盘，host 核对镜像"

local src="$WORK/src" blob_sha d0_sha wr_sha
mkdir -p "$src"
head -c 4M /dev/urandom > "$src/blob"
blob_sha="$(sha256sum < "$src/blob" | cut -d' ' -f1)"
mke2fs -q -F -t ext2 -d "$src" "$WORK/d0.img" 64M
d0_sha="$(sha256sum < "$WORK/d0.img" | cut -d' ' -f1)"
truncate -s 32M "$WORK/d1.img"

# 单条命令不能超过 255 字节（见 guest_shell.py），所以拆成几条；
# 每条的标记都由命令替换展开生成，回显的命令行里不会出现展开后的值
boot \
    --cmd "dmesg | grep -aE 'ata[0-9]+\.00|\[sd[ab]\]'" \
    --cmd "echo QD-\$(cat /sys/block/sda/device/queue_depth)" \
    --cmd "echo RAW-\$(dd if=/dev/sda bs=1M iflag=direct 2>/dev/null | sha256sum)" \
    --cmd "mkdir -p /mnt /m2 && mount -o ro -t ext2 /dev/sda /mnt" \
    --cmd "echo RD-\$(sha256sum < /mnt/blob)" \
    --cmd "mkfs.ext2 /dev/sdb >/dev/null && mount -t ext2 /dev/sdb /m2" \
    --cmd "dd if=/dev/urandom of=/m2/up bs=1M count=8 2>/dev/null" \
    --cmd "echo WR-\$(sha256sum < /m2/up)" \
    --cmd "umount /m2 && umount /mnt && echo UMOUNT-\$(echo ok)" \
    --stop 'poweroff -f' \
    -- "$VMM" --mode linux --mem 512M \
          --drive id=d0,file="$WORK/d0.img" --device ahci,drive=d0 \
          --drive id=d1,file="$WORK/d1.img" --device ahci,drive=d1 \
          --trace-mmio "$WORK/mmio.txt" --initrd "$INITRD" "$VMLINUX"

check_case "IDENTIFY DEVICE：Linux 认出型号、LBA48、UDMA 和两块盘的容量"
expect_rc 0
expect_has "ata1.00: ATA-8: MINIVMM AHCI DISK, 1.0, max UDMA/133"
expect_has "ata1.00: 131072 sectors, multi 0: LBA48 NCQ (depth 32)"
expect_has "ata1.00: configured for UDMA/133"
expect_has "[sda] 131072 512-byte logical blocks: (67.1 MB/64.0 MiB)"
expect_has "[sdb] 65536 512-byte logical blocks: (33.6 MB/32.0 MiB)"
expect_has "[sda] Write cache: enabled, read cache: enabled"
expect_not "failed command"
expect_not "not supported, aborted"
end_case

# 只读挂载不写盘，镜像在 guest 运行之后必须原样不变。flush 不计：关机时
# sd 驱动对每块盘都发 SYNCHRONIZE CACHE（"Synchronizing SCSI cache"）
check_case "读：整盘直读和文件内容都与 host 的镜像一致，只读挂载不改镜像"
expect_has "RAW-$d0_sha"
expect_has "RD-$blob_sha"
expect_re 'drive d0: read [1-9][0-9]* ops [0-9]+ bytes, write 0 ops 0 bytes, [0-9]+ flushes, 0 errors'
end_case
run_case "d0 镜像在 guest 运行后没有变化" sha256sum "$WORK/d0.img"
expect_has "$d0_sha"
end_case

wr_sha="$(printf '%s\n' "$BOOT_OUT" | sed -n 's/^WR-\([0-9a-f]\{64\}\) .*/\1/p' | head -1)"
# Linux 发 NCQ 命令时先写 PxSACT 再写 PxCI（ahci_qc_issue），非 NCQ 命令
# 只写 PxCI；两个控制器的 ABAR 在 0xc0000000、0xc0002000，端口 0 的
# PxSACT 在 0x134
check_case "NCQ：队列深度 32，文件系统的读写走 FPDMA（两个控制器都写了 PxSACT）"
expect_has "QD-32"
end_case
run_case "MMIO trace 里两个控制器端口 0 的 PxSACT 写次数" \
    awk '$4 == "W4" && ($3 == "c0000134" || $3 == "c0002134") { n[$3]++ }
         END { printf "SACT0-%d SACT1-%d\n", n["c0000134"], n["c0002134"] }' "$WORK/mmio.txt"
expect_re 'SACT0-[1-9][0-9]* SACT1-[1-9][0-9]*'
end_case

check_case "写：guest mkfs 后写入的文件，guest 算出了 sha256"
expect_re '^WR-[0-9a-f]{64} '
expect_has "UMOUNT-ok"
end_case
# 文件内容由 host 从镜像里取出，和 guest 报的 sha256 比
run_case "host 用 debugfs 从 d1 镜像取出的文件与 guest 写的一致，e2fsck 无错" \
    bash -c 'debugfs -R "dump /up $1" "$2" 2>/dev/null; sha256sum < "$1"; e2fsck -fn "$2" >/dev/null 2>&1; echo "fsck=$?"' \
    _ "$WORK/up" "$WORK/d1.img"
expect_has "${wr_sha:-guest-reported-no-sha}"
expect_has "fsck=0"
end_case

# 写缓存默认开着，卸载时 ext2 通过块层发 FLUSH CACHE EXT，后端执行 fdatasync
check_case "FLUSH CACHE EXT：卸载时 flush 到达后端，全程没有 I/O 错误"
expect_re 'drive d1: read [0-9]+ ops [0-9]+ bytes, write [1-9][0-9]* ops [0-9]+ bytes, [1-9][0-9]* flushes, 0 errors'
end_case
}

# libata.force=noncq 时 Linux 不用 NCQ，读写回到 READ/WRITE DMA (EXT)：
# 这条路径在 NCQ 打开时只剩探测和 flush 在用，单独验证一遍数据
noncq_tests() {
head1 "AHCI 控制器：关掉 NCQ 后的 DMA 读写"

local d0_head
head -c 64M /dev/urandom > "$WORK/n0.img"
d0_head="$(head -c 1M "$WORK/n0.img" | sha256sum | cut -d' ' -f1)"

boot \
    --cmd "dmesg | grep -a 'ata1.00: 131072'" \
    --cmd "echo RAW-\$(dd if=/dev/sda bs=1M count=1 iflag=direct 2>/dev/null | sha256sum)" \
    --cmd "dd if=/dev/urandom of=/tmp/w bs=1M count=2 2>/dev/null && echo W-\$(sha256sum < /tmp/w)" \
    --cmd "dd if=/tmp/w of=/dev/sda bs=1M seek=8 oflag=direct conv=fsync 2>/dev/null; echo RC-\$?" \
    --stop 'poweroff -f' \
    -- "$VMM" --mode linux --mem 512M \
          --drive id=n0,file="$WORK/n0.img" --device ahci,drive=n0 \
          --trace-mmio "$WORK/n0-mmio.txt" --append "libata.force=noncq" \
          --initrd "$INITRD" "$VMLINUX"

local w_sha
w_sha="$(printf '%s\n' "$BOOT_OUT" | sed -n 's/^W-\([0-9a-f]\{64\}\) .*/\1/p' | head -1)"
check_case "libata.force=noncq：NCQ 不用，READ DMA 读出的数据与镜像一致"
expect_rc 0
expect_has "ata1.00: 131072 sectors, multi 0: LBA48 NCQ (not used)"
expect_has "RAW-$d0_head"
expect_has "RC-0"
end_case
run_case "WRITE DMA 写进镜像的 2 MiB 与 guest 写的一致，全程没有 PxSACT 写" \
    bash -c 'dd if="$1" bs=1M skip=8 count=2 2>/dev/null | sha256sum;
             awk '"'"'$4 == "W4" && $3 == "c0000134" { n++ } END { printf "SACT-%d\n", n }'"'"' "$2"' \
    _ "$WORK/n0.img" "$WORK/n0-mmio.txt"
expect_has "${w_sha:-guest-reported-no-sha}"
expect_has "SACT-0"
end_case
}

# NCQ 命令执行出错：guest 运行中 host 把镜像截短一半，guest 读后半段时
# 后端读到文件尾报 EIO，设备回 ERR+UNC 的 SDB FIS 并停止接受命令。
# Linux 的 EH 用 READ LOG EXT 读日志 10h 找出是哪个 tag 出的错
# （ata_eh_analyze_ncq_error），日志不对时它会打印 "failed to read log
# page 10h"、"invalid checksum" 或 "inactive tag"。恢复之后的读照常完成。
ncq_error_tests() {
head1 "AHCI 控制器：NCQ 出错与恢复"

head -c 64M /dev/urandom > "$WORK/e0.img"
boot \
    --cmd "dd if=/dev/sda of=/dev/null bs=1M count=4 iflag=direct 2>/dev/null; echo PRE-\$?" \
    --host "truncate -s 32M '$WORK/e0.img'" \
    --cmd "dd if=/dev/sda of=/dev/null bs=4k skip=12288 count=1 iflag=direct 2>/dev/null; echo RC-\$?" \
    --cmd "echo OK-\$(dd if=/dev/sda bs=4k count=1 iflag=direct 2>/dev/null | wc -c)" \
    --stop 'poweroff -f' \
    -- "$VMM" --mode linux --mem 512M \
          --drive id=e0,file="$WORK/e0.img" --device ahci,drive=e0 \
          --initrd "$INITRD" "$VMLINUX"

check_case "读出错：Linux 报 READ FPDMA QUEUED 的 UNC，经日志 10h 找到 tag 后恢复"
expect_rc 0
expect_has "PRE-0"
expect_has "drive e0: read 4096 bytes at offset 0x3000000: Input/output error"
expect_has "ata1.00: failed command: READ FPDMA QUEUED"
expect_has "ata1.00: status: { DRDY ERR }"
expect_has "ata1.00: error: { UNC }"
expect_has "ata1: EH complete"
expect_has "RC-1"
expect_has "OK-4096"
expect_not "log page 10h"
expect_not "invalid checksum"
expect_not "inactive tag"
expect_re 'drive e0: read [0-9]+ ops [0-9]+ bytes, write 0 ops 0 bytes, [0-9]+ flushes, 1 errors'
end_case
}

# 根文件系统在 AHCI 盘上：镜像由 initramfs 的内容加一个标记文件做成。
# initrd 照样传（VMM 的 linux 模式要求有），rdinit 指向不存在的路径，
# 内核找不到 initramfs 里的 init 就转去挂 root=（init/main.c
# kernel_init_freeable -> prepare_namespace），之后的 init 来自磁盘。
root_tests() {
head1 "AHCI 控制器：从磁盘启动"

local rootfs="$WORK/rootfs"
mkdir -p "$rootfs"
# dev/console 是字符设备，非 root 解不出来；根文件系统上的 /dev 由
# CONFIG_DEVTMPFS_MOUNT 在挂根之后自动挂上 devtmpfs
( cd "$rootfs" && zcat "$INITRD" | cpio -idm --quiet -f './dev/console' )
echo minivmm-disk-root > "$rootfs/disk-root"
mke2fs -q -F -t ext2 -d "$rootfs" "$WORK/root.img" 64M

boot \
    --cmd "echo ROOT-\$(cat /disk-root)" \
    --cmd "grep -a ' / ' /proc/mounts" \
    --stop 'poweroff -f' \
    -- "$VMM" --mode linux --mem 512M \
          --drive id=r0,file="$WORK/root.img" --device ahci,drive=r0 \
          --append "root=/dev/sda rdinit=/nonexistent init=/init" \
          --initrd "$INITRD" "$VMLINUX"

check_case "root=/dev/sda：内核从 AHCI 盘挂根，init 和 shell 来自磁盘"
expect_rc 0
expect_has "VFS: Mounted root (ext2 filesystem) on device 8:0."
expect_has "ROOT-minivmm-disk-root"
expect_re '^/dev/root / ext2 rw'
expect_has "guest requested poweroff via ACPI S5"
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
