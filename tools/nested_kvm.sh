#!/usr/bin/env bash
# tools/nested_kvm.sh — 在嵌套 QEMU 里用另一个内核的 KVM 运行 mini-vmm
#
# mini-vmm 的 CPU 虚拟化全部交给 host 内核的 KVM。开发机的 host 内核（WSL）
# 换不了，要测上游 KVM（kvm-x86/next 或者打了补丁的分支）时，就让 QEMU 启动
# 一台虚拟机（L1），里面跑被测内核，再在 L1 里运行 mini-vmm：
#
#   host 内核（KVM，只负责跑 QEMU）
#    └─ QEMU：L1 = 被测内核（KVM）    ← mini-vmm 的 ioctl 由这里处理
#        └─ mini-vmm 进程
#            └─ guest
#
# L1 不需要自己的根文件系统：host 的 / 经 virtio-9p 只读共享进去，init 把它
# chroot 成根，所以 host 上的 bash、python、gdb、编译出的 build/vmm 在 L1 里
# 原样可用。仓库目录另外以读写方式共享在同一路径上，测试写的 build/、
# build/test-failures/ 落回 host。/proc、/sys、/dev、/tmp、/run 是 L1 自己的。
#
# 退出码透传 L1 里命令的退出码。L1 没有报告结果（卡死被超时杀掉、内核
# panic、命令没跑起来）时用一个命令自己不太会用到的专用值，调用方据此区分
# “测试失败”和“环境坏了”。
set -euo pipefail

readonly EXIT_OK=0
readonly EXIT_FAIL=1
readonly EXIT_USAGE=2
readonly EXIT_NO_RESULT=125      # 小于 126：126 起 shell 另有含义
readonly EXIT_INTERRUPTED=130    # shell 对 SIGINT 的约定 128+2

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly ROOT
readonly FRAGMENT="$ROOT/tools/nested-host-kernel.config"
readonly DEFAULT_OUT="$ROOT/nested-kernel"
readonly BASE_DEFCONFIG="x86_64_defconfig"

usage() {
    cat <<EOF
Usage: tools/nested_kvm.sh build --src DIR [--out DIR] [-j N]
       tools/nested_kvm.sh run [--kernel BZIMAGE] [--mem SIZE] [--smp N]
                               [--timeout SEC] [--rw DIR]... [--log FILE] -- CMD [ARG...]

Run mini-vmm on the KVM of another kernel (for example kvm-x86/next) inside a
nested QEMU guest ("L1").

build   Configure and build an L1 kernel: $BASE_DEFCONFIG + kvm_guest.config +
        tools/nested-host-kernel.config, out of tree. Prints the bzImage path.
          --src DIR   kernel source tree to test (never written to)
          --out DIR   build directory (default: nested-kernel/ in the repository)
          -j N        parallel jobs (default: number of CPUs)

run     Boot BZIMAGE under QEMU, chroot into the host's / (read-only over 9p,
        with the repository read-write at the same path) and run CMD in the
        repository directory. Serial output streams to stdout.
          --kernel BZIMAGE  L1 kernel (default: nested-kernel/arch/x86/boot/bzImage)
          --mem SIZE        L1 memory, QEMU syntax (default: 4G)
          --smp N           L1 vCPUs (default: 8)
          --timeout SEC     kill QEMU after SEC seconds (default: 7200)
          --rw DIR          extra directory shared read-write at the same path,
                            repeatable
          --log FILE        also write the serial output to FILE

EXAMPLE:
  # build an L1 kernel from a kvm-x86/next checkout
  tools/nested_kvm.sh build --src ~/src/kvm-x86-next
  # one suite on that KVM
  tools/nested_kvm.sh run -- make test linux-boot
  # environment variables go through env(1)
  tools/nested_kvm.sh run -- env IRQCHIP=split make test split-irqchip
  # a patched kernel built elsewhere, with the log kept
  tools/nested_kvm.sh run --kernel /path/to/bzImage --log nested.log -- make test

NOTE:
  The host must expose SVM/VMX to QEMU (kvm_amd/kvm_intel nested=1) and have a
  statically linked busybox for the L1 init. build/vmm and the guest kernel are
  used as built on the host; build them before 'run'.
  The first output line from L1 is "== L1 kernel <osrelease>"; with
  CONFIG_LOCALVERSION_AUTO the release carries the git hash of the tested KVM.
  Guests run two levels deep: boots are several times slower and timing-sensitive
  test cases may need a larger margin than on the host.
  L1 sees the host's / read-only: commands must write only inside the
  repository, a --rw directory or /tmp (L1's own tmpfs, lost on exit).

  Exit codes: CMD's exit status; 0 success, 1 build failure, 2 usage error,
              125 L1 reported no result (hang, panic or CMD did not start),
              130 interrupted.
EOF
}

die() {
    printf 'nested_kvm: %s\n' "$1" >&2
    exit "${2:-$EXIT_FAIL}"
}

usage_error() {
    printf 'nested_kvm: %s\n' "$1" >&2
    printf "Try 'tools/nested_kvm.sh --help'.\n" >&2
    exit "$EXIT_USAGE"
}

log() { printf '==> %s\n' "$*" >&2; }

trap 'printf "nested_kvm: interrupted\n" >&2; exit $EXIT_INTERRUPTED' INT

# ------------------------------------------------------------------
cmd_build() {
    local src="" out="$DEFAULT_OUT" jobs
    jobs="$(nproc)"
    while [ $# -gt 0 ]; do
        case "$1" in
        --src) [ $# -ge 2 ] || usage_error "--src needs a directory"; src="$2"; shift 2 ;;
        --out) [ $# -ge 2 ] || usage_error "--out needs a directory"; out="$2"; shift 2 ;;
        -j)    [ $# -ge 2 ] || usage_error "-j needs a number"; jobs="$2"; shift 2 ;;
        -h|--help) usage; exit "$EXIT_OK" ;;
        *)     usage_error "build: unknown argument: $1" ;;
        esac
    done
    [ -n "$src" ] || usage_error "build: --src DIR is required"
    [[ "$jobs" =~ ^[1-9][0-9]*$ ]] || usage_error "-j expects a positive integer, got '$jobs'"
    [ -f "$src/Makefile" ] && [ -f "$src/Kconfig" ] || die "$src does not look like a Linux source tree"
    src="$(cd "$src" && pwd)"
    # kbuild 在源码树里有构建产物时拒绝 O= 构建，提前给出明确提示
    if [ -e "$src/.config" ] || [ -d "$src/include/config" ]; then
        die "$src has in-tree build artifacts and kbuild will refuse an O= build; run 'make mrproper' there first"
    fi
    mkdir -p "$out"
    out="$(cd "$out" && pwd)"
    [ "$out" != "$src" ] || usage_error "--out must differ from the source tree"
    # 同一个 O= 目录上同时跑两个 make 不会立刻报错：一方重新生成 .config 和头文件、
    # 删改目标文件时，另一方照常编译，结果是 ar 找不到 .o 或者留下半截的 .o。后者
    # 时间戳是新的，之后的构建不会重编它。所以整个构建期间持有目录锁，第二个构建
    # 直接拒绝。锁随进程退出自动释放，不会因中断残留。
    exec 9>"$out/.build.lock"
    flock -n 9 || die "another build is running in $out (lock: $out/.build.lock)"

    local head
    head="$(git -C "$src" describe --always --dirty 2>/dev/null || echo "not a git checkout")"
    log "Linux $(make -s -C "$src" kernelversion) ($head) from $src, building in $out"
    make -s -C "$src" O="$out" "$BASE_DEFCONFIG" >&2
    make -s -C "$src" O="$out" kvm_guest.config >&2
    # -m：只合并，不让 merge_config.sh 以 alldefconfig 为底重新生成
    ( cd "$src" && scripts/kconfig/merge_config.sh -m -Q -O "$out" "$out/.config" "$FRAGMENT" ) >&2
    make -s -C "$src" O="$out" olddefconfig >&2

    # olddefconfig 会静默改掉依赖不满足的选项，逐条核对片段的值都留下了
    local line bad=0
    while IFS= read -r line; do
        case "$line" in CONFIG_*=*) ;; *) continue ;; esac
        if ! grep -qxF -- "$line" "$out/.config"; then
            printf 'nested_kvm: fragment wants "%s", .config has "%s"\n' "$line" \
                "$(grep -E "^(# )?${line%%=*}[= ]" "$out/.config" || echo "(absent)")" >&2
            bad=1
        fi
    done < "$FRAGMENT"
    [ "$bad" -eq 0 ] || die "Kconfig did not accept the fragment as written (unmet dependencies?)"

    log "build: make -j$jobs bzImage"
    make -s -C "$src" O="$out" -j"$jobs" bzImage >&2
    log "done: $(cat "$out/include/config/kernel.release")"
    printf '%s\n' "$out/arch/x86/boot/bzImage"
}

# ------------------------------------------------------------------
cmd_run() {
    local kernel="$DEFAULT_OUT/arch/x86/boot/bzImage" mem=4G smp=8 timeout=7200
    local logfile="" rw_dirs=()
    while [ $# -gt 0 ]; do
        case "$1" in
        --kernel)  [ $# -ge 2 ] || usage_error "--kernel needs a file"; kernel="$2"; shift 2 ;;
        --mem)     [ $# -ge 2 ] || usage_error "--mem needs a size"; mem="$2"; shift 2 ;;
        --smp)     [ $# -ge 2 ] || usage_error "--smp needs a number"; smp="$2"; shift 2 ;;
        --timeout) [ $# -ge 2 ] || usage_error "--timeout needs seconds"; timeout="$2"; shift 2 ;;
        --rw)      [ $# -ge 2 ] || usage_error "--rw needs a directory"; rw_dirs+=("$2"); shift 2 ;;
        --log)     [ $# -ge 2 ] || usage_error "--log needs a file"; logfile="$2"; shift 2 ;;
        -h|--help) usage; exit "$EXIT_OK" ;;
        --)        shift; break ;;
        *)         usage_error "run: unknown argument '$1' (put the command after --)" ;;
        esac
    done
    [ $# -gt 0 ] || usage_error "run: no command given after --"
    [[ "$smp" =~ ^[1-9][0-9]*$ ]] || usage_error "--smp expects a positive integer, got '$smp'"
    [[ "$timeout" =~ ^[1-9][0-9]*$ ]] || usage_error "--timeout expects seconds, got '$timeout'"
    [ -f "$kernel" ] || die "no L1 kernel at $kernel (build one with: tools/nested_kvm.sh build --src DIR)"
    [ -e /dev/kvm ] || die "/dev/kvm missing on this host: nested QEMU needs KVM"
    command -v qemu-system-x86_64 >/dev/null || die "qemu-system-x86_64 not found"
    local busybox
    busybox="$(command -v busybox)" || die "busybox not found (needed for the L1 init)"
    file -L "$busybox" | grep -q 'statically linked' || die "$busybox is not statically linked"

    local work
    work="$(mktemp -d)"
    # QEMU 的子进程在中断时也要收掉，否则留下一台没人管的虚拟机
    # 路径在设 trap 时就展开：EXIT trap 在函数返回之后才跑，那时 local 的
    # work 已经不存在，set -u 下引用它会报 unbound variable 并漏删目录
    trap "rm -rf -- $(printf '%q' "$work")" EXIT
    trap 'printf "nested_kvm: interrupted\n" >&2; kill "${qpid:-}" 2>/dev/null; exit $EXIT_INTERRUPTED' INT TERM

    # 9p 共享：host 的 / 只读，仓库和 --rw 目录读写，挂在 L1 里同样的路径上
    local virtfs=(-virtfs "local,path=/,mount_tag=hostroot,security_model=none,readonly=on,multidevs=remap")
    local mounts="" i=0 dir
    for dir in "$ROOT" "${rw_dirs[@]}"; do
        [ -d "$dir" ] || die "--rw $dir is not a directory"
        dir="$(cd "$dir" && pwd)"
        virtfs+=(-virtfs "local,path=$dir,mount_tag=rw$i,security_model=none,multidevs=remap")
        mounts+="mount -t 9p -o trans=virtio,version=9p2000.L,msize=262144 rw$i /host$dir"$'\n'
        i=$((i + 1))
    done

    local root="$work/root" cmd
    mkdir -p "$root"/{bin,dev,proc,sys,host}
    cp "$busybox" "$root/bin/busybox"
    for a in sh mount chroot poweroff ln mkdir cat cp sed; do ln -s busybox "$root/bin/$a"; done
    # 命令写成脚本带进 initramfs，init 把它拷到 L1 的 /tmp 再在 chroot 里执行：
    # 嵌进 init 的命令行要过两层 shell 引号，含空格或引号的参数会被拆坏
    cmd="$(printf '%q ' "$@")"
    printf 'cd %q || exit\nexec %s\n' "$ROOT" "$cmd" >"$root/cmd.sh"

    cat >"$root/init" <<EOF
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mount -t 9p -o trans=virtio,version=9p2000.L,msize=262144,ro hostroot /host
$mounts
mount -t proc proc /host/proc
mount -t sysfs sysfs /host/sys
mount -t devtmpfs devtmpfs /host/dev
# devtmpfs 没有 udev/systemd 平时补上的链接和挂载；bash 的进程替换要 /dev/fd
ln -s /proc/self/fd /host/dev/fd
ln -s /proc/self/fd/0 /host/dev/stdin
ln -s /proc/self/fd/1 /host/dev/stdout
ln -s /proc/self/fd/2 /host/dev/stderr
mkdir -p /host/dev/shm /host/dev/pts
mount -t tmpfs tmpfs /host/dev/shm
mount -t devpts devpts /host/dev/pts
mount -t tmpfs tmpfs /host/tmp
mount -t tmpfs tmpfs /host/run
cp /cmd.sh /host/tmp/nested-kvm-cmd.sh
echo "== L1 kernel \$(cat /proc/sys/kernel/osrelease), \$(cat /sys/module/kvm_amd/parameters/avic 2>/dev/null | sed 's/^/avic=/')"
chroot /host /usr/bin/env -i HOME=/root TERM=dumb \
    PATH=/usr/local/bin:/usr/bin:/usr/sbin:/bin:/sbin \
    /bin/bash /tmp/nested-kvm-cmd.sh
echo "NESTED_KVM_RC=\$?"
poweroff -f
EOF
    chmod +x "$root/init"
    (cd "$root" && find . | cpio -o -H newc --quiet | gzip -1) >"$work/initrd.gz"

    local out="$work/serial.log"
    log "L1: $kernel, $smp vCPUs, $mem; running: $cmd"
    timeout "$timeout" qemu-system-x86_64 -enable-kvm -cpu host \
        -smp "$smp" -m "$mem" -nographic -no-reboot -nic none \
        -kernel "$kernel" -initrd "$work/initrd.gz" "${virtfs[@]}" \
        -append "console=ttyS0 quiet panic=-1 rdinit=/init" </dev/null >"$out" 2>&1 &
    qpid=$!
    # 串口输出边产生边转给 stdout；QEMU 退出后 tail 收尾退出
    tail -n +1 -f --pid="$qpid" "$out" | tee ${logfile:+"$logfile"} || true
    wait "$qpid" || true

    local rc
    rc="$(grep -ao 'NESTED_KVM_RC=[0-9]*' "$out" | tail -1 | cut -d= -f2)"
    if [ -z "$rc" ]; then
        printf 'nested_kvm: L1 reported no result (hang, panic or timeout after %ss)\n' "$timeout" >&2
        exit "$EXIT_NO_RESULT"
    fi
    exit "$rc"
}

# ------------------------------------------------------------------
[ $# -gt 0 ] || usage_error "missing subcommand: build or run"
sub="$1"; shift
case "$sub" in
build) cmd_build "$@" ;;
run)   cmd_run "$@" ;;
-h|--help) usage; exit "$EXIT_OK" ;;
*)     usage_error "unknown subcommand: $sub" ;;
esac
