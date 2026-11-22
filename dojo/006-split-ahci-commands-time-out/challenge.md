# 006：split irqchip 下 AHCI 磁盘的命令只成功一次

- 基线：master `7980fce`
- 范围：中断（blog 03–04 原理与实现，29 MSI，34–36 split irqchip）

## 现象

用 `--kernel-irqchip split` 启动，挂一块 AHCI 磁盘，并用 `pci=nomsi` 让它使用传统的 INTx 引脚中断。内核读到了盘的型号，说明 IDENTIFY 命令成功了，但之后的命令全部超时：

```
ata1.00: ATA-8: MINIVMM AHCI DISK, 1.0, max UDMA/133
ata1.00: 131072 sectors, multi 0: LBA48 NCQ (depth 32)
ata1.00: qc timeout after 15000 msecs (cmd 0xef)
ata1.00: failed to set xfermode (err_mask=0x4)
...
ata1.00: qc timeout after 5000 msecs (cmd 0xec)
ata1.00: failed to IDENTIFY (I/O error, err_mask=0x4)
```

启动多花了约 30 秒，进 shell 后 `/dev/sda` 不存在，`/proc/interrupts` 里 IRQ 17（ahci）的计数是 1。

对照：

- 去掉 `--kernel-irqchip split`，保留 `pci=nomsi`：磁盘正常。
- 保留 `--kernel-irqchip split`，去掉 `pci=nomsi`（走 MSI）：磁盘正常。

每次运行结果都一样。

## 预期

启动时没有超时，`/dev/sda` 存在，`dd if=/dev/sda of=/dev/null bs=1M count=64` 能读完。

## 复现

在 worktree 里执行：

```sh
truncate -s 64M build/disk.img
./build/vmm --initrd build/initramfs.cpio.gz --mem 512M \
    --drive id=d0,file=build/disk.img --device ahci,drive=d0 \
    --kernel-irqchip split --append pci=nomsi \
    guest-kernel/arch/x86/boot/bzImage
```

进 shell 后执行 `ls /dev/sda`、`grep ahci /proc/interrupts`，用 `poweroff -f` 退出。

## 报告

写在本目录的 `report.md`，格式不限，至少回答：

1. IRQ 17 为什么恰好来了一次？第一次之后，是设备没再请求中断，还是请求了没送到 CPU，你的证据是什么；
2. 根因是什么，正确的行为应该是什么，依据是什么（芯片手册、KVM 文档或源码、blog 里的哪一段）；
3. 你的修复，以及怎么证明它修好了；
4. 还有哪些没弄懂。

加分题：同样的设备、同样的 `pci=nomsi`，中断控制器都在 KVM 里时就没问题。这两种模式下，“guest 处理完一个中断”这件事分别是怎么通知到 IOAPIC 的？

## 流程

```sh
dojo start 6       # 建 worktree、编译、链接 guest 内核和 initramfs，然后进入 worktree
# 调试、修复，报告写在 dojo/006-split-ahci-commands-time-out/report.md
dojo submit        # 提交 worktree 里的全部改动并推送 solve/006，然后回到 mini-vmm 主目录；可以反复执行
dojo clean 6       # 评审归档后执行：删除 worktree 和本地分支，有未提交的内容时会拒绝
```

- 评审后 challenge、report、feedback 会归档到 master 的 `dojo/006-split-ahci-commands-time-out/`，`git pull` 后就能看到评审意见。

- `dojo` 是 `tools/dojo.sh` 定义的 shell 函数，在 `~/.zshrc` 里 source 它。没有 source 时用 `tools/dojo` 加同样的参数，目录要自己 `cd`（worktree 是 `../mini-vmm-dojo-006`）。
- 换一台机器继续：在那边执行 `dojo start 6`，它会从远端取回已经推送的 `solve/006`。
- `dojo list` 查看所有题目和状态。
- 不要拿 `dojo/006` 和 master 做 diff，那样等于直接看答案。
- 向 Claude 要了提示，就在报告里写明得到了什么提示。
