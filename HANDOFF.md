# HANDOFF.md — 接手须知

<!-- HANDOFF.md -->

这里只放挂不到某一处代码上的东西：开发环境、跨模块的约束、测试环境的坑。具体某个设备或函数为什么这样写，看它旁边的注释和对应的 `learning-note/` 卷；进度看 `MILESTONES.md`；工作方式看 `AGENTS.md`。

## 开发环境

项目在两台机器上开发和验证过，同一份代码两边都要能通过 `make test`。

- **海光物理机**：Hygon C86 3280，vendor string `HygonGenuine`，微架构派生自 Zen1，走 `kvm_amd`，所以 `src/arch/amd/` 同时服务 AMD 和海光。Ubuntu 22.04，host 内核 5.15，经 ssh 使用。
  - host 内核源码在 `~/china_os/linux-5.15`，guest 内核源码在 `~/china_os/linux-6.6`。查 KVM 行为看 5.15，查 guest 行为看 6.6。那棵 6.6 树里有树内构建的预编译产物（vmlinux 在树根），kbuild 拒绝对这样的源码树做 `O=` 构建，所以它不能直接作为 `tools/build_guest_kernel.sh --src`：另解压一份干净的 6.6 源码，或者在那棵树里 `make mrproper`（会删掉预编译产物）。
  - 两棵树都有 `compile_commands.json` 和 clangd background index，index 由 `~/.local/bin/clangd`（clangd 22）建立。非交互 ssh 的 PATH 里是 `/usr/bin/clangd`（clangd 14），版本对不上，必须显式用 22 版。符号查询用 `~/.local/bin/clangd_query.py`，带环境变量 `CLANGD=~/.local/bin/clangd`；clangd 打开第一个文件之前不加载 index，所以先对任意 `.c` 跑一次 `outline` 再用 `symbol`。`rg` 在 `~/.local/bin/`，同样不在 ssh 的 PATH 里。
  - 远端命令超时时被中断的只是连接，远端的循环和子进程可能还在跑，事后检查有没有残留的 `vmm` 或 clangd 进程。
- **WSL2**：Arch Linux，嵌套虚拟化提供 `/dev/kvm`。guest 内核由 `tools/build_guest_kernel.sh --src ~/linux-6.6` 构建到仓库的 `guest-kernel/`（已被 `.gitignore` 忽略），测试自动使用那里的 vmlinux 和 bzImage。`~/linux-6.6` 的源码树本身不放产物，它的另一个构建目录 `~/linux-6.6-build` 是给 clangd 建索引用的 allmodconfig，驱动多为 `=m` 且强制模块签名，不适合直接当测试 guest。`/dev/kvm` 是否存在取决于 Windows 侧的嵌套虚拟化状态，消失时先查 Windows 更新是否改动了 Hyper-V 的嵌套特性。

**跑测试之前先 `make`。** `make test` 只构建测试二进制，不保证 `build/vmm` 是最新的，直接跑会用到旧二进制。

## guest 内核配置兼容

VMM 不能假设 guest 内核是哪一份配置：换一个 vmlinux 也要能启动，测试断言也不能写死某一份配置下的日志。下面这些配置项会让 guest 走不同的路径，改设备模型或固件表时逐条想一遍：

- `CONFIG_X86_MPPARSE`：开了才读 MP table，否则只用 ACPI MADT。两份表都要提供，内容一致（CPU 数、IOAPIC ID、ISA IRQ 到 IOAPIC pin 的映射）。
- `CONFIG_HYPERVISOR_GUEST` / `CONFIG_KVM_GUEST`：开了会启用 kvmclock，TSC 频率直接取自 KVM，不做 PIT 校准，日志里只有 `tsc: Detected ... MHz processor`；没开时走 PIT 校准。判据见 `tests/lib.sh` 的 `expect_tsc_calibrated`。
- `CONFIG_SERIAL_8250_16550A_VARIANTS`：开了会在 LCR=0x80 / 0xBF / 0xE0 下探测 EFR、NatSemi、TI16750、XScale 并调用 `size_fifo()`。串口在这些组合下都要表现得像真 16550A，`tests/unit/serial8250_test.c` 回放了整段探测。
- `CONFIG_ACPI_REDUCED_HARDWARE_ONLY`：开了 ACPICA 不访问 PM1 寄存器，关机只走 FADT 的 SLEEP_CONTROL/SLEEP_STATUS，重启走 RESET_REG。三者都已在 FADT 里提供（`src/boot/acpi.c`）。
- `CONFIG_BINFMT_SCRIPT=m`：initramfs 里的 `/init` 必须是 ELF，脚本执行不了。`tools/build_initramfs.sh` 让 `/init` 就是 busybox init。
- 驱动编成 `=m`（8250、i8042、rtc_cmos，以及后面的 e1000、ahci 等）：initramfs 里要有对应模块并加载，这是准备 initramfs 的人的责任，VMM 管不了。

arch-distro 里程碑用 Arch 官方内核作为第二份配置，正是为了持续检验这一条。

## 测试环境

- **测试要在带 pty 的环境下也跑一遍。** 通过无 pty 的 ssh 运行时 stdin 不是终端，碰不到进程组和 SIGTTOU 那类问题（见 `src/console.c` 的注释），而用户是在交互式终端里跑测试的。用 `python3 -c 'import pty; pty.spawn(["/bin/sh","-c","..."])'` 复现。不要用 `script` 管道，它会挂住；也不要在 pty 里用 `bash -c`，它会被用户的 profile 切到 zsh。
- **人工运行 VMM 时用 `timeout --foreground`**，不带 `--foreground` 时 VMM 会被放进后台进程组，guest 输入被禁用。
- 给 guest shell 送命令、写断言标记的规则见 `tests/README.md`。

## 观测手段

`--trace-pio` 记录每一次落到用户态的 PIO，`tools/pio_decode.py` 负责切片和标注（用法见 `README.md`）。读 trace 时要记住：

- **VMM 只看得见自己拥有的设备。** 内核态接管的 PIC、PIT、LAPIC、IOAPIC 永远不会出现在 trace 里，“trace 里没有”不等于“guest 没访问”。
- 一次完整启动加关机的 PIO 次数是判断有没有轮询超时的快速指标：某个探测循环跑满超时，次数会多出几万次。这个基线依赖 guest 内核配置和串口输出量，换内核或改设备后先用 `--summary` 重新得到基线。
- 症状到原因的对照表在 `learning-note/07` 第 10.2 节。

## 子系统与笔记索引

改动下面这些区域之前，先读对应的卷。每一卷都记录了“少做一点 guest 会怎样出错”的判据，判据来自 guest 内核源码和实测 trace。

| 区域 | 代码 | 笔记 |
|---|---|---|
| vCPU 运行、世界切换 | `src/vm.c`、`src/arch/amd/` | `learning-note/01`、`03` |
| guest 内存、memslot、MMIO hole | `src/mem.c` | `blog/14`、`learning-note/02`、`learning-note/05` 4.4 节 |
| 中断控制器、中断注入 | `src/devices/irqchip.c` | `learning-note/04` |
| boot_params、E820、MP/ACPI 表 | `src/boot/` | `learning-note/05` |
| SMP、CPUID 拓扑、停机协调 | `src/smp.c`、`src/vm.c` | `learning-note/06` |
| 16550A、RTC、i8042、ACPI PM、未注册端口 | `src/devices/`、`src/boot/acpi.c`、`src/vm.c` | `learning-note/07` |
| I/O 线程、后端 fd、加锁顺序 | `src/io/event_loop.c`、`src/console.c` | `blog/15` |
| exit 统计、KVM 二进制统计 | `src/exit_stats.c`、`src/kvm_stats.c`、`src/vm.c` 分发路径 | `blog/16` |
| PCI 总线、BAR 路由、PCI0 的 `_CRS`/`_PRT`、AML 编码 | `src/pci/`、`src/boot/aml.c`、`src/boot/acpi.c`、`src/boot/mptable.c` | `blog/17`、`blog/18` |

## 已知的小缺口

这些不属于任何里程碑，但碰到相关代码时要知道：

- `src/boot/cmdline.c` 是空占位，不在 `SRCS` 里，内核命令行拼接目前在 `src/main.c`。等出现第二个使用者再独立出来。
- RTC 只读，不产生 IRQ 8（见 cmos-rtc 里程碑）；i8042 的键盘只应答命令，没有 host 按键输入。
- 没有内核态 irqchip 的裸 payload 模式下，所有 vCPU 一创建就是 RUNNABLE，会从同一个入口同时开跑，所以 `--smp` 大于 1 只允许 `--mode linux`。
