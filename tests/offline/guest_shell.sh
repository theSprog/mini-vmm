#!/usr/bin/env bash
# tests/offline/guest_shell.sh — e2e 驱动脚本 tests/guest_shell.py 的自测
#
# e2e 用例全靠 guest_shell.py 往 guest shell 里送命令，它要是送早了、送丢了，
# 失败会表现成"VMM 超时"，和 VMM 自身的 bug 分不开。这里用
# tests/offline/fake_guest.py 冒充 VMM：启动期间收到的输入全部丢弃，
# 和真 guest 打开 tty 时清空 RX FIFO 一样，不需要 /dev/kvm，几秒跑完。
#
# 第一条是对照：按固定延迟喂命令，在假 guest 上必须丢命令。它证明假 guest
# 确实复现了那种失败，后面"慢启动照样不丢"的用例才有说服力。
#
# 用法：./tests/offline/guest_shell.sh

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

GS="$ROOT/tests/guest_shell.py"
FAKE="$ROOT/tests/offline/fake_guest.py"

# 整行精确匹配：guest 回显的命令行里也有同样的标记串，
# 只有单独成行的那一份才是命令真正执行后的输出
expect_line() {
    if ! printf '%s\n' "$CUR_OUT" | grep -qxF -- "$1"; then
        printf '   %sFAIL%s no output line equal to: %s\n' "$C_RED" "$C_RST" "$1"
        CUR_BAD=1
    fi
}

auto_tests() {

head1 "对照：固定延迟喂命令会丢"

# 故意用固定延迟：第 1 秒发命令，假 guest 要 3 秒才起来
CUR_NAME="fixed-delay feeding loses commands sent during boot"
printf '%s->%s %s\n' "$C_CYA" "$C_RST" "$CUR_NAME"
CUR_BAD=0
CUR_OUT="$( { sleep 1; printf 'echo MARK-A\npoweroff -f\n'; } \
            | timeout 5 python3 "$FAKE" --boot 3 2>&1 )"
CUR_RC=$?
expect_rc 124
expect_not "MARK-A"
end_case

head1 "guest_shell.py"

run_case "slow boot: every command waits for the prompt" \
    python3 "$GS" --timeout 20 --cmd 'echo MARK-A' --cmd 'echo MARK-B' \
        --stop 'poweroff -f' -- python3 "$FAKE" --boot 3
expect_rc 0
expect_line "MARK-A"
expect_line "MARK-B"
expect_has "guest requested poweroff via ACPI S5"
expect_not "[guest_shell]"
end_case

# --host 不往 guest 发字节，不能消耗提示符：后面的 MARK-B 还得发得出去。
# VMM_PID 指向的进程命令行里要有 fake_guest.py，证明给的是 VMM 的 pid
# 而不是中间某个 shell 的
run_case "--host runs on the host at a prompt, in order, with VMM_PID" \
    python3 "$GS" --timeout 20 --cmd 'echo MARK-A' \
        --host 'tr "\0" " " < /proc/$VMM_PID/cmdline; echo' \
        --cmd 'echo MARK-B' --stop 'poweroff -f' -- python3 "$FAKE" --boot 1
expect_rc 0
expect_line "MARK-A"
expect_line "MARK-B"
expect_has "fake_guest.py --boot 1"
expect_line "[host] rc=0"
order="$(printf '%s\n' "$CUR_OUT" | grep -xE 'MARK-A|MARK-B|\[host\] rc=0' | tr '\n' ' ')"
if [ "$order" != "MARK-A [host] rc=0 MARK-B " ]; then
    printf '   %sFAIL%s step order is "%s"\n' "$C_RED" "$C_RST" "$order"
    CUR_BAD=1
fi
end_case

run_case "VMM exit code is passed through" \
    python3 "$GS" --timeout 20 --stop 'exit' -- python3 "$FAKE" --rc 3
expect_rc 3
expect_has "guest requested poweroff via ACPI S5"
end_case

run_case "guest never reaches a prompt: 124 and the stuck step is named" \
    python3 "$GS" --timeout 2 --cmd 'echo MARK-A' --stop 'poweroff -f' \
        -- python3 "$FAKE" --no-prompt
expect_rc 124
expect_has "[guest_shell] timeout after 2s while waiting for prompt before cmd 1/1 'echo MARK-A'"
end_case

run_case "VMM exits before the commands are sent" \
    python3 "$GS" --timeout 20 --cmd 'echo MARK-A' --stop 'poweroff -f' -- false
expect_rc 1
expect_has "[guest_shell] VMM output ended before cmd 1/1 'echo MARK-A' was sent"
end_case

head1 "--login 和 --reboot"

run_case "--login: log in at the getty prompt, then commands use the standard prompt" \
    python3 "$GS" --timeout 20 --login root --cmd 'echo MARK-A' --cmd bootid \
        --stop 'systemctl poweroff' -- python3 "$FAKE" --login --boot 1
expect_rc 0
expect_line "MARK-A"
expect_line "BOOT=1"
expect_has "guest requested poweroff via ACPI S5"
expect_not "command not found"
end_case

# 对照：systemctl reboot 立刻回到提示符，用 --cmd 发时下一条命令进了
# 正在关闭的旧 shell，丢失；重启后的 stop 也就提前发了
run_case "control: reboot sent with --cmd loses the next command" \
    python3 "$GS" --timeout 20 --login root --cmd 'systemctl reboot' \
        --cmd 'echo MARK-B' --stop 'systemctl poweroff' \
        -- python3 "$FAKE" --login --boot 1
expect_rc 0
if printf '%s\n' "$CUR_OUT" | grep -qxF "MARK-B"; then
    printf '   %sFAIL%s MARK-B ran; the fake guest did not drop it\n' "$C_RED" "$C_RST"
    CUR_BAD=1
fi
end_case

run_case "--reboot: later steps wait for the login after the reboot" \
    python3 "$GS" --timeout 20 --login root --cmd bootid \
        --reboot 'systemctl reboot' --cmd bootid --cmd 'echo MARK-B' \
        --stop 'systemctl poweroff' -- python3 "$FAKE" --login --boot 1
expect_rc 0
expect_line "BOOT=1"
expect_line "BOOT=2"
expect_line "MARK-B"
expect_has "guest requested poweroff via ACPI S5"
end_case

run_case "--reboot without --login is rejected" \
    python3 "$GS" --timeout 20 --reboot 'systemctl reboot' --stop 'exit' \
        -- python3 "$FAKE"
expect_rc 2
expect_has "--reboot requires --login"
end_case

run_case "login asks for a password: 124 and the stuck step is named" \
    python3 "$GS" --timeout 2 --login nobody --cmd 'echo MARK-A' \
        --stop 'systemctl poweroff' -- python3 "$FAKE" --login
expect_rc 124
expect_has "[guest_shell] timeout after 2s while waiting for the shell after logging in as 'nobody'"
end_case

run_case "command that overflows the guest RX buffer is rejected" \
    python3 "$GS" --timeout 20 --cmd "echo $(printf 'x%.0s' $(seq 260))" \
        --stop 'poweroff -f' -- python3 "$FAKE"
expect_rc 2
expect_has "would overflow the guest RX FIFO"
end_case

}

main() {
    case "${1:-all}" in
    -m|--manual) return 0 ;;
    esac
    auto_tests
    summary
}

main "${1:-all}"
