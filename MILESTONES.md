# mini-vmm 里程碑

<!-- MILESTONES.md -->

这份清单是项目的完整路线图，目标是一台覆盖 CPU、内存、中断、时钟、总线、全模拟设备、半虚拟化设备、内核加速、设备直通、热插拔、迁移和固件的完整 VMM。每个里程碑是一项能观察到的能力，下面的勾选项是证明这项能力成立的验收证据，不是操作步骤。所有勾选项都勾上，这个里程碑才算达成。

## 约定

- **ID 用语义名，不用序号。** 插入新里程碑或调整顺序时，已有 ID 不变。代码注释、测试 suite、笔记都用 ID 引用里程碑，e2e suite 以里程碑 ID 命名（`make test <id>`）。
- **文档顺序就是一个合法的拓扑序。** 每个里程碑只依赖排在它前面的里程碑。章节按主题分组，同一章里排在前面的不一定要先做，真正的先后约束只有 `依赖`，最早什么时候能开始见下面的「层级总览」。
- **arch-distro 是一道闸门。** 排在 arch-distro 之后的里程碑都隐含依赖它，不再逐个写出。这是路线安排而不是技术依赖：先让 VMM 能承载一个完整发行版，后面的验收统一在 Arch guest 里做，fio、iperf3、perf、gcc、`linux-headers` 都能直接装。
- **snapshot 之后新增的设备，验收隐含一项：** 设备状态能被 snapshot 保存和恢复。
- **一次只推进一个里程碑。** 完成标准、工作方式和长期 invariant 见 `AGENTS.md`。
- **`参考` 列出的是权威资料。** 协议常量、寄存器布局只能从这些资料推导，不能凭记忆填写。QEMU、kvmtool、SeaBIOS 源码只作实现参考。
- **标了 `前提` 的里程碑依赖特定的 host 能力**（硬件特性、内核模块）。开始前先确认环境满足。

## 层级总览

层级是“最早可以开始的位置”：一个里程碑的层级等于它所有依赖里最大的层级加一。同一层的里程碑互不依赖，可以任选顺序。

| 层级 | 里程碑 |
|---|---|
| 0 | kvm-basics ✓ |
| 1 | linux-boot ✓ |
| 2 | legacy-devices ✓ · smp ✓ · guest-memory ✓ · event-loop ✓ · exit-stats · gdbstub |
| 3 | pci-bus |
| 4 | emu-net · emu-blk |
| 5 | pci-msi |
| 6 | arch-distro |
| 7 | monitor · pvpanic · edu-dev · pcie-ecam · watchdog · cpu-model · split-irqchip · cmos-rtc · acpi-pm-timer · hpet · mem-backend · virtqueue · fw-cfg-seabios · iommufd-selftest |
| 8 | acpi-events · pci-msix · pci-bridge · pv-features · vpmu · nested-virt · pv-channel · tsc · numa · bochs-display · ovmf-uefi |
| 9 | pci-hotplug · guest-pv-probe · cpu-hotplug · exit-cost · kvmclock · dimm-hotplug · nvme · virtio-pci · vfio-user · sev-csv |
| 10 | ptp-kvm · virtio-blk · virtio-net · virtio-console · virtio-scsi · virtio-input · virtio-vsock · virtio-gpu · virtio-balloon · virtio-mem · custom-virtio · ioeventfd-irqfd · viommu · snapshot |
| 11 | virtio-mmio · virtqueue-packed · vhost-net · vhost-vsock · vhost-user-blk · blk-io-uring · shmem-channel · usb-xhci · vfio-mdev · sriov-emu · guest-agent · dirty-log |
| 12 | virtio-fs · sriov-pf-driver · guest-vfio · live-migration |
| 13 | vfio-pci · postcopy |

层级表由 `tools/check_milestones.py --write` 生成，增删里程碑或改依赖后重新生成；`make test milestones` 会检查依赖有没有写错、表有没有过时。层级只说明“能不能开始”，不是推荐顺序。推荐顺序是文档顺序：先打通到 arch-distro，然后按章节往下走。

---

## 执行基础

### kvm-basics — 裸 payload 在 KVM 中运行

依赖: 无 · 参考: `linux/kvm.h`、Intel SDM / AMD APM 系统编程卷

- [x] 实模式 16 位 payload 执行 `out`/`in`，VMM 解析出正确的 port、方向、宽度和数据，`hlt` 产生 `KVM_EXIT_HLT`
- [x] 4 级恒等映射页表加 GDT 进入 64 位长模式，payload 写入 GPA 0x500000 的魔数被 host 读回，2M 页和 4K 页两种页表都通过
- [x] 内存不足、payload 与页表区重叠、未知选项都给出可操作的报错

### linux-boot — Linux 直启到 BusyBox shell

依赖: kvm-basics · 参考: `Documentation/arch/x86/boot.rst`、`asm/bootparam.h`、ACPI spec、MP spec 1.4

- [x] vmlinux（ELF）和 bzImage（64 位启动协议、内核自解压、KASLR）两种格式都能直启
- [x] boot_params 和 E820 正确；MP table 与 ACPI（RSDP → XSDT → FADT/DSDT/MADT）同时提供，两份表里的 CPU 数、IOAPIC 和 IRQ 映射一致
- [x] 经 initramfs 进入 BusyBox shell，串口是中断驱动的双向控制台
- [x] `poweroff -f`（ACPI S5）、panic 后重启（i8042 reset）、`Ctrl-A x` 三条退出路径都能让 VMM 正常退出

### legacy-devices — PC 传统设备让驱动探测零超时

依赖: linux-boot · 参考: 16550A datasheet、8254 / MC146818 / 8042 datasheet、ACPI spec

- [x] 16550A、CMOS RTC、i8042、ACPI PM 都被驱动探测到，没有一次探测超时；8250 variants 探测下串口始终表现为 16550A
- [x] PIT 由 KVM 内核态处理，不落到用户态；未注册的端口读回 0xFF；S5 关机按 guest 的两次写入完成
- [x] `reboot -f` 经 i8042 的 0xFE 脉冲退出

### smp — 多 vCPU

依赖: linux-boot · 参考: `linux/kvm.h`、AMD APM CPUID 拓扑章节、ACPI MADT

- [x] `--smp N`（1 到 32）每个 vCPU 一个线程，AP 经 INIT/SIPI 启动，guest 报告的 CPU 数等于 N
- [x] CPUID 拓扑字段按 vCPU 改写，每个 vCPU 的 APIC ID 互不相同
- [x] 停机没有竞态：任何 vCPU 睡在 HLT 里时，退出请求都能让所有线程结束

---

## 通往发行版

### guest-memory — 多 slot 内存布局与带检查的访问 API

依赖: linux-boot · 参考: `linux/kvm.h`（`KVM_SET_USER_MEMORY_REGION`）、`asm/bootparam.h`（E820）

- [x] guest RAM 可以超过 3 GiB：4 GiB 以下留出 MMIO hole，超出部分放到 4 GiB 以上，分成多个 memslot，E820 与 memslot 一致
- [x] 所有 guest 地址访问都经过一个带长度的 GPA → HVA 接口，检查整数溢出、跨 slot、越界，返回错误而不是指针
- [x] 单元测试覆盖跨 slot 边界、溢出、零长度，并且每类错误都有一个故意写错的 mutant 能让测试变红
- [x] 原来直接调用 GPA → HVA 的地方全部换成新接口

### event-loop — 独立于 vCPU 的事件循环

依赖: linux-boot · 参考: `epoll(7)`、`eventfd(2)`

- [x] 一个基于 `epoll` 的 I/O 线程负责后端 fd（TAP、磁盘、socket），vCPU 线程不再阻塞在后端 I/O 上
- [x] 后端完成事件能安全地更新设备状态并注入中断，加锁顺序写进代码注释，和 `io_lock`、串口锁的已有顺序不冲突
- [x] 串口输入改由事件循环驱动后，现有 e2e 全部通过

### exit-stats — 按设备统计 VM exit

依赖: linux-boot

- [ ] VMM 按设备、按 exit 类型（PIO / MMIO / HLT 等）统计次数，退出时输出，也可以写到指定文件
- [ ] 统计本身对 guest 无副作用，打开和关闭统计时 guest 行为一致

后面全模拟设备、VirtIO、vhost 之间的性能对比都靠这个手段。

### gdbstub — 用 gdb 调试 guest 内核

依赖: linux-boot · 参考: GDB Remote Serial Protocol、`linux/kvm.h`（`KVM_SET_GUEST_DEBUG`、`KVM_TRANSLATE`）

- [ ] gdb 经 TCP 连接 VMM，加载 vmlinux 符号后能在 `start_kernel` 停下
- [ ] 软件断点、硬件断点、单步、继续都正确；停下时所有 vCPU 一起停
- [ ] 读写寄存器和内存（GVA 经页表翻译到 GPA）正确，每个 vCPU 在 gdb 里是一个线程

放在 arch-distro 之前，是为了让后面排查发行版启动问题时有单步和断点可用。

### pci-bus — PCI host bridge、配置空间与 BAR

依赖: guest-memory · 参考: PCI Local Bus Spec 3.0、`linux/pci_regs.h`、ACPI spec（PNP0A03、`_CRS`、`_PRT`）

- [ ] 通过 0xCF8/0xCFC 访问配置空间，guest `lspci` 看到 host bridge 和 VMM 注册的设备
- [ ] BAR 大小探测（写全 1 读回）符合规范，guest 分配的 BAR 地址落在 MMIO hole 里，访问能路由到对应设备
- [ ] DSDT 描述 host bridge 的地址窗口，guest 不需要重新分配资源
- [ ] INTx 经 IOAPIC 送达，`_PRT` 和 MP table 的 PCI 中断路由一致，guest `/proc/interrupts` 计数随设备中断增加

### emu-net — e1000 网卡

依赖: pci-bus, event-loop · 参考: Intel PCI/PCI-X Family of Gigabit Ethernet Controllers Software Developer's Manual（8254x）、Linux `drivers/net/ethernet/intel/e1000/`；实现参考 QEMU `hw/net/e1000.c`

全模拟设备复刻 QEMU 的传统做法：VMM 逐个寄存器模拟一块真实存在的硬件，guest 用原厂驱动，完全不知道自己在虚拟机里。选 e1000 和 AHCI，是因为它们是 QEMU q35 的默认网卡和磁盘控制器，同一个 guest 可以放到 QEMU 里跑同样的负载作对照；两者的数据面都是描述符环加 DMA，正好和后面的 virtqueue 对比。

- [ ] guest e1000 驱动完成探测：从 EEPROM 读出 VMM 配置的 MAC，链路状态为 up
- [ ] TX 和 RX 描述符环经 DMA 读写 guest 内存，每个描述符的地址和长度都经 guest-memory 接口检查
- [ ] 中断原因寄存器（ICR/IMS/IMC）语义正确，guest 中断计数和收发包数对得上
- [ ] 网络后端是 TAP，后端接口和设备模型分离，后面的网卡设备直接复用
- [ ] guest 与 host 能互相 ping 通并完成 TCP 传输

### emu-blk — AHCI 磁盘控制器

依赖: pci-bus · 参考: Serial ATA AHCI 1.3.1 Specification、ATA8-ACS、Linux `drivers/ata/ahci.c` 和 `libahci.c`；实现参考 QEMU `hw/ide/ahci.c`

- [ ] HBA 复位、端口枚举、IDENTIFY DEVICE 正确，guest 识别出磁盘型号和容量
- [ ] 命令列表、FIS、PRDT 经 DMA 访问 guest 内存，PRDT 的每个分段都单独检查
- [ ] 支持 READ/WRITE DMA EXT 和 FLUSH CACHE EXT；guest `mkfs` 加 `mount` 后写入的文件，host 侧读镜像校验一致
- [ ] 磁盘后端是 raw 镜像文件，后端接口和设备模型分离，后面的块设备直接复用
- [ ] guest 能从这块盘启动（`root=/dev/sda`）

### pci-msi — MSI 中断

依赖: emu-blk · 参考: PCI Local Bus Spec 3.0 MSI 章节、`linux/kvm.h`（`KVM_SIGNAL_MSI`）

- [ ] PCI 层提供通用的 MSI capability，emu-blk 启用后 guest 切换到 MSI，`/proc/interrupts` 显示 PCI-MSI
- [ ] guest 关闭 MSI 后回到 INTx，设备照常工作
- [ ] MSI 地址和数据非法时有诊断输出，不会静默丢弃中断

### arch-distro — 启动完整的 Arch Linux

依赖: emu-net, pci-msi, smp, exit-stats

VMM 要能像 QEMU 一样承载一个未经裁剪的发行版。Arch 官方内核是一份和 BusyBox 测试内核完全不同的配置，能顺带检验“VMM 不假设 guest 内核配置”这条原则。

- [ ] 磁盘镜像由仓库脚本用 `pacstrap` 生成，同一份脚本重跑得到等价的镜像
- [ ] 用 Arch 官方 `linux` 包的 vmlinuz 和 initramfs 直启，不经 bootloader，rootfs 在 emu-blk 的磁盘上
- [ ] systemd 启动到串口 login 提示符，登录后 `systemctl is-system-running` 为 `running`；出现失败的 unit 时，每一个都要查明原因，属于 VMM 的问题就修掉
- [ ] guest dmesg 里没有设备探测超时
- [ ] 经 e1000 拿到网络配置，`pacman -Syu` 和安装软件包成功
- [ ] 在 `--smp 4`、2 GiB 以上内存下稳定运行
- [ ] `systemctl poweroff` 和 `systemctl reboot` 都正常完成；关机后再次启动，文件系统不需要 fsck 修复
- [ ] 在 Arch guest 里用 fio 和 iperf3 跑固定负载，用 exit-stats 记录 emu-blk 每次 IO、emu-net 每个包的 exit 次数，作为后面 VirtIO、vhost 对比的基线

mkinitcpio 的 autodetect hook 按构建机上已加载的驱动挑选模块，在 host 上 chroot 生成 initramfs 时会漏掉 `ahci`、`e1000`，要在 `MODULES` 里显式列出或者使用 fallback 镜像。

---

## 控制面与平台设备

### monitor — 运行时控制接口

依赖: event-loop

- [ ] VMM 在 unix socket 上提供按行的文本命令，能查询运行状态、vCPU 信息和 exit-stats，能让 VMM 退出
- [ ] 未知命令和参数错误返回明确的错误信息，不影响 guest 运行
- [ ] 命令注册接口和具体命令分离，后面的热插拔、迁移、balloon 等里程碑直接往上加命令

### acpi-events — ACPI 事件通知

依赖: monitor · 参考: ACPI spec（SCI、PM1 事件寄存器、GPE、Notify）

- [ ] SCI 中断接通，PM1 事件寄存器的状态位和使能位语义正确
- [ ] monitor 发出关机请求后，VMM 置电源按钮事件，Arch guest 的 systemd-logind 走完正常关机流程

### pvpanic — guest panic 通知

依赖: legacy-devices · 参考: QEMU `docs/specs/pvpanic.rst`、Linux `drivers/misc/pvpanic/`

- [ ] 提供 pvpanic 设备（ISA 端口加 ACPI 描述），guest pvpanic 驱动完成绑定
- [ ] guest panic 时 VMM 记录 panic 事件，退出码能区分 panic 和正常重启

### edu-dev — 教学 PCI 设备与自己写的 guest 驱动

依赖: pci-msi · 参考: `linux/pci_regs.h`、Linux `Documentation/PCI/pci.rst`、`Documentation/core-api/dma-api.rst`；设计思路参考 QEMU `hw/misc/edu.c`

guest 侧内核模块这条线的起点。设备的寄存器规范由本项目自己定义并写进仓库，VMM 侧和 guest 侧的代码都在自己手里，任何一侧出问题都能直接加诊断。后面 MSI-X、IOMMU、SR-IOV 的验证都以它为试验台。

- [ ] 设备规范文档写明寄存器布局、DMA 引擎和中断语义
- [ ] guest 驱动模块在 Arch guest 里用 `linux-headers` 编译和加载，探测设备、映射 BAR、注册字符设备
- [ ] DMA 引擎能在 guest 内存和设备内部缓冲区之间双向搬运数据，驱动用 DMA API 分配缓冲区，结果校验一致
- [ ] INTx 和 MSI 两种中断都能完成一次“提交命令 → 设备完成 → 中断 → 驱动确认”的闭环
- [ ] 模块卸载后没有资源泄漏，重复加载卸载 100 次行为一致

### pci-msix — MSI-X 中断

依赖: edu-dev · 参考: PCI Local Bus Spec 3.0 MSI-X 章节

- [ ] PCI 层提供通用的 MSI-X capability，table 和 PBA 位于 BAR 中
- [ ] edu-dev 驱动用 `pci_alloc_irq_vectors` 申请多个 vector，每个 vector 独立计数
- [ ] 单个 vector 的 mask、function mask 和 pending 位语义正确：mask 期间到来的中断在 unmask 后补发

### pcie-ecam — PCIe 扩展配置空间

依赖: pci-bus · 参考: PCIe Base Spec（ECAM）、PCI Firmware Spec（MCFG）

- [ ] 提供 MCFG 表和 ECAM MMIO 窗口，guest 能读写 0x100 以上的扩展配置空间
- [ ] guest `lspci -vvv` 能遍历扩展 capability 链

### pci-bridge — PCIe root port 与多级总线

依赖: pcie-ecam · 参考: PCI-to-PCI Bridge Architecture Spec 1.2、PCIe Base Spec（Root Port）

- [ ] root port 使用 type 1 配置头，guest 能分配次级总线号并枚举挂在它后面的设备，`lspci -t` 显示正确的树
- [ ] bridge 的 I/O、内存、预取内存窗口按 guest 的配置转发访问，窗口外的访问不会到达下游设备

### pci-hotplug — PCIe 原生热插拔

依赖: pci-bridge, pci-msix, monitor · 参考: PCIe Base Spec（Slot Capabilities、Hot-Plug）、Linux `drivers/pci/hotplug/pciehp*`

- [ ] monitor 命令在 root port 上插入设备，guest pciehp 识别、分配资源并加载驱动，设备可用
- [ ] monitor 命令请求拔出设备，guest 停用驱动并确认后 VMM 才释放设备
- [ ] 插拔 50 个来回后 guest 和 VMM 都没有资源泄漏

### watchdog — i6300esb 看门狗

依赖: pci-bus · 参考: Intel 6300ESB I/O Controller Hub datasheet（watchdog 章节）、Linux `drivers/watchdog/i6300esb.c`

- [ ] guest i6300esb 驱动绑定，systemd 能按配置喂狗
- [ ] guest 停止喂狗后，VMM 按配置执行复位、关机或只记录事件

---

## CPU 虚拟化

### cpu-model — CPUID 与 MSR 策略

依赖: smp · 参考: Intel SDM / AMD APM（CPUID、MSR）、`linux/kvm.h`（`KVM_SET_CPUID2`、`KVM_X86_SET_MSR_FILTER`、`KVM_CAP_X86_USER_SPACE_MSR`）

- [ ] 可以按配置屏蔽或保留 CPUID feature，guest `/proc/cpuinfo` 与配置一致，所有 vCPU 看到的 feature 相同
- [ ] 指定的 MSR 经 MSR filter 交给用户态处理并有日志，guest 访问未知 MSR 时的行为（注入 #GP 或忽略）是配置决定的，不是偶然结果

### pv-features — KVM 半虚拟化特性

依赖: cpu-model · 参考: Linux `Documentation/virt/kvm/x86/cpuid.rst`、`msr.rst`

- [ ] kvmclock、steal time、PV EOI、PV TLB flush、PV sched yield 都可以单独开关，开启 `KVM_CAP_ENFORCE_PV_FEATURE_CPUID` 后，没有声明的特性 guest 用不了
- [ ] 每项开关前后，用 exit-stats 或 guest 统计（`/proc/stat` 的 steal 等）对比出可观察的差异，结论写进笔记

### guest-pv-probe — guest 模块验证 PV 接口

依赖: pv-features, edu-dev

- [ ] guest 模块读取 steal time 结构，host 侧制造 CPU 争用时 steal 值随之增长
- [ ] 每个 PV 特性关闭时，模块能检测到它不可用

### vpmu — 虚拟 PMU

依赖: cpu-model · 参考: Intel SDM / AMD APM 性能监控章节、Linux `arch/x86/kvm/pmu.c`

- [ ] guest `perf stat` 能统计 cycles 和 instructions，数值量级合理
- [ ] 关闭 vPMU 时 guest 的 perf 报告硬件事件不可用，而不是给出错误数值

### split-irqchip — 中断控制器搬到用户态

依赖: legacy-devices, exit-stats · 参考: `linux/kvm.h`（`KVM_CAP_SPLIT_IRQCHIP`、`KVM_EXIT_IOAPIC_EOI`）、82093AA IOAPIC datasheet、8259A datasheet、8254 datasheet

现在 PIC、IOAPIC、PIT 都由 KVM 内核态模拟。split 模式下 LAPIC 留在内核，其余由 VMM 自己实现，这是亲手写一遍中断控制器的机会。

- [ ] 启动参数可以在内核 irqchip 和 split irqchip 之间切换，两种模式下现有 e2e 全部通过
- [ ] split 模式下 PIC、IOAPIC、PIT 都在用户态，电平触发中断的 EOI 经 `KVM_EXIT_IOAPIC_EOI` 正确处理
- [ ] 两种模式下对比启动过程的用户态 exit 次数和 TSC 校准路径，结论写进笔记
- [ ] guest 以 `noapictimer` 启动、由 PIT 承担 tick，host 过载时对比补发策略开关下的 guest tick 计数与时间漂移：内核 PIT 用 `KVM_REINJECT_CONTROL` 开关，用户态 PIT 自己实现同样的两种策略，结论写进笔记

### cpu-hotplug — CPU 热插拔

依赖: acpi-events, smp · 参考: ACPI spec（Processor 设备、GPE、`_STA`、`_EJ0`）、QEMU `docs/specs/acpi_cpu_hotplug.rst`

- [ ] MADT 声明可能存在的最大 CPU 数，DSDT 里有对应的 Processor 设备和 GPE 处理方法
- [ ] monitor 命令加入 vCPU 后，guest 收到 ACPI 通知并能把新 CPU online
- [ ] guest offline 后 monitor 命令拔出 vCPU，VMM 回收线程和 KVM vCPU 状态

### nested-virt — 嵌套虚拟化

依赖: cpu-model · 参考: AMD APM SVM 章节、Linux `Documentation/virt/kvm/x86/nested-vmx.rst` 和 `arch/x86/kvm/svm/nested.c`

前提: host KVM 开启 nested（`kvm_amd nested=1` 或 `kvm_intel nested=1`）。

- [ ] cpu-model 向 guest 暴露 SVM（或 VMX），guest 里出现 `/dev/kvm`
- [ ] guest 里运行 minivmm，启动一个 BusyBox L2 guest 并通过 kvm-basics、linux-boot 的 e2e

### pv-channel — guest 与 VMM 之间的自定义通道

依赖: edu-dev, cpu-model · 参考: `linux/kvm.h`（`KVM_EXIT_HYPERCALL`、`KVM_CAP_EXIT_HYPERCALL`、MSR filter）、host 内核 `arch/x86/kvm/x86.c` 的 `kvm_emulate_hypercall()`

- [ ] guest 模块分别用 PIO、MMIO、自定义 MSR 三种方式向 VMM 发请求，VMM 收到并回复
- [ ] guest 模块执行 `vmmcall`（或 `vmcall`），依据 host 内核源码查明 KVM 在内核态处理了哪些 hypercall、`KVM_CAP_EXIT_HYPERCALL` 能把哪些交给用户态，结论写进笔记

### exit-cost — 测量每类 exit 的代价

依赖: pv-channel, exit-stats

exit-stats 回答“exit 了多少次”，这个里程碑回答“每次 exit 有多贵”。

- [ ] guest 模块关中断循环执行 `cpuid`、内核态处理的 PIO、用户态处理的 PIO、MMIO、MSR、hypercall，用 TSC 算出每类操作的平均周期数
- [ ] 结果表区分“在 KVM 内核态处理”和“返回 VMM 用户态处理”两条路径，写进笔记

---

## 时钟与定时器

guest 的时间由两类部件拼成：clocksource 回答“现在几点”，要求单调、精确、读起来便宜；clockevent 负责“到点叫醒我”，靠定时中断。这一章把 x86 上 guest 能用的每一种都做一遍：模拟的 RTC、ACPI PM timer、HPET 每读一次都要 exit，TSC 由硬件直接给出但需要 VMM 定好频率和偏移，kvmclock 是 KVM 为了让 TSC 在虚拟机里可靠而加的一层。PIT 和 LAPIC timer 由 KVM 内核态提供，PIT 的用户态实现在 split-irqchip 里做。

### cmos-rtc — RTC 中断与时间写回

依赖: legacy-devices · 参考: MC146818 datasheet、Linux `drivers/rtc/rtc-cmos.c`、ACPI spec（FADT 的 RTC 字段）

- [ ] periodic、alarm、update-ended 三种中断按寄存器 A/B 的配置经 IRQ8 送达，读寄存器 C 清除标志
- [ ] `rtcwake -m on -s 3` 在 3 秒后被 alarm 中断唤醒返回
- [ ] `hwclock --systohc` 写入的时间被 VMM 记成相对 host 时间的偏移，guest 重启后 `hwclock --show` 读回一致；更新周期中的 UIP 位语义正确

### acpi-pm-timer — ACPI PM timer

依赖: legacy-devices · 参考: ACPI spec（PM Timer、FADT `PM_TMR_BLK` 与 `TMR_VAL_EXT`）

- [ ] FADT 声明 `PM_TMR_BLK`，计数器按 3.579545 MHz 从 host 单调时钟换算，位宽与 `TMR_VAL_EXT` 一致，回绕正确
- [ ] guest 的可用 clocksource 里出现 `acpi_pm`，切换过去后 `sleep 60` 的耗时与 host 一致
- [ ] exit-stats 显示每次读取都是一次用户态 PIO exit；有无 PM timer 时 guest 的 TSC 校准路径不同，结论写进笔记

### hpet — 高精度事件定时器

依赖: legacy-devices · 参考: IA-PC HPET Specification 1.0a、ACPI HPET 表

- [ ] 提供 HPET 表和 MMIO 寄存器，guest 的可用 clocksource 里出现 hpet，切换过去后计时与 host 墙钟一致
- [ ] guest 把 HPET 用作 clockevent 时定时中断准确；legacy replacement 模式下通知 KVM 的 PIT 让出 IRQ0（`KVM_PIT_FLAGS_HPET_LEGACY`）

### tsc — TSC 虚拟化

依赖: cpu-model · 参考: AMD APM / Intel SDM（TSC、invariant TSC 的 CPUID 位、TSC ratio / TSC scaling、TSC-deadline）、`linux/kvm.h`（`KVM_SET_TSC_KHZ`、`KVM_GET_TSC_KHZ`、`KVM_CAP_TSC_CONTROL`）、Linux `Documentation/virt/kvm/x86/timekeeping.rst`

前提: 指定与 host 不同的 TSC 频率需要硬件 TSC scaling，开始前查 `KVM_CAP_TSC_CONTROL`。

- [ ] 命令行指定的 TSC 频率在 guest 里生效（`tsc: Detected ... MHz`），所有 vCPU 一致
- [ ] invariant TSC 的 CPUID 位打开和关闭时，guest 对 tsc clocksource 的稳定性判定和最终选用的 clocksource 不同，与内核源码里的判定条件对得上
- [ ] 多 vCPU 之间 TSC 单调：guest 里跨 CPU 迁移的线程连续读 TSC 不回退，dmesg 没有 TSC 不同步的警告
- [ ] TSC-deadline 的 CPUID 位打开和关闭时，guest 的 LAPIC clockevent 分别是 deadline 模式和 oneshot 模式

### kvmclock — KVM 半虚拟化时钟

依赖: pv-features, tsc, acpi-pm-timer, hpet · 参考: Linux `Documentation/virt/kvm/x86/msr.rst`、`timekeeping.rst`、`arch/x86/include/asm/pvclock-abi.h`、`linux/kvm.h`（`KVM_GET_CLOCK`、`KVM_SET_CLOCK`）

- [ ] guest 模块直接读取 pvclock 结构，算出的时间和 guest 时钟一致；master clock 生效时 `PVCLOCK_TSC_STABLE_BIT` 置位，失效的条件依据 host 内核源码写进笔记
- [ ] `KVM_GET_CLOCK` 读到的值和 guest 内读到的 kvmclock 一致
- [ ] monitor 暂停 VM 期间 guest 时间的走法（停住，或恢复后追上墙钟）由配置决定，两种都能观察到
- [ ] 用户态循环调用 `clock_gettime`，依次切换 `current_clocksource` 到 tsc、kvm-clock、hpet、acpi_pm，测出每次读取的成本并结合 vDSO 能否处理解释差异，结论写进笔记

### ptp-kvm — guest 与 host 精确对时

依赖: kvmclock · 参考: Linux `drivers/ptp/ptp_kvm_x86.c`、host 内核 `arch/x86/kvm/x86.c` 的 `KVM_HC_CLOCK_PAIRING` 处理

前提: `KVM_HC_CLOCK_PAIRING` 要求 host 的 clocksource 是 KVM 能配对的类型，开始前依据 host 内核源码确认并检查 host 的 `current_clocksource`。

- [ ] guest 加载 `ptp_kvm` 后出现 `/dev/ptp0`，`phc_ctl` 读到的时间和 host 墙钟一致
- [ ] guest 的 chrony 以 `/dev/ptp0` 为唯一时间源，稳定后 `chronyc tracking` 报告的偏差在微秒量级

---

## 内存虚拟化

### mem-backend — guest 内存后端

依赖: guest-memory · 参考: `memfd_create(2)`、`Documentation/admin-guide/mm/hugetlbpage.rst`、`mbind(2)`

- [ ] guest RAM 可以选择匿名内存、memfd 或 hugetlbfs 作为后端，memfd 和 hugetlbfs 后端可以把 fd 交给其他进程共享
- [ ] 使用大页后端时 host `/proc/meminfo` 的大页用量与 guest RAM 对得上
- [ ] 支持启动时预分配并锁定内存

### numa — NUMA 拓扑

依赖: mem-backend, smp · 参考: ACPI spec（SRAT、SLIT）

- [ ] 提供 SRAT 和 SLIT，guest `numactl -H` 显示的节点、CPU 归属和距离与配置一致
- [ ] 每个 guest 节点的内存可以绑定到指定的 host 节点，vCPU 线程可以绑定到指定的 host CPU

### dimm-hotplug — ACPI 内存热插拔

依赖: acpi-events, guest-memory · 参考: ACPI spec（Memory Device PNP0C80）、QEMU `docs/specs/acpi_mem_hotplug.rst`

- [ ] SRAT 预留可热插拔的地址范围，monitor 命令插入内存后 guest 收到通知并能 online 新的内存块
- [ ] guest offline 内存块后 monitor 命令拔出，VMM 删除 memslot 并释放 host 内存

---

## 更多全模拟设备

### nvme — NVMe 控制器

依赖: pci-msix, emu-blk · 参考: NVM Express Base Specification、Linux `drivers/nvme/host/pci.c`；实现参考 QEMU `hw/nvme/`

- [ ] admin queue 上的 Identify Controller / Namespace、Create I/O SQ/CQ 正确，guest `nvme list` 看到盘
- [ ] 多个 I/O 队列各有独立的 MSI-X vector，READ、WRITE、FLUSH 正确，复用 emu-blk 的磁盘后端
- [ ] 同一 fio 负载下和 emu-blk 对比每次 IO 的 exit 次数和吞吐，结论写进笔记

### bochs-display — 全模拟显示设备

依赖: pci-bus, monitor · 参考: Bochs VBE（DISPI）接口说明、Linux `drivers/gpu/drm/tiny/bochs.c`

- [ ] framebuffer 位于 BAR，DISPI 寄存器可经 MMIO 访问，guest bochs DRM 驱动绑定并设置分辨率
- [ ] 显示输出后端和设备模型分离，能把 guest 画面输出到 host 窗口，也能用 monitor 命令截图成图像文件
- [ ] guest fbcon 的画面和截图内容一致

---

## VirtIO

### virtqueue — Split virtqueue 核心

依赖: guest-memory · 参考: OASIS VirtIO Spec（Basic Facilities、Split Virtqueues）、`linux/virtio_ring.h`

- [ ] 解析 descriptor chain，支持 indirect descriptor，生成 used ring 条目，处理 idx 回绕，支持 event idx 通知抑制
- [ ] 越界地址、成环的 chain、超长 chain、读写方向不符都会被拒绝并输出诊断（queue index、desc index、flags、len、GPA）
- [ ] 不依赖任何 transport，用合成的内存镜像做单元测试和 fuzz；每类拒绝都有 mutant 能让测试变红

### virtio-pci — Modern VirtIO-PCI transport

依赖: virtqueue, pci-msix · 参考: OASIS VirtIO Spec（PCI transport、Entropy Device）、`linux/virtio_pci.h`

- [ ] common / notify / ISR / device cfg 四个 capability 正确，设备状态机按 spec 走完 ACKNOWLEDGE → DRIVER → FEATURES_OK → DRIVER_OK
- [ ] feature 协商只接受双方都支持的位，guest 写入 0 状态时设备完整复位
- [ ] 每个 virtqueue 有独立的 MSI-X vector，也支持没有 MSI-X 时退回 INTx
- [ ] 第一个设备是 virtio-rng（只有一个队列、设备到驱动单向），guest 从 `/dev/hwrng` 读到数据

### virtio-blk — 半虚拟化磁盘

依赖: virtio-pci, emu-blk · 参考: OASIS VirtIO Spec（Block Device）、`linux/virtio_blk.h`

- [ ] 复用 emu-blk 的磁盘后端，读、写、flush、discard 正确，guest 能从 `/dev/vda` 启动
- [ ] 支持多队列，队列数与 vCPU 数对应
- [ ] 同一镜像、同一 fio 负载下，和 emu-blk、nvme 对比每次 IO 的 exit 次数和吞吐，结论写进笔记

### virtio-net — 半虚拟化网卡

依赖: virtio-pci, emu-net · 参考: OASIS VirtIO Spec（Network Device）、`linux/virtio_net.h`

- [ ] 复用 emu-net 的 TAP 后端，RX/TX 队列和控制队列工作正常，guest 与 host 互通
- [ ] 支持 checksum offload 和 TSO，guest 和 TAP 两侧的 offload 设置一致
- [ ] 同一负载下和 emu-net 对比每包的 exit 次数和吞吐，结论写进笔记

### virtio-mmio — 第二种 transport

依赖: virtio-blk · 参考: OASIS VirtIO Spec（MMIO transport）、`linux/virtio_mmio.h`

- [ ] 同一个 virtio-blk 设备挂到 MMIO transport 上，设备代码一行不改
- [ ] guest 通过内核命令行或 ACPI 发现设备并正常读写

这个里程碑证明 transport 和设备的分层是真的，而不是只在 PCI 上碰巧成立。

### virtqueue-packed — Packed virtqueue

依赖: virtio-blk · 参考: OASIS VirtIO Spec（Packed Virtqueues）

- [ ] 实现 packed ring：descriptor ring 上的 avail/used 标志位与 wrap counter、driver/device event suppression
- [ ] 协商 `VIRTIO_F_RING_PACKED` 后 virtio-blk 正常工作，设备代码不感知 ring 格式
- [ ] 越界、环绕、wrap counter 不一致的输入都被拒绝，单元测试和 fuzz 覆盖
- [ ] 同一负载下和 split ring 对比 guest 与 VMM 之间的缓存行访问模式和吞吐，结论写进笔记

### virtio-console — 多端口控制台

依赖: virtio-pci · 参考: OASIS VirtIO Spec（Console Device）、`linux/virtio_console.h`

- [ ] guest 能以 `console=hvc0` 把控制台放到 virtio-console 上，登录和交互正常
- [ ] 协商 `VIRTIO_CONSOLE_F_MULTIPORT` 后经 control queue 完成端口的添加、命名和打开状态通知，带名字的端口在 host 侧对应 unix socket，guest 内 `/dev/virtio-ports/<name>` 与 host 双向通信
- [ ] monitor 命令在运行时增删端口，guest 里的设备节点随之出现和消失；host 端 socket 断开时 guest 侧读到端口关闭
- [ ] 协商 `VIRTIO_CONSOLE_F_SIZE` 后，host 终端改变窗口大小，guest 里 `stty size` 随之改变

### virtio-scsi — SCSI 控制器

依赖: virtio-pci, emu-blk · 参考: OASIS VirtIO Spec（SCSI Host Device）、SPC-4、SBC-3

- [ ] 实现独立的 SCSI 磁盘模拟层：INQUIRY、READ CAPACITY(16)、READ/WRITE(10/16)、SYNCHRONIZE CACHE、REPORT LUNS、MODE SENSE、TEST UNIT READY，出错时返回规范的 sense 数据
- [ ] 一个控制器下挂多个 LUN，guest 看到多个 `/dev/sdX`，复用 emu-blk 的磁盘后端
- [ ] SCSI 磁盘模拟层和 virtio transport 分离，usb-xhci 的 U 盘直接复用

### virtio-input — 输入设备

依赖: virtio-pci, monitor · 参考: OASIS VirtIO Spec（Input Device）、`linux/virtio_input.h`

- [ ] 提供键盘和鼠标两个设备，guest `evtest` 能看到正确的设备能力描述
- [ ] 来自 host 窗口或 monitor 命令的按键、移动事件送达 guest

### virtio-vsock — guest 与 host 的 socket 通信

依赖: virtio-pci, event-loop · 参考: OASIS VirtIO Spec（Socket Device）、`linux/virtio_vsock.h`

- [ ] guest 和 host 双向建立连接、传输数据、关闭连接，credit 流控正确
- [ ] 连接断开和设备复位时不泄漏资源

### virtio-gpu — 2D 显示

依赖: virtio-pci, bochs-display · 参考: OASIS VirtIO Spec（GPU Device）、`linux/virtio_gpu.h`

- [ ] 支持 2D 资源创建、attach backing、transfer、flush，guest 的 framebuffer console 能显示
- [ ] 复用 bochs-display 的显示输出后端，窗口和截图都与 guest 画面一致

### virtio-balloon — 内存气球

依赖: virtio-pci, monitor · 参考: OASIS VirtIO Spec（Traditional Memory Balloon Device）

- [ ] monitor 命令设定目标大小，guest 膨胀后 VMM 把对应页还给 host，host 侧进程 RSS 下降；收缩后 guest 可用内存恢复
- [ ] 统计队列上报 guest 内存使用情况，monitor 能查询
- [ ] 支持 free page reporting，guest 空闲页被持续还给 host

### virtio-mem — 半虚拟化内存热插拔

依赖: virtio-pci, guest-memory, monitor · 参考: OASIS VirtIO Spec（Memory Device）、Linux `drivers/virtio/virtio_mem.c`

- [ ] monitor 命令调整设备的请求大小，guest 按块 plug 或 unplug 内存，`free` 显示的总内存随之变化
- [ ] unplug 的块在 host 侧被释放

### custom-virtio — 自定义 virtio 设备与 guest 驱动

依赖: virtio-pci, edu-dev · 参考: OASIS VirtIO Spec、Linux `include/linux/virtio.h`、`Documentation/driver-api/virtio/writing_virtio_drivers.rst`

- [ ] 自己定义一个 virtio 设备类型（device ID、feature 位、配置空间、队列语义），规范写进仓库
- [ ] guest 驱动模块用 virtio core API（`virtio_find_vqs`、`virtqueue_add_sgs` 等）完成 feature 协商和请求收发
- [ ] 设备复位、模块卸载重载后状态正确

---

## 事件加速与 vhost

### ioeventfd-irqfd — 通知与中断绕过用户态

依赖: virtio-pci, event-loop · 参考: `linux/kvm.h`（`KVM_IOEVENTFD`、`KVM_IRQFD`、`KVM_SET_GSI_ROUTING`）

- [ ] virtqueue notify 走 ioeventfd，不再产生 MMIO exit 到用户态
- [ ] MSI-X 中断经 GSI routing 加 irqfd 注入，vector mask 变化时 routing 同步更新
- [ ] 用 exit-stats 对比开启前后用户态 exit 次数，结论写进笔记

### vhost-net — 网络数据面进内核

依赖: ioeventfd-irqfd, virtio-net · 参考: `linux/vhost.h`、Linux `drivers/vhost/net.c`

- [ ] 启动参数可以在用户态后端和 vhost-net 之间切换，guest 侧完全一样
- [ ] 内存表、vring 地址、kick/call fd 正确交给 vhost，guest 内存布局变化时同步更新
- [ ] 同一负载下对比用户态后端和 vhost-net 的吞吐，结论写进笔记

### vhost-vsock — vsock 数据面进内核

依赖: ioeventfd-irqfd, virtio-vsock · 参考: `linux/vhost.h`、Linux `drivers/vhost/vsock.c`

- [ ] 可以在用户态后端和 vhost-vsock 之间切换，guest 侧完全一样，host 侧用普通 `AF_VSOCK` socket 通信

### vhost-user-blk — 数据面放到另一个用户态进程

依赖: ioeventfd-irqfd, virtio-blk, mem-backend · 参考: QEMU `docs/interop/vhost-user.rst`

- [ ] 经 unix socket 把 memfd 内存表、vring 和 kick/call fd 交给外部后端进程，guest 读写正确
- [ ] vhost-user 前端协议实现和具体设备分离，virtio-fs 直接复用
- [ ] 后端进程崩溃时 VMM 能检测到并给出明确报错

### virtio-fs — 共享 host 目录

依赖: vhost-user-blk · 参考: OASIS VirtIO Spec（File System Device）、virtiofsd 文档

- [ ] 接入外部 virtiofsd，guest `mount -t virtiofs` 后能读写 host 目录
- [ ] 文件的创建、删除、重命名、权限、mmap 在 host 和 guest 两侧看到的结果一致

### blk-io-uring — 异步磁盘后端

依赖: virtio-blk, event-loop · 参考: `io_uring(7)`

- [ ] 磁盘后端可以在同步 `preadv/pwritev` 和 io_uring 之间切换，所有块设备都能用
- [ ] 高队列深度负载下对比两种后端，结论写进笔记

### shmem-channel — 共享内存设备

依赖: pci-msix, mem-backend, ioeventfd-irqfd · 参考: QEMU `docs/specs/ivshmem-spec.rst`

- [ ] 设备的一个 BAR 映射 host 共享内存，另一个 BAR 提供 doorbell，中断经 MSI-X 和 irqfd 送达
- [ ] guest 驱动模块和 host 进程经共享内存零拷贝交换数据
- [ ] 两个 minivmm 实例经同一块共享内存和 doorbell 互相通信

---

## USB

### usb-xhci — xHCI 控制器与 USB 设备

依赖: pci-msix, virtio-scsi · 参考: xHCI Specification 1.2、USB 2.0 / 3.x Specification、USB HID 1.11、USB Mass Storage Bulk-Only Transport

- [ ] xHCI 的命令环、事件环、传输环正确，guest `lsusb` 看到挂在根集线器上的设备
- [ ] USB HID 键盘的按键送达 guest
- [ ] USB 存储设备复用 virtio-scsi 的 SCSI 磁盘模拟层，guest 能挂载并读写

---

## 设备直通与 IOMMU

### vfio-user — 接入外部进程模拟的 PCI 设备

依赖: pci-msix, mem-backend · 参考: QEMU `docs/interop/vfio-user.rst`、libvfio-user

- [ ] minivmm 作为 vfio-user client 连接外部设备进程，guest 看到该设备的配置空间和 BAR
- [ ] BAR 访问、DMA（设备进程经共享内存读写 guest 内存）、中断三条路径都通

这条路径完全在用户态，不需要 `/dev/vfio` 和 IOMMU。

### vfio-mdev — 经 VFIO 内核接口使用 mdev 设备

依赖: ioeventfd-irqfd · 参考: `linux/vfio.h`、Linux `samples/vfio-mdev/mtty.c`

前提: host 内核能加载 `mtty` 模块。mdev 不需要硬件 IOMMU。

- [ ] host 创建 mtty 的 mdev 实例，minivmm 经 VFIO 打开它
- [ ] 枚举 region 和 IRQ 信息，配置空间、BAR 访问、中断（eventfd）都转发正确，guest 能用这个串口收发

### viommu — 给 guest 一个 IOMMU

依赖: virtio-pci, edu-dev · 参考: OASIS VirtIO Spec（IOMMU Device）、ACPI VIOT

- [ ] 提供 virtio-iommu 和 VIOT 表，guest 识别出 IOMMU 和 IOMMU group
- [ ] edu-dev 的 DMA 按 guest 建立的映射翻译，驱动拿到的是 IOVA 而不是 GPA
- [ ] 访问未映射的 IOVA 时 DMA 被拒绝，fault 事件报告给 guest

### sriov-emu — 模拟带 SR-IOV 的 PF

依赖: pcie-ecam, virtio-net, pci-msix · 参考: PCIe Base Spec（SR-IOV 章节）、OASIS VirtIO Spec（`VIRTIO_F_SR_IOV`）、Linux `drivers/pci/iov.c`；实现参考 QEMU 的 virtio-net-pci SR-IOV 组合

- [ ] PF 带 SR-IOV 扩展 capability，guest `echo N > sriov_numvfs` 后 VF 出现在 `lspci` 中，VF 的 BDF 符合 VF Offset/Stride
- [ ] 每个 VF 有独立的配置空间和 BAR，能作为独立的 virtio-net 设备收发数据
- [ ] `echo 0 > sriov_numvfs` 后 VF 消失，资源完整回收

### sriov-pf-driver — 自己写 SR-IOV PF 驱动

依赖: sriov-emu, edu-dev · 参考: Linux `Documentation/PCI/pci-iov-howto.rst`

- [ ] edu-dev 增加 SR-IOV capability，guest 驱动模块实现 `sriov_configure`，调用 `pci_enable_sriov` 创建 VF
- [ ] VF 由同一个模块以 VF 身份驱动，PF 能给每个 VF 分配资源（例如 DMA 缓冲区配额），VF 之间互不越界

### guest-vfio — guest 内的 VFIO

依赖: viommu, sriov-emu

- [ ] guest 把 sriov-emu 的 VF 绑定到 `vfio-pci`，guest 内一个用户态程序映射 BAR 并完成 DMA 收发
- [ ] VF 的 DMA 只能访问这个程序映射过的 IOVA

### vfio-pci — PCI 设备直通

依赖: vfio-mdev, guest-vfio, nested-virt · 参考: `linux/vfio.h`、`linux/iommufd.h`

前提: 一个有 IOMMU 并能把 PCI 设备交给 `vfio-pci` 的环境。在家没有合适硬件时，用 minivmm 自己搭：外层 minivmm 提供 viommu 和 sriov-emu 的 VF，guest 里开 nested-virt，再运行一个 minivmm 把 VF 直通给最内层 guest。

- [ ] 打开 VFIO group（或 iommufd），把 guest RAM 全部 DMA map 进 IOMMU 地址空间，内存布局变化时同步更新
- [ ] 配置空间转发，BAR 直接 mmap 进 guest，MSI/MSI-X 经 irqfd 送达
- [ ] 最内层 guest 用原厂驱动正常使用直通进来的设备
- [ ] 失败时输出 region/IRQ 元数据和 ioctl 的 errno

---

## 状态保存与迁移

### snapshot — 暂停、保存与恢复

依赖: monitor, smp, kvmclock · 参考: `linux/kvm.h`（vCPU、irqchip、PIT、clock 状态相关 ioctl，`KVM_VCPU_TSC_CTRL`、`KVM_CLOCK_REALTIME`）

前提: 恢复 TSC offset 和墙钟需要 host 内核提供 `KVM_VCPU_TSC_CTRL` 和 `KVM_CLOCK_REALTIME`，开始前查 host 的 `linux/kvm.h`。

- [ ] monitor 命令暂停 VM：所有 vCPU 停在 `KVM_RUN` 外，后端 I/O 全部排空；恢复后 guest 无感知
- [ ] vCPU 寄存器、LAPIC/IOAPIC/PIT、kvmclock、TSC offset、所有已实现设备的状态都能序列化，格式带版本号
- [ ] 保存后在新进程里恢复，guest 继续运行，网络和磁盘 I/O 正常
- [ ] 恢复后 guest 的 TSC 和 kvmclock 不回退，`CLOCK_REALTIME` 按 kvmclock 里程碑定下的暂停语义衔接

### guest-agent — 请 guest 配合的操作

依赖: snapshot, virtio-console · 参考: QEMU `docs/interop/qemu-ga.rst`、`qga/qapi-schema.json`

前面的机制都是 VMM 拦截 guest 的操作，有些事只靠拦截做不到，例如让 guest 把文件系统缓存刷到盘上再冻结。guest agent 是 guest 里的一个守护进程，经 virtio-console 的命名端口接收 VMM 的请求。guest 侧直接用 Arch 的 `qemu-guest-agent` 包，它的 JSON 协议是 guest 侧协议，VMM 在 monitor 里只实现发请求的最小客户端，和 `AGENTS.md` 排除的 JSON 管理 API 不是一回事。

- [ ] monitor 命令经 agent 完成 `guest-sync` 和 `guest-ping`，`guest-get-time` 返回的时间和 host 一致
- [ ] snapshot 前经 `guest-fsfreeze-freeze` 冻结文件系统、保存后 thaw，guest 写盘负载下恢复的镜像不需要 fsck 修复
- [ ] guest 里没有运行 agent 时，请求在超时后报错，VMM 和 guest 都不受影响

### dirty-log — 脏页跟踪

依赖: snapshot · 参考: `linux/kvm.h`（`KVM_GET_DIRTY_LOG`、dirty ring）

- [ ] 开启脏页跟踪后，guest 写内存的页被准确记录，设备 DMA 写入的页由 VMM 自己补记
- [ ] 增量保存只写脏页，恢复结果与全量保存一致

### live-migration — 预拷贝热迁移

依赖: dirty-log

- [ ] 本机两个进程之间，预拷贝迭代传输脏页，停机阶段传送剩余页和设备状态，目标进程接着运行 guest
- [ ] 迁移期间 guest 内的网络连接不断，迁移后磁盘内容一致，停机时间可以测量
- [ ] 迁移后 guest 的 TSC 不回退，墙钟和 host 的偏差不超过停机时间

### postcopy — 后拷贝热迁移

依赖: live-migration, mem-backend · 参考: `userfaultfd(2)`、`Documentation/admin-guide/mm/userfaultfd.rst`

- [ ] 目标端先运行 guest，缺页经 userfaultfd 按需向源端拉取，后台同时推送剩余页
- [ ] 高写入负载下预拷贝无法收敛时，切换到后拷贝能完成迁移

---

## 固件与机密计算

### fw-cfg-seabios — 经 SeaBIOS 和 GRUB 启动

依赖: emu-blk, pci-bus · 参考: QEMU `docs/specs/fw_cfg.rst`、SeaBIOS 源码、ACPI spec

到这里之前 VMM 一直直接装载内核。这个里程碑补上 BIOS 启动链：复位向量 → 固件 → 磁盘上的引导程序 → 内核。

- [ ] 实现 fw_cfg 接口，VMM 把 E820、ACPI 表、启动顺序交给固件
- [ ] SeaBIOS 映射在 4 GiB 以下的复位向量处，从实模式开始执行，串口能看到固件输出
- [ ] 固件依赖的芯片组寄存器（例如 shadow RAM 控制）按 SeaBIOS 源码确认后实现
- [ ] SeaBIOS 从 AHCI 盘上的 GRUB 启动 Arch，结果和直启一致

### ovmf-uefi — 经 UEFI 启动

依赖: fw-cfg-seabios, pcie-ecam · 参考: UEFI Specification、OVMF 源码、CFI flash 规范

- [ ] 实现 pflash 设备，OVMF 的代码区只读、变量区可写并持久化到 host 文件
- [ ] OVMF 从 EFI 系统分区启动 Arch
- [ ] guest 里用 `efibootmgr` 修改启动项，重启后修改仍然生效

### sev-csv — 机密虚拟机

依赖: ovmf-uefi, mem-backend · 参考: AMD SEV API Specification、Linux `Documentation/virt/kvm/x86/amd-memory-encryption.rst`

前提: CPU 支持 SEV（AMD）或 CSV（海光），host 内核开启对应支持。

- [ ] 按 SEV launch 流程启动 guest，guest dmesg 报告内存加密已启用
- [ ] host 直接读取 guest 私有内存只能看到密文
- [ ] launch measurement 能在 host 侧独立算出并校验

---

## 关联实验

这些实验研究的是 host 内核子系统，不改 minivmm 的代码。单列出来，是为了避免 VMM 为了容纳所有东西而膨胀。

### iommufd-selftest — IOMMUFD mock 测试

依赖: 无 · 参考: Linux `tools/testing/selftests/iommu/`、`CONFIG_IOMMUFD_TEST`

- [ ] 自编译开启 `CONFIG_IOMMUFD_TEST` 的内核，在 Arch guest 里跑通 iommu selftests
- [ ] 弄清 mock IOMMU 下 IOAS、HWPT、DMA 映射对象的生命周期，写成笔记
