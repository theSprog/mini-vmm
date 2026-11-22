# 004：Linux 启动到一半卡住，时间戳不再前进

- 基线：master `7980fce`
- 范围：中断（blog 03–04 原理与实现，29 MSI，34–36 split irqchip）

## 现象

不加任何额外参数启动 Linux，guest 卡在启动过程中，进不了 shell。最后几行是：

```
clocksource: Switched to clocksource kvm-clock
pnp: PnP ACPI init
pnp: PnP ACPI: found 0 devices
```

从某一行开始，后面所有日志行开头的时间戳都是同一个值，不再增长。时间戳停住的那些行里有一行：

```
APIC timer disabled due to verification failure
```

VMM 没有报错，一直在运行，要用 `Ctrl-A x` 退出。不加参数时每次都卡在同一个位置。加 `--smp 2` 或 `--kernel-irqchip split` 也会卡住，但停下的位置不一定和上面相同。

## 预期

正常启动到 shell，时间戳一直增长，没有 `APIC timer disabled` 这一行。

## 复现

在 worktree 里执行：

```sh
./build/vmm --initrd build/initramfs.cpio.gz guest-kernel/arch/x86/boot/bzImage
```

卡住后用 `Ctrl-A x` 退出。

## 报告

写在本目录的 `report.md`，格式不限，至少回答：

1. guest 缺的是哪一个中断，它本该从哪个设备、经过哪些环节送到 CPU；你从哪里看出来的；
2. 根因是什么，正确的状态应该是什么，依据是什么（规范章节、内核源码或 blog 里的哪一段）；
3. 你的修复，以及怎么证明它修好了；
4. 还有哪些没弄懂。

加分题：日志说 LAPIC 定时器“verification failure”，可这次改动并没有碰 LAPIC。LAPIC 定时器的校验依赖了什么？

## 流程

```sh
dojo start 4       # 建 worktree、编译、链接 guest 内核和 initramfs，然后进入 worktree
# 调试、修复，报告写在 dojo/004-boot-hangs-time-frozen/report.md
dojo submit        # 提交 worktree 里的全部改动并推送 solve/004，然后回到 mini-vmm 主目录；可以反复执行
dojo clean 4       # 评审归档后执行：删除 worktree 和本地分支，有未提交的内容时会拒绝
```

- 评审后 challenge、report、feedback 会归档到 master 的 `dojo/004-boot-hangs-time-frozen/`，`git pull` 后就能看到评审意见。

- `dojo` 是 `tools/dojo.sh` 定义的 shell 函数，在 `~/.zshrc` 里 source 它。没有 source 时用 `tools/dojo` 加同样的参数，目录要自己 `cd`（worktree 是 `../mini-vmm-dojo-004`）。
- 换一台机器继续：在那边执行 `dojo start 4`，它会从远端取回已经推送的 `solve/004`。
- `dojo list` 查看所有题目和状态。
- 不要拿 `dojo/004` 和 master 做 diff，那样等于直接看答案。
- 向 Claude 要了提示，就在报告里写明得到了什么提示。
