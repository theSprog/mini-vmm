#!/usr/bin/env bash
# tools/build_initramfs.sh — linux-boot 等 e2e 用的最小 busybox initramfs
#
# 用法：tools/build_initramfs.sh [输出文件]   默认 build/initramfs.cpio.gz
#
# 依赖一个静态链接的 busybox（Ubuntu: apt install busybox-static，
# 或者 BUSYBOX=/path/to/busybox 指定）。initramfs 里没有动态链接器，
# 动态 busybox 放进去会报 "No such file or directory"（找不到 ld.so）。
# 打包用 bsdtar（libarchive），原因见下方 /dev/console 一段。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/build/initramfs.cpio.gz}"
BUSYBOX="${BUSYBOX:-$(command -v busybox || true)}"

if [ -z "$BUSYBOX" ] || [ ! -x "$BUSYBOX" ]; then
    echo "busybox not found, set BUSYBOX=/path/to/static/busybox" >&2
    exit 1
fi
if ! command -v bsdtar >/dev/null; then
    echo "bsdtar not found (package: libarchive / libarchive-tools)" >&2
    exit 1
fi
if ! file -L "$BUSYBOX" | grep -q 'statically linked'; then
    echo "$BUSYBOX is not statically linked" >&2
    exit 1
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE"/{bin,sbin,etc,proc,sys,dev,tmp,root,usr/bin,usr/sbin}
cp "$BUSYBOX" "$STAGE/bin/busybox"
for app in $("$STAGE/bin/busybox" --list-full); do
    [ "$app" = "bin/busybox" ] && continue
    mkdir -p "$STAGE/$(dirname "$app")"
    ln -sf /bin/busybox "$STAGE/$app"
done

# /init 是内核解开 initramfs 后执行的第一个用户态程序（PID 1），这里让它就是
# busybox 本身（argv[0] 为 init，即 busybox init）。
#
# 为什么不写成 #!/bin/sh 脚本：执行脚本要靠内核的 binfmt_script，发行版式配置
# 常把它编成模块（CONFIG_BINFMT_SCRIPT=m），而 initramfs 里没有模块，
# 内核会报 "Failed to execute /init (error -8)"。ELF 的 /init 不依赖它。
# 同理 inittab 里调用脚本一律显式写 /bin/sh 前缀：busybox init 对不含 shell
# 元字符的命令直接 execve，脚本同样会撞上 binfmt_script。
#
# PID 1 退出会导致 kernel panic，busybox init 自己永不退出。流程是 sysinit
# 跑初始化，第一条 wait 跑交互 shell，shell 退出后第二条 wait 跑 poweroff，
# 让 VMM 正常结束。
ln -sf /bin/busybox "$STAGE/init"

mkdir -p "$STAGE/etc/init.d"
cat > "$STAGE/etc/init.d/rcS" <<'EOF'
mount -t proc     proc     /proc
mount -t sysfs    sysfs    /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mkdir -p /dev/pts && mount -t devpts devpts /dev/pts

echo
echo "mini-vmm initramfs: $(uname -sr), $(nproc) CPU(s), $(awk '/MemTotal/{print $2" kB"}' /proc/meminfo)"
echo "type 'poweroff -f' to exit the VM"
echo
EOF

# busybox init 给每个子进程 setsid()，cttyhack 再把 /dev/console 背后的真实
# 终端（ttyS0）设成 shell 的控制终端，Ctrl-C 才有效。
cat > "$STAGE/etc/inittab" <<'EOF'
::sysinit:/bin/sh /etc/init.d/rcS
::wait:/bin/cttyhack /bin/sh -l
::wait:/sbin/poweroff -f
EOF

# inittab 和 /init 里写的绝对路径必须在 initramfs 内真实存在。busybox 把每个
# applet 装在 --list-full 给出的固定目录（例如 poweroff 在 sbin，sh 在 bin），
# 路径写错不会在构建时暴露：guest 里 init 只打印 "can't run" 然后跳过这一条，
# 关机那一条失效时 VM 就停在 init 里不退出。所以这里在打包前逐个核对。
#
# 链接按 initramfs 根解析而不是 host 根：applet 链接的目标是 /bin/busybox，
# 直接 test -e 会去看 host 的 /bin/busybox，host 有没有它都和 guest 无关。
stage_has() {
    local p="$1" n
    for n in 1 2 3 4 5 6 7 8; do
        if [ -L "$STAGE$p" ]; then
            local t
            t="$(readlink "$STAGE$p")"
            case "$t" in
                /*) p="$t" ;;
                *)  p="$(dirname "$p")/$t" ;;
            esac
        else
            [ -e "$STAGE$p" ]
            return
        fi
    done
    return 1
}
missing=0
for p in /init $(cut -d: -f4- "$STAGE/etc/inittab" | tr ' ' '\n' | grep '^/'); do
    if ! stage_has "$p"; then
        echo "initramfs self-check: $p does not exist in the image" >&2
        missing=1
    fi
done
[ "$missing" -eq 0 ] || exit 1

cat > "$STAGE/etc/profile" <<'EOF'
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
export PS1='[guest \w]# '
EOF

# 内核在执行 /init 之前就要打开 /dev/console 作为 PID 1 的 stdin/stdout/stderr，
# 此时 /init 还没来得及挂 devtmpfs，所以设备节点必须预先放进 cpio。
# 缺了它内核报 "unable to open an initial console"，/init 的输出全部丢失。
# 普通用户不能 mknod，这里用 bsdtar 从 mtree 描述直接生成设备节点条目，不需要 root。
cat > "$STAGE.mtree" <<'EOF'
#mtree
./dev/console type=char mode=0600 uid=0 gid=0 device=native,5,1
EOF
trap 'rm -rf "$STAGE" "$STAGE.mtree"' EXIT

mkdir -p "$(dirname "$OUT")"
( cd "$STAGE" && bsdtar --format newc -cf - . "@$STAGE.mtree" ) | gzip -9 > "$OUT"
echo "wrote $OUT ($(stat -c %s "$OUT") bytes)"
