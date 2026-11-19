# tests/ — 验收与回归

<!-- tests/README.md -->

```
make test                 默认集合：unit negctl tables gshell milestones kbump 加所有 e2e
make test linux-boot      只跑一个 suite
make test unit fuzz       跑多个
make test all             默认集合 + fuzz
make test-list            列出所有 suite
./tests/run.sh --manual linux-boot    只看人工验收指引
IRQCHIP=split make test linux-boot    所有 Linux 模式的启动改用 --kernel-irqchip split
```

退出码：`0` 全过，`1` 有 suite 失败，`2` 用法错。

验证设计（要确认什么、为什么、判据、预算、未决问题、剩余风险）在
[`TEST_DESIGN.md`](TEST_DESIGN.md)。改测试之前先看那份，尤其是 O1 和 O2 两个未决问题。

## 目录结构

```
tests/
├── run.sh          总入口，被 make test 调用；选 suite、跑、汇总
├── lib.sh          shell 断言框架，被各 suite source，不直接执行
├── guest_shell.py  启动 VMM，等 guest shell 提示符逐条送命令（e2e 用）
├── net_peer.py     host 侧 TCP 对端：HTTP 提供随机数据、接收一次上传并算哈希，listen 好之后才报 ready
├── mutate.py       源码层 mutant 的运行器：在源码副本里替换一段文字，重新构建单元测试，看它红没红
├── mutants/        源码层 mutant 清单，一个里程碑一个文件（pci-msi.py、gdbstub.py、system-reset.py、monitor.py、split-irqchip.py、acpi-events.py、pvpanic.py、smp.py）
├── gdb_session.py  gdbstub e2e 的协调者：起 VMM 和 gdb，从 host 侧探测 vCPU 线程停在哪，gdb 退出后经串口关机
├── gdb_checks.py   gdbstub e2e 里由 gdb -x 执行的检查脚本，每项打一行 GDBCHECK ok/FAIL
├── vmm_irqchip.sh  IRQCHIP=on|split 时代替 build/vmm 被调用：给 --mode linux 的启动补上 --kernel-irqchip，命令行里已经写了就不动
├── monitor_client.py  monitor e2e 的客户端：连 socket 发命令、按协议切出回复，@ 开头的步骤做核对（线程号对 /proc、rip 在内核代码段、exit 计数增长、连接数上限），打 CHECK ok/FAIL
├── TEST_DESIGN.md  验证设计与批准记录
├── offline/        不需要 /dev/kvm，可以在任何 Linux 上跑
│   ├── unit.sh         跑 Makefile 的 UNIT_BINS 列出的单元测试（make print-unit-bins），外加回归输入重放
│   ├── negctl.sh       negative control：每个 mutant 都必须让测试变红
│   ├── boot_tables.sh  把 ACPI 表交给 iasl 反汇编，DSDT 再编译回 AML 逐字节比对
│   ├── guest_shell.sh  guest_shell.py 的自测
│   ├── fake_guest.py   冒充 VMM+guest：启动期间丢弃输入，给上面那条用
│   ├── milestones.sh   MILESTONES.md 依赖与层级表检查，外加注入错误的对照
│   ├── kernel_bump.sh  tools/kernel_bump.py 的自测：现场造两棵小内核树，逐项核对每种输出
│   └── fuzz.sh         跑 build/fuzz/*，按墙钟时间计费
├── e2e/            需要 /dev/kvm，多数还需要 guest 内核；一个里程碑一个 suite，以里程碑 ID 命名
│   ├── kvm-basics.sh  linux-boot.sh  smp.sh  legacy-devices.sh  guest-memory.sh  event-loop.sh  exit-stats.sh  pci-bus.sh  emu-net.sh  emu-blk.sh  pci-msi.sh  gdbstub.sh  system-reset.sh  monitor.sh  split-irqchip.sh  acpi-events.sh  pvpanic.sh  arch-distro.sh
├── unit/           C 单元测试源码
│   ├── harness.h           最小断言框架
│   ├── mutants.h           negative control 的接口定义
│   ├── mem_test.c          内存 slot 与 GPA→HVA 边界契约
│   ├── mem_mutants.c       故意写错的 gpa_to_hva，共 9 份
│   ├── io_bus_test.c       PIO/MMIO 注册、查找、分发
│   ├── io_bus_mutants.c    故意写错的 dispatch，共 8 份
│   ├── boot_tables_test.c  MP/ACPI/boot_params 字段检查，MP table 与 _PRT 的 PCI 中断路由一致
│   ├── aml_test.c          AML 编码：PkgLength 各长度的切换点、整数前缀、EisaId
│   ├── pci_bus_test.c      配置空间 conf1 访问、BAR 探测与路由、COMMAND decode、INTx 线电平、Bus Master 与 DMA、MSI capability 与 INTx 互斥、非法 MSI 的诊断
│   ├── opts_test.c         --device/--netdev/--drive 参数值的 QEMU 风格解析与报错、布尔值和 id 规则
│   ├── netdev_test.c       --netdev 解析与引用规则；在自建的 user + net namespace 里经真实 TAP 收发帧、RX 流控
│   ├── ahci_test.c         AHCI 控制器：HBA/端口复位、COMRESET 与签名、PxIS 到 IS 再到 INTx 的三级中断、MSI 模式下何时发消息、命令出错停端口与恢复、寄存器位与访问宽度；IDENTIFY 数据、DMA 读写与 PRDT 的逐段检查、FLUSH 与写缓存、SET FEATURES；NCQ 的接收与 SDB 完成、出错后经日志 10h 恢复，READ/WRITE LOG EXT
│   ├── blockdev_test.c     --drive 解析与引用规则；镜像加锁、多段读写、越界与溢出、只读盘、部分传输与 EINTR
│   ├── e1000_test.c        e1000 设备模型：EEPROM 位操作、PHY、中断原因、TX 校验和与 TSO、RX 过滤与描述符环
│   ├── serial8250_test.c   按 Linux 8250 探测序列回放，判定 16550A
│   ├── gdb_test.c          gdb 远程协议：组包与转义、ack 与重发、g/p/P 的寄存器布局、m/M、Z/z 与 int3 隐藏、vCont 与停止应答、target.xml；x86 断点表、DR7、KVM_SET_GUEST_DEBUG 控制字、KVM_EXIT_DEBUG 归类、寄存器换算
│   ├── event_loop_test.c   I/O 线程的派发、同批删除、停机契约
│   ├── ioapic_test.c       82093AA IOAPIC：间接寄存器窗口、RTE 读写与只读位、边沿与电平投递、remote IRR 只由 EOI 清、EOI 时仍有效就重投、屏蔽期间电平保持、RTE 改成边沿清 remote IRR、RTE 到 MSI 的换算
│   ├── pic_test.c          8259A 主从对：ICW 初始化序列、IRR/ISR/IMR、优先级与特殊屏蔽、级联、AEOI、轮转、OCW3 读 IRR/ISR 与 poll、伪中断、ELCR 让一条线变成电平触发
│   ├── pit_test.c          82C54 计数器（注入假时钟）：六种模式的 OUT 波形与边沿计数、写入后下一拍才装载、GATE、计数锁存与 read-back、BCD、模式 2/3 运行中改写在周期末生效；PIIX4 端口 0x61 的 GATE2、OUT2、刷新位翻转
│   ├── monitor_test.c      monitor 协议：回复的缩进与结束标记、拆包与 CRLF、空行、各类错误、超长行的边界与只回一次 ERR、help/info 由注册表生成、异步命令期间后续请求等待、连接断开后的孤儿请求、每连接的当前 CPU、回复积压上限
│   ├── acpi_pm_test.c      ACPI PM1 寄存器（ACPI 6.5 4.8.3）：上电值、PWRBTN_STS 只由硬件置位且写 1 清、SCI = STS & EN 只在电平变化时通知、先按后使能也拉起 SCI、未实现位读 0、GBL_EN 能写进去且不发 SCI、高低字节单独访问、PM1_CNT 的 SCI_EN 恒 1 和只写位、三种写法的 S5 请求、复位落下 SCI
│   └── kvm_stats_test.c    KVM_GET_STATS_FD 文件解析：乱序布局、不对齐、损坏文件
└── fuzz/           libFuzzer target
    ├── fuzz_common.h   公共桩：guest RAM、memfd 输入
    ├── elf_loader_fuzz.c
    ├── bzimage_fuzz.c
    ├── mkseeds.py      生成种子语料
    └── regress/        曾经崩过的输入，修好后留下来当回归
```

## suite 一览

| suite | 需要 `/dev/kvm` | 需要 guest 内核 | 判据来自哪里 |
|---|---|---|---|
| `unit` | 否 | 否 | 头文件里写明的契约 + 一份独立写出的模型 + ASan/UBSan |
| `negctl` | 否 | 否 | 反过来判：mutant 没让测试变红就是 FAIL |
| `tables` | 否 | 否 | iasl（ACPICA）反汇编，外部 oracle |
| `gshell` | 否 | 否 | 假 guest 复现“启动期间输入丢失”，先证明固定延迟会丢，再要求 `guest_shell.py` 不丢 |
| `milestones` | 否 | 否 | `tools/check_milestones.py` 按依赖重算的层级表；对照用例注入错误后必须变红 |
| `kbump` | 否 | 否 | 手工构造的新旧两棵小内核树，行号偏移、原文改动、函数挪文件各有一处，期望输出由构造直接推出 |
| `fuzz` | 否 | 否（有则用作种子） | ASan/UBSan + “必须有限时间内返回” |
| `kvm-basics` | 是 | 否 | VMM 日志 + payload 自检码 |
| `linux-boot` | 是 | 是 | guest Linux 的启动日志 |
| `smp` | 是 | 是 | guest 的 `/proc/cpuinfo`、`/sys` 拓扑；`cpuidprobe` 在每个 vCPU 上读出的原始 CPUID 拓扑叶，期望值按 SDM/APM 的字段定义从 vCPU 数算出 |
| `legacy-devices` | 是 | 是 | guest 驱动探测结论 + PIO trace 结构 |
| `guest-memory` | 是 | 布局用例否，guest 用例是 | VMM 的 memslot/E820 日志 + guest 的 dmesg、`/proc/zoneinfo` |
| `event-loop` | 是 | 是 | host 的 `/proc/<pid>/task/*/{comm,wchan}` + guest shell 回显 + VMM 日志 |
| `exit-stats` | 是 | payload 用例否，Linux 用例是 | 从 payload 源码逐条数出的 exit 次数 + KVM 的 `io_exits`/`exits` + 开关统计时 stdout 逐字节相同 |
| `pci-bus` | 是 | 是 | guest 的 PCI 枚举日志和 `lspci` + 经 `devmem`、sysfs 读写 BAR 的回读值 + ACPI 与 `acpi=off` 两次启动分到的 IRQ |
| `emu-net` | 是 | 是 | `--netdev` 参数和引用关系的报错早于创建 VM；在 `unshare -rn` 建的 namespace 里经 TAP 跑 Linux e1000 驱动：探测与链路、双向 ping、双向 TCP 哈希一致、两端 TCP 校验和错误为 0、20 万个 UDP 包的洪泛除 host 内核在 TAP 上丢的之外一个不少、且后端因环满暂停过（guest 用 48 项的接收环）、INTx 计数与 ICR 读次数和包数对得上、`--trace-mmio` 记录 |
| `emu-blk` | 是 | 是 | `--drive` 参数和引用关系的报错早于创建 VM；Linux ahci 驱动绑定 AHCI 控制器，端口 0 链路建立、其余 5 个端口报告无设备，中断经 INTx 送达；IDENTIFY 出的型号和容量；guest 读 host 用 `mke2fs -d` 做的镜像、整盘和文件的 sha256 与 host 一致；guest mkfs 后写的文件由 host 用 `debugfs` 取出核对、`e2fsck` 无错；卸载时 flush 到达后端；NCQ 深度 32、文件系统读写经 PxSACT 发出，`libata.force=noncq` 时走 READ/WRITE DMA；host 截短镜像造出读错误，Linux 经日志 10h 定位出错的 tag 并恢复；`root=/dev/sda` 从盘启动 |
| `pci-msi` | 是 | 是 | Linux ahci 驱动打开 MSI，`/proc/interrupts` 上走 PCI-MSI、每次完成都送达；运行中经 `msi_bus` 和重新绑定驱动回到 INTx（IO-APIC 17）再切回 MSI，读盘内容都与镜像一致；guest 把 MSI 的向量、地址、目标 ID 写坏时 VMM 报出 BDF、地址、数据和原因 |
| `gdbstub` | 是 | 是 | 没改过的 gdb 按 vmlinux 符号在 `start_kernel`、`kernel_init` 停下，单步前进一条指令的长度，读写寄存器和内核虚拟地址的内存再还原，每个 vCPU 一个线程；Ctrl-C 时 host 的 `/proc/<pid>/task/*/syscall` 显示两个 vCPU 线程都从 `ioctl` 变成停在 `futex` 上；guest 启动时自己的 int3 被交还（VMM 报交还次数不为 0、没有 kernel BUG）；detach 后 guest 跑到关机，`--no-reboot` 下 guest 自己重启时 gdb 收到退出报告；不带 `--no-reboot` 时 guest panic 重启，连接不断，软件断点在新装载的内核里命中 |
| `monitor` | 是 | 是（路径用例不需要） | `info cpus` 的线程号在 `/proc/<pid>/task/<tid>/comm` 里是对应的 `vcpuN`，空闲 vCPU 的 host 状态是 S、guest 里绑核空转时是 R；`info registers -a` 的 rip 落在 vmlinux 的 `_stext.._etext` 里且 CR0.PG、EFER.LMA 置位，反复读 300 次全部 OK；guest 打印一行前后 `info exit-stats` 的总数增长；各类错误输入、半行断开、第 5 个连接被拒之后 guest 照常回显；`quit` 退出码 0、socket 文件删除 |
| `system-reset` | 是 | 是 | guest 每次启动打印的 CPU 列表、PCI 设备、磁盘探测汇成的哈希在多次 `reboot -f` 之间相同，复位前写进盘的内容读得回来；4 个 vCPU 都在忙时 sysrq-b，新内核所有 CPU 上线；guest 改过的设备寄存器和配置空间复位后回到上电值；`--no-reboot` 下重启和 panic 都只退出一次 |
| `split-irqchip` | 是 | 是 | `--kernel-irqchip` 各种取值的报错；两种模式各启动一次：`pci=nomsi` 下 AHCI 的 INTx 走 IO-APIC 17-fasteoi、读盘不超时，split 下 `info irqchip` 的 EOI exit 数和 pin 17 投递数不少于 AHCI 中断数、exit 统计里有 IOAPIC_EOI 和用户态 PIT/PIC/IOAPIC 的访问，内核模式里一项都没有；guest 里重放 Linux 的 PIT 校准（`pitprobe`），测出的 TSC 频率与 kvmclock 报告的相差不到 1%；`noapic` 下时钟和串口走 XT-PIC，split 下经 `KVM_INTERRUPT` 注入过 ExtINT；`no-kvmclock` 下 TSC 校准走 PIT 的三种合法结局之一；`noapictimer` 把 tick 钉在 PIT 上，VMM 被 SIGSTOP 一秒后，补发开时 jiffies 与 CLOCK_MONOTONIC 相差不超过 50 ms、关时落后至少 500 ms，两种模式下 IRQ0 计数与 jiffies 一致 |
| `acpi-events` | 是 | 是（Arch 用例用 `tools/build_arch_image.sh` 的镜像，没有时 SKIP） | 两种 irqchip 下各一次：FADT 声明的固定电源按钮只有一个 `LNXPWRBN`，IRQ 9 在 guest 里是 `9-fasteoi acpi`，没有全局锁不存在的报错；monitor `system_powerdown` 两次，guest 里 `evkey` 每次收到 `KEY_POWER` 按下和松开，SCI 中断计数和 `ff_pwr_btn` 每次正好加 1，monitor 报 `presses since reset: 2, SCI raised 2 times`；`poweroff -f` 经 PM1_CNT 写 S5；`acpi=off` 时回复说明 `PWRBTN_EN is clear`、SCI 不拉起；Arch 的 systemd-logind 在监听 Power Button，按下后正常关机、`reboot: Power down`、VMM 以 S5 退出码 0 结束 |
| `pvpanic` | 是 | 是（Arch 用例用 `tools/build_arch_image.sh` 的镜像，没有时 SKIP；guest 内核不带 `CONFIG_PVPANIC_MMIO=y` 时 BusyBox 用例 SKIP 并提示重新构建） | `--device pvpanic` 时 guest 的 `pvpanic-mmio` 绑定 `QEMU0001:00`，capability 为 7；`/dev/port` 读 0x505 得 0x07，写 0xF8 被忽略，写 0xFA 只记 `CRASH_LOADED`、guest 继续；sysrq-c 后 VMM 打印 `pvpanic: guest panicked (events 0x01: PANICKED)`、退出码 3；对照组（不挂设备、guest 把 `events` 清零）同一个 panic 走复位，`--no-reboot` 下退出码 0；写 `SHUTDOWN` 退出码 0，`PANICKED|SHUTDOWN` 退出码 3；两个 pvpanic 启动前报错；Arch 的驱动模块自动加载、capability 7，`systemctl poweroff` 经 S5 退出且没有 SHUTDOWN 事件，sysrq-c 退出码 3 |
| `arch-distro` | 是 | 否，用 `tools/build_arch_image.sh` 生成的 Arch 镜像 | 官方 linux 包的内核和 initramfs 直启，systemd 自己报告 `running`、`systemctl --failed` 为空，dmesg 没有超时、err/warn 只有已查明与 VMM 无关的几条；e1000 经 DHCP 上网，pacman 从本地仓库装包并校验签名；`systemctl reboot` 后 boot_id 变了、再次 `running`、systemd-fsck 报 clean；`systemctl poweroff` 经 ACPI S5 退出，host 上 `e2fsck -fn` 无错；两次关机各在 30 秒内完成 |

`linux-boot` / `smp` / `legacy-devices` / `guest-memory` / `event-loop` / `exit-stats` / `pci-bus` / `emu-net` / `emu-blk` / `pci-msi` / `gdbstub` / `system-reset` / `monitor` / `split-irqchip` / `acpi-events` / `pvpanic` 需要 guest 内核。没设 `VMLINUX=`、`BZIMAGE=` 时用 `tools/build_guest_kernel.sh` 构建在 `guest-kernel/` 里的 vmlinux 和 bzImage，那里也没有时相关用例 SKIP。Linux 模式必须带 `--initrd`，各 suite 经 `lib.sh` 的 `initrd_ready` 使用 `build/initramfs.cpio.gz`：它不存在，或者比 `tools/build_initramfs.sh`、`tools/guest/*.c` 里任何一个旧时，先调 `tools/build_initramfs.sh` 重新生成；打包失败（例如没有静态 busybox）时相关用例 SKIP。

`arch-distro` 不用 `guest-kernel/` 的内核，而是 `tools/build_arch_image.sh` 生成在 `arch-image/` 里的整套发行版（要联网、几分钟、不需要 root，`ARCH_IMAGE=` 可指向别处），没有时 SKIP；它还需要非特权 user namespace 和 dnsmasq。网络在 `unshare -rn` 里离线完成，结果不随外网和上游版本变化。设 `ARCH_DISTRO_BASELINE=1` 时额外跑 fio 和 iperf3 的固定负载基线（需要 host 装 iperf3），打印每次 I/O、每帧的 exit 数；没设时不跑也不计 SKIP，因为它多三次启动，而且给出的是供对照的数字，不是通过与否的判据。设 `ARCH_DISTRO_ONLINE=1` 时再跑真实外网的 `pacman -Syu`（需要 host 装 passt 包里的 pasta）：脚本在 pasta 建的 namespace 里把 pasta 的 tap 接口 `pst0` 和 VMM 的 `tap0` 挂进同一个 bridge，guest 的 DHCP 由 pasta 应答，拿到和 host 相同的地址，pacman 用镜像自带的 mirrorlist；结果取决于外网和软件源，所以不进默认用例，没设时不跑也不计 SKIP。

`IRQCHIP=on|split` 让所有 suite 的 Linux 模式启动都带上对应的 `--kernel-irqchip`，用来确认两种中断控制器模式下现有设备的中断都对（split-irqchip 里程碑的第一条验收）；不设时 VMM 用默认的内核态 irqchip，取其他值时每个 suite 在 source `lib.sh` 时以退出码 2 退出。`split-irqchip` suite 自己显式指定模式，不受它影响。`tools/guest/` 下的程序（例如 `devmem`、`pitprobe`、`evkey`、`cpuidprobe`）编进 initramfs，改了之后下一个 suite 会按 mtime 发现 initramfs 过期并重新打包。

`gdbstub` 还要求 PATH 里有 gdb，没有时 gdb 用例 SKIP。检查只用到 vmlinux 的符号表，不需要 DWARF。读 vCPU 线程的 `/proc/<pid>/task/*/syscall` 由 `gdb_session.py` 做而不是放进 gdb 脚本：Yama `ptrace_scope=1` 时只有 VMM 的祖先进程能读它，gdb 是 VMM 的兄弟进程。

要在 guest shell 里执行命令的用例都通过 `tests/guest_shell.py`：每条命令等提示符出现后才发送，结束命令发出后等 VMM 自己退出。不能按固定延迟发送，因为 guest 串口驱动打开 tty 时会清空 RX 缓冲区，shell 起来之前送进去的字节全部丢失，而启动耗时随内核配置、vCPU 数和 host 负载变化。提示符格式和单条命令长度的约束见脚本头注释。需要在 guest 跑起来时观察 VMM 进程本身（线程、fd）的用例用 `--host`，它在提示符处于 host 上执行一条命令，`VMM_PID` 是 VMM 的 pid；由 host 让 VMM 退出的用例（monitor 的 `quit`）用 `--stop-host` 代替 `--stop`。 完整发行版的串口上是 getty 而不是 shell：`--login USER` 在每次出现 `login:` 时以 USER 登录，再把提示符换成脚本认得的格式、关掉 systemd 给交互 shell 加的 OSC 3008 转义；`--reboot CMD` 用于会让 guest 重启的命令，之后的命令等下一次登录后的提示符再发，因为 `systemctl reboot` 发出后马上回到提示符，下一条命令会送进正在关闭的 shell 里丢掉。

`fuzz` 不在默认集合里：它按墙钟时间计费（默认每 target 300 秒），而 `make test`
应该是随手就能跑的。要跑就显式写出来。可用 `FUZZ_TIME=` 调整预算。

## 为什么构建在 Makefile、判定在 tests/

`make` 负责“把源码变成产物”，验收负责“判断产物对不对”。所以 Makefile 里只有
测试二进制的构建规则和一次 `./tests/run.sh` 的转发，一行断言都没有。理由和当初
把验收挪出 Makefile 时一样：

- 断言需要比对输出、区分 PASS/FAIL/SKIP，写成 make recipe 会变成一堆
  `@if ... grep -q ... || exit 1`，出错时看不到实际输出是什么；
- 有些验收需要人眼判断（串口日志），这类东西不该出现在构建系统里；
- 后续 Step 的验收会越来越重，跟编译解耦之后才好单独重跑某一项。

## shell 断言框架

```sh
run_case "用例名" 命令 参数...   # 执行，stdout+stderr 合并进 $CUR_OUT
expect_rc 0                      # 退出码
expect_has "子串"                # 必须出现（固定字符串匹配，非正则）
expect_not "子串"                # 必须不出现
expect_any "候选 1" "候选 2"     # 出现任意一个即可
end_case                         # 判定；失败时自动打印完整实际输出
```

失败用例的完整输出还会原样（保留 `\r` 和终端转义序列）写到 `build/test-failures/<suite>/NN.txt`，开头是用例名和退出码，失败信息里打印路径。终端输出常被调用方过滤或截断，偶发失败重跑不一定复现，这份文件就是留下来的现场。每个 suite 只保留最近一次运行：本次第一个失败时清空，整个 suite 通过时删除，所以目录里有文件就说明最近一次运行有失败。

`expect_not` 同样重要：`kvm-basics.sh` 里 `expect_not "SHUTDOWN"` 才能区分
“跑对了”和“跑飞了但恰好也打印了那行日志”。

`expect_not` 的子串要能唯一指认那条不该出现的日志，用日志前缀（如 `pvpanic:`）或整句，不用裸的设备名、文件名这类短词。`$CUR_OUT` 里有 VMM 打印的完整路径（例如 `loaded .../initramfs.cpio.gz`），`tests/mutate.py` 的副本目录又以 mutant 名命名，裸词会撞上路径，用例在 mutant 运行里被误判失败，掩盖它真正挡住的是哪条用例。

判不了就老实写进 `manual_guide`，不要为了凑自动化去断言一个其实证明不了什么的字符串。

## C 单元测试框架

`unit/harness.h`，只有计数和打印：

```c
T_CASE("用例名");
CHECK(cond, "说明 %d", x);
CHECK_U64(got, want, "说明");     // 失败时同时打十进制和十六进制
CHECK_PTR(got, want, "说明");
return T_SUMMARY();               // 打印 "N checks in C cases, M failed"
```

单元测试一律用 ASan + UBSan 编译，而且带 `-fno-sanitize-recover=all`。
最后这个开关是关键：默认 UBSan 只打一行警告然后继续跑，退出码仍是 0，等于没测。

`unit.sh` 的判据是「退出码为 0」且「摘要里是 `0 failed`」且「输出里没有
`AddressSanitizer` / `runtime error:`」——三条缺一不可，因为断言全过但
sanitizer 报错是完全可能的（本仓库现在就有这么一例，见 TEST_DESIGN.md 的 E6）。

## negative control

`make test negctl`。一套断言全绿，只说明它在当前实现上没报警，不说明它**有能力**报警。这个 suite 把被测代码换成故意写错的版本，跑同一套用例，要求每一个 mutant 都因为断言失败而让测试非零退出——**退出码为 0 才是 FAIL**。

mutant 有两类，优先用第一类：

- **接口层**：`unit/*_mutants.c` 里每个 mutant 是一份故意写错的完整对外函数，测试用 `--mutant N` 换上它。它只依赖函数签名，`src/` 内部怎么重构都不影响。
- **源码层**：`mutants/<里程碑>.py` 里每个 mutant 是一段源码文字的替换，`mutate.py` 把构建要用的文件复制到临时目录、打上替换、用 Makefile 的规则重新构建列出的单元测试再跑。错误点在 static 函数或一条条件表达式里（例如 AHCI 什么时候发 MSI），换不掉整个对外函数时只能这样做。替换按文字定位而不按行号；源码改动让那段文字不再恰好出现一次时报 `STALE`，suite 失败，这时照新源码改写那一项，而不是删掉它。
- **源码层的 e2e mutant**：条目写 `suite=` 而不是 `tests=`，`mutate.py` 在副本里构建 `build/vmm` 再跑那个 e2e suite，要求至少一个用例 FAIL。用于只有让 guest 真跑起来才看得到的错误（例如整机复位漏恢复 AP 的状态）。每个要跑一整个 suite，negctl 里只检查它们的文字有没有过期，要跑时点名：`tests/mutate.py system-reset`。e2e suite 一次只跑一个：几个 `--smp 4` 的 guest 抢 CPU 时，用例的超时会被当成 FAIL，mutant 看起来被抓住了，结论却不可信。基线要求 suite 全部 PASS 且没有 SKIP，否则被 SKIP 的用例根本没跑，mutant 逃掉也看不出来。

```sh
build/tests/mem_test --list-mutants      # 接口层：列出 mutant 及它违反的契约
build/tests/mem_test --mutant 3          # 单独跑一个，看它红在哪条用例
tests/mutate.py --list                   # 源码层：列出 mutant、要跑的测试、违反的契约
tests/mutate.py pci-msi/ahci-no-msi-cap  # 单独跑一个；--keep 留下临时目录看补丁
tests/mutate.py system-reset             # 点名的 set 连 e2e mutant 一起跑，需要 /dev/kvm 和 guest 内核
tests/mutate.py monitor                  # 同上，monitor 的协议 mutant 和 vCPU 工作队列 mutant
tests/mutate.py split-irqchip            # 同上，三个芯片模型的 mutant 和主板接线、补发策略的 e2e mutant
```

`mutate.py` 先在没打补丁的副本上跑一遍同一批测试，这个基线不绿时它直接报错，因为那时每个 mutant 都会“被抓住”，结论没有意义。源码层的构建是这个 suite 的大头，18 个 mutant 并行构建约 20 秒。

**加新契约时配一个对应的 mutant**，这份清单才一直有意义。这套东西是有回报的：`mem_test` 最初漏掉了 `len-truncated-32` 这个 mutant——长度表里没有“低 32 位小、高 32 位非零”的值，任何把 `len` 截成 32 位的实现都能溜过去。是 negctl 逼出来的，不是想出来的。

它证明什么、不证明什么，见 `unit/mutants.h` 开头。

## 写新测试时的几条约定

- **oracle 不能是被测实现自己。** 期望值要来自头文件里的契约注释、spec、
  Linux 驱动源码或一份独立写出的模型。不要用当前输出生成 golden file。
- **判不了就标出来。** 语义尚未定下来的行为写成 `[CHARACTERIZATION]` 用例，
  只记录当前行为，并在 TEST_DESIGN.md 里登记成未决问题。
- **断言用的标记必须由 guest 计算生成**（`echo MARK-$((6*7))` 而不是直接断言
  命令输出），否则会被 shell 的命令回显误匹配。
- **`timeout` 直接包 VMM 会因为进程组和 SIGTTOU 卡死**，人工跑要用
  `timeout --foreground`；`run_case` 已经把 stdin 接到 `/dev/null` 规避了这一点。
