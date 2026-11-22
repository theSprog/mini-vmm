# 005：split irqchip 加 noapic 启动后没有 shell 提示符

- 基线：master `7980fce`
- 范围：中断（blog 03–04 原理与实现，29 MSI，34–36 split irqchip）

## 现象

用 `--kernel-irqchip split` 启动 Linux，并给内核加 `noapic`。内核日志一路正常，打到下面这一行之后只多出一个空行，然后再也没有输出，shell 提示符一直不出现：

```
Run /init as init process

```

VMM 没有报错，一直在运行，要用 `Ctrl-A x` 退出。加 `--smp 2` 也一样。

对照：

- 去掉 `--kernel-irqchip split`（中断控制器都在 KVM 里），保留 `noapic`：正常进 shell。
- 保留 `--kernel-irqchip split`，去掉 `noapic`：正常进 shell。

每次运行结果都一样。

## 预期

正常进 shell。`/proc/interrupts` 里 timer 和 ttyS0 都是 `XT-PIC`，计数在增长：

```
   0:        310  XT-PIC          timer
   4:         56  XT-PIC          ttyS0
```

## 复现

在 worktree 里执行：

```sh
./build/vmm --initrd build/initramfs.cpio.gz --kernel-irqchip split \
    --append noapic guest-kernel/arch/x86/boot/bzImage
```

卡住后用 `Ctrl-A x` 退出。

## 报告

写在本目录的 `report.md`，格式不限，至少回答：

1. 卡住的时候，哪些中断还在送达，哪些不再送达，你的证据是什么；
2. 根因是什么，正确的行为应该是什么，依据是什么（芯片手册、内核源码或 blog 里的哪一段）；
3. 你的修复，以及怎么证明它修好了；
4. 还有哪些没弄懂。

加分题：内核自己的日志一直打得出来，用户态程序的输出却出不来。两者往串口写字节的方式有什么不同？

## 流程

```sh
dojo start 5       # 建 worktree、编译、链接 guest 内核和 initramfs，然后进入 worktree
# 调试、修复，报告写在 dojo/005-split-noapic-no-shell-prompt/report.md
dojo submit        # 提交 worktree 里的全部改动并推送 solve/005，然后回到 mini-vmm 主目录；可以反复执行
dojo clean 5       # 评审归档后执行：删除 worktree 和本地分支，有未提交的内容时会拒绝
```

- 评审后 challenge、report、feedback 会归档到 master 的 `dojo/005-split-noapic-no-shell-prompt/`，`git pull` 后就能看到评审意见。

- `dojo` 是 `tools/dojo.sh` 定义的 shell 函数，在 `~/.zshrc` 里 source 它。没有 source 时用 `tools/dojo` 加同样的参数，目录要自己 `cd`（worktree 是 `../mini-vmm-dojo-005`）。
- 换一台机器继续：在那边执行 `dojo start 5`，它会从远端取回已经推送的 `solve/005`。
- `dojo list` 查看所有题目和状态。
- 不要拿 `dojo/005` 和 master 做 diff，那样等于直接看答案。
- 向 Claude 要了提示，就在报告里写明得到了什么提示。
