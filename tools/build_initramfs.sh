#!/usr/bin/env bash
# tools/build_initramfs.sh — Step 2.3 用的最小 busybox initramfs
#
# 用法：tools/build_initramfs.sh [输出文件]   默认 build/initramfs.cpio.gz
#
# 依赖一个静态链接的 busybox（Ubuntu: apt install busybox-static，
# 或者 BUSYBOX=/path/to/busybox 指定）。initramfs 里没有动态链接器，
# 动态 busybox 放进去会报 "No such file or directory"（找不到 ld.so）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/build/initramfs.cpio.gz}"
BUSYBOX="${BUSYBOX:-$(command -v busybox || true)}"

if [ -z "$BUSYBOX" ] || [ ! -x "$BUSYBOX" ]; then
    echo "busybox not found, set BUSYBOX=/path/to/static/busybox" >&2
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

# /init 是内核解开 initramfs 后执行的第一个用户态程序（PID 1）。
# 它退出会导致 kernel panic，所以最后用 exec 把 PID 1 交给 shell，
# shell 退出后再 poweroff，让 VMM 正常结束。
cat > "$STAGE/init" <<'EOF'
#!/bin/sh
mount -t proc     proc     /proc
mount -t sysfs    sysfs    /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mkdir -p /dev/pts && mount -t devpts devpts /dev/pts

echo
echo "mini-vmm initramfs: $(uname -sr), $(nproc) CPU(s), $(awk '/MemTotal/{print $2" kB"}' /proc/meminfo)"
echo "type 'poweroff -f' to exit the VM"
echo

# setsid + cttyhack 让 shell 拿到 ttyS0 作为控制终端，Ctrl-C 才有效
setsid cttyhack /bin/sh -l
poweroff -f
EOF
chmod +x "$STAGE/init"

cat > "$STAGE/etc/profile" <<'EOF'
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
export PS1='[guest \w]# '
EOF

mkdir -p "$(dirname "$OUT")"
( cd "$STAGE" && find . -print0 | cpio --null -o -H newc --quiet ) | gzip -9 > "$OUT"
echo "wrote $OUT ($(stat -c %s "$OUT") bytes)"
