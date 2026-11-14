#!/usr/bin/env bash
# tools/build_arch_image.sh — 生成 arch-distro 用的 Arch Linux 磁盘镜像
#
# 产物（默认放在仓库的 arch-image/，已被 .gitignore 忽略）：
#   disk.img              整盘 ext4，没有分区表，guest 里是 /dev/sda
#   vmlinuz-linux         从镜像 /boot 拷出来的官方内核，VMM 直启它
#   initramfs-linux.img   同上，mkinitcpio 生成
#   packages.txt          镜像里每个包的名字和版本
#   repo/                 测试用的本地 pacman 仓库（e2e 在 host 侧用 HTTP 提供）
#   cache/                pacman 包缓存，重跑时不必重新下载
#
# 用法：
#   tools/build_arch_image.sh [-o DIR] [-s SIZE] [-d YYYY/MM/DD | -m SERVER]
#
#   -d  从 Arch Linux Archive 这一天的快照安装（默认 2026/11/01）
#   -m  改用普通镜像源，SERVER 写成 pacman mirrorlist 的 Server 格式，例如
#       'https://mirrors.ustc.edu.cn/archlinux/$repo/os/$arch'（加单引号，
#       $repo 和 $arch 由 pacman 展开）。连不上 archive.archlinux.org 的
#       环境用它
#
# 可复现：默认软件源是 Archive 的某一天快照，同一份脚本重跑装进去的包和
# 版本都相同，packages.txt 逐字一致；配置文件全部由本脚本写出。镜像不是
# 逐字节相同，ext4 的时间戳和目录哈希种子每次不同，等价性以 packages.txt
# 和配置文件为准。普通镜像源只保留每个包的最新版本，用 -m 时装的是镜像
# 当天的版本，换一天重跑包集合相同而版本可能不同，对比 packages.txt 能
# 看出差在哪些包。
#
# 全程不需要 root：pacstrap -N 在 user namespace 里安装，之后的配置和
# mkfs.ext4 也在同样映射的 user namespace 里做（见 in_ns）。镜像目录里
# 属于 guest 各个系统用户的文件在 host 上显示为 /etc/subuid 分出的 uid，
# 所以清理这个目录也要经过 in_ns。

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/arch-image"
SIZE=8G
SNAPSHOT=2026/11/01
SERVER=""
SNAPSHOT_SET=0

# 根文件系统的 UUID 固定下来，fstab 和内核命令行都按它找根，重建镜像后
# e2e 的命令行不用跟着变。
ROOT_UUID=6d696e69-766d-4d2d-8172-636800000001

# guest 里的包：base 是最小系统，linux 带来官方内核和 mkinitcpio；fio 和
# iperf3 是 exit 基线的负载。
PACKAGES=(base linux fio iperf3)
# 测试仓库里的包：镜像里没装、依赖只有 glibc 的小包，e2e 让 guest 经
# e1000 从 host 侧的本地仓库下载安装它。
REPO_PACKAGES=(tree)

usage() {
    sed -n '2,/^$/{s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"
}

while getopts ':o:s:d:m:h' opt; do
    case $opt in
    o) OUT="$(realpath -m "$OPTARG")" ;;
    s) SIZE="$OPTARG" ;;
    d) SNAPSHOT="$OPTARG"; SNAPSHOT_SET=1 ;;
    m) SERVER="$OPTARG" ;;
    h) usage; exit 0 ;;
    *) echo "unknown option -$OPTARG, see -h" >&2; exit 1 ;;
    esac
done

if [[ ! $SNAPSHOT =~ ^[0-9]{4}/[0-9]{2}/[0-9]{2}$ ]]; then
    echo "-d expects YYYY/MM/DD, got '$SNAPSHOT'" >&2
    exit 1
fi

for tool in pacstrap pacman repo-add mkfs.ext4 unshare; do
    command -v "$tool" >/dev/null || { echo "missing $tool (arch-install-scripts, pacman, e2fsprogs, util-linux)" >&2; exit 1; }
done

if [[ -n $SERVER ]] && (( SNAPSHOT_SET )); then
    echo "-d and -m pick the package source two ways, give only one" >&2
    exit 1
fi
if [[ -z $SERVER ]]; then
    SERVER="https://archive.archlinux.org/repos/$SNAPSHOT/\$repo/os/\$arch"
elif [[ $SERVER != *'$repo'* || $SERVER != *'$arch'* ]]; then
    echo "-m expects a pacman Server URL containing \$repo and \$arch, got '$SERVER'" >&2
    echo "example: -m 'https://mirrors.ustc.edu.cn/archlinux/\$repo/os/\$arch'" >&2
    exit 1
fi
FS="$OUT/rootfs"
CONF="$OUT/pacman.conf"

# 和 pacstrap -N 同样的映射：namespace 里的 0 是当前用户，1 起是
# /etc/subuid 分给当前用户的那段。两次进入映射相同，文件属主才前后一致。
in_ns() {
    unshare --map-auto --map-root-user --setuid 0 --setgid 0 "$@"
}

mkdir -p "$OUT/cache"
if [[ -e $FS ]]; then
    echo "removing previous $FS"
    in_ns rm -rf "$FS"
fi
rm -f "$OUT/disk.img"

# 只开 core 和 extra，不继承 host 的 pacman.conf：host 上可能有第三方仓库
# 和本地选项，会让镜像内容随 host 而变。
cat > "$CONF" <<EOF
[options]
HoldPkg = pacman glibc
Architecture = auto
CacheDir = $OUT/cache/
SigLevel = Required DatabaseOptional
LocalFileSigLevel = Optional
ParallelDownloads = 5

[core]
Server = $SERVER

[extra]
Server = $SERVER
EOF

# 挂根需要的驱动：mkinitcpio 的 autodetect 按构建机上已加载的驱动挑模块，
# 构建机（host）上没有 ahci，所以驱动若是模块就会被漏掉，内核找不到根。
# Arch 的内核配置目前把 ahci、sd_mod、ext4 都编进内核（看镜像里的
# modules.builtin），mkinitcpio 遇到编进内核的条目直接跳过；显式列出是为
# 了 Arch 哪天把它们改成模块时 initramfs 里仍然有。e1000 不需要进
# initramfs，switch_root 之后 udev 从根文件系统加载它。
# 这个文件要在装 linux 之前放好，因为装 linux 时的 hook 就会生成 initramfs。
mkdir -p "$FS/etc/mkinitcpio.conf.d"
cat > "$FS/etc/mkinitcpio.conf.d/mini-vmm.conf" <<'EOF'
MODULES=(ahci sd_mod ext4)
EOF
# sd-vconsole hook 找不到这个文件会报警告
echo KEYMAP=us > "$FS/etc/vconsole.conf"

echo "pacstrap from $SERVER"
# 安装脚本在新根里运行，那里没有生成任何 locale；host 的 LC_ALL 漏进去
# 只会让每个脚本都报 setlocale 警告
LC_ALL=C pacstrap -N -C "$CONF" -c -K -M "$FS" "${PACKAGES[@]}"

# ---- 配置 ----
in_ns bash -euo pipefail -s -- "$FS" "$ROOT_UUID" "$SERVER" <<'EOS'
fs=$1 uuid=$2 server=$3

echo "UUID=$uuid / ext4 rw,relatime 0 1" > "$fs/etc/fstab"
echo mini-vmm-arch > "$fs/etc/hostname"
echo LANG=C.UTF-8 > "$fs/etc/locale.conf"
ln -sf /usr/share/zoneinfo/UTC "$fs/etc/localtime"

# guest 里 pacman 用构建时的同一个源，默认是同一天的快照，和镜像里已装
# 的版本一致
echo "Server = $server" > "$fs/etc/pacman.d/mirrorlist"

# root 空密码，串口 login 提示符输入 root 直接登录。镜像只在本机测试用。
sed -i 's/^root:[^:]*:/root::/' "$fs/etc/shadow"

# e1000 在 guest 里的接口名由 udev 按 PCI 位置命名（enp0sN），匹配 en*
mkdir -p "$fs/etc/systemd/network"
cat > "$fs/etc/systemd/network/20-wired.network" <<'EOF'
[Match]
Name=en*

[Network]
DHCP=ipv4
EOF
systemctl --root="$fs" enable systemd-networkd.service systemd-resolved.service
ln -sf /run/systemd/resolve/stub-resolv.conf "$fs/etc/resolv.conf"

# 包缓存不进镜像
rm -rf "$fs/var/cache/pacman/pkg/"*
EOS

pacman -Q --config "$CONF" --root "$FS" --dbpath "$FS/var/lib/pacman" > "$OUT/packages.txt"
cp "$FS/boot/vmlinuz-linux" "$FS/boot/initramfs-linux.img" "$OUT/"
chmod 644 "$OUT/vmlinuz-linux" "$OUT/initramfs-linux.img"

# ---- 测试仓库 ----
# 包和签名从构建用的同一个源下载；repo-add 把签名写进数据库，guest 按镜像里
# archlinux-keyring 的公钥校验，和从真实镜像源安装走同样的校验。
rm -rf "$OUT/repo" "$OUT/repo-db"
mkdir -p "$OUT/repo" "$OUT/repo-db"
in_ns pacman --config "$CONF" --dbpath "$OUT/repo-db" -Sy >/dev/null
mapfile -t urls < <(in_ns pacman --config "$CONF" --dbpath "$OUT/repo-db" -Sp "${REPO_PACKAGES[@]}" --assume-installed glibc)
rm -rf "$OUT/repo-db"
for url in "${urls[@]}"; do
    name="${url##*/}"
    curl -fsSL -o "$OUT/repo/$name" "$url"
    curl -fsSL -o "$OUT/repo/$name.sig" "$url.sig"
done
repo-add -q "$OUT/repo/mvmm.db.tar.gz" "$OUT/repo/"*.pkg.tar.zst

# ---- 磁盘镜像 ----
# mkfs.ext4 -d 按 stat 看到的属主写 inode，必须在同一个 namespace 里跑，
# 否则 guest 里所有文件都会属于 host 上的那些 uid。
in_ns mkfs.ext4 -q -F -L arch -U "$ROOT_UUID" -d "$FS" "$OUT/disk.img" "$SIZE"

echo
echo "image:   $OUT/disk.img ($SIZE, root UUID $ROOT_UUID)"
echo "kernel:  $OUT/vmlinuz-linux"
echo "initrd:  $OUT/initramfs-linux.img"
echo "repo:    $OUT/repo ($(ls "$OUT/repo" | grep -c '\.pkg\.tar\.zst$') packages)"
echo "packages: $(wc -l < "$OUT/packages.txt") installed, list in $OUT/packages.txt"
