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

一道题的全部材料都在题目分支 `dojo/NNN` 上：植入了 bug 的源码加上 `challenge.md`。你只在一个 worktree 里工作，修复和 `report.md` 都留在那里，一起提交到 `solve/NNN`。

| 步骤 | 谁 | 命令 |
|---|---|---|
| 出题 | Claude | `tools/dojo pack <现象> --from <植入后的副本> --challenge <文件> --push` |
| 看题 | 你 | `tools/dojo list` |
| 开始 | 你 | `tools/dojo start N`，然后 `cd ../mini-vmm-dojo-NNN` |
| 交作业 | 你 | 在 worktree 里 `tools/dojo submit` |
| 评审 | Claude | 读 `solve/NNN`，写 `feedback.md`，把三个文件归档到 master 的 `dojo/NNN-<现象>/` |
| 收尾 | 你 | `tools/dojo clean N` |

- `start` 会在仓库旁边建 worktree、编译，并把主目录里的 guest 内核和 initramfs 链接进去，`challenge.md` 里的命令可以原样运行。在另一台机器上它会先 fetch。
- `submit` 把 worktree 里的所有改动提交并推送 `solve/NNN`，包括调试时加的打印，评审时都看得到。它不删除任何东西，可以反复执行。
- `clean` 只删 worktree，有未提交或未推送的内容时拒绝执行；分支保留，确认不再需要时再手动删。
- 题号由 `pack` 自动分配：取本地和远端分支、master 已归档目录里用过的最大编号加一，打包前会先 fetch，两台机器轮流出题不会撞号。
- 题目分支没有父提交，`git log`、`git show` 和编辑器的修改高亮都看不出改了哪里。Claude 评审时拿基线 commit 和题目分支比较，得到标准答案。

## 约定

- 不要拿 `dojo/NNN` 去和 master diff，那样等于直接看答案。
- 向 Claude 要提示时，在报告里写明得到了什么提示，以后才能分清哪些结论是自己推出来的。
- 仓库是公开的，推送上去的题目分支和报告别人也能看到。
