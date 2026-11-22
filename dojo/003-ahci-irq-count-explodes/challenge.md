# 003：AHCI 磁盘的中断次数多出几千倍

- 基线：master `7980fce`
- 范围：中断（blog 03–04 原理与实现，29 MSI，34–36 split irqchip）

## 现象

给 guest 挂一块 AHCI 磁盘，并用 `pci=nomsi` 让它使用传统的 INTx 引脚中断。启动变慢，进 shell 后盘还能读，但读 64 MiB 比正常慢一个数量级以上。读完之后 `/proc/interrupts` 里 IRQ 17（ahci）的计数有几十万。每次运行都是这样。

有的运行里，内核还会在启动时打出一段带调用栈的报告，然后关掉这个中断；这一段出不出现和机器当时的负载有关：

```
irq 17: nobody cared (try booting with the "irqpoll" option)
...
handlers:
[<...>] ahci_single_level_irq_intr
Disabling IRQ #17
```

## 预期

没有 `nobody cared`，IRQ 17 的计数和读盘的次数在同一个量级（启动加读 64 MiB 只有几十次），读盘速度正常。

## 复现

在 worktree 里执行：

```sh
truncate -s 64M build/disk.img
./build/vmm --initrd build/initramfs.cpio.gz --mem 512M \
    --drive id=d0,file=build/disk.img --device ahci,drive=d0 \
    --append pci=nomsi guest-kernel/arch/x86/boot/bzImage
```

进 shell 后执行：

```sh
dd if=/dev/sda of=/dev/null bs=1M count=64
grep ahci /proc/interrupts
```

用 `poweroff -f` 退出。

## 报告

写在本目录的 `report.md`，格式不限，至少回答：

1. 这几十万次中断里，设备真正有事要报的大概有几次？多出来的那些是怎么产生的，你的证据是什么；
2. 根因是什么，正确的行为应该是什么，依据是什么（规范章节、内核源码或 blog 里的哪一段）；
3. 你的修复，以及怎么证明它修好了；
4. 还有哪些没弄懂。

加分题：去掉 `--append pci=nomsi`，让设备用 MSI，就没有这个问题。为什么同一个设备模型换成 MSI 就不出事？

## 流程

```sh
dojo start 3       # 建 worktree、编译、链接 guest 内核和 initramfs，然后进入 worktree
# 调试、修复，报告写在 dojo/003-ahci-irq-count-explodes/report.md
dojo submit        # 提交 worktree 里的全部改动并推送 solve/003，然后回到 mini-vmm 主目录；可以反复执行
dojo clean 3       # 评审归档后执行：删除 worktree 和本地分支，有未提交的内容时会拒绝
```

- 评审后 challenge、report、feedback 会归档到 master 的 `dojo/003-ahci-irq-count-explodes/`，`git pull` 后就能看到评审意见。

- `dojo` 是 `tools/dojo.sh` 定义的 shell 函数，在 `~/.zshrc` 里 source 它。没有 source 时用 `tools/dojo` 加同样的参数，目录要自己 `cd`（worktree 是 `../mini-vmm-dojo-003`）。
- 换一台机器继续：在那边执行 `dojo start 3`，它会从远端取回已经推送的 `solve/003`。
- `dojo list` 查看所有题目和状态。
- 不要拿 `dojo/003` 和 master 做 diff，那样等于直接看答案。
- 向 Claude 要了提示，就在报告里写明得到了什么提示。
