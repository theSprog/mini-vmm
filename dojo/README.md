# dojo：故障驱动的调试训练

Claude 在 mini-vmm 里植入一个 bug，只交给你一个故障现场，你自己定位、修复并写实验报告，再由 Claude 评审。目的是用故障逼自己建立对系统的因果模型，而不是读懂别人写好的解释。

## 每道题的文件

每道题一个目录 `dojo/NNN-<现象>/`，名字描述最初看到的现象，不描述根因，因为根因写进名字就等于给了答案。

| 文件 | 谁写 | 内容 |
|---|---|---|
| `challenge.md` | Claude | 故障现象、预期行为、复现步骤、基线 commit。不含根因、改动位置和涉及的机制 |
| `report.md` | 你 | 假设、实验、证据、根因、被破坏的约束、修复与验证、仍未理解的问题 |
| `feedback.md` | Claude | 对报告的技术审查：哪些结论缺证据或有误、有没有更有区分度的实验、值得继续追的问题 |

`report.md` 只由你撰写，Claude 只读不改。评审时如果你的解释有错，Claude 指出错在哪、提出可以验证的问题，不直接给出完整解释；等你改完或明确要答案时再讲全。理由：报告的价值在于检验你自己的理解，被代写或润色就失去了这个作用。

## 流程

一道题的全部材料都在题目分支 `dojo/NNN` 上：植入了 bug 的源码加上 `challenge.md`。你只在一个 worktree 里工作，修复和 `report.md` 都留在那里，一起提交到 `solve/NNN`。评审后所有材料归档到 master 的 `dojo/NNN-<现象>/`，题目的分支随之删除，分支数不会随题目增长。

在 `~/.zshrc` 里加一行 `source ~/mini-vmm/tools/dojo.sh`（路径按实际位置），就能用 `dojo` 命令：`dojo start N` 建好 worktree 后直接进入，`dojo submit` 推送后回到 mini-vmm 主目录。不加这一行也能用 `tools/dojo`，只是要自己 `cd`。

| 步骤 | 谁 | 在哪 | 命令 |
|---|---|---|---|
| 出题 | Claude | master | `tools/dojo pack <现象> --from <植入后的副本> --challenge <文件> --push` |
| 看题 | 你 | 任意 | `dojo list` |
| 开始 | 你 | master | `dojo start N` |
| 交作业 | 你 | worktree | `dojo submit` |
| 评审归档 | Claude | master | `tools/dojo archive N --feedback <文件>` |
| 收尾 | 你 | master | `git pull` 看评审意见，然后 `dojo clean N` |

- 每个命令只能在表里写的位置运行，在别处会被拒绝。这样不会在题目分支上开新题，也不会把作业交到错的分支。
- `start` 会在仓库旁边建 worktree、编译、构建缺失的 initramfs，并把 guest 内核和 initramfs 链接进去，`challenge.md` 里的命令可以原样运行。在另一台机器上它会先 fetch，接着已经推送的 `solve/NNN` 做。
- `submit` 把 worktree 里的所有改动提交并推送 `solve/NNN`，包括调试时加的打印，评审时都看得到。它不删除任何东西，可以反复执行。
- `archive` 把 challenge、report、feedback，以及植入的改动 `bug.diff`、你的修复 `fix.diff` 写进 master 的题目目录，提交并推送 master 之后才删除远端和本地的题目分支。
- `clean` 删除 worktree 和本地残留的分支；有未提交的内容，或者 `solve/NNN` 比归档时更新，会拒绝执行。
- 题号由 `pack` 自动分配：取本地和远端分支、master 已归档目录里用过的最大编号加一，打包前会先 fetch，两台机器轮流出题不会撞号。
- 题目分支没有父提交，`git log`、`git show` 和编辑器的修改高亮都看不出改了哪里。

## 出题要求

- `challenge.md` 里写的每一条现象、每一个对照（例如“换成某参数就没有这个问题”）都要在植入后的版本上实际跑过。没验证的对照会把解题者引向错误方向。
- 难度跟着解题者的水平走：先出现象和根因在同一处、线索能在日志里直接看到的题，再逐步加长推理链。

## 约定

- 不要拿 `dojo/NNN` 去和 master diff，那样等于直接看答案。
- 向 Claude 要提示时，在报告里写明得到了什么提示，以后才能分清哪些结论是自己推出来的。
- 仓库是公开的，推送上去的题目分支和报告别人也能看到。
