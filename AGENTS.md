# AGENTS.md — mini-vmm 工作契约

<!-- AGENTS.md -->

mini-vmm 是一个用 C 写在 Linux/KVM 上的学习型 VMM，目标是亲手把现代虚拟机涉及的机制完整走一遍：CPU、内存、中断、PCI、全模拟设备、VirtIO、vhost、VFIO、SR-IOV、热插拔、迁移、固件。它不是生产级 VMM，优先级依次是：正确性、可观测性、模块小而易懂、学习价值。功能数量和 benchmark 数字排在这些之后。

接手时按这个顺序读：

1. `MILESTONES.md`：完整路线图和每个里程碑的验收标准，勾选状态就是当前进度。
2. `HANDOFF.md`：开发环境，以及挂不到某一处代码上的坑。
3. `tests/README.md` 和 `tests/TEST_DESIGN.md`：测试怎么跑、判据从哪来。
4. 要改的子系统对应的 `learning-note/` 卷（见 `HANDOFF.md` 的索引）。

## 权威资料

协议常量、结构体布局、寄存器定义、boot entry 约定、ioctl 用法，只能从权威资料推导，不能凭记忆填写。每个里程碑的 `参考` 字段列出了它用到的资料，通用的几类是：

- Linux x86 启动：`Documentation/arch/x86/boot.rst`、`arch/x86/include/uapi/asm/bootparam.h`
- KVM / VFIO / vhost：`linux/kvm.h`、`linux/vfio.h`、`linux/iommufd.h`、`linux/vhost.h`，以及 host 内核里对应的实现源码
- VirtIO：OASIS VirtIO Specification，以及 Linux 的 `virtio_*` UAPI 头文件
- 硬件设备：对应芯片的 datasheet 或行业规范（PCI/PCIe、AHCI、NVMe、xHCI、ACPI 等）

QEMU、kvmtool、SeaBIOS、Linux samples 只能作为实现参考，它们的做法不等于规范。理由：参考实现常常为兼容历史 guest 或自身架构做了取舍，照抄会把这些取舍当成协议带进来。

当前里程碑需要的规范或头文件拿不到时，停下这一部分，说清缺的是哪一个定义，不要猜一个值继续写。猜出来的常量在 guest 侧往往表现为“偶尔失败”，排查成本远高于停下来补资料。

## 推进里程碑的方式

**一次只推进一个里程碑。** 先读它依赖的稳定代码和参考资料，明确最小的可观察目标，只实现达到这个目标所需的代码，验证通过后写笔记，然后停下。不顺手做下一个，也不重构和它无关的稳定模块。理由：一次只引入一个新机制，出问题时才能确定是哪个机制坏了，笔记也才能讲清楚这个机制本身。

**新的大型子系统先定接口。** 先确定数据结构、所有权和生命周期规则、对外函数签名、状态机状态、必须始终成立的 invariant，再写 `.c`。只有出现至少两个真实调用方时才做泛化，不提前设计庞大的通用抽象。

**调试由证据驱动。** crash、hang、启动失败、中断丢失、virtqueue 损坏这类问题，依据的必须是真实现场：`KVM_EXIT_*` 信息、返回值、`errno`、guest dmesg、host 日志、寄存器状态、queue index、descriptor 内容、中断计数。证据不够就加诊断输出（精确的 GPA/HVA、queue index、descriptor flags/len、状态迁移、ioctl 返回值、vector/GSI、相关寄存器），不根据代码位置相邻去猜。修改代码之前尽量保留并解释失败现场。说不清具体失败机理的 patch 不算修好。

## 长期 invariant

### guest 内存

- 始终维护显式的 guest 内存区域模型，不把 GPA 直接当 host 指针用。
- guest 提供的任何地址范围都经过统一的、带长度检查的 GPA → HVA 接口，检查整数溢出、是否完整落在合法区域内、长度是否有效，需要时检查读写方向。
- descriptor chain、PRDT、scatter-gather list 里的每一个分段都单独检查。

guest 完全控制这些地址和长度，漏检一处就是 host 进程的任意读写。

### 设备分层

- **transport / 总线**（PCI、VirtIO-PCI、VirtIO-MMIO）、**队列机制**（split / packed virtqueue、描述符环）、**设备语义**（blk、net、vsock、SCSI 等）、**后端**（TAP、磁盘文件、vhost、vhost-user）四层分开。
- 同一种设备语义在协议允许的范围内能被多种 transport 复用；同一个后端能被全模拟设备和半虚拟化设备共用（例如 e1000 和 virtio-net 共用 TAP 后端）。
- descriptor walking 逻辑只有一份，不在各个 transport 里各写一遍。
- guest 和 host 共享的 ring index 附近的内存序要重点审查。barrier 由 VirtIO spec 和目标架构的内存模型推导，不无理由地到处加。

### 先慢路径，后快路径

先走 `KVM_EXIT_MMIO` 和显式中断注入，再上 ioeventfd / irqfd；先做用户态后端，再做 vhost；先做同步块 I/O，再做 io_uring。理由：这个项目的学习价值之一，就是能对照观察加速前后到底变了什么，跳过慢路径就没有了对照组。

### 范围之外

- seccomp、jailer、cgroup 沙箱，HTTP/REST/JSON 管理 API，rate limiting，生产级 metrics：这些是生产化需要的，不涉及新的虚拟化机制。运行时控制用 monitor 里程碑的文本协议就够了。
- VirtIO legacy（0.95）transport：现代 Linux guest 都支持 modern 接口，legacy 只多出一套兼容分支，没有新的机制可学。
- Windows guest 及其专用机制（如 Hyper-V enlightenment）：项目只面向 Linux guest，guest 侧内核模块、验收手段和对内核源码的追踪都建立在 Linux 上，同一类机制在 Linux guest 上已经有 kvmclock、PV 特性这些对应物。
- 路线图以外的通用硬件模拟：每个设备都是为了演示某个机制而选的，见 `MILESTONES.md` 里各设备的选型理由。

## 完成标准

一个里程碑除了它自己的勾选项，还要同时满足下面四条。

**代码**
- 修改范围只覆盖当前里程碑。
- 不引入新的编译警告。
- `open`、`mmap`、`ioctl`、`pread/pwrite`、`read/write` 等可预期失败的调用都检查返回值。
- 所有 guest 控制的地址和长度在解引用之前完成检查。

**可复现验证**
- 自动化验收进 `make test`，e2e suite 以里程碑 ID 命名。
- 写清 build 命令、run 命令、需要的 host 准备、精确的预期输出和客观的通过判据。“在我机器上能跑”不算判据。
- 验收测试先在修复前或功能缺失时看到它失败，证明它真的能挡住回归。

**失败可观测**

失败路径要暴露足够的状态，让后续调试不靠猜。例如：KVM exit reason 和 payload；启动失败时的寄存器状态；virtqueue 失败时的 desc index、flags、len、GPA；transport 失败时的 MMIO/PCI offset 和访问宽度；中断失败时的 GSI、vector、eventfd、中断计数；直通失败时的 VFIO region/IRQ 元数据和 ioctl `errno`。

**笔记**

验证通过之后写，只描述真正验证通过的代码，不写最初计划中的设计。`blog/` 是面向读者的《从零入门虚拟化》系列，按编号顺序阅读。需要大量第三方知识的主题（例如 ACPI、中断控制器这类要先讲清规范的内容）拆成原理篇和实现篇，否则只写一章，原理和实现放在一起，因为原理部分撑不起单独一篇，拆开只会让读者在两篇之间来回跳；`learning-note/` 是按卷组织的深入笔记，追到内核源码和实测 trace。两者按内容性质分工：VMM 自身的工程实现（线程模型、事件循环、观测与统计这类）只写进 blog，因为它们的要点是做法和踩坑，放进 blog 一处讲全，读者不用在两边对照；learning-note 只放需要追到 guest 内核、KVM 实现或硬件规范深处的专题，blog 在这些地方指向对应的卷。内容至少包括：

- 这个机制解决什么问题，在整体架构里的位置；
- 关键数据结构和状态机；
- 在本实现里实际观察到的执行流和数据流；
- 最难或最反直觉的地方；
- 现有验证为什么足以证明实现有效；
- 合适时配一张 ASCII 或 Mermaid 图；
- 实际引用过的 spec 和头文件。

## 代码结构

```text
src/
  main.c, vm.c, smp.c   命令行、VM/vCPU 生命周期、PIO/MMIO 分发、run loop、停机协调
  mem.c                 guest 内存区域与 GPA → HVA
  kvm_wrappers.c        KVM ioctl 封装
  console.c             host stdin → 串口
  arch/                 vendor 相关（amd/ 同时服务 AMD 和海光）
  boot/                 内核装载、boot_params/E820、MP table、ACPI 表、页表
  devices/              传统设备与 irqchip
  virtio/               virtqueue、transport、设备语义
  io/                   事件循环、异步 I/O 后端
  vhost/  vfio/  snapshot/
include/                与 src/ 同构的头文件
tests/                  见 tests/README.md
tools/                  guest 内核、initramfs、rootfs 构建脚本，trace 解码
```

`src/` 下没有列进 `Makefile` 的 `SRCS` 的 `.c` 是占位文件，首行注释标着它属于哪个里程碑。新子系统可以新建目录，但上面的分层边界不要打乱：总线和 transport 不认识具体设备语义，设备语义不直接操作后端 fd。

## 重点审查区域

下面这些地方出错时症状离原因很远。改动它们时，要求一个小而准的测试加对应的 spec 引用，不接受“看起来应该对”的大 patch：

- KVM 特殊寄存器和段的设置；boot parameter 偏移与内核入口假设；E820 与 guest 物理内存布局
- PCI 配置空间访问宽度、对齐、BAR mask、capability 链；MSI / MSI-X / INTx 接线
- VirtIO feature 协商与设备状态迁移；descriptor chain 的终止条件和环检测；avail/used index 回绕；共享 ring 的内存序
- guest 提供的 GPA + 长度的算术
- 中断的确认与撤销；eventfd 的所有权与生命周期
- VFIO DMA map/unmap 的生命周期；BAR mmap 与 region capability
- vCPU 线程停止和暂停的竞态；snapshot 的静止点

## 开始一个里程碑时

动手前先给出：

1. 当前里程碑的 ID 和它要证明的那一项可观察行为；
2. 它依赖的、已经验证通过的模块；
3. 要用到的规范章节和头文件；
4. 最小实现方案。

宣布完成时给出：

1. 改了哪些文件、为什么；
2. build 命令和精确的 run/test 命令；
3. 预期的验证证据和实际跑出的证据；
4. 这个里程碑仍然存在的限制；
5. `MILESTONES.md` 里勾上的项。

任何时候都应该能回答这五个问题：上一个里程碑新增了什么机制；哪一个具体观察结果证明它真的工作；哪一份规范定义了这个行为；当前状态属于哪个子系统；从干净的 checkout 开始怎样完整复现。答不上来，说明里程碑切得太大，或者验证还不充分。
