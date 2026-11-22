# 002：挂上 AHCI 磁盘后 guest 里没有 /dev/sda

- 基线：master `7980fce`
- 范围：中断（blog 03–04 原理与实现，29 MSI，34–36 split irqchip）

## 现象

给 guest 挂一块 AHCI 磁盘，不加任何别的参数启动。内核探测到控制器和 ata1 的链路（`ata1: SATA link up`），但对盘发的 IDENTIFY 命令（`cmd 0xec`）先后超时三次，分别等了 5、10、30 秒：

```
ata1.00: qc timeout after 5000 msecs (cmd 0xec)
ata1.00: failed to IDENTIFY (I/O error, err_mask=0x4)
```

最后启动进了 shell，但 `/dev/sda` 不存在。每次运行结果都一样。

## 预期

启动时没有超时，进 shell 后能读盘：

```
[guest ~]# dd if=/dev/sda of=/dev/null bs=1M count=64
67108864 bytes (64.0MB) copied, ...
```

## 复现

在 worktree 里执行：

```sh
truncate -s 64M build/disk.img
./build/vmm --initrd build/initramfs.cpio.gz --mem 512M \
    --drive id=d0,file=build/disk.img --device ahci,drive=d0 \
    guest-kernel/arch/x86/boot/bzImage
```

进 shell 后执行 `ls /dev/sda`、`cat /proc/interrupts`，用 `poweroff -f` 退出。

## 报告

写在本目录的 `report.md`，格式不限，至少回答：

1. 盘的命令其实执行完了吗？完成通知卡在了哪一步，你的证据是什么；
2. 根因是什么，正确的行为应该是什么，依据是什么（规范章节、内核源码或 blog 里的哪一段）；
3. 你的修复，以及怎么证明它修好了；
4. 还有哪些没弄懂。

加分题：启动参数加上 `--append pci=nomsi` 之后磁盘就正常了，为什么？

## 流程

```sh
dojo start 2       # 建 worktree、编译、链接 guest 内核和 initramfs，然后进入 worktree
# 调试、修复，报告写在 dojo/002-ahci-identify-timeout/report.md
dojo submit        # 提交 worktree 里的全部改动并推送 solve/002，然后回到 mini-vmm 主目录；可以反复执行
dojo clean 2       # 评审归档后执行：删除 worktree 和本地分支，有未提交的内容时会拒绝
```

- 评审后 challenge、report、feedback 会归档到 master 的 `dojo/002-ahci-identify-timeout/`，`git pull` 后就能看到评审意见。

- `dojo` 是 `tools/dojo.sh` 定义的 shell 函数，在 `~/.zshrc` 里 source 它。没有 source 时用 `tools/dojo` 加同样的参数，目录要自己 `cd`（worktree 是 `../mini-vmm-dojo-002`）。
- 换一台机器继续：在那边执行 `dojo start 2`，它会从远端取回已经推送的 `solve/002`。
- `dojo list` 查看所有题目和状态。
- 不要拿 `dojo/002` 和 master 做 diff，那样等于直接看答案。
- 向 Claude 要了提示，就在报告里写明得到了什么提示。
