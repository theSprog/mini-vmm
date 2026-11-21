# 001：实模式载荷一条指令都没执行

- 基线：master `7fcce1c`
- 范围：blog 01–02（一次 `out` 指令的完整旅程）涉及的机制

## 现象

用 `real16` 模式运行 16 位测试载荷，VMM 进入 guest 后立刻报 `KVM_EXIT_INTERNAL_ERROR` 退出，载荷的第一条 `out` 都没有发生（`exits=1 (io=0 ...)`）。VMM 自己打印的入口是 `CS:IP = 0000:0000`，载荷也确实加载到了 GPA 0。

每次运行结果都一样。

## 预期

载荷源码是 `tools/payload/real_mode_stub.S`。正常运行应该看到：

```
>>> guest wrote port 0x10: 0x42 (1 bytes) = 'B'
>>> guest wrote port 0x10: 0x5a (1 bytes) = 'Z'
[INFO] vCPU 0: HLT, stopping
[INFO] --- left guest (ok) ---
```

## 复现

在 worktree 里执行：

```sh
./build/vmm --mode real16 --mem 2M build/payload16.bin
```

不需要 guest 内核和 initramfs。

## 报告

写在本目录的 `report.md`，格式不限，至少回答：

1. vCPU 出错时实际在哪个 guest 物理地址取指，你从哪里看出来的；
2. 根因是什么，正确的状态应该是什么，依据是什么（规范、KVM 源码或 blog 里的哪一段）；
3. 你的修复，以及怎么证明它修好了；
4. 还有哪些没弄懂。

加分题：这个错误为什么表现为 `KVM_EXIT_INTERNAL_ERROR`，而不是 `KVM_EXIT_MMIO`？

## 流程

```sh
dojo start 1       # 建 worktree、编译、链接 guest 内核和 initramfs，然后进入 worktree
# 调试、修复，报告写在 dojo/001-real16-payload-internal-error/report.md
dojo submit        # 提交 worktree 里的全部改动并推送 solve/001，然后回到 mini-vmm 主目录；可以反复执行
dojo clean 1       # 评审归档后执行：删除 worktree 和本地分支，有未提交的内容时会拒绝
```

- 评审后 challenge、report、feedback 会归档到 master 的 `dojo/001-real16-payload-internal-error/`，`git pull` 后就能看到评审意见。

- `dojo` 是 `tools/dojo.sh` 定义的 shell 函数，在 `~/.zshrc` 里 source 它。没有 source 时用 `tools/dojo` 加同样的参数，目录要自己 `cd`（worktree 是 `../mini-vmm-dojo-001`）。
- 换一台机器继续：在那边执行 `dojo start 1`，它会从远端取回已经推送的 `solve/001`。
- `dojo list` 查看所有题目和状态。
- 不要拿 `dojo/001` 和 master 做 diff，那样等于直接看答案。
- 向 Claude 要了提示，就在报告里写明得到了什么提示。
