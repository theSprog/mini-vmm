# Agent Guide：基于 C / KVM / VirtIO / VFIO 的学习型 VMM

> 接手本项目的 agent 请先读第 11 节「接手上下文」：当前进度、开发环境，以及已经踩过并解决的坑。

## 0. 项目使命

在 Linux/KVM 上使用 **C 语言实现一个面向学习的 VMM**，通过渐进式开发，最终能够启动 Linux，并实际演示本项目涉及的主要虚拟化机制。

项目范围会刻意比 Firecracker 的最小设备模型更广：先从一个类似 Firecracker 的 MicroVM 核心开始，再逐步加入 Modern VirtIO-PCI、VFIO/SR-IOV/mdev、vhost、SMP 和 vsock，使这些机制能够在同一套代码中被实现、观察和比较。

这**不是生产级 VMM**。优先级依次是：

1. 正确性；
2. 可观测性；
3. 模块足够小且容易理解；
4. 每次只完成一个可以独立验证的 Step；
5. 学习价值优先于功能数量和 benchmark 数字。

一个子系统的简单版本没有验证通过之前，不要提前优化、泛化或“生产化”。

---

## 1. 项目范围

### 核心功能

最终项目应覆盖：

- KVM VM/vCPU 创建以及 `KVM_RUN` 主循环；
- x86_64 Guest 执行与 Linux Direct Boot；
- Guest RAM 注册，以及 GPA -> HVA 地址转换；
- 最小串口控制台支持；
- VirtIO Split Virtqueue 基础设施；
- VirtIO-MMIO transport；
- VirtIO 1.x Modern PCI transport；
- VirtIO block；
- 基于 TAP 的 VirtIO network；
- VirtIO vsock；
- 基于 `epoll`、`ioeventfd` 和 `irqfd` 的事件处理；
- vhost-kernel 加速，至少实现 `vhost-net` 和 `vhost-vsock`；
- SMP / 多 vCPU；
- VFIO 设备直通；
- Host 硬件条件满足时的 SR-IOV VF passthrough；
- 以 VFIO 为后端的 mdev 实验；
- 其余机器模型稳定以后，再选择性实现 Snapshot/Restore。

### 学习型核心明确不做的事情

项目早期**不要**把时间花在：

- seccomp / jailer / cgroup 沙箱；
- HTTP/REST 管理 API；
- JSON 控制面；
- rate limiting 和生产级 metrics；
- QEMU 级别的通用硬件模拟；
- BIOS/UEFI 兼容；
- VirtIO legacy 0.95 兼容；
- 第一版就支持 Packed Virtqueue；
- 通用设备热插拔；
- vhost-user / DPDK / SPDK，除非以后作为独立扩展加入。

后端优先选择最直接、最简单的实现。只有 userspace 路径验证完成后，才加入性能优化机制。

---

## 2. Source of Truth 规则

最初的项目讨论只是**路线图，不是协议规范**。

聊天里出现过的地址、offset、magic、寄存器布局、结构体字段、中断细节、boot entry 假设以及 ioctl 用法，都只能看作示意，除非已经被当前实现所使用的权威资料确认。

对于协议敏感代码，以下资料是唯一可信来源。

### Linux x86 Boot

- `Documentation/arch/x86/boot.rst`
- `arch/x86/include/uapi/asm/bootparam.h`

### VirtIO

- OASIS VirtIO Specification：
  - Basic Facilities 与 Virtqueues；
  - MMIO 与 PCI transport；
  - Block、Network、Socket 设备章节。
- Linux 中相关 UAPI / Kernel Header：
  - `virtio_mmio`
  - `virtio_pci`
  - `virtio_ring`
  - `virtio_blk`
  - `virtio_net`
  - `virtio_vsock`

### KVM / VFIO / vhost

- `linux/kvm.h`
- `linux/vfio.h`
- `linux/vhost.h`

### 参考实现

这些只能作为**实现参考**，不能当作规范：

- Linux `tools/kvm/kvmtool`，重点参考其 KVM、PCI、VirtIO MMIO、block、network 等对应模块；
- Linux `samples/vfio-mdev/mtty.c`，用于 mdev 实验。

**禁止凭记忆编造协议常量。**

如果当前 Step 所需的 spec/header 不可用，应停止该部分实现，并明确说明缺少哪一个定义，而不是猜一个值继续写。

---

## 3. Agent 工作契约

### 一个 Step 只做一个任务

禁止一次实现多个 roadmap Step。

对于当前 Step，严格执行：

1. 阅读它依赖的稳定代码；
2. 阅读相关 spec/header 定义；
3. 明确本 Step 最小可观察目标；
4. 只实现达成这个目标所需的代码；
5. 打开 compiler warnings 并完成构建；
6. 执行明确的验证步骤；
7. 调试直到验证通过；
8. 验证通过后，才写本 Step 的学习笔记 / 博客；
9. 停止，不要顺手开始下一个 Step。

实现当前 Step 时，禁止重构无关的稳定模块。

### 新子系统采用 Header-First

引入一个新的大型子系统时，先确定：

- 数据结构；
- ownership / lifetime 规则；
- 对外函数签名；
- 重要状态机状态；
- 必须始终成立的 invariant。

之后再实现 `.c` 文件。

不要提前设计庞大的通用抽象。只有当至少出现两个真实调用方后，才考虑泛化。

### 调试必须由证据驱动

对于 crash、hang、boot failure、中断丢失、Virtqueue 损坏等问题：

- 禁止根据代码位置相邻关系猜原因；
- 必须依据真实的 `KVM_EXIT_*` 信息、返回值、`errno`、Guest `dmesg`、Host 日志、寄存器状态、queue index、descriptor 内容、中断计数器等事实；
- 如果证据不够，就显式增加诊断打印；
- 需要时打印准确的 GPA/HVA、queue index、descriptor flags/length、状态迁移、ioctl 返回值、interrupt vector/GSI 以及相关寄存器；
- 在修改代码之前，应尽可能保留并解释失败现场。

一个无法说明**具体 failure mechanism** 的 patch，不能被视为有效调试结果。

---

## 4. 项目必须长期保持的核心 Invariant

### Guest Memory

始终维护显式的 Guest Memory Region 模型。

禁止直接把 GPA 当 Host pointer 使用。

任何 Guest 提供的地址范围，都必须经过统一的、带检查的 GPA -> HVA helper，并验证：

- integer overflow；
- 是否完整落在合法 region 中；
- length 是否有效；
- 有需要时，验证读写方向。

Virtqueue descriptor chain 与 scatter-gather list 中的**每一个 segment 都必须单独验证**。

### Virtqueue

实现一个可复用的 **Split Virtqueue core**，由 MMIO 与 PCI transport 共用。

职责必须明确分离：

- Transport：设备发现、配置和通知；
- Virtqueue：descriptor / avail / used ring 机制；
- Device：block/net/vsock 的请求语义。

禁止在 MMIO 和 PCI transport 中分别复制 block/network descriptor walking 逻辑。

Guest/Host 共享 ring index 附近的 memory ordering 必须重点审查。具体 barrier 实现应由 VirtIO spec 和目标 compiler/architecture memory model 推导，而不是无理由地到处添加 barrier。

### Transport 与 Device 分层

始终保持以下四层边界：

- **Transport：** VirtIO-MMIO 或 VirtIO-PCI Modern；
- **Queue：** descriptor / avail / used；
- **Device：** block / net / vsock 请求语义；
- **Backend / Dataplane：** userspace I/O、vhost、TAP、file I/O 等。

在协议允许的范围内，同一个 block/net device 实现应能够同时被 MMIO 与 Modern PCI transport 复用。

### Slow Path 先于 Fast Path

始终先实现并验证简单路径，再引入加速：

- 先走 `KVM_EXIT_MMIO` / 显式通知，再实现 `ioeventfd`；
- 有学习价值时，先显式注入中断，再切换 `irqfd`；
- 先实现 userspace VirtIO-net，再实现 `vhost-net`；
- 先实现 userspace vsock，再实现 `vhost-vsock`；
- 先实现同步 / 简单 block I/O，再考虑 `io_uring`。

本项目的学习价值之一，就是能够对照观察“加速之前”和“加速之后”到底发生了什么变化。

---

## 5. Roadmap：尽量小、每一步都能独立验证

具体编号可以调整，但禁止仅仅为了省时间而把相邻步骤随意合并。

### Phase A — KVM 执行基础

#### A1. KVM capability probe

**目标：** 打开 `/dev/kvm`，验证 API version / capability，并创建 VM。

**验证：** 程序成功退出，并打印检测到的 KVM API 与 capability。

#### A2. 单 vCPU + 最小 Guest 代码

**目标：** 创建一个 vCPU，映射 Guest RAM，执行一段会触发已知 VM Exit 的最小 Guest payload。

**验证：** Host 收到预期的 `KVM_EXIT_*` reason，并正确解析其内容。

#### A3. PIO round-trip

**目标：** Guest 执行已知的 `in/out` 操作。

**验证：** VMM 能准确观察到 port、direction、size 和 data。

#### A4. x86_64 long mode payload

**目标：** 配置执行受控 64-bit Guest 代码所需的最小 CPU state。

**验证：** Guest 向 Guest RAM 写入指定 64-bit value，Host 读出完全相同的值。

### Phase B — Console 与 Linux Direct Boot

#### B1. 最小 8250 / Serial 输出

**目标：** 实现足够的 serial PIO，使受控 Guest payload 能够输出文本。

**验证：** Guest 输出的字符串原样出现在 Host stdout。

#### B2. Linux Image Loader

**目标：** 根据选定的 Linux boot protocol 加载对应 kernel image。

**验证：** 在真正运行 vCPU 前，loader 能校验 image，并打印确定性的 Guest memory layout。

#### B3. Boot Parameters / Memory Map / Command Line

**目标：** 只构造当前 direct-boot path 所需的 boot structures。

**验证：** Launch 前 dump 最终 boot parameters，并能独立检查其中关键字段。

#### B4. Linux Early Boot

**目标：** 在 serial console 上看到 Linux 内核输出。

**验证：** 捕获 kernel banner 与 early boot log。

#### B5. Initramfs + BusyBox Shell

**目标：** 不依赖 VirtIO block，先启动到可交互 userspace。

**验证：** 可以通过 serial console 进入 BusyBox `/bin/sh`。

### Phase C — 共享 VirtIO 基础设施

#### C1. Guest Memory Access API 加固

**目标：** 集中实现带检查的 GPA -> HVA 与 range validation。

**验证：** 单元式测试必须拒绝：

- out-of-range；
- overflow；
- 跨 region 访问。

#### C2. Split Virtqueue Parser

**目标：** 在没有真实设备后端的前提下，实现 descriptor table、avail ring、used ring 和 descriptor chain walking。

**验证：** synthetic fixtures 覆盖：

- 单 descriptor；
- chained descriptors；
- read/write flags；
- index wraparound；
- invalid chain。

#### C3. VirtIO Device State Machine Core

**目标：** 将 feature negotiation、queue configuration、device status、reset 和 notification hook 独立于 MMIO/PCI transport 表示。

**验证：** 用一个 Host 侧小测试驱动合法与非法状态迁移。

### Phase D — VirtIO-MMIO

#### D1. VirtIO-MMIO Transport Discovery

**目标：** 暴露一个最小 VirtIO-MMIO device window。

**验证：** Linux 能绑定 `virtio_mmio`，并报告预期 VirtIO Device ID。

#### D2. Userspace VirtIO-BLK Read Path

**目标：** 实现从 raw backend 读取数据所需的最小 block request path。

**验证：** Guest 能读到已知 sector，Host 日志能打印完整 request / descriptor chain。

#### D3. Userspace VirtIO-BLK Write / Flush Path

**目标：** 支持 write，以及项目当前选定的最小 flush semantics。

**验证：** Guest 内创建或修改文件，关闭 VM 后直接检查 raw filesystem image，确认数据已经持久化。

#### D4. 从 `/dev/vda` 启动

**目标：** 以 VirtIO-BLK 作为 root filesystem。

**验证：** Linux 使用 `root=/dev/vda...` 成功进入 userspace，文件系统可正常使用。

#### D5. Userspace VirtIO-NET TX

**目标：** 将 Guest 发出的 frame 发送到 Host TAP backend。

**验证：** Host 能抓到 Guest TX 发出的预期 Ethernet frame。

#### D6. Userspace VirtIO-NET RX

**目标：** 将 TAP 收到的 frame 填入 Guest RX buffer，并完成 interrupt notification。

**验证：** Guest 能 ping 通 Host 侧 TAP 地址。

### Phase E — Event Acceleration 与 vhost

#### E1. `epoll` Event Loop

**目标：** 将 Host 侧 device/event fd 统一接入 `epoll`。

**验证：** event loop 重构后，原有 block/network 功能保持正确。

#### E2. `ioeventfd`

**目标：** 将部分 Virtqueue kick 从通用 MMIO exit 中移出。

**验证：** 功能不变，同时针对 queue notification 的 VM Exit 数量明显下降。

#### E3. `irqfd`

**目标：** 对完成的 I/O 使用 eventfd-backed interrupt injection。

**验证：** Guest interrupt counter 持续增长，并且重复负载下 I/O 保持正确。

#### E4. `vhost-net`

**目标：** VMM 保留 VirtIO control plane，但将 network Virtqueue dataplane 交给 vhost-kernel。

**验证：**

- Guest 网络仍然可用；
- Host 能观察到 vhost worker activity；
- 在网络负载下，VMM userspace 不再是主要 packet-moving dataplane。

### Phase F — Modern VirtIO-PCI

#### F1. 最小 PCI Config Space Access

**目标：** 实现 Linux 枚举一个虚拟 PCI endpoint 所需的最小 x86 PCI configuration mechanism。

**验证：** `lspci -nn` 能看到 synthetic PCI endpoint。

#### F2. BAR Probe / Configuration

**目标：** 正确实现当前虚拟设备所需的 BAR 行为。

**验证：** Linux 能为 BAR 分配并映射资源，`lspci -vv` 报告预期 resource。

#### F3. Modern VirtIO PCI Capabilities

**目标：** 按 VirtIO spec 暴露 Modern VirtIO 所需 capability regions。

**验证：** Linux 绑定 modern `virtio_pci` path，而不是 legacy path。

#### F4. 在 PCI 上复用 VirtIO-BLK

**目标：** 将已有 block device / Virtqueue 逻辑挂到 Modern PCI transport。

**验证：** 删除 MMIO device 声明后，仍能从 PCI VirtIO block device 成功启动。

#### F5. 在 PCI 上复用 VirtIO-NET

**目标：** 将已有 net device / Virtqueue 逻辑挂到 Modern PCI transport。

**验证：** Guest 网络通过 PCI VirtIO device 正常工作。

### Phase G — VFIO、SR-IOV 与 mdev

进入这个 Phase 前，先独立验证 Host 环境：

- KVM access；
- IOMMU availability；
- VFIO support；
- IOMMU group layout；
- target device 是否适合实验。

如果没有对应硬件，真实 SR-IOV 可以不做，但不能阻塞前面的 VFIO/mdev 学习路径。

#### G1. VFIO Environment Inspector

**目标：** 枚举选定 device/group，并打印 region、IRQ capability 和当前 VFIO API 所需 Host 条件。

**验证：** 此阶段不启动 Guest。工具应能为目标设备输出一份可复现的 resource report。

#### G2. Guest Memory DMA Mapping

**目标：** 通过当前选择的 VFIO/IOMMU API，将设备 DMA 所需 Guest RAM 映射到 IOMMU。

**验证：** mapping 成功，并且所有映射 GPA range 与 VMM 已注册 Guest memory region 完全一致。

#### G3. PCI Config Space Forwarding / Model Integration

**目标：** 让 VFIO-backed endpoint 通过 VMM PCI model 被 Guest 枚举。

**验证：** Guest `lspci` 看到预期 Vendor/Device Identity。

#### G4. BAR Access

**目标：** 使用当前 VFIO region/API 支持的方式，将设备 region 提供给 Guest。

**验证：** Guest driver 能访问相关 BAR，并且没有出现未处理 VMM exit 或 Host fault。

#### G5. Interrupt Delivery

**目标：** 将 VFIO device interrupt 连接到 Guest interrupt delivery。

**验证：** Guest `/proc/interrupts` 在受控设备操作期间持续增加。

#### G6. mdev Test Device

**目标：** 如果开发内核支持，使用内核 sample `mtty` 等 mediated device backend，再走一遍 VFIO-backed 路径。

**验证：** Guest 能枚举并实际操作 mediated device。

#### G7. SR-IOV VF Experiment（依赖硬件）

**目标：** 当 Host 存在合适 SR-IOV 硬件且 IOMMU isolation 安全时，直通一个真实 VF。

**验证：** Guest 绑定 native driver，并完成一个最小 device-specific operation。

真实 SR-IOV 硬件不能成为前面 VFIO/mdev 学习步骤的前置阻塞项。

### Phase H — SMP

#### H1. 多 vCPU Thread

**目标：** 使用清晰的 lifecycle control 运行 N 个 vCPU。

**验证：** 所有 vCPU thread 都能稳定启动与停止，此时不要求完整 Linux SMP boot 已经正确。

#### H2. Guest SMP Startup / Topology

**目标：** 实现 Linux 启动 AP 所需的 CPU state / topology。

**验证：** Guest `nproc` / `lscpu` 显示配置的 vCPU 数量，多线程 workload 能使用多个 vCPU。

### Phase I — vsock 与 vhost-vsock

#### I1. Userspace VirtIO-vsock

**目标：** 实现受控 Host/Guest 交换所需的最小 rx/tx/event queue 与 connection state。

**验证：** 一条已知消息可以 Host -> Guest 和 Guest -> Host 双向传递。

#### I2. `vhost-vsock`

**目标：** 将 vsock dataplane 下沉到 kernel vhost backend，同时 VMM 继续负责配置。

**验证：** AF_VSOCK 通信正常，同时 VMM 已不再搬运 payload data。

### Phase J — 可选高级功能

#### J1. Pause / Resume Quiescence

**目标：** 在定义明确的 quiescent point 停止 vCPU 和 device processing。

**验证：** 多次 pause/resume 不会破坏 Guest I/O 或时间状态。

#### J2. VM / Device State Serialization

**目标：** 序列化当前 snapshot design 所需的 KVM 与 emulated-device state。

**验证：** 在 Guest memory snapshot 优化之前，先能将状态 dump 并恢复到一个等价的 stopped VM。

#### J3. Guest Memory Snapshot / Restore

**目标：** 第一版只做 full memory dump；dirty-page / lazy restore 放到后续。

**验证：** 通过受控 workload，restore 后能回到相同 Guest-visible execution state。

---

## 6. 每一个 Step 的 Definition of Done

一个 Step 只有同时满足以下条件才算完成。

### 1. Code

- 修改范围只覆盖当前 Step；
- 不引入新的 unresolved compiler warnings；
- 对 `open`、`mmap`、`ioctl`、`pread/pwrite`、`read/write` 等可预期失败进行检查；
- 所有 Guest-controlled address / length 在 dereference 前完成验证。

### 2. 可复现验证

必须记录：

- build command；
- run command；
- 必需的 Host setup；
- 精确的 Guest/Host 预期输出；
- 一个客观 pass criterion。

“在我的机器上能跑”不算验证标准。

### 3. Failure Observability

失败路径必须暴露足够状态，使后续调试不依赖猜测。

例如：

- KVM exit reason 与 payload；
- boot failure 时 CPU register state；
- Virtqueue failure 时 descriptor index / flags / length / GPA；
- transport failure 时 MMIO/PCI offset 和 access width；
- interrupt failure 时 GSI/vector/eventfd/IRQ count；
- passthrough failure 时 VFIO region/IRQ metadata 与 ioctl `errno`。

### 4. 学习笔记 / 博客

验证通过后，写一篇短章节，至少包括：

- 这个机制解决什么问题；
- 它在整个架构中的位置；
- 关键数据结构 / 状态机；
- 本实现中实际观察到的 execution / data flow；
- 哪些部分最困难或反直觉；
- 当前验证为什么足以证明实现有效；
- 合适时给出一张 ASCII 或 Mermaid 图；
- 本 Step 实际引用过的 spec / header。

博客必须描述**真正验证通过的代码**，而不是最初计划中的设计。

---

## 7. Agent 必须保持的代码架构

推荐目标结构：

```text
src/
  kvm/            VM、vCPU、CPUID、memory slot、irqchip
  arch/x86/       long mode、boot protocol、serial / 最小 legacy support
  memory/         GPA/HVA region 与安全 Guest memory access
  virtio/
    queue/        Split Virtqueue core
    core/         device state 与 feature negotiation
    mmio/         VirtIO-MMIO transport
    pci/          Modern VirtIO-PCI transport adapter
    blk/          block device semantics
    net/          network device semantics
    vsock/        vsock semantics
  pci/            最小通用 PCI config/BAR model
  event/          epoll、eventfd、ioeventfd、irqfd
  vhost/          vhost-net / vhost-vsock setup
  vfio/           VFIO device、DMA map、region、IRQ
  snapshot/       可选的后期 state serialization

tests/
  payloads/       最小 Guest assembly / binary
  fixtures/       synthetic Virtqueue / input fixture
  scripts/        Host setup 与可复现 integration checks

docs/
  steps/          每个完成 Step 对应一篇学习笔记 / 博客
```

具体文件名允许不同，但这些层次边界不要打乱。

---

## 8. 审查 Agent 生成代码时的高风险区域

以下部分必须重点审查：

- KVM special register / segment setup；
- boot parameter offset 与 kernel entry 假设；
- E820 / Guest physical memory layout；
- PCI config space access width、alignment、BAR mask、capability chain；
- VirtIO feature negotiation 与 device-status transition；
- descriptor-chain 终止条件和 loop detection；
- Guest-provided GPA + length 算术；
- avail / used index wraparound；
- shared Virtqueue 的 memory ordering；
- interrupt acknowledge / deassert；
- eventfd ownership / lifetime；
- VFIO DMA map / unmap lifetime；
- BAR mmap / region capability；
- MSI / MSI-X / INTx wiring；
- vCPU thread stop / pause race；
- snapshot quiescence。

对于这些区域，宁可要求一个小型 proof/test 加对应 spec 引用，也不要接受一个大型“看起来应该对”的 patch。

---

## 9. 每个 Step 新开 Agent Thread 时使用的 Prompt 模板

```text
我们现在只实现 C/KVM 学习型 VMM 的一个 Step。

当前 Step：
<STEP NAME>

目标：
<ONE OBSERVABLE BEHAVIOR>

已经稳定并验证通过的前置模块：
<FILES / PREVIOUS STEPS THAT ALREADY PASS>

当前 Step 可使用的权威资料：
<SPEC SECTIONS / LINUX HEADERS>

约束：
- 不实现后续 roadmap 功能。
- 不重构与当前 Step 无关的稳定代码。
- 协议常量、结构体 layout 和寄存器定义必须从提供的 spec/header 推导，不能凭记忆填写。
- 所有 Guest-controlled address 和 length 必须检查。
- 出现故障时先收集证据；如果证据不足，就添加显式诊断信息，不要猜测根因。

首先阅读当前接口与参考资料，然后只针对这个 Step 给出最小实现方案。

在宣布完成之前，必须提供：
1. 修改了哪些文件，以及原因；
2. build command；
3. 精确的 run/test command；
4. 预期出现的验证证据；
5. 实际运行得到的证据；
6. 当前 Step 仍然存在的限制。

只有验证通过以后，才编写 docs/steps/<step>.md 作为本 Step 的学习博客。
不要开始下一个 roadmap Step。
```

---

## 10. 最终原则

这个项目的成功标准不是“代码越来越多”，而是不断积累**已经被验证的小机制**。

在任何阶段，都应该能够明确回答：

- 上一个 Step 到底新增了什么机制；
- 哪一个具体观察结果证明它真的工作；
- 哪一份规范定义了这个行为；
- 当前状态属于哪个子系统；
- 从一个干净 checkout 开始，怎样完整复现结果。

如果这五个问题无法清楚回答，就说明当前 Step 太大，或者验证还不够充分。

先拆小，再继续。

---

## 11. 接手上下文：当前进度、环境与已踩过的坑

本节记录的是已经被实际运行验证过的结论。每一条坑都曾经真实发生过，修复方式已经落在代码里；动到相关代码时先读对应条目，避免把问题重新引入。

### 11.1 当前进度与编号对照

代码注释和 `tests/` 使用的是仓库自己的 Step 编号，和本文第 5 节的 Phase 对照如下：

| 仓库 Step | 本文 Phase | 状态 | 验收脚本 |
|-----------|-----------|------|----------|
| Step 1.1 / 1.2 | A1–A4 | 完成 | `tests/step1.sh` |
| Step 2.1 / 2.2 / 2.3 | B1–B5 | 完成 | `tests/step2.sh` |
| Step 6.1 | H1–H2 | 完成 | `tests/step6.sh` |
| Step 3.x / 4.x / 5.x / 6.2–6.4 | C–G、I | 未开始 | `src/` 下同名占位文件，首行注释标了 `[Step x.y]` |

跑全部回归：`VMLINUX=<vmlinux 路径> ./tests/step1.sh -a && ./tests/step2.sh -a && ./tests/step6.sh -a`。不设 `VMLINUX` 时需要 guest 内核的用例会 SKIP。initramfs 由 `tools/build_initramfs.sh` 生成（需要静态 busybox），输出到 `build/initramfs.cpio.gz`。

当前能力：Linux 6.6 以 vmlinux ELF 直启，走 64 位 boot protocol，经 initramfs 进入 BusyBox shell，`--smp N` 支持多核（上限 32），可以通过 `poweroff -f`、panic 后 reboot、Ctrl-A x 三种方式退出。已知限制：RAM 不超过 3 GiB（还没有 MMIO hole）；只支持未压缩的 vmlinux，不支持 bzImage；`--smp` 大于 1 只能配合 `--mode linux`。

### 11.2 开发环境

- Host 是海光 Hygon C86 3280，vendor string 为 `HygonGenuine`，微架构派生自 Zen1，走 `kvm_amd`，所以 `src/arch/amd/` 同时服务 AMD 和 Hygon。系统是 Ubuntu 22.04，host 内核 5.15。
- Host 内核源码在 `~/china_os/linux-5.15`，guest 内核源码和预编译的 vmlinux 在 `~/china_os/linux-6.6`。查 KVM 行为以 5.15 源码为准，查 guest 行为以 6.6 源码为准，不要凭记忆。
- Guest 内核配置里跟 VMM 相关的事实：`CONFIG_X86_MPPARSE` 未开启，所以 MP table 会被忽略；`CONFIG_ACPI=y`；`CONFIG_KVM_GUEST` 未开启，所以没有 kvmclock，clocksource 用的是 TSC。
- 两棵内核树都有 `compile_commands.json` 和 clangd background index，index 由 `~/.local/bin/clangd`（clangd 22）建立。非交互 ssh 的 PATH 里是 `/usr/bin/clangd`（clangd 14），版本对不上，必须显式指定 22 版。符号查询用 `~/.local/bin/clangd_query.py`，调用时带上环境变量 `CLANGD=~/.local/bin/clangd`。clangd 在打开第一个文件之前不会加载 index，所以先对任意 `.c` 执行一次 `outline`，再用 `symbol`。纯文本搜索用 `~/.local/bin/rg`，它同样不在 ssh 的 PATH 里。
- 远端命令如果超时，被中断的只是连接，远端的循环和子进程可能还在跑，事后要检查有没有残留的 `vmm` 或 clangd 进程。

### 11.3 Linux 直启与机器模型

- **MP table 被 guest 忽略**：guest 没开 `CONFIG_X86_MPPARSE`，只提供 MP table 时 guest 会退化成 virtual wire 模式。现在同时提供 MP table（`0x9fc00`）和 ACPI（`0xE0000`，RSDP rev2 → XSDT → FADT/DSDT/MADT），guest 开了哪个就用哪个。两份表里的 CPU 数、IOAPIC ID（等于 nr_cpus）、ISA IRQ 到 IOAPIC pin 的恒等映射必须保持一致。
- **FADT 不能标成 HW-reduced**：reduced 模式下 Linux 会禁用 PIT 和 PIC，而 TSC 校准和 legacy timer 路径都依赖它们。
- **`KVM_CREATE_IRQCHIP` 和 `KVM_CREATE_PIT2` 必须在创建 vCPU 之前调用**：KVM 在 `KVM_CREATE_VCPU` 时才决定是否给 vCPU 挂内核态 LAPIC。
- **PIT2 必须带 `KVM_PIT_SPEAKER_DUMMY`**：不带的话，端口 0x61（PIT channel 2 的 gate）会落到用户态且没人处理。Linux 的 TSC 校准因此间歇性失败，症状是启动卡在 "skipped IO-APIC setup"，时间戳停在 0.001000。
- **必须模拟 CMOS RTC（0x70/0x71）**：没有 RTC 时 guest 会产生约 4 万次 exit，浪费约 1.4 秒，并打印 "Unable to read RTC"。
- **i8042 的控制器命令要全部应答**：应答不全时 guest 探测会浪费约 600ms（日志里有 "Can't read CTR"）。AUX 口要让 guest 当场判定为不存在：AUX_LOOP 的回应不置 AUXDATA 位，AUX_TEST 返回 0x01。`reboot=k` 依赖 i8042 reset（命令 0xFE，或者写输出端口时 bit0 为 0），VMM 以此作为退出信号。
- **poweroff 走 ACPI S5**：DSDT 里需要 `_S5_` package；guest 往 PM1a_CNT（端口 0x604）写入 SLP_TYP=5 并置 SLP_EN，VMM 据此退出。
- **串口中断是边沿触发**：`KVM_IRQ_LINE` 驱动的 ISA IRQ 是边沿触发，每个字节都需要一次新的 0→1 跳变，只保持高电平不会产生新中断。
- **kernel 和 initrd 必须配套**：既没有 `--initrd`、cmdline 里也没有 `root=` 时，guest 一定以 "VFS: Unable to mount root fs" panic 收场。VMM 只打印 warning 而不拒绝启动，因为 step2/step6 的 "VFS panic → i8042 reset" 用例依赖这条路径。

### 11.4 vCPU 与 SMP

- **CPUID 的拓扑字段必须按 vCPU 改写**：`KVM_GET_SUPPORTED_CPUID` 返回的是 host 的原始拓扑，比如 leaf 1 里的 initial APIC ID 是执行这条 ioctl 的那颗 host CPU 的。原样透传的话所有 vCPU 会报同一个 APIC ID。`smp_fixup_cpuid()` 改写了 leaf 1、0x80000008、0x8000001D、0x8000001E。Hygon guest 的拓扑解析走 TOPOEXT 这条路径（见 linux-6.6 `arch/x86/kernel/cpu/hygon.c` 的 `hygon_get_topology()`）。leaf 0xB 在本机 KVM 上只返回了 subleaf 0，所以整体清零，表示"不提供"。
- **`init_vcpu` 里不能用 `vm->nr_vcpus`**：它在 `vm_create_vcpu()` 里随 vCPU 逐个创建递增，调用 `init_vcpu` 时还不是最终值，要用 `vm->cfg.nr_vcpus`。
- **AP 第一次 `KVM_RUN` 返回 `-EAGAIN`**：有内核态 irqchip 时，AP 创建后处于 `KVM_MP_STATE_UNINITIALIZED`，`KVM_RUN` 会阻塞到收到 INIT/SIPI，醒来后返回 `-EAGAIN` 而不是进入 guest（见 host 5.15 `kvm_arch_vcpu_ioctl_run()`）。这个返回值要当成"重进 KVM_RUN"处理，当成错误的话 AP 一醒来整台 VM 就会退出。
- **`immediate_exit` 路径不写 `exit_reason`**：`KVM_RUN` 以 `-EINTR` 返回时，`kvm_run->exit_reason` 可能是上一次 exit 的旧值。这时不能再调用 `vcpu_handle_exit()`，否则会把上一次的 IO 再执行一遍。`kvm_run()` 对 EINTR 和 EAGAIN 都返回 `VMM_ERR_INTR`，run loop 直接 `continue`。
- **停机只靠信号有竞态**：目标线程刚检查完 `should_stop`、还没进入 `KVM_RUN` 时，信号会被 handler 吃掉，随后线程进入 `KVM_RUN`，并永远睡在 guest 的 HLT 里。`vm_request_stop()` 的做法是先置 `immediate_exit=1` 再发 SIGUSR1。SIGUSR1 的 handler 不能带 `SA_RESTART`。依赖 `KVM_CAP_IMMEDIATE_EXIT`。
- **设备模型不是线程安全的**：多个 vCPU 线程可能同时陷出，所以 `vcpu_handle_exit()` 在 IO/MMIO 分发外面套了一把大锁 `vm->io_lock`。串口另有自己的 mutex，用于和 console 线程同步。加锁顺序固定为先 `io_lock` 后串口 mutex，console 线程只拿串口 mutex。
- **Step 1 模式不能多核**：没有内核态 irqchip 时，所有 vCPU 一创建就是 RUNNABLE，会从同一个入口同时开跑，所以 `--smp` 大于 1 只允许 `--mode linux`。

### 11.5 Console 与测试

- **`timeout` 包着运行时 VMM 会卡死**：`timeout` 会把子进程放进单独的后台进程组。stdin 是终端时，`tcsetattr` 会触发 SIGTTOU，`read` 会触发 SIGTTIN，进程被停住，Ctrl-C 也没反应。现在 `console_start()` 发现 stdin 是终端、但自己不在前台进程组时，不切 raw 模式也不读输入，只打印 warning；`tests/lib.sh` 的 `run_case` 把 stdin 接到 `/dev/null`。人工运行时请用 `timeout --foreground`。
- **测试要在带 pty 的环境下也跑一遍**：通过无 pty 的 ssh 运行时 stdin 不是终端，碰不到上面那类问题。用户是在交互式终端里跑测试的，所以要用 `python3 -c 'import pty; pty.spawn(["/bin/sh","-c","..."])'` 复现。不要用 `script` 管道，实测会挂住；也不要在 pty 里用 `bash -c`，它会被用户的 profile 切到 zsh。
- **给 guest shell 喂命令要留间隔**：串口 RX FIFO 只有 256 字节，一次灌入太多会丢字符。`feed_shell` 先等 6 秒让 shell 起来，之后每条命令间隔 1 秒。
- **断言用的标记必须由 guest 计算生成**：guest 会回显命令行，直接断言 `nproc` 的输出会被回显误匹配。正确写法是 `echo MARK-$((6*7))` 然后断言 `MARK-42`，或者 `echo NPROC-$(nproc)` 然后断言 `NPROC-4`。
