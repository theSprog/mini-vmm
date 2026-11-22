#!/usr/bin/env bash
# tests/vmm_irqchip.sh — IRQCHIP=split|on 时 tests/lib.sh 用它代替 build/vmm
#
# 给每个 --mode linux 的启动补上 --kernel-irqchip $IRQCHIP，让同一套 e2e 用例
# 不改一行就能跑在另一种中断控制器布局上。裸 payload 模式不创建中断控制器，
# VMM 会拒绝这个选项，所以只在命令行里有 --mode linux 时才加；用例自己写了
# --kernel-irqchip 时以用例为准。用 exec 保持 PID 不变：guest_shell.py 的
# VMM_PID 和用例里的 kill 都指向这个进程。
set -u
VMM_BIN="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/build/vmm"
linux=0
prev=""
for a in "$@"; do
    [ "$a" = "--kernel-irqchip" ] && exec "$VMM_BIN" "$@"
    [ "$prev" = "--mode" ] && [ "$a" = "linux" ] && linux=1
    prev="$a"
done
if [ "$linux" = 1 ]; then
    exec "$VMM_BIN" --kernel-irqchip "$IRQCHIP" "$@"
fi
exec "$VMM_BIN" "$@"
