# Step 2 交接文档：Console 与 Linux Direct Boot

<!-- HANDOFF_STEP2_CN.md -->

> 面向下一位接手本仓库的 agent。读完这一份就能接着往下做，不需要回看历史会话。
>
> 配套材料：`AGENT_GUIDE_KVM_VMM_CN.md`（项目契约与全局踩坑记录）、`learning-note/05.固件表与Linux启动协议.md`（启动协议）、`learning-note/07.Linux眼中的PC传统设备.md`（本 Step 所有设备模型的判据来源）。本文不重复它们的内容，只给指针。
>
> 最后更新：2026-09-12，对应 commit `93205e1 step2: doc`。

---

## 1. 一句话现状

Step 2 已完成并验收通过：mini-vmm 能以 vmlinux（ELF）或 bzImage 两种形式直启 Linux 6.6，经 initramfs 进入 BusyBox shell，串口是中断驱动的双向控制台，可以通过 `poweroff -f`（ACPI S5）、panic 后 reboot（i8042 reset）、`Ctrl-A x` 三种方式正常退出。四个传统设备（16550A、CMOS RTC、i8042、ACPI PM1a）都做到了"驱动探测一次超时都不产生"的程度，每一条判据都有实测 PIO trace 支撑。

**下一步是 Step 3（Phase C，共享 VirtIO 基础设施），不是继续扩 Step 2。** Step 2 剩下的都是文档同步和小尾巴，见第 7 节。

---

## 2. 范围与编号对照

仓库自己的 Step 编号和 `AGENT_GUIDE_KVM_VMM_CN.md` 第 5 节的 Phase 对照：

| 仓库 Step | Phase | 内容 | 状态 |
|---|---|---|---|
| 2.1 | B1 | 8250 UART：THR 输出、SCR 读写、MCR.LOOP 回环 | 完成 |
| 2.2 | B2–B4 | vmlinux ELF 加载 + boot_params/E820 + MP/ACPI 表，内核跑到 VFS panic | 完成 |
| 2.3 | B5 | initramfs + BusyBox shell + ACPI S5 关机 | 完成 |
| 2.4 | —（B2 的扩展） | bzImage：64 位启动协议 + 内核自解压 + KASLR | 完成 |

Step 2.4 不在原 roadmap 里，是做完 2.3 之后补的：同一套 boot_params/E820/ACPI，只是入口从 ELF entry 换成装载地址 + 0x200，其余交给解压器。

**`tests/step7.sh` 验的也是 Step 2 的设备，不是 roadmap 里的 Step 7。** 它是随 `learning-note` 卷七一起加的，脚本名跟的是笔记卷号；而 roadmap 的 Step 7.x 指的是 Modern VirtIO-PCI（见 `src/virtio/pci_transport.c` 和 `src/virtio/pci_bus.c` 的首行注释）。这个编号撞车是已知的，见第 7 节。

**注意：`AGENT_GUIDE_KVM_VMM_CN.md` 第 11.1 节的"已知限制"段落还写着"只支持未压缩的 vmlinux，不支持 bzImage"，这一条已经过时。** 见第 7 节。

---

## 3. 怎么复现验收

### 3.1 环境

- host：海光 Hygon C86 3280（`HygonGenuine`，Zen1 派生，走 `kvm_amd`），Ubuntu 22.04，host 内核 5.15。
- guest 内核源码与预编译产物：`~/china_os/linux-6.6`，vmlinux 在树根，bzImage 在 `arch/x86/boot/bzImage`。host 侧 KVM 源码：`~/china_os/linux-5.15`。查 KVM 行为看 5.15，查 guest 行为看 6.6，不要凭记忆。
- 符号查询用 `~/.local/bin/clangd_query.py --clangd ~/.local/bin/clangd`（系统 PATH 里的 clangd 是 14 版，和 index 对不上）。先对任意 `.c` 跑一次 `outline` 让 index 加载，再用 `symbol` / `refs`。纯文本搜索用 `~/.local/bin/rg`。

### 3.2 构建与跑测试

```sh
cd ~/mini-vmm
make                                   # 产物 build/vmm + 三个 payload
export VMLINUX=~/china_os/linux-6.6/vmlinux
export BZIMAGE=~/china_os/linux-6.6/arch/x86/boot/bzImage
./tests/step1.sh -a && ./tests/step2.sh -a && ./tests/step6.sh -a
bash tests/step7.sh -a                 # 见下面关于可执行位的说明
./tests/boot_tables.sh                 # 不需要 /dev/kvm，可在任何 Linux 上跑
```

initramfs 由 `tools/build_initramfs.sh` 生成到 `build/initramfs.cpio.gz`（需要静态 busybox），`step2.sh` 发现它不存在时会自己调。

### 3.3 最后一次实测结果（2026-09-12，soc63）

| 脚本 | 自动用例 | 结果 |
|---|---|---|
| `tests/step1.sh` | 7 | 7/7 |
| `tests/step2.sh` | 8 | 8/8 |
| `tests/step6.sh` | 5 | 5/5 |
| `tests/step7.sh` | 3 | 3/3 |
| `tests/boot_tables.sh` | 6 | 6/6 |

同一份代码在 WSL2（嵌套虚拟化）上也全绿，只有 `boot_tables.sh` 因为没装 `iasl` 会 SKIP 一条。

**跑之前先 `make`。** 仓库里 `build/vmm` 的时间戳早于最近一次源码改动，直接跑测试会用到旧二进制。

---

## 4. 代码地图

只列 Step 2 相关的文件，行号对应当前 commit。

| 文件 | 职责 | 入口 |
|---|---|---|
| `src/main.c` | 命令行解析、默认 cmdline、Linux 启动编排、0x10 调试端口、0xF4 退出端口 | `vmm_config_parse_args()` :179、`boot_linux()` :379、`main()` :459 |
| `src/vm.c` | PIO/MMIO 总线与分发、未注册端口的 0xFF 约定、`io_lock`、`--trace-pio` | `vmm_register_pio()` :58、`handle_io()` :236、`vcpu_handle_exit()` :353、`vcpu_run_loop()` :423 |
| `src/boot/kernel_loader.c` | 按文件头分辨 ELF / bzImage 并分派 | `kernel_load()` :27 |
| `src/boot/elf_loader.c` | 解析并装载 vmlinux ELF64 | `elf_load_vmlinux()` :45 |
| `src/boot/bzimage.c` | setup header 拷贝、保护模式部分装载 | `bzimage_load()` :44 |
| `src/boot/zeropage.c` | boot_params、E820 内存图、initrd | `zeropage_setup()` :77 |
| `src/boot/mptable.c` | MP 浮动指针 + 配置表（GPA `0x9fc00`） | — |
| `src/boot/acpi.c` | RSDP/XSDT/FADT/DSDT/MADT（GPA `0xE0000`）+ PM1a 寄存器块 | `acpi_setup()` :187、`pm_write()` :331、`acpi_pm_attach()` :357 |
| `src/devices/serial8250.c` | 16550A：寄存器、IIR 优先级、THRE 语义、MCR.OUT2 门控、回环、EFR | `serial_read()` :146、`serial_write()` :202、`serial8250_attach()` :271、`serial8250_push_rx()` :289 |
| `src/devices/rtc.c` | MC146818 CMOS RTC：索引/数据端口、只读时钟、UIP 恒为 0 | `rtc_read_reg()` :51、`rtc_attach()` :104 |
| `src/devices/i8042.c` | 键盘控制器 + 一个只应答命令的 AT 键盘、AUX 口"不存在"应答、复位 | `kbd_write()` :159、`kbc_command()` :205、`i8042_attach()` :326 |
| `src/devices/irqchip.c` | `KVM_CREATE_IRQCHIP`、`KVM_CREATE_PIT2`、`KVM_IRQ_LINE` | `irqchip_create()` :11、`irqchip_set_irq()` :48 |
| `src/console.c` | host stdin → 串口 RX FIFO、raw 模式、`Ctrl-A x` | `console_thread()` :34、`console_start()` :68 |
| `tools/pio_decode.py` | `--trace-pio` 日志的切片与标注 | `--help` |
| `tests/step2.sh` / `tests/step7.sh` | 验收 | — |

关键 GPA 常量（都在头文件里，不要在别处重复写死）：`ZP_BOOT_PARAMS_GPA` 0x7000、`ZP_CMDLINE_GPA` 0x20000、`MPTABLE_GPA` 0x9fc00、`ACPI_TABLES_GPA` 0xE0000、`BOOT_GDT_GPA` 0x1000、页表 0x2000–0x205000、vmlinux 装载在 0x100000。

默认内核命令行（`src/main.c:34`）：

```
console=ttyS0 earlyprintk=serial,ttyS0,115200 reboot=k panic=-1
```

四项都不是可有可无的：`earlyprintk` 让 8250 驱动加载之前就有输出，`console=ttyS0` 换成中断驱动的 tty，`reboot=k` 把 reboot 路径钉在 i8042（VMM 据此退出），`panic=-1` 让 panic 立刻 reboot 而不是挂住。改动它们之前先看 `learning-note/07` 第 2 节和第 6 节。

`src/boot/cmdline.c` 是占位文件，不在 Makefile 的 `SRCS` 里，命令行拼接目前在 `main.c` 内。

---

## 5. 设备模型的契约清单

这一节是本文档最该被读的部分。每一条都是"少做这一点，guest 就会以某种具体方式出错"，判据来自 guest 6.6 源码加实测 PIO trace，出处见 `learning-note/07.Linux眼中的PC传统设备.md` 对应小节。**改这几个设备之前先核对本表。**

### 5.1 16550A（`serial8250.c`，卷七第 2 节）

| 契约 | 少做会怎样 |
|---|---|
| IER 低 4 位必须可读写（写 0 读 0，写 0x0f 读 0x0f） | `autoconfig()` 判定端口不存在，完全没有串口输出 |
| IIR 的 bit7:6 必须跟随 FCR bit0（打开 FIFO 后读回 0b11） | 端口被判成 `PORT_UNKNOWN`，`console [ttyS0] enabled` 之后就没有输出 |
| MCR.OUT2 未置位时不得拉中断线 | 中断提前泄漏，或反过来永远收不到 IRQ 4 |
| IRQ 4 是边沿触发，每个字节要一次独立的 0→1 跳变 | 输出停在第一个字节，或速率退化成定时器轮询 |
| 打开 IER.THRI 时若 THR 已空要补一次中断 | 内核判定 `UART_BUG_THRE`，转成 5ms 定时器轮询，输出变慢且字符间隔均匀 |
| LCR 写成 0xBF 时偏移 2 是 EFR，读回 0、写丢弃 | 扩展变体探测会往 FCR 写 0xA8，把 FIFO 使能位清掉 |

`autoconfig()` 整整四百行，在当前 guest 配置下真正起判定作用的只有三次读，一共 22 次端口访问。回环测试（`MCR.LOOP`）在 COM1 上**从来不会执行**，因为 x86 的 `SERIAL_PORT_DFNS` 给 COM1–COM3 打了 `UPF_SKIP_TEST`；`msr_value()` 里那段回环逻辑是给 COM4（0x2e8）和 `CONFIG_SERIAL_8250_16550A_VARIANTS=y` 的场景留的。

### 5.2 8254 PIT（`irqchip.c`，卷七第 3 节）

PIT **不在 VMM 里实现**，由 KVM 内核态接管，正确做法就是"什么都不做"：

- `KVM_CREATE_IRQCHIP` 和 `KVM_CREATE_PIT2` 必须在创建 vCPU 之前调用——KVM 在 `KVM_CREATE_VCPU` 时才决定给不给 vCPU 挂内核态 LAPIC。
- `KVM_CREATE_PIT2` 必须带 `KVM_PIT_SPEAKER_DUMMY`。这个名字有误导性，它的实际作用是让 KVM 把端口 0x61 也注册进内核 bus；不带的话 0x61 落到用户态没人处理，TSC 校准间歇失败，症状是启动卡在 "skipped IO-APIC setup"，时间戳停在 0.001000。
- 副作用要知道：speaker 设备按长度 4 注册（0x61–0x64），但对非 0x61 的地址返回 `-EOPNOTSUPP`，KVM 的 bus 分发会继续往下找，所以 **0x64 仍然归 VMM 的 i8042**。这不是巧合，是 `__kvm_io_bus_write()` 的落空行为，改 KVM 版本时值得复查。

因此 `--trace-pio` 里看不到 0x40–0x43 和 0x61 是**正确**现象，`tests/step7.sh` 专门断言了这一点。

### 5.3 CMOS RTC（`rtc.c`，卷七第 4 节）

| 契约 | 少做会怎样 |
|---|---|
| 寄存器 A 的 UIP 位必须能读到 0 | `mc146818_avoid_UIP()` 跑满两次 1 秒超时 |
| 时间必须是 BCD（x86 定义了 `RTC_ALWAYS_BCD 1`，不看寄存器 B 的 DM 位） | 时间完全错乱 |
| 寄存器 B 的 bit1（24 小时制）必须置位 | `rtc_cmos: only 24-hr supported`，驱动拒绝注册 |
| 世纪寄存器的索引要和 FADT 的 `century` 字段一致（本仓库用 0x32） | 年份差 100 年 |

不模拟 RTC 的实测代价：整次启动的用户态 PIO exit 从 30449 涨到 110187，`Run /init` 从 0.75 s 推到 2.70 s。多出来的正好是 4 万次写 0x70 加 4 万次读 0x71——两个调用者（`mach_get_cmos_time` 和 rtc-cmos 探测里的 `mc146818_does_rtc_work`）各跑满一次 10000 圈的超时循环。

### 5.4 i8042（`i8042.c`，卷七第 5 节）

**"把不存在讲清楚"比"把存在讲清楚"更难，这个设备是最好的例子。**

- 控制器命令必须**全部**有应答。`i8042_wait_read()` 转 `I8042_CTL_TIMEOUT`（10000）圈、每圈 `udelay(50)`，一次超时就是 **0.5 秒**；探测路径上有十几条命令。
- `i8042_controller_init()` 会连读两次 CTR 并要求两次值相同，所以 CTR 必须是稳定的。
- 自检命令 0xAA 在当前配置下不会被发（`I8042_RESET_DEFAULT = I8042_RESET_ON_S2RAM`），但仍应答，代价很小。
- AUX 口靠两个**精确的否定回答**被拒绝，缺一不可：AUX_LOOP（0xD3）把字节原样回送但**不置** AUXDATA 状态位（驱动在 `__i8042_command` 里当场检查，之后根本不会去读 0x60）；AUX_TEST（0xA9）返回 **0x01**——只有 0x01–0x03 表示"没有 AUX"，0x00/0xFA/0xFF 都会被理解成"AUX 存在"。
- 键盘的应答字节必须经 IRQ 1 送出（serio → libps2 走中断路径），而**控制器自身的应答不能打中断**（驱动是关着中断轮询读的）。`tests/step7.sh:122` 里 `/proc/interrupts` 的 IRQ 1 计数断言为 **9**（`KBDIRQ-9`），正好等于 atkbd 初始化时键盘回复的字节数（GETID 的 3 个 + 另外 6 条命令各 1 个 ACK）。这一个数字同时锁住三件事：每字节一次独立边沿、控制器应答不打中断、没有多余中断。
- 命令 0xFE（脉冲 reset 线）或写输出端口时 bit0 = 0，都让 VMM 退出。`reboot=k` 依赖这条路径。

### 5.5 ACPI 关机与重启（`acpi.c`，卷七第 6 节）

guest 关机时往 PM1a_CNT（端口 0x604）**写两次**：先 `0x1401`（只有 SLP_TYP），再 `0x3401`（SLP_TYP | SLP_EN）。ACPICA 的 `hwsleep.c` 注释原文是 "We split the writes of SLP_TYP and SLP_EN to workaround poorly implemented hardware."。所以判据必须是"SLP_EN 置位**并且** SLP_TYP == 5"，只看 SLP_TYP 会提前退出（`pm_write()` `src/boot/acpi.c:346`）。DSDT 里 `_S5_` package 的值要和这个判据一致。

FADT **不能**标成 HW-reduced：reduced 模式下 Linux 会禁用 PIT 和 PIC，而 TSC 校准和 legacy timer 路径都依赖它们。

### 5.6 未注册端口读回 0xFF（`vm.c:236`）

这不是"随便填个默认值"，而是一条真机契约。`arch/x86/kernel/i8237.c` 的 `i8237A_init_ops()` 里写着：

```c
if (dma_inb(DMA_PAGE_0) == 0xFF)
        return -ENODEV;
```

注释是 "All removed ports must return 0xff for a inb() request."。读回 0x00 的话 Linux 就认为这台机器有 8237 DMA 控制器，凭空多出一个子系统。反过来 PCI 探测里 0xFF 和 0x00 的角色是**反的**（`pci_check_type1` 要求 0xCF8 能读回 0x80000000，`pci_check_type2` 要求 0xCF8/0xCFA 读回 0x00），所以不要试图用一个全局默认值把两边都糊过去。

端口 0x80 每次启动被写 7 次，是 `native_io_delay()`（`CONFIG_IO_DELAY_0X80=y`）；丢弃写入是正确的。

### 5.7 并发（`vm.c`，卷七第 8 节）

`vcpu_handle_exit()` 在 IO/MMIO 分发外面套了一把大锁 `vm->io_lock`，所以**所有设备回调默认都是串行的，设备内部不需要自己加锁**。串口是唯一例外：它另有一条不由 vCPU 线程发起的路径（stdin 的 console 线程往 RX FIFO 里塞数据），所以它自己带一把 mutex。加锁顺序固定为先 `io_lock` 后串口 mutex，console 线程只拿串口 mutex。

判断一个新设备要不要自己的锁，只要问：它有没有一条不由 vCPU 线程发起的路径。Step 3 起做 VirtIO 后端（io_uring、tap fd、event loop 线程）时这条会立刻用上。

---

## 6. 观测手段：`--trace-pio`

Step 2 之后所有设备问题都**不需要靠猜**，guest 的每一次寄存器访问都可以完整录下来。

```sh
./build/vmm --mode linux --mem 512M --trace-pio /tmp/pio.txt \
    --initrd build/initramfs.cpio.gz $VMLINUX
```

每行一次陷出到用户态的 PIO：`<秒>.<微秒> <设备名> <端口> R|W<宽度> <值> [rip=...]`。RIP 只对未注册端口采集（多一次 `KVM_GET_REGS`，这类访问才需要反查调用者）。

```sh
python3 tools/pio_decode.py /tmp/pio.txt --summary                 # 按端口/设备统计次数
python3 tools/pio_decode.py /tmp/pio.txt --console | less          # 还原控制台文本，每行带 trace 行号区间
python3 tools/pio_decode.py /tmp/pio.txt --from 25700 --to 25800   # 看某一段
python3 tools/pio_decode.py /tmp/pio.txt --dev unhandled --syms $VMLINUX   # 谁在访问我没实现的端口
```

`--console` 那条是把 trace 行号和内核日志对上的标准手段：看到一段奇怪的访问，能查出它发生在哪条日志附近；反过来也成立。

一次完整启动加关机的基线是 **30449 条**（`--smp 1`），其中串口占 98.2%（29912 条）。次数明显偏离这个量级通常意味着某个轮询循环在跑超时。**VMM 只能看见自己拥有的设备**——内核态接管的 PIC/PIT/LAPIC/IOAPIC 永远不会出现在 trace 里，不要把"trace 里没有"读成"guest 没访问"。

症状到原因的完整对照表在 `learning-note/07` 第 10.2 节，这里不复制。

---

## 7. 待办与已知不一致

按优先级排列。都不影响功能，但会误导下一位接手的人。

1. **`tests/step7.sh` 和 `tools/pio_decode.py` 没有可执行位。** 两个文件是以 `diff -u /dev/null` 形式打进来的，patch 不带 mode。修：

   ```sh
   chmod +x tests/step7.sh tools/pio_decode.py
   git update-index --chmod=+x tests/step7.sh tools/pio_decode.py
   ```

   在那之前用 `bash tests/step7.sh` / `python3 tools/pio_decode.py` 调用。

2. **`AGENT_GUIDE_KVM_VMM_CN.md` 第 11.1 节的"当前能力"段落过时**：写着"只支持未压缩的 vmlinux，不支持 bzImage"，而 Step 2.4 已经支持并有验收用例。

3. **`AGENT_GUIDE_KVM_VMM_CN.md` 第 11.3 节两处数字已被实测订正**：RTC 那条写的是"约 4 万次 exit、约 1.4 秒"，实测是 **8 万次 exit、约 2 秒**（4 万次写 0x70 + 4 万次读 0x71）；i8042 那条写的是"约 600ms"，实际是每次超时 **0.5 秒**（10000 × `udelay(50)`）。源码注释里已经改对了，指南还没同步。

4. **脚本编号撞车**：`tests/step7.sh` 验的是 Step 2 的传统设备（脚本名跟的是 `learning-note` 卷七的卷号），而 roadmap 的 Step 7.x 是 Modern VirtIO-PCI。真到做 VirtIO-PCI 时这个名字就占用了。建议改名成 `tests/step2_devices.sh` 之类，或者在真正的 Step 7 开始前重新编号——现在改代价最小。

5. **`tests/README.md` 的"各 Step 覆盖情况"表没有 `step7.sh` 的三行。**

6. **`src/boot/cmdline.c` 是空占位**，不在 `Makefile` 的 `SRCS` 里；命令行拼接目前散在 `main.c`。要不要独立出来等有第二个使用者再说。

7. **仍然存在的功能限制**（不是缺陷，是 roadmap 没走到）：guest RAM 不超过 3 GiB（还没有 MMIO hole，Step 3 做 VirtIO-MMIO 时必须先解决）；RTC 只读、不产生 IRQ 8；i8042 的键盘只应答命令，没有 host 按键输入；`--smp` 大于 1 只能配合 `--mode linux`。

---

## 8. 接 Step 3 之前要知道的几件事

Step 3 是 Phase C（Guest Memory Access API 加固、split virtqueue 解析、VirtIO 设备状态机），全部是不依赖 transport 的纯逻辑，可以先用 synthetic fixture 验证，不要一上来就接 MMIO。

会直接用到 Step 2 留下的接口：

- **设备注册**：`vmm_register_pio(vm, base, len, &dev)` / `vmm_register_mmio()`（`src/vm.c:58` / `:64`）。回调签名 `int (*)(void *opaque, uint64_t off, uint32_t size, void *data)`，`off` 是相对 `base` 的偏移。
- **回调返回值**：返回 `VMM_OK` 表示已处理。返回 `VMM_ERR_EXIT` 是**约定的正常退出信号**（guest 请求关机/复位），`vcpu_run_loop()` `src/vm.c:453` 会把它翻译成正常停机；其它负值才是错误。新设备不要用非零返回值表达"我不处理这个访问"。
- **中断注入**：`irqchip_set_irq()`（`src/devices/irqchip.c:48`）包了 `KVM_IRQ_LINE`。ISA IRQ 是边沿触发，电平语义要自己在设备里维护。
- **锁**：见第 5.7 节。event loop / io_uring 线程属于"非 vCPU 路径"，碰共享状态一定要自己加锁，而且不能在持有设备锁时回头去拿 `io_lock`。
- **内存**：`include/memory.h` 里的 GPA→HVA 转换**必须带长度校验**，这正是 C1 要加固的东西。目前只有一个 slot（GPA 0 起的整块 RAM），Step 3 要挖 MMIO hole 时会变成多个 slot，`vmm_gpa_to_hva()` 的所有调用点都要复查。

写新代码前先读 `AGENT_GUIDE_KVM_VMM_CN.md` 第 3 节（工作契约）、第 4 节（长期 invariant）、第 6 节（Definition of Done）和第 11 节（踩坑记录）。特别是第 11.5 节那几条关于测试环境的：`timeout` 直接包 VMM 会因为进程组和 SIGTTOU 卡死（人工跑要用 `timeout --foreground`）；断言用的标记必须由 guest 计算生成（`echo MARK-$((6*7))` 而不是直接断言命令输出），否则会被 shell 的命令回显误匹配。

---

## 9. 文档索引

| 文件 | 内容 |
|---|---|
| `AGENT_GUIDE_KVM_VMM_CN.md` | 项目契约、roadmap、Definition of Done、全局踩坑记录（第 11 节） |
| `learning-note/05.固件表与Linux启动协议.md` | MP table / ACPI 表 / boot_params / E820 / 64 位启动协议 |
| `learning-note/06.SMP虚拟化与CPU拓扑.md` | 多 vCPU、CPUID 拓扑改写、INIT/SIPI、停机竞态 |
| `learning-note/07.Linux眼中的PC传统设备.md` | **本 Step 所有设备判据的出处**，逐寄存器的实测序列 |
| `blog/02.从零理解KVM_中断设备与多核.md` | 面向读者的长文版本 |
| `tests/README.md` | 验收脚本的组织约定与断言框架 |
