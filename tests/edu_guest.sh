# tests/edu_guest.sh — 在 Arch guest 里编译并使用 edu 驱动的 e2e 共用步骤
#
# 由 tests/e2e 下用到 edu 设备的 suite 在 source tests/lib.sh 之后 source。
# 驱动必须在 guest 里用镜像自带的 linux-headers 编译：树外模块的 vermagic
# 要和运行中的内核一致，host 上没有那份内核的构建目录。源码经 debugfs 直接
# 写进磁盘镜像的副本，guest 一启动就在 /root/edu 里。
#
# 需要 tools/build_arch_image.sh 生成的镜像（默认 arch-image/，可用 ARCH_IMAGE
# 指定），镜像里要有 gcc、make 和 linux-headers；host 上要有 debugfs
# （e2fsprogs）。条件不满足时用 skip_case 说明缺什么，调用方直接返回。

EDU_IMG="${ARCH_IMAGE:-$ROOT/arch-image}"
EDU_SRC="$ROOT/tools/guest/edu"

# edu 是第一个 --device，落在 slot 1；INTA 按 VMM 的 swizzle 接到
# 16 + ((1 + 1 - 1) & 3) = GSI 17
EDU_BDF=0000:00:01.0
EDU_INTX_GSI=17

# edu_guest_prepare NAME EXTRA FILE...
#   检查镜像和工具，复制一份磁盘镜像，把驱动源码、edu_regs.h 和 FILE... 写进
#   guest 的 /root/edu。EXTRA 是额外的 debugfs 命令文件（可为空串），用来
#   写别的目录。成功时设置 EDU_DISK、EDU_UUID 并返回 0；不满足条件时以 NAME
#   记一条 skip_case 并返回 1。EDU_DISK 由 edu_guest_boot 删除。
edu_guest_prepare() {
    local name="$1" extra="$2" f cmds log
    shift 2
    for f in disk.img vmlinuz-linux initramfs-linux.img packages.txt; do
        if [ ! -f "$EDU_IMG/$f" ]; then
            skip_case "$name" "$EDU_IMG/$f missing: run tools/build_arch_image.sh (needs network) or set ARCH_IMAGE=/path"
            return 1
        fi
    done
    if ! grep -q '^linux-headers ' "$EDU_IMG/packages.txt"; then
        skip_case "$name" "$EDU_IMG has no linux-headers: rerun tools/build_arch_image.sh (the package list includes gcc, make, linux-headers)"
        return 1
    fi
    if ! command -v debugfs >/dev/null; then
        skip_case "$name" "debugfs missing (e2fsprogs)"
        return 1
    fi

    mkdir -p "$ROOT/build"
    EDU_DISK="$(mktemp "$ROOT/build/edu-guest.XXXXXX.img")"
    cp --sparse=always "$EDU_IMG/disk.img" "$EDU_DISK"
    EDU_UUID="$(blkid -p -o value -s UUID "$EDU_DISK")"

    cmds="$(mktemp)"
    log="$(mktemp)"
    {
        echo "mkdir /root/edu"
        for f in "$EDU_SRC/edu.c" "$EDU_SRC/edu_ioctl.h" "$EDU_SRC/eductl.c" "$EDU_SRC/Makefile" \
                 "$ROOT/include/devices/edu_regs.h" "$@"; do
            echo "write $f /root/edu/$(basename "$f")"
        done
        [ -n "$extra" ] && cat "$extra"
    } > "$cmds"
    if ! debugfs -w -f "$cmds" "$EDU_DISK" > "$log" 2>&1; then
        skip_case "$name" "debugfs could not write the sources into $EDU_DISK: $(tail -1 "$log")"
        rm -f "$EDU_DISK" "$cmds" "$log"
        return 1
    fi
    rm -f "$cmds" "$log"
    return 0
}

# edu_guest_boot GUEST_SHELL_ARGS...
#   启动带 edu 设备的 Arch guest，以 root 登录，依次执行给出的 --cmd，最后
#   关机；打印 VMM 和 guest 的全部输出，返回 guest_shell 的退出码。--smp 2
#   让中断处理和发起 ioctl 的线程可以落在不同的 vCPU 上。结束后删除 EDU_DISK。
edu_guest_boot() {
    local rc
    python3 "$GUEST_SHELL" --timeout 900 --login root "$@" \
        --stop "exec systemctl poweroff" \
        -- "$VMM" --mode linux --smp 2 --mem 1G --device edu \
              --device ahci,drive=d0 --drive id=d0,file="$EDU_DISK" \
              --initrd "$EDU_IMG/initramfs-linux.img" \
              --append "root=UUID=$EDU_UUID rw" "$EDU_IMG/vmlinuz-linux" 2>&1
    rc=$?
    rm -f "$EDU_DISK"
    return $rc
}
