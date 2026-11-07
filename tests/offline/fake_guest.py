#!/usr/bin/env python3
"""tests/offline/fake_guest.py — 冒充 "VMM + guest shell"，给 guest_shell.py 做自测

模拟真 guest 上会让固定延迟喂命令失败的那个行为：启动期间（--boot 秒）送进
stdin 的字节全部丢弃，对应 guest 串口驱动打开 tty 时清空 RX FIFO。启动完成后
打印和真 initramfs 一样的提示符，逐行执行：

  echo X          输出 X
  poweroff -f / exit
                  输出 VMM 的关机日志，以 --rc 退出
  其他            输出 "sh: X: not found"

--no-prompt 时永远不打印提示符，模拟 guest 卡在启动里。
"""

import argparse
import os
import select
import sys
import time


def main():
    p = argparse.ArgumentParser(description="Fake VMM+guest shell for "
                                            "testing guest_shell.py offline.")
    p.add_argument("--boot", type=float, default=0.0,
                   help="seconds of boot during which stdin is discarded")
    p.add_argument("--rc", type=int, default=0, help="exit code on poweroff")
    p.add_argument("--no-prompt", action="store_true",
                   help="never print a prompt (guest hangs in boot)")
    a = p.parse_args()

    out = sys.stdout
    fd = sys.stdin.fileno()
    out.write("[    0.000000] Linux version 6.6.0 (fake)\n")
    out.flush()

    end = time.monotonic() + a.boot
    while True:
        left = end - time.monotonic()
        if left <= 0:
            break
        r, _, _ = select.select([fd], [], [], left)
        if r and not os.read(fd, 4096):
            # stdin EOF：不会再有输入，把剩下的启动时间走完
            time.sleep(max(end - time.monotonic(), 0))
            break
        # 读到的字节丢掉：tty 还没打开

    if a.no_prompt:
        out.write("[    1.000000] Run /init as init process\n")
        out.flush()
        time.sleep(3600)        # 等调用方超时后杀掉
        return 0

    out.write("[    1.000000] Run /init as init process\n")
    while True:
        out.write("[guest ~]# ")
        out.flush()
        line = sys.stdin.readline()
        if not line:
            time.sleep(3600)    # 真 shell 收到 EOF 前串口一直在，这里同样不退出
            return 0
        cmd = line.rstrip("\n")
        out.write(cmd + "\n")   # 回显
        if cmd.startswith("echo "):
            out.write(cmd[5:] + "\n")
        elif cmd in ("poweroff -f", "exit"):
            out.write("[vmm] guest requested poweroff via ACPI S5\n")
            out.flush()
            return a.rc
        else:
            out.write(f"sh: {cmd}: not found\n")


if __name__ == "__main__":
    sys.exit(main())
