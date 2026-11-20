# tools/dojo.sh — 在 shell 里定义 dojo 函数，包装 tools/dojo 并负责切换目录
#
# 用法：在 ~/.zshrc 或 ~/.bashrc 里加一行 source <mini-vmm>/tools/dojo.sh
#   dojo start 1    建好 worktree 并 cd 进去
#   dojo submit     提交推送后 cd 回 mini-vmm 主目录
#   dojo list / clean / where / pack 和 tools/dojo 相同，在任何目录都能用
#
# 子进程改不了父 shell 的当前目录，所以“进入 worktree”只能由 shell 函数完成。

if [ -n "${ZSH_VERSION:-}" ]; then
    eval '_dojo_src=${(%):-%x}'     # zsh 里被 source 的文件路径
else
    _dojo_src=${BASH_SOURCE[0]}
fi
DOJO_MAIN=$(cd "$(dirname "$_dojo_src")/.." && pwd -P)
unset _dojo_src

dojo() {
    DOJO_MAIN="$DOJO_MAIN" "$DOJO_MAIN/tools/dojo" "$@" || return
    case "${1:-}" in
    start)  cd "$(DOJO_MAIN="$DOJO_MAIN" "$DOJO_MAIN/tools/dojo" where "$2")" ;;
    submit) cd "$DOJO_MAIN" ;;
    esac
}
