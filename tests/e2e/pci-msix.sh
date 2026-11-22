#!/usr/bin/env bash
# tests/e2e/pci-msix.sh — 里程碑 pci-msix 的验收
#
#   edu 驱动以 irq_mode=msix 加载：Linux 经 MSI-X capability 找到 BAR1 里的
#     table，申请 4 个向量，/proc/interrupts 里出现 4 行 PCI-MSIX
#   eductl selftest：DMA 完成走向量 0，软件中断按所在字节走向量 1-3，每个
#     向量认领的次数和处理函数被调用的次数都与按轮数算出的值精确相等
#   eductl masktest（PCI 3.0 第 6.8.3.5 节）：单个向量屏蔽、Function Mask、
#     屏蔽期间原因被确认三种情形下，PBA 位和中断次数都对得上
#   之后换成 INTx 加载，INTA 回到线上；卸载后 MSI-X Enable 和中断全部归还
#
# 用法：
#   ./tests/e2e/pci-msix.sh       全部
#   ./tests/e2e/pci-msix.sh -a    只跑自动用例
#   ./tests/e2e/pci-msix.sh -m    只看人工指引
#
# 镜像和 host 工具的要求见 tests/edu_guest.sh。
#
# capability 的布局、table/PBA 的读写属性、屏蔽和补发的每条规则由单元测试
# pci_bus_test 逐条验证，edu 按字节分项由 edu_test 验证；这里验证的是 Linux
# 的 MSI-X 实现按规范把这些机制用起来，以及屏蔽语义在真实 guest 里可观察。

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"
source "$ROOT/tests/edu_guest.sh"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

BDF=$EDU_BDF
ROUNDS=20
CYCLES=20

BOOT_OUT=""
BOOT_RC=0

check_case() {
    CUR_NAME="$1"
    printf '%s->%s %s\n' "$C_CYA" "$C_RST" "$CUR_NAME"
    CUR_BAD=0
    CUR_OUT="$BOOT_OUT"
    CUR_RC=$BOOT_RC
}

# 第 r 轮（从 0 数）的软件中断位是 1 << (8 + r % 24)，落在第 1 + (r % 24) / 8
# 个字节；DMA 完成每轮 3 次，都在向量 0
per_vector() {
    local n=$1 r v=(0 0 0 0)
    for ((r = 0; r < n; r++)); do
        v[0]=$((v[0] + 3))
        v[$((1 + (r % 24) / 8))]=$((v[1 + (r % 24) / 8] + 1))
    done
    echo "+${v[0]} +${v[1]} +${v[2]} +${v[3]}"
}

# 资源状态，和 edu-dev 的同名脚本一样每样打一行，多了 MSI-X Enable 和
# Function Mask。MSI 在 0x40（14 字节，下一项 DWORD 对齐），MSI-X 在 0x50，
# 它的 Message Control 在 0x52：bit 15 是 Enable，bit 14 是 Function Mask。
# 先核对 0x50 处确实是 MSI-X（ID 0x11），位置不对时打 WRONGCAP 而不是读错的字节。
write_state_script() {
cat > "$1" <<'EOF'
# 用法: state.sh TAG
tag=$1
D=/sys/bus/pci/devices/0000:00:01.0
echo "$tag-MOD-$(grep -c '^edu ' /proc/modules)"
echo "$tag-IRQ-$(grep -c ' edu$' /proc/interrupts)"
echo "$tag-DRV-$([ -e $D/driver ] && basename "$(readlink $D/driver)" || echo none)"
id=$(od -An -tu1 -j80 -N1 $D/config | tr -d ' ')
[ "$id" = 17 ] || echo "$tag-WRONGCAP-$id"
ctl=$(od -An -tu2 -j82 -N2 $D/config | tr -d ' ')
echo "$tag-MSIXEN-$(( (ctl >> 15) & 1 )) $tag-MASKALL-$(( (ctl >> 14) & 1 ))"
EOF
}

# 加载卸载 n 次，每次跑一轮 selftest 和一次 Function Mask 测试
write_cycle_script() {
cat > "$1" <<'EOF'
# 用法: cycle.sh N
n=$1
cd /root/edu || exit 1
dmesg -C
for i in $(seq "$n"); do
    insmod edu.ko irq_mode=msix || { echo "CYC-FAIL insmod $i"; break; }
    ./eductl selftest 1 > /dev/null || echo "CYC-FAIL selftest $i"
    ./eductl masktest function > /dev/null || echo "CYC-FAIL masktest $i"
    rmmod edu || { echo "CYC-FAIL rmmod $i"; break; }
done
echo "CYC-PROBE-$(dmesg | grep -c '/dev/edu0, irqs [0-9]*-[0-9]* (MSI-X, 4 vectors)')"
dmesg --level=err,warn | grep -v -e 'taints kernel' -e 'module verification failed' > /tmp/warn
echo "CYC-DMESG-WARN-$(wc -l < /tmp/warn)"
sed "s/^/CYC-DMESG /" /tmp/warn
EOF
}

auto_tests() {
head1 "edu 驱动以 MSI-X 加载，多向量计数与屏蔽语义"

write_state_script "$WORK/state.sh"
write_cycle_script "$WORK/cycle.sh"
edu_guest_prepare "pci-msix 用例" "" "$WORK/state.sh" "$WORK/cycle.sh" || return

BOOT_OUT="$(edu_guest_boot \
    --cmd "cd /root/edu && make 2>&1 | tail -3; echo BUILD-RC-\${PIPESTATUS[0]}" \
    --cmd "insmod edu.ko irq_mode=msix; echo INSMOD-RC-\$?; dmesg | grep -a 'edu $BDF'" \
    --cmd "bash state.sh LOADED; grep -a ' edu\$' /proc/interrupts" \
    --cmd "./eductl selftest $ROUNDS; echo SELFTEST-RC-\$?" \
    --cmd "for v in 1 2 3; do ./eductl masktest vector \$v; echo MASKTEST-VECTOR-\$v-RC-\$?; done" \
    --cmd "./eductl masktest function; echo MASKTEST-FUNCTION-RC-\$?; bash state.sh FMASK" \
    --cmd "for v in 1 3; do ./eductl masktest acked \$v; echo MASKTEST-ACKED-\$v-RC-\$?; done" \
    --cmd "./eductl info" \
    --cmd "rmmod edu; bash state.sh UNLOADED" \
    --cmd "insmod edu.ko irq_mode=intx && ./eductl selftest 4; echo INTX-RC-\$?; rmmod edu" \
    --cmd "insmod edu.ko irq_mode=msi && ./eductl masktest function; echo MSI-MASKTEST-RC-\$?; rmmod edu" \
    --cmd "bash cycle.sh $CYCLES" \
    --cmd "bash state.sh AFTER")"
BOOT_RC=$?

check_case "guest 编出驱动，以 MSI-X 加载：4 个向量，table 在 BAR1，资源看得见"
expect_rc 0
expect_has "BUILD-RC-0"
expect_has "INSMOD-RC-0"
expect_re "edu $BDF: /dev/edu0, irqs [0-9]+-[0-9]+ \\(MSI-X, 4 vectors\\), BAR0 \\[mem 0x[0-9a-f]+-0x[0-9a-f]+\\]"
expect_re "PCI-MSIX-$BDF +0-edge +edu"
expect_re "PCI-MSIX-$BDF +3-edge +edu"
expect_has "pci 00:01.0 edu MSI-X: 4 entries, table BAR1+0x0, PBA BAR1+0x800"
expect_re "pci 00:01\\.0 edu: MSI-X enabled, 4 entries"
expect_has "LOADED-MOD-1"
expect_has "LOADED-IRQ-4"
expect_has "LOADED-DRV-edu"
expect_has "LOADED-MSIXEN-1 LOADED-MASKALL-0"
expect_not "WRONGCAP"
expect_has "guest requested poweroff via ACPI S5"
expect_not "TIMEOUT"
end_case

check_case "每个向量独立计数：$ROUNDS 轮，DMA 完成在向量 0，软件中断按字节分到向量 1-3"
expect_has "selftest: $ROUNDS rounds ok (MSI-X), irqs +$((4 * ROUNDS)), dma +$((3 * ROUNDS)), per vector $(per_vector $ROUNDS)"
expect_has "SELFTEST-RC-0"
end_case

check_case "单个向量屏蔽：屏蔽期间只置 PBA 位、不发消息，解除屏蔽补发一条并清 PBA"
expect_has "masktest vector 1: masked pba 0x2 calls +0 +0 +0 +0; after pba 0x0 calls +0 +1 +0 +0"
expect_has "masktest vector 2: masked pba 0x4 calls +0 +0 +0 +0; after pba 0x0 calls +0 +0 +1 +0"
expect_has "masktest vector 3: masked pba 0x8 calls +0 +0 +0 +0; after pba 0x0 calls +0 +0 +0 +1"
expect_has "MASKTEST-VECTOR-1-RC-0"
expect_has "MASKTEST-VECTOR-2-RC-0"
expect_has "MASKTEST-VECTOR-3-RC-0"
end_case

check_case "Function Mask：三个向量同时只记 pending，清掉后各补发一条，各自的 Mask 位不受影响"
expect_has "masktest function: masked pba 0xe calls +0 +0 +0 +0; after pba 0x0 calls +0 +1 +1 +1"
expect_has "MASKTEST-FUNCTION-RC-0"
expect_has "FMASK-MSIXEN-1 FMASK-MASKALL-0"
end_case

check_case "屏蔽期间原因被确认：PBA 位跟着清掉，解除屏蔽后不补发"
expect_has "masktest acked 1: masked pba 0x2 calls +0 +0 +0 +0; acked pba 0x0; after pba 0x0 calls +0 +1 +0 +0"
expect_has "masktest acked 3: masked pba 0x8 calls +0 +0 +0 +0; acked pba 0x0; after pba 0x0 calls +0 +0 +0 +1"
expect_has "MASKTEST-ACKED-1-RC-0"
expect_has "MASKTEST-ACKED-3-RC-0"
end_case

check_case "卸载后 MSI-X Enable 和中断归还，INTx 与 MSI 照常工作"
expect_has "UNLOADED-MOD-0"
expect_has "UNLOADED-IRQ-0"
expect_has "UNLOADED-DRV-none"
expect_has "UNLOADED-MSIXEN-0 UNLOADED-MASKALL-0"
expect_has "pci 00:01.0 edu: MSI-X disabled, interrupts go to INTA"
expect_has "selftest: 4 rounds ok (INTx), irqs +16, dma +12"
expect_has "INTX-RC-0"
expect_re "EDU_IOC_MASKTEST: Operation not supported"
expect_has "MSI-MASKTEST-RC-1"
end_case

check_case "加载卸载 $CYCLES 次：每次都拿到 4 个向量，计数和屏蔽测试都通过，没有内核告警"
expect_has "CYC-PROBE-$CYCLES"
expect_has "CYC-DMESG-WARN-0"
expect_not "CYC-FAIL"
expect_has "AFTER-MOD-0"
expect_has "AFTER-IRQ-0"
expect_has "AFTER-MSIXEN-0 AFTER-MASKALL-0"
end_case

check_case "VMM 侧没有丢消息，也没有非法的 table 访问"
expect_not "not delivered"
expect_not "table and PBA take aligned"
expect_not "pci_msix_notify("
end_case
}

manual_guide() {
guide "在 Arch guest 里用 MSI-X 加载驱动，观察屏蔽和补发" \
"cp --sparse=always arch-image/disk.img /path/to/copy.img
# 把 tools/guest/edu/ 下的文件和 include/devices/edu_regs.h 放进 guest 的同一个目录（例如 scp，或在 host 上用 debugfs -w 写进镜像副本）
./build/vmm --mode linux --smp 2 --mem 1G --device edu \\
    --device ahci,drive=d0 --drive id=d0,file=/path/to/copy.img \\
    --initrd arch-image/initramfs-linux.img \\
    --append \"root=UUID=\$(blkid -p -o value -s UUID /path/to/copy.img) rw\" arch-image/vmlinuz-linux
# guest 里：
make && insmod edu.ko irq_mode=msix
grep edu /proc/interrupts
./eductl selftest 24 && ./eductl masktest vector 2 && ./eductl masktest function && ./eductl info" \
"/proc/interrupts 有 4 行 PCI-MSIX-0000:00:01.0 0..3-edge edu；selftest 打印 'per vector +72 +8 +8 +8'；masktest vector 2 打印 'masked pba 0x4 calls +0 +0 +0 +0; after pba 0x0 calls +0 +0 +1 +0' 和 'ok'"
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
