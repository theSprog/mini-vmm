# 测试设计

状态：`PARTIALLY APPROVED`（P1a/P1b/P1c 与 P3-min 已批准并执行完毕，三个缺陷已修复；P0、P2、P4 仍为 `PROPOSED`）

计划版本：2

对应的代码版本或构建产物：基线 `6eedbd5 step2: handoff`，其上有三处缺陷修复与一次 IO 分发接口提取（见下）

未提交改动的指纹：

- `src/boot/elf_loader.c` — 修 `check_ehdr()` 的整数回绕、`p_offset + p_filesz` 的同类回绕、`Elf64_Phdr` 未对齐解引用
- `src/boot/acpi.c` — 修 XSDT 条目的未对齐 `uint64_t` 存储
- `src/vm.c` + `include/vmm.h` — 把 PIO/MMIO 分发提成公开的 `io_bus_dispatch_pio/mmio()`，`handle_io()/handle_mmio()` 退化成从 `kvm_run` 取字段的薄封装
- `Makefile` 追加“验收测试”一节；`tests/` 目录重构

状态含义：`PROPOSED` 等待审批；`PARTIALLY APPROVED` 只批准部分阶段；`APPROVED` 全部批准；`EXECUTING` 正在执行；`COMPLETED` 已完成；`SUPERSEDED` 已被新计划替代。

---

## 我要确认什么

- **当前准备验收的改动**：不是某一次改动，而是 kvm-basics、linux-boot、smp 完成后的既有实现。触发点是仓库里的测试方法过于单一，见下节。
- **希望确认的行为**：
  1. `gpa_to_hva()` 对越界、跨 slot、零长度和整数回绕的地址范围一律拒绝（`AGENTS.md`「长期 invariant」里的头号 invariant）；
  2. PIO/MMIO 总线的注册、查找、分发语义符合 `include/vmm.h` 写明的约定；
  3. `elf_load_vmlinux()` 与 `bzimage_load()` 面对任意畸形文件都不会越界访问或崩溃。
- **为什么这些行为重要**：这三处是后面 PCI 设备和 VirtIO 会直接依赖的地基。virtqueue 的每一个 descriptor segment 都要过 `gpa_to_hva()`；virtio-mmio 的配置空间会按各种宽度访问总线；内核镜像是这个 VMM 唯一解析的外部文件。三者出错的后果都是 VMM 进程内的内存破坏，而不是“guest 跑不起来”这种一眼能看见的失败。
- **用来比较的旧行为、规范或其他正确标准**：
  - `include/memory.h` 对 `gpa_to_hva()` 的语义注释（契约本身）；
  - `include/vmm.h` 对 `vmm_io_dev` 回调的语义注释、`learning-note/07` 第 7.3 节（未注册端口读回 0xFF）；
  - `arch/x86/kernel/i8237.c` 的 `i8237A_init_ops()`（未注册端口必须读回 0xFF）；
  - ASan / UBSan（内存安全与未定义行为的判据，完全独立于本实现）。

### 既有测试为什么不够

仓库原有 29 条自动用例集中在两种方法上：整机启动一个真 Linux 然后 grep 内核日志，以及一次离线的固件表字段检查。前者的 oracle（guest 内核）很强，但粒度极粗且只覆盖 happy path；后者只管固件表。结果是：

- 三个被测行为里，只有第 3 项被“喂过一个好文件”这种程度覆盖过；
- 反向断言依赖“出错时内核恰好打印某串”，换一种坏法就静默通过；
- 单点环境：一个 host、一个内核、一份 config。

---

## 长期原则：覆盖冷门但合法的路径

mini-vmm 的测试同时用来检验它下面的 KVM。设计里程碑的验收用例和 mutant 时，除了主流 guest、主流 VMM 会走的路径，还要覆盖冷门但合法的路径。判断标准是：spec 允许、guest 能合法做到，但 QEMU 默认配置和常见发行版的启动方式不会触发。可以从这几个维度去找：irqchip 模式（内核、split）、APIC 模式（xAPIC、x2APIC、关闭）、关掉常用特性的 guest 启动参数（`noapic`、`nolapic`、`noapictimer`、`no-kvmclock`、`nohz=off` 这一类）、spec 允许但少见的寄存器写入顺序，以及恰好落在模式切换窗口里的中断。

理由：KVM 的常用路径经过大量真实负载检验，冷门组合却可能多年没人走过，那里的 bug 只有专门去走的测试才能发现。例如 in-kernel PIC 在 LVT0 被屏蔽期间到来的 ExtINT 会永久丢失，只在内核 irqchip 加 `noapic` 时触发，见“没有验证的内容和剩余风险”。

落实时的判据：

- 冷门组合上的偶发失败不当作 flaky 绕开，也不靠重试或放宽超时消除，按 `AGENTS.md`「调试由证据驱动」查清是 VMM 还是 KVM 的问题。
- 判定为 KVM 问题之前，先在最新上游 KVM 上复现，因为 host 发行版内核里看到的问题上游可能已经修了。
- 已确认由 host KVM bug 导致失败的组合，在“没有验证的内容和剩余风险”里写明机理和修复状态，host 带上修复后再放回矩阵。

---

## 这次会测什么、不测什么

### 会覆盖（P1，已执行）

- **P1a** `src/boot/elf_loader.c`、`src/boot/bzimage.c` 的畸形输入路径（libFuzzer + ASan + UBSan）。
- **P1b** `src/mem.c` 的 `gpa_to_hva()` / `mem_read` / `mem_write` / `mem_add_region` / `mem_find_slot`。
- **P1c** `src/vm.c` 的 `vmm_register_pio/mmio`、`vmm_lookup_pio/mmio`、`handle_io()`、`handle_mmio()`。

### 明确不覆盖

- **`src/virtio/`、`src/vfio/`、`src/vhost/`、`src/snapshot/`、`src/io/`、`src/boot/cmdline.c`**：全部是 0–1 行占位文件，不在 `Makefile` 的 `SRCS` 里，没有行为可验。
- **`src/arch/intel/`**：只有一个 README，Intel 后端未实现。
- **设备状态机的寄存器级契约**（8250 / i8042 / RTC / ACPI PM1a）：目前只有整机启动这一条间接证据。属于 P2，未批准。
- **并发**（`io_lock` 与串口 mutex 的加锁顺序、SMP 停机竞态）：属于 P4a，未批准。
- **性能与 exit 次数回归**：属于 P4b，未批准。
- **跨 host、跨 guest 内核版本的可移植性**：机器上只有一个内核，且无网络。

### 可能因此遗漏的问题

- 设备模型的寄存器语义错误，只要不让 Linux 驱动报错就发现不了；
- 只在多 vCPU 竞争下出现的数据竞争；
- 换一份 guest 内核 config 才会走到的探测分支。8250 的变体探测（`CONFIG_SERIAL_8250_16550A_VARIANTS=y` 才执行的 EFR / NatSemi / TI16750 / XScale / `size_fifo()` 路径）已由 `tests/unit/serial8250_test.c` 按 `8250_port.c` 的访问顺序回放，与内核配置无关；其余设备的配置相关分支仍只靠手头那一个内核的 e2e。

---

## 准备怎样检查

| 阶段 | 具体动作和目的 | 会触碰的文件、模块或环境 | 什么现象算通过或失败 | 要留下什么证据 | 什么时候停止 | 是否获批 |
|---|---|---|---|---|---|---|
| P0 | `make -B` 后把 5 个既有 suite 各跑 3 遍，分清 pre-existing failure 与 flaky | 只读 `src/`，写 `build/`；需要 `/dev/kvm` | 每条用例 3/3 结果一致 | 每次运行的完整输出 | 出现 FAIL 即停 | 未批准 |
| P1a | 把两个加载器链进 libFuzzer target，输入经 memfd 喂进真实的 `open`+`mmap` 路径 | 新增 `tests/fuzz/`；写 `build/fuzz/` | 无 ASan/UBSan 报告、无 crash artifact、进程正常结束 | libFuzzer 的 `stat::` 行、crash artifact、复现命令 | 每 target 用满时间预算或首次 crash | **已批准，已执行** |
| P1b | 用一份独立写出的 128 位区间模型做 differential，覆盖边界网格、整数回绕、跨 slot、20 万轮随机 | 新增 `tests/unit/mem_test.c`；链接 `src/mem.c` | 实现与模型逐点一致，且 `0 failed` | 摘要行、随机种子 | 首次不一致即停并打印 gpa/len | **已批准，已执行** |
| P1c | 伪造 `kvm_run` 页，直接驱动真实的 `handle_io()`/`handle_mmio()` | 新增 `tests/unit/io_bus_test.c`；`#include src/vm.c` | 断言全过且 `0 failed` | 摘要行 | 首次失败即停 | **已批准，已执行** |
| P2 | host 侧直接驱动四个设备的读写回调，期望值取自 datasheet / Linux 驱动源码 / 卷七实测序列 | 新增 `tests/unit/devices_*.c`；可能需要设备解耦 | 逐寄存器比对通过 | 每条契约对应的用例名 | 首次失败即停 | 未批准 |
| P3-min | **接口层** negative control：把被测函数换成故意写错的完整实现（`tests/unit/*_mutants.c`），跑同一套用例，要求每个 mutant 都让测试非零退出 | 只在测试代码内，`--mutant N` 选择 | 每个 mutant 都被抓住，且是因为断言失败而红 | `make test negctl` 的逐条结果 | 全部 mutant 跑完 | **已批准，已执行** |
| P3-full | 按 `learning-note/07` 记录的设备契约对四个传统设备逐条注入已知缺陷 | 需要先有 P2 的设备单元测试 | 同上 | 同上 | — | 未批准（依赖 P2） |
| P4a | ASan+UBSan 与 TSan 构建下各跑一次完整启动关机 | 需要 `/dev/kvm`；TSan 下明显变慢 | 无 sanitizer 报告 | 报告全文 | 单次超过 300s 即中止 | 未批准 |
| P4b | 把 `trace_decode.py --summary` 变成带容差的回归断言 | 扩 `tests/e2e/legacy-devices.sh` | 总数 30449 ±5%、PIT 端口恒为 0 | 每次的统计输出 | — | 未批准 |
| P4c | 去掉 guest 用例对固定延迟的依赖：命令改由 `tests/guest_shell.py` 等 shell 提示符后发送，`gshell` suite 用假 guest 证明固定延迟会丢命令而新驱动不丢；再把 guest 用例连跑多遍确认稳定 | 新增 `tests/guest_shell.py`、`tests/offline/{guest_shell.sh,fake_guest.py}`，改 `tests/e2e/step{2,6,7}.sh` | `gshell` 全过；guest 用例多遍连跑无 FAIL | `gshell` 输出、连跑的逐遍结果 | 出现 FAIL 即停 | **已批准，已执行** |

---

## 时间和资源估计

| 阶段 | 大概耗时 | 使用多少 CPU | 大概内存 | 新增文件或磁盘占用 | 外部服务、硬件或人工配合 | 可能的副作用和恢复办法 |
|---|---|---|---|---|---|---|
| P1a | 构建 ~30s；每 target 300s | 1 核（`-jobs=1`） | 峰值 ~500 MiB（ASan 影子内存） | `build/fuzz/` 语料随运行增长，约几 MiB | 无 | 只写 `build/`，`make tests-clean` 即可清掉 |
| P1b | 构建 ~5s；运行 <1s（20 万轮） | 1 核 | ~10 MiB | 无 | 无 | 无 |
| P1c | 构建 ~5s；运行 <0.1s | 1 核 | ~5 MiB | 无 | 无 | 无 |

实测：P1b 20 万轮随机在 ASan 下不到 1 秒，比估计快，因此保留了 `mem_test <iters>` 参数，需要时可以加大。

---

## 执行细节

- **准备运行的具体命令**：

  ```sh
  make test unit                 # P1b + P1c + 固件表字段检查
  make test tables               # iasl 外部 oracle
  FUZZ_TIME=300 make test fuzz   # P1a
  make test                      # 默认集合
  make test linux-boot           # 单个 suite
  ```

- **允许在不重新审批时替换的同类命令及限制**：可以调整 `FUZZ_TIME`（上限 600s/target）、`mem_test` 的随机轮数、`-max_len`。不得在未批准的情况下开多 worker（`FUZZ_JOBS>1`）或让 fuzz 无限跑。
- **准备新增或修改的测试、输入样本和辅助程序**：见 `tests/README.md` 的目录说明。种子语料由 `tests/fuzz/mkseeds.py` 生成，手写的最小镜像不依赖机器上有没有真内核；如果设了 `VMLINUX` / `BZIMAGE`，再补两份真镜像的头部切片。
- **需要安装、启动或配置的东西**：无。soc63 上 gcc 11.4（ASan/UBSan）、clang 14（libFuzzer）、iasl 均已就位；这台机器没有外网，本方案刻意不引入任何新依赖。
- **后续维护这些测试的大概成本**：单元测试与实现的耦合点只有函数签名，改动成本低。唯一的例外是 `io_bus_test.c` 直接 `#include src/vm.c`（见 O2），`vm.c` 的外部依赖变化时要同步补桩。

---

## 估计依据和不确定性

- **时间和资源估计来自哪里**：先做了 30s 冒烟跑批，用实测的 exec/s 外推 300s 的规模；单元测试的耗时是直接实测的。
- **目前还不知道什么**：300s 的 fuzz 覆盖率上限在哪；`elf_loader` 关掉 alignment 检查之后还能挖出多深。
- **如需先做小范围试跑来收窄估计，它的上限是什么**：已做，30s/target。

---

## 安全和隔离

- **在哪里运行**：soc63（hygon-srv1，Ubuntu 22.04，裸机）。
- **如何避免污染当前工作区或真实数据**：P1 全程不需要 root，不碰 `/dev/kvm`，不改 `src/`；所有产物写在 `build/` 下，而 `build/` 已在 `.gitignore` 里。fuzz 的输入走 memfd，不落盘。
- **出现卡死、崩溃或空间增长时怎样停止和恢复**：libFuzzer 有 `-max_total_time` 和 `-rss_limit_mb` 双重上限；`make tests-clean` 清掉 `build/tests` 与 `build/fuzz`。

---

## 用户批准记录

- **获批的计划版本**：2
- **获批时对应的代码版本**：基线 `6eedbd5`
- **获批的测试范围**：`gpa_to_hva` 及内存 slot 管理、IO 总线注册/查找/分发、两个内核加载器的畸形输入
- **获批的阶段**：P1a、P1b、P1c、P3-min
- **额外获批的生产代码改动**：修复本轮查出的三个缺陷；把 IO 分发提成公开接口（O2）
- **时间、资源或其他限制**：fuzz 每 target 5 分钟、单 worker
- **用户明确批准的依据**：“先测 p1 a+b+c” → “三个缺陷可修 …… io_bus_test 这个，可以提成分发 dispatch”，negative control 以“能长期复用才沉淀”为条件批准
- **哪些变化必须重新询问用户**：开始 P0/P2/P4；延长 fuzz 预算；引入新依赖；对 `src/` 做本轮范围之外的改动

---

## 未决问题

- **O1 — 跨越设备末端的 IO 访问该是什么语义？**
  `handle_io()` / `handle_mmio()` 算出 `off = addr - dev->base` 之后没有校验 `off + size <= dev->len`，直接交给设备回调。三种可选契约：总线层拒绝、总线层截断、交给设备自己判断。在定下来之前，`io_bus_test.c` 的对应用例标记为 `[CHARACTERIZATION]`，只记录当前行为、不做对错判定。定了之后那条用例要改写成契约断言。
- **O3 — 多内核配置矩阵还没有搭。**
  e2e 只能覆盖手头那一个 guest 内核的配置，而 VMM 的目标是换一个 vmlinux 也能启动。会让 guest 走不同路径的配置项见 `HANDOFF.md`「guest 内核配置兼容」。理想做法是 `tests/run.sh` 接受多个 `VMLINUX`，对每个内核跑 linux-boot / smp / legacy-devices、结果按内核分列；候选内核是发行版式的大配置（大量 `=m`、`ACPI_REDUCED_HARDWARE_ONLY=y`）、`make defconfig` + `kvm_guest.config`、以及不开 `HYPERVISOR_GUEST` 的精简配置。卡在手头没有这么多内核，每份要单独编译。在搭起来之前，配置相关的设备语义优先用单元测试按内核源码回放（如 `serial8250_test.c`）。
- **O2 — 分发接口（已解决）。**
  已把分发提成 `io_bus_dispatch_pio()` / `io_bus_dispatch_mmio()`（`include/vmm.h` 声明，`src/vm.c` 实现），`handle_io()` / `handle_mmio()` 退化成从 `kvm_run` 取字段的薄封装。`io_bus_test.c` 不再 `#include src/vm.c`，改为正常链接并通过公开接口调用。
  代价要记下来：**KVM_EXIT 路径上从 `kvm_run` 取字段那几行现在没有单元测试覆盖**，尤其是“必须按 `io.data_offset` 定位缓冲区、不能假设它紧跟结构体”这一条。它只由 e2e 覆盖（任何一次 guest IO 都会走到）。

---

## 执行结果摘要

环境：soc63（hygon-srv1），Ubuntu 22.04.5，kernel 5.15.0-190，gcc 11.4.0，clang 14.0.0。代码版本 `6eedbd5`，`src/` 无改动。

| 证据编号 | 确认的行为 | 实际结果 | 证据位置 |
|---|---|---|---|
| E1 | `gpa_to_hva` 与独立 128 位模型在边界网格、整数回绕、跨 slot、20 万轮随机输入上逐点一致 | **PASS** — 201692 checks / 10 cases / 0 failed；seed `0x2545f4914f6cdd1d` | `make test unit` → `mem_test` |
| E2 | IO 总线的注册重叠检测、表满、查找边界、offset 计算、串操作步进、`VMM_ERR_EXIT` 中止、未注册端口读回 0xFF，以及 exit 统计按 exit_reason、按设备、按方向计数（串操作一次 exit 只计一次） | **PASS** — 1161 checks / 11 cases / 0 failed | `make test unit` → `io_bus_test` |
| E3 | 跨越设备末端的访问当前不做校验：`off=7, size=4` 照常交给回调（`dev->len=8`） | **CHARACTERIZED** — 记录现状，语义未定，见 O1 | `io_bus_test` 最后一个用例 |
| E4 | `bzimage_load()` 面对任意畸形文件无内存错误、无未定义行为 | **PASS** — 2346815 runs / 301s，0 发现 | `build/fuzz/logs/bz_strict.log` |
| E5 | `elf_loader.c` 的截断检查被 64 位回绕绕过，导致映射区之前的越界读 | **FAIL → 已修复** — 修复前 290271 runs 内触发；修复后 7074730 runs / 301s 零发现 | 修复前 `build/fuzz/logs/elf_noalign.log`；修复后 `elf_noalign_postfix.log` |
| E6 | `elf_loader.c` 以 `Elf64_Phdr*` 解引用文件里任意偏移，未对齐即未定义行为 | **FAIL → 已修复** — 修复前 9313 runs / 30s 内触发；修复后 6909083 runs / 301s 零发现 | 修复前 `elf_noalign.log`；修复后 `elf_strict_postfix.log` |
| E7 | 固件表生成过程无未定义行为 | **FAIL → 已修复** — `acpi.c:275` 未对齐 `uint64_t` 存储；修复后严格版退出码 0 | `build/tests/boot_tables_test` |
| E8 | 生成的 ACPI 表结构本身合法，DSDT 的 AML 与 ACPICA 的理解一致 | **PASS** — iasl 反汇编 APIC/DSDT/FACP/XSDT，无 Incorrect/Invalid/Error/Warning；DSDT 反汇编后再编译，AML 逐字节相同 | `make test tables`，6/6 |
| E9 | E5/E6 的最小复现输入在修复后被干净拒绝（RED→GREEN） | **PASS** — 两个样本 × 两个 target，全部退出码 0 无 sanitizer 报告；已沉淀为 `tests/fuzz/regress/*.bin`，每次 `make test unit` 重放 | `make test unit` → regress 段 |
| E10 | 单元测试有能力报警（negative control） | **PASS** — 17 个接口层 mutant 全部被抓住，且都是因断言失败而红 | `make test negctl`，17/17 |
| E11 | negctl 反过来查出了 `mem_test` 自身的覆盖漏洞 | **已修复** — 长度表缺“低 32 位小、高 32 位非零”一类，`len-truncated-32` 最初逃逸；补齐取值后 15/15 | `mem_test.c` 的 `g_lens` |
| E13 | `KVM_GET_STATS_FD` 文件按 header 的 offset 解析，块乱序、块间留空、desc 不对齐都能读对，七种损坏文件都被拒绝且无越界 | **PASS** — 52 checks / 5 cases / 0 failed；按位置猜 desc 块、按位置猜数据块、强转不对齐指针、不限数据块长度四种错误实现各自变红 | `make test unit` → `kvm_stats_test` |
| E14 | PCI conf1 与 BAR：只认 dword 对齐的 0xCF8 写、保留位非零当 master abort、BAR 只读位按大小屏蔽、IO BAR 高 16 位为 0、COMMAND 关 decode 后撤销路由、改写 BAR 后路由跟着走；INTx：KVM_IRQ_LINE 序列只在线电平变化时出现、共线设备取或、Interrupt Disable 屏蔽输出但 status.INT 照旧；Bus Master 默认关、关闭时 DMA 被拒且不碰内存，`pci_dma_map` 同样受 Bus Master 约束、不在 RAM 里的范围被拒且不改出参 | **PASS** — 187 checks / 15 cases / 0 failed；BAR 全可写、忽略 COMMAND、保留位别名、字节写地址口、32 位 IO BAR、窗口终点越界六种错误实现各自变红；INTx 不取或、忽略 Interrupt Disable、写 COMMAND 不重算线电平、不维护 status.INT、DMA 不查 Bus Master、复位时打开 Bus Master 六种错误实现各自变红 | `make test unit` → `pci_bus_test` |
| E15 | MP table 的 PCI 中断项与 DSDT `_PRT` 一致；AML PkgLength 在 1/2/3/4 字节编码的切换点上正确 | **PASS** — `boot_tables_test` 890 checks、`aml_test` 48 checks / 4 cases；MP 总线号错、GSI 偏一、INTx pin 偏一，以及 PkgLength 边界差一、不含自身长度、高位移位错共六种错误实现各自变红 | `make test unit` |
| E16 | Linux guest 用上 PCI 总线：type 1 配置访问、`_CRS` 窗口原样接收不重新分配、BAR 读写路由、ACPI 与 `acpi=off` 都分到 GSI 17 | **PASS** — `pci-bus` 10/10；`--pci-test` 时代的二进制 7/7 变红；`_CRS` 窗口缩小、忽略 COMMAND、MP GSI 偏一三种错误实现分别只让对应用例变红；`--device` 的三条命令行用例在没有 `--device` 的二进制上报 unknown option 变红 | `make test pci-bus` |
| E17 | `--device` 参数值解析：隐含键、`,,` 转义的结果与 QEMU `get_opt_name_value` 一致；重复键、短格式布尔、空元素、空键、超过 16 个属性在启动前报错，错误信息带原始参数和出错的键；未知属性列出合法属性；布尔值的取值集合同 QEMU `qapi_bool_parse`，id 规则同 `id_wellformed` | **PASS** — `opts_test` 118 checks / 7 cases；重复键取最后一个、不认转义、短格式当 on、第一个元素无条件当隐含键、缓冲区少 1 字节五种错误实现各自变红（最后一种由 ASan 报堆越界） | `make test unit` → `opts_test` |
| E18 | 网络后端：`--netdev` 语法与 id 规则（同 QEMU `id_wellformed`）；`netdev=` 引用一一对应，未知 id、两张网卡共用、没有网卡引用、id 重复都报错；经真实 TAP 收发（对端是绑在接口上的 AF_PACKET socket）：多段 iovec 拼成一帧原样出现、接口上的帧原样交给前端且在 I/O 线程上；前端回 FULL 后停止读 fd、`netdev_rx_resume` 后按原顺序交付且恢复读 fd；同一 TAP 第二次打开报 EBUSY、同名非 TAP 接口报 EINVAL、接口 down 时写入 EIO 且只警告一次；`evl_mod` 改关心的事件后原回调照常派发 | **PASS** — `netdev_test` 78 checks / 13 cases，`event_loop_test` 46 checks / 8 cases；暂停时不停读 fd、FULL 时丢弃挂起帧、resume 不写 eventfd、唤醒后不恢复读 fd、只写第一段 iovec、漏 `IFF_NO_PI`、不查未被引用的 netdev、每次写失败都警告、不查 id 格式九种错误实现各自变红（等待都有 5 s 上限，超时报出在等哪一帧）；内核不允许非特权 user namespace 时 TAP 部分打印 SKIP | `make test unit` → `netdev_test` |
| E19 | 真实 `vmm` 程序接好了 `--netdev`：非 linux 模式拒绝、未被引用的后端、拼错的属性、未知类型都在创建 VM 前报错；`--netdev help` 列出类型 | **PASS** — `emu-net` 前 5 条；没有 `--netdev` 的二进制报 unknown option 变红 | `make test emu-net` |
| E21 | 真实 Linux e1000 驱动在 user + net namespace 里经 TAP 工作：dmesg 里的 MAC 等于 `mac=`，链路 1000 Mbps 全双工，没有未实现寄存器的警告；guest 与 host 互相 ping 通；host → guest 64 MiB、guest → host 16 MiB 随机数据两端 sha256 一致（对端 `tests/net_peer.py`）；两端 TCP InCsumErrors 都为 0；host 向 guest 洪泛 20 万个 UDP 包，guest `rx_packets` 增量不少于发出的包数减去 host 内核在 TAP 上丢的包数（接口 tx dropped，即 tun ptr_ring 满时的丢包，加上 qdisc drops），且 VMM 退出时报告的后端暂停次数大于 0（guest 以 `e1000.RxDescriptors=48` 启动，环必然被用完）；`/proc/interrupts` 上 eth0 走 IO-APIC 17，中断数 ≤ ICR 读次数 ≤ 中断数 + 16，且不超过收发包数；`--trace-mmio` 记下 ICR 读和 TDT 写 | **PASS** — `emu-net` 后 8 条，单次运行约 12 s，洪泛期间后端暂停 658–1212 次（中断 8062、ICR 读 8065、包 65241）。四种错误实现各自变红：TSO 段不插 TCP 校验和（哈希仍一致，host InCsumErrors 2609，只有校验和计数这条变红）、RX 环不回绕（探测之后收不到包）、ICR 读不清零（中断 133 万次，传输失败）、MMIO 不记录 trace、RX 环满时不返回 FULL（20 万个包丢 7062 个，后端暂停 0 次，只有洪泛这条变红） | `make test emu-net` |
| E20 | e1000 设备模型（82540EM）：按 Linux 的 Microwire 序列逐位读出 EEPROM，校验和 0xBABA、MAC、EERD 一致，写命令被忽略；PHY ID 0x01410C20、link up 与自协商完成、只读位、自清零位、错误 PHY 地址置 ERROR；ICR/ICS/IMS/IMC 与 INTx 电平；CTRL.RST（含经 IO BAR）回到复位值；TX：SG 拼帧、legacy IC 与上下文描述符的 IP/TCP 校验和、TSO 分段（长度、IP ID、跨 2^32 的序号、FIN/PSH 只在最后一段、逐段校验和、载荷拼回原样）、PSP 补齐、VLAN 插入、环越界与 Bus Master 关闭时不处理、回绕；RX：补齐与 FCS、SECRC、超长丢弃、单播/广播/组播哈希/混杂过滤、环满返回 FULL 且不写、RDT 唤醒后端、回绕、多描述符与 null 缓冲、VLAN 剥离与 VFTA 过滤 | **PASS** — `e1000_test` 171 checks / 20 cases；校验和与 FCS 用独立实现验算（RFC 1071 含伪首部、逐位 CRC-32）；TSO 不加序号、不补每段 L4 长度、不清 FIN，RX 不回绕、不查容量、不加 FCS，ICR 读不清零、TX 不回写 DD、EEPROM 少哑位九种错误实现各自变红 | `make test unit` → `e1000_test` |
| E22 | 磁盘后端：`--drive` 必填 id/file、format 只有 raw、readonly 是布尔、没有隐含键；`drive=` 引用一一对应，未知 id、两个控制器共用、没有控制器引用、id 重复都报错；容量取文件长度；可写盘排他锁、只读盘共享锁；多段 iovec（含长度为 0 的段）连续落在 offset 上，用另一个 fd 核对镜像内容；最后一个字节可达、多一个字节被拒；offset+len 回绕、iovec 总长溢出、超过 `BLK_MAX_IOV` 段都被拒且不写镜像；只读盘写被拒；preadv/pwritev 每次只传 3 字节并夹带 EINTR 时仍逐字节正确；镜像运行中被截短时报 EIO | **PASS** — `blockdev_test` 1008 checks / 8 cases（测试自带 preadv/pwritev 替身制造部分传输）；范围检查用会回绕的加法、部分传输后不推进 iovec、不加锁、EINTR 当错误、读到文件尾不报错（死循环，被 30 s 闹钟截停）、不查 iovec 总长溢出、不查段数（栈上拷贝越界，ASan 中止）、不查只读八种错误实现各自变红 | `make test unit` → `blockdev_test` |
| E23 | 真实 `vmm` 程序接好了 `--drive`：非 linux 模式拒绝、未被引用的 `--drive`、拼错的属性、非 raw 格式、重复 id 都在创建 VM 前报错 | **PASS** — `emu-blk` 前 5 条；没有 `--drive` 的二进制报 unknown option，5 条全部变红 | `make test emu-blk` |
| E24 | AHCI 控制器（`ahci_test`）：PCI 身份与 CAP/PI/VS/CAP2；上电时有盘端口链路建立、PxSIG 为 0x00000101、PxTFD 0x0140，空端口 DET 0；PxSCTL.DET 1→0 的 COMRESET 置 PxSERR、发签名 D2H FIS（只在 FRE 打开时写入 FB），SPD 限速反映在 PxSSTS；GHC.HR 复位不清 PxCLB/PxFB；PxIS 是粘的、写 1 清，PxIS&PxIE 置 IS.IPS，INTx = GHC.IE 且 IS 非零；命令出错置 TFES、停端口且 PxCI 保留，ST 1→0 清 PxCI 后可恢复；ST=0 时写 PxCI 被忽略，软复位 FIS 被拒；命令头或 FIS 的 DMA 失败置 HBFS；CAP/PI 写被忽略、8 字节访问拆两次 dword、字节读取对应字节。命令（接真实的 `blockdev.c` 和临时镜像，镜像内容另用 pread 核对）：IDENTIFY 数据各字（序列号即 drive id、容量的 28 位饱和与 48 位值、校验和），IDENTIFY 经 PIO Setup FIS 结束（E_Status、PxTFD、PRDBC）；READ/WRITE DMA 的 LBA28（含 Device 低 4 位）与 LBA48 字节位置、Count 0 即 256/65536 个扇区；超过 `BLK_MAX_IOV` 的 1100 段 PRDT 分批读写且顺序正确；越界报 IDNF、CHS 寻址与只读盘上的写中止，镜像不变；PRD 指向非 RAM、PRDT 本身不可读置 HBFS，PRDT 短于传输长度置 OFS；DBA bit 0 与 DBC bit 0 按规定值处理，PRD 的 I 位置 DPS；FLUSH CACHE (EXT)，关写缓存时先 flush 且之后每次写都落盘；SET FEATURES 传输模式的合法范围，COMRESET 恢复默认；STANDBY IMMEDIATE 成功，不支持的命令中止且每个操作码只记一次日志。NCQ：CAP.SNCQ 与 IDENTIFY 字 75/76 的队列深度 32、字 84/87 的 GPL；FPDMA 读写的 Feature 计数（0 即 65536）、tag、48 位 LBA，接收时 I=0 的 D2H FIS 清 PxCI，完成时 SDB FIS 带 I=1 与该 tag 的 ACT、清 PxSACT 的这一位、只置 SDBS，PRDBC 不写，FUA 时 flush 一次；接收阶段出错报 ABRT 并记进日志 10h，此后除读日志 10h 外的命令都中止，读日志先发 ACT 全 1 的 SDB，日志的 tag/状态/错误/LBA/Device/Count 与校验和正确，没有待处理的错误时读日志不发 SDB；执行阶段读错（镜像被截短）报 SDB ERR UNC、ACT 为 0、PxSACT 保留该 tag；COMRESET 清错误状态、日志 NQ=1；日志目录（版本、10h 一页、80h-9Fh 各 16 页、其他 0），WRITE LOG EXT 经 PIO Setup（D=0、I=0）写主机日志再读回，六种非法读写日志中止 | **PASS** — 277 checks / 24 cases；PxIS 不粘、TFES 不停端口、ST 清零不清 PxCI、空端口也建链、签名字节序反、忽略 GHC.IE、FRE 关闭仍写 FIS、HR 清 PxCLB、不按 SPD 限速、ST=0 仍执行命令十种错误实现各自变红（每个 mutant 10 s 上限，TFES 不停端口那个曾让 vCPU 在 PxCI 上活锁，所以 `port_run` 每次只扫一遍 PxCI 的快照）；命令部分另有十四种：PRD 只检查起始地址（ASan 报越界写中止）、不按 `BLK_MAX_IOV` 分批、忽略 LBA28 的高 4 位、Count 0 不当 256、48 位 Count 0 不当 65536、PIO Setup 的 E_Status 带 DRQ、IDENTIFY 后 PxTFD 带 DRQ、读写不写 PRDBC、关写缓存后写不落盘、DBC bit 0 不置 1、DBA bit 0 不清、不置 DPS、溢出当主机总线错误、越界检查不计 LBA，各自变红；NCQ 与日志部分另有二十种：SDB 不清 PxSACT、不置 SDBS、接收后不清 PxCI、Feature 0 不当 65536、计数取自 Count、不读 LBA 字节 8、FUA 不 flush、出错后不拦其他命令、读日志 10h 不发 SDB 或不清错误、日志不算校验和、COMRESET 不清错误、执行出错仍在 ACT 里报完成、接收错误不记日志、页号按 r4a 取自 LBA 31:16、Count 0 的日志读不中止、允许写只读日志（ASan 报越界）、目录全报 1 页、PIO 写置 PSS、CAP 不报 SNCQ，各自变红 | `make test unit` → `ahci_test` |
| E25 | 真实 Linux ahci 驱动按类代码绑定控制器：`AHCI vers 0001.0301, 32 command slots, 6 Gbps, SATA mode`、`6/6 ports implemented (port mask 0x3f)`，`flags: 64bit clo only pio`；ata1 `SATA link up 6.0 Gbps (SStatus 133 SControl 300)`，ata2–ata6 `SATA link down (SStatus 0 SControl 300)`；`/proc/interrupts` 上 ahci 的 IRQ 计数大于 0；`--exit-stats` 有 `ahci.bar5` 的 MMIO 行。两个控制器各一块盘：IDENTIFY 出 `ATA-8: MINIVMM AHCI DISK, 1.0, max UDMA/133`、`131072 sectors, multi 0: LBA48`、`configured for UDMA/133`，`[sda]` 131072、`[sdb]` 65536 个扇区，写缓存开；host 用 `mke2fs -d` 做的镜像，guest `dd iflag=direct` 整盘读与只读挂载后读文件的 sha256 都与 host 一致，镜像运行后不变；guest 在空盘上 `mkfs.ext2`、写 8 MiB 随机文件、卸载，host 用 `debugfs` 取出的文件 sha256 与 guest 报的一致，`e2fsck -fn` 无错；后端统计 d1 有写有 flush、两盘 0 错误；NCQ：`flags` 行有 `ncq`，IDENTIFY 行 `LBA48 NCQ (depth 32)`，`queue_depth` 为 32，`--trace-mmio` 里两个控制器端口 0 都有 PxSACT 写；`libata.force=noncq` 时 `NCQ (not used)`，整盘读与 2 MiB 直写的内容由 host 核对，没有 PxSACT 写；host 在 guest 运行中把镜像截短一半，guest 读后半段时 VMM 报 `Input/output error`，Linux 报 `failed command: READ FPDMA QUEUED`、`error: { UNC }` 并 `EH complete`，没有日志 10h 读取失败、校验和错误或 tag 不符的警告，之后读前半段仍正常；`root=/dev/sda rdinit=/nonexistent` 时内核从这块盘挂根（`VFS: Mounted root (ext2 filesystem) on device 8:0.`），shell 读到盘上的标记文件 | **PASS** — `emu-blk` 后 16 条；没有 ahci 设备的二进制报 unknown driver，这 16 条全部变红；只有 HBA 外观、所有 ATA 命令都中止的第 2 步二进制上，读写与启动的用例变红；没有 NCQ 的第 3 步二进制上，NCQ 相关的 6 条变红、其余 10 条通过 | `make test emu-blk` |
| E26 | MSI（`pci_bus_test`、`ahci_test`）：capability 在 0x40、是链表上唯一一项，控制字只有 Enable 与 MME 可写、报 64 位、MMC=0，地址 bit 1:0 读 0，数据只有 16 位，设备声明 MSI 时 Bus Master 可写；MSI Enable 打开时撤下 INTx 线上的请求、关掉时挂着的请求回到线上，status.INT 照旧跟随设备，字节写也能清 Enable；发消息时高低地址拼成 64 位、数据取 16 位，Bus Master 关着不发；地址不在 0xFEE 窗口（含高 32 位非 0）、投递模式 011/110、Fixed/Lowest 的向量不在 0x10-0xFE 都报原因不投递，NMI 不看向量，地址保留位、RH/DM、电平触发原样交给 KVM，KVM 没收下时报它给的原因；丢弃日志对新的地址/数据立刻记，同一对按第 1、2、4、8… 次记，中间成功一次后重新计数。AHCI 什么时候发消息按第 10.7.2.1 节图 22：IS 有新位置上发一条，同一端口再置已置的位不发，软件清掉一部分还剩别的位发一条，清掉了但虚拟线仍为 1、位立刻重新置上发一条，写 IS 不清任何位不发，全清不发，GHC.IE=0 时不发、0→1 时有挂着的位补发一条、重写 1 不发，MSI 关着时只走 INTx | **PASS** — `pci_bus_test` 257 checks / 19 cases，`ahci_test` 300 checks / 25 cases；十八种错误实现（`tests/mutants/pci-msi.py`）各自变红（AHCI 用例每一步单独比较消息数的增量，一处错不会带红后面的行）：AHCI 只看有没有新置位（软件部分清除、GHC.IE 0→1 漏发）、按 INTx 电平的上升沿发（另一个端口置位、软件部分清除、清后重新置上漏发）、每次同步都发、GHC.IE=0 也发、重写 GHC.IE=1 也发、GHC.IE 0→1 不补发、写 IS 不清位也发、不申请 MSI capability 八种由 `ahci_test` 抓到；MSI 打开仍驱动 INTx、Bus Master 关着照发、改 Enable 不重算 INTx、不查地址高 32 位、不查 0xFEE 窗口、放行保留投递模式、放行 0x00-0x0F 向量、每次丢弃都记日志、只记第一次丢弃、Enable 不可写十种由 `pci_bus_test` 抓到 | `make test unit` → `pci_bus_test`、`ahci_test`；`make test negctl` → 源码层 mutant 18/18 |
| E27 | 真实 Linux ahci 驱动用上 MSI：VMM 报 `pci 00:01.0 ahci [1234:0003] class 010601, MSI capable`，驱动打开时记下 `MSI enabled, address 0xfee….. data 0x00..`，`/proc/interrupts` 上 ahci 是 `PCI-MSI-0000:00:01.0 0-edge`，256 次 4 KiB 直读中断计数增量不少于 256、读到的内容与镜像一致；`msi_bus` 写 0 后解绑再绑定驱动，VMM 报 `MSI disabled, interrupts go to INTA`，ahci 回到 `IO-APIC 17-fasteoi`，读盘照常；`msi_bus` 写 1 再绑定后回到 PCI-MSI，读盘照常；全程没有 `not delivered`、正常关机。guest 经 sysfs 的 config 文件把向量写成 0x05、把地址最高字节写成 0、把目标 ID 写成 0，各启动一次：发起一次读后 VMM 报出带 BDF、地址、数据和原因（异常向量、不在中断窗口、KVM 没收下）的 `not delivered` 行 | **PASS** — `pci-msi` 7/7；没有 MSI 的上一版二进制（883bd1b）上 6 条变红，只剩“两次重新绑定后正常关机”这条通过（它走 INTx，本来就能工作）。emu-blk 的第一次启动加了 `pci=nomsi`，INTx 的用例继续守住 INTx 路径。按 INTx 电平上升沿发 MSI 的错误实现在真实 guest 上不会丢中断（单核、4 核 4 路并发直读都通过）：命令在发起它的 vCPU 上同步完成，而 Linux 发命令和整段中断处理持着同一把 `host->lock`，完成落不进“清 PxIS 之后、写 IS 之前”的窗口，所以这一类只由 `ahci_test` 挡住 | `make test pci-msi` |
| E28 | gdb 远程协议与 x86 部分（`gdb_test`，判据是 GDB 手册附录 E、gdb 的 64bit-core/sse/segments.xml、`asm/debugreg.h` 与 KVM `DR7_FIXED_1` 的 DR6/DR7 位、`linux/kvm.h`）：包按线上字节算校验和，错的回 '-' 不执行，超长包回 '-'，gdb 回 '-' 时重发上一个包，去转义的结果决定执行哪条命令；stub 发出的 `# $ } *` 都转义且校验和含转义字节；vCPU 在跑时收到的包只 ack 不执行，0x03 只在跑时打断；停止应答的线程号是 vCPU 下标 + 1，swbreak/hwbreak 只在 gdb 声明支持时带上，停下的线程成为 g/m 的当前线程，Hg0 不改选择；g 包 552 字节、rbp 在 rsp 前、拿不到的 x87 字段是 `xx`，P 只接受 gpr/rip/eflags；m 可以短读、一个字节都读不到才报 E0e，M 的数据长度必须和 LENGTH 一致；target.xml 59 个寄存器，带着最后一段数据的应答就是 'l'；vCont 每个线程取最左边匹配的动作；软件断点读回原字节、写入时保住 int3 并更新原字节、删除表里任意一项不影响其余项、重复插入不重复记录；硬件断点 4 个槽、同地址复用槽、DR7 的 Ln 在 bit 2n；只有软件断点时也带 ENABLE，单步带上 BLOCKIRQ；不在断点表里的 #BP、DR6 里不是我们槽位的 Bn、单步时没有 BS 的 #DB 都还给 guest | **PASS** — `gdb_test` 188 checks / 12 cases；三十二种错误实现（`tests/mutants/gdbstub.py`）各自变红：stub 不转义 '*'、校验和漏掉转义字节、收包不异或 0x20、收包校验和漏掉转义后的字节、放行错的校验和、放行超长包、'-' 不重发、跑着时执行包、停着时响应 Ctrl-C、不看 gdb 是否支持就发 swbreak、线程号从 0 起、停止后不切换当前线程、拿不到的寄存器填 0、允许写段寄存器、rbp/rsp 对调、M 忽略多余数据、短读报错、qXfer 永远回 'm'、vCont 最右边的动作生效、Hg0 改了选择、读不隐藏 int3、写覆盖掉 int3、写不更新原字节、删除时丢掉最后一项、重复插入软件断点、DR7 使能位错位、同地址硬件断点占两个槽、只设 USE_SW_BP 不带 ENABLE、单步丢掉 BLOCKIRQ、吞掉 guest 自己的 int3、单步时不看 BS、DR6 里没用的槽也当断点 | `make test unit` → `gdb_test`；`make test negctl` → 源码层 mutant 32/32 |
| E29 | 没改过的 gdb 经 TCP 调试 2 vCPU 的 Linux guest：`--gdb 70000` 报 `--gdb must be a TCP port in 0..65535: 70000`；连上后 2 个线程，`hbreak start_kernel` 停在 vCPU 0，`stepi` 后 pc 前进的正好是 gdb 反汇编给出的那条指令的长度（先确认它不是跳转），改写 rax 再还原（刷新 gdb 的寄存器缓存后读回），读写 `linux_banner` 再还原，读没有映射的地址报 MemoryError，`break kernel_init` 停在 kernel_init，读另一个线程的寄存器；guest 在 shell 提示符处运行时 Ctrl-C，之前 host 上两个 vCPU 线程的 syscall 是 `ioctl`（或正在运行），之后都是 `futex`；detach 后 guest 继续跑，经串口执行 `poweroff -f` 正常关机，VMM 报 `gdbstub: passed 1 guest #BP and 0 guest #DB back to the guest`，全程没有 kernel BUG；`rdinit=/nonexistent` 让 guest panic 后重启，gdb 收到退出事件、退出码 0 | **PASS** — `gdbstub` 3/3。把不在断点表里的 #BP 报给 gdb 而不交还的 VMM 上：guest 在 `int3_selftest_ip` 处 kernel BUG 后 panic，`break kernel_init` 停在错误的 pc，2 vCPU 用例变红；暂停时不发 SIGUSR1 只置 immediate_exit 的 VMM 上：第一次停止（`hbreak start_kernel`）就永远等不齐，因为 vCPU 1 还是等 SIPI 的 AP，阻塞在 KVM_RUN 里不会自己出来，2 vCPU 用例超时变红 | `make test gdbstub` |
| E30 | 整机复位（`system-reset`，判据是 guest 自己的启动输出、guest 读到的设备寄存器和磁盘内容、VMM 日志）：`--no-reboot` 在非 linux 模式下报错；`--smp 4` 带 AHCI 和 pci-test 的 guest 连续 3 次 `reboot -f`，每次启动打印的 CPU 列表、PCI 设备、磁盘探测汇成的一行哈希都相同，复位前写进盘的 `MARK42` 每次都读得回来，VMM 报 `guest reset #3 done`；4 个 vCPU 都在跑忙循环时 sysrq-b，新内核 4 个 CPU 全部上线；guest 改过的 pci-test scratch 寄存器和配置空间 cache line size 复位后回到 0；bzImage 重启后再次进入 shell；`--no-reboot` 下 `reboot -f` 和 panic 都让 VMM 退出一次而不循环；gdb 用 `hbreak start_kernel` + `break rest_init` 时 guest panic 重启，连接不断，复位后停在 `rest_init` 且 `panic_cpu` 为 -1（新内核），即软件断点在重新装载的内核里重新写好了 | **PASS** — `system-reset` 7/7，`gdbstub` 4/4。对照：不恢复 vCPU 状态、只恢复 BSP 时 sysrq 用例变红（AP 带着旧内核的寄存器在新装载的内存上执行，triple fault，VMM 报 `vCPU N: SHUTDOWN` 后按 guest 错误停机，`vCPU N stopped with error -5`）；不恢复 irqchip 时 sysrq 用例的第二次启动停在 `Serial: 8250/16550 driver` 之后，120 秒超时（卡在 8250 驱动的哪一步没有追查）；不复位设备时 pci-test 用例读回旧值变红。只用 `reboot -f` 的用例挡不住这几类：`reboot -f` 先让 AP 停机，新内核再给它们发 INIT，KVM 的 INIT 处理本身就把 AP 复位了。这几种错误实现，加上复位后不重新写 int3（gdbstub suite 的跨复位用例变红），都在 `tests/mutants/system-reset.py` 里，`tests/mutate.py system-reset` 重放 | `make test system-reset gdbstub`；`tests/mutate.py system-reset` |
| E31 | 完整发行版（`arch-distro`，判据是 systemd、pacman、guest 内核和 host 上 e2fsck 各自的结论）：`tools/build_arch_image.sh` 生成的 Arch 镜像，官方 linux 包的 vmlinuz 和 initramfs 直启，`--smp 4 --mem 2G`，AHCI 上的根盘加 e1000；systemd 报 `running`，两次启动 `systemctl --failed` 都为空，dmesg 没有超时，err/warn 只有 `DMESG_ALLOWED` 里逐条查明与 VMM 无关的几条；DHCP 拿到 10.0.2.15，`pacman -Syu` 从 host 上的本地仓库装 tree，签名按 archlinux-keyring 校验；`systemctl reboot` 原地复位，boot_id 变了、再次 `running`、tree 还在，initrd 里的 systemd-fsck 报 clean；`systemctl poweroff` 经 ACPI S5 退出码 0，host 上 `e2fsck -fn` 无错；两次关机各在 30 秒内完成。同一快照日期重新构建一次，`packages.txt` 逐字一致、vmlinuz 逐字节相同，新镜像同样 10/10。`ARCH_DISTRO_BASELINE=1` 时空载、fio、iperf3 三次启动差分：VMM 块后端统计的读请求比空载多出 fio 的 16384 次（容差 2%），tx、rx 帧数各多出至少 64 MiB / 1514。`ARCH_DISTRO_ONLINE=1` 时 guest 经 pasta 接到 host 网络：pasta 的 DHCP 经 bridge 到达 e1000，`pacman -Syu tree` 从镜像自带的软件源下载安装、签名通过，poweroff 后 VMM 退出码 0 | **PASS** — `arch-distro` 10/10，基线 3/3，联网 2/2。基线数字（两次运行一致到小数点后一位）：4 KiB O_DIRECT 随机读每次 I/O 约 7.6 次 `ahci.bar5` MMIO exit、17 次 KVM exit、2.5 次中断注入；iperf3 双向每帧约 0.08 次 `e1000.bar0` exit、0.17 次 KVM exit。对照：复位请求改成退出（`tests/mutants/arch-distro.py` 的 `reboot-exits`）时 5 个用例 FAIL。复位时不调设备的复位回调挡不住，因为 Linux 的 ahci、e1000 驱动 probe 时自己复位硬件，这类错误由 `system-reset` 挡。联网用例暴露了 VMM 依赖继承来的信号掩码：pasta 启动子进程时阻塞了 SIGUSR1，VMM 只装处理函数不解除阻塞，停机时踢 vCPU 的信号一直 pending，HLT 中的 AP 不醒，VMM 挂住；`smp` suite 的“父进程阻塞 SIGUSR1”用例在 `vm_run` 解除阻塞之前 FAIL、之后 PASS | `make test arch-distro smp`；`ARCH_DISTRO_BASELINE=1 tests/e2e/arch-distro.sh -a`；`ARCH_DISTRO_ONLINE=1 tests/e2e/arch-distro.sh -a`；`TMPDIR=$PWD/build tests/mutate.py arch-distro` |
| E32 | 运行时控制接口（`monitor_test` 和 `monitor` suite，判据是 `include/monitor.h` 写明的协议、host 的 `/proc`、vmlinux 的符号表、guest 自己的回显）：协议层面，正文行以两个空格开头、最后一行 `OK` 或 `ERR <原因>`，正文里像结束标记的行也被缩进；一行命令拆成多段、一段多行、CRLF 结果相同，空行不回复；未知命令、未知 info 子命令、参数多或少、超过 16 个词、含 NUL 各回一行 ERR，之后的命令照常执行；1024 字节的行照常解析，1025 字节的整行丢弃，分段到达时只在换行处回一次 ERR；help 和 info 列出注册表里的命令；异步命令完成前同一连接的后续命令不执行，完成后按请求顺序回复，连接先断开时 finish 仍被调用且标记为孤儿；回复积压超过 1 MiB 置 overflow。真实 VMM 上，路径是普通文件时拒绝启动且文件保留，路径超过 107 字节报错，上次留下的 socket 文件被替换，退出时删除；2 vCPU 的 BusyBox guest 停在提示符时 `info status` 报 `running`，`info cpus` 每个 thread_id 在 `/proc/<pid>/task/<tid>/comm` 里是对应的 `vcpuN`，空闲 vCPU 的 host 状态为 S；`cpu 1` 之后 `info registers` 输出 vCPU 1；`info registers -a` 两个 vCPU 的 rip 都在 `_stext.._etext` 里，CR0.PG 与 EFER.LMA 置位，连读 200 次全部 OK；guest 打印一行前后 `info exit-stats` 的总数增长；guest 把空转进程绑到 CPU 1 后 vCPU 1 的 host 状态为 R，此时连读 100 次 `info registers -a` 全部 OK；5000 字节的行、半行断开、第 5 个连接（回 `ERR too many monitor clients (max 4)`）之后 guest 照常回显；`quit` 后连接被关闭、VMM 退出码 0、不打印 vCPU 现场、socket 文件删除 | **PASS** — `monitor_test` 53 checks / 10 cases，`monitor` 5/5。对照：13 个源码 mutant 全部变红（`tests/mutate.py monitor`），其中协议的 7 个由 `monitor_test` 挡住；vCPU 被踢出后不执行工作、不清 immediate_exit、thread_id 写成 pid、host 状态写死、`quit` 不停机各让一条 e2e 用例变红；查询时才在 I/O 线程上调 `KVM_GET_STATS_FD` 时客户端 10 秒等不到 `info exit-stats` 的回复，之后 guest 的提示符也不再出现，即 I/O 线程卡在要 `vcpu->mutex` 的 ioctl 上，串口输出随之停住 | `make test unit monitor`；`tests/mutate.py monitor` |
| E33 | 用户态中断控制器（`ioapic_test`、`pic_test`、`pit_test` 和 `split-irqchip` suite，判据是 82093AA、8259A、82C54、PIIX4 数据手册，KVM 6.6 的 `kvm_scan_ioapic_routes` 与 `KVM_INTERRUPT` 语义，guest 的 `/proc/interrupts`、`/proc/timer_list`、dmesg）：三个芯片各自不依赖 KVM 单测：IOAPIC 的寄存器窗口、只读位、边沿与电平投递、remote IRR 只由 EOI 清、EOI 时仍有效就重投、RTE 改成边沿清 remote IRR、RTE 到 MSI 的换算；8259A 的 ICW 序列、IRR 由线电平与 armed 位决定、优先级与特殊屏蔽、级联、AEOI、轮转、OCW3、伪中断、ELCR；8254 用假时钟检查六种模式的 OUT 波形与上升沿计数、写入下一拍装载、GATE、锁存与 read-back、BCD、运行中改写的生效时刻，0x61 的 GATE2/OUT2/刷新位。`IRQCHIP=split` 时全部 e2e suite（含 arch-distro）以 split 模式跑通。suite 本身：`--kernel-irqchip off`/非法值/payload 模式的报错；两种模式各一次 `pci=nomsi` 启动，AHCI INTx 走 IO-APIC 17-fasteoi、无超时，split 下 EOI exit 与 pin 17 投递数不少于 AHCI 中断数，exit 统计里有 IOAPIC_EOI 与用户态 PIT/PIC/IOAPIC 访问而内核模式没有，guest 里重放 PIT 校准（pitprobe）得到的 TSC 频率与 kvmclock 相差 <1%；`noapic` 下 timer 和 ttyS0 走 XT-PIC、split 下注入过 ExtINT；`no-kvmclock` 下 TSC 校准落在三种合法结局之一；`noapictimer nohz=off highres=off` 时 SIGSTOP VMM 一秒，补发 on 的 |漂移| ≤ 50 ms、off 的漂移 ≥ 500 ms、IRQ0 增量等于 jiffies 增量，split 下 on 不合并、积压清空，off 合并 ≥ 500 个 | **PASS** — `ioapic_test` 115 checks / 17 cases，`pic_test` 94 / 24，`pit_test` 145 / 18；`split-irqchip` 16/16。对照：53 个源码 mutant 全部变红（`tests/mutate.py split-irqchip`），芯片模型的 45 个由单测挡住；主板接线的 8 个各让一条 e2e 用例变红：RTE 改了不同步路由、忽略 EOI exit、IRQ0 脉冲以低电平结束（只有 `noapic` 用例能发现）、pin 0 路由不标 level、补发丢积压、策略被忽略、解除屏蔽不清积压、内核 PIT 不下发 `KVM_REINJECT_CONTROL`。记录而不判定的数字（blog 34、37）：默认启动 split 比内核 irqchip 多约 1600 次用户态 exit（IOAPIC MMIO 1212、EOI 281、PIC/PIT 端口约 120），VM exit 总数几乎不变；`no-kvmclock` 连续 10 次启动，内核 irqchip 3 次 `Using PIT calibration value`、7 次 `Unable`，split 10 次全部 `Using PIT`；`noapic` 下补发 on 也丢掉停顿的一秒（Linux 每个 PIC 中断都解除一次屏蔽，触发清积压）。内核 irqchip 加 `noapic noapictimer` 约五分之一的启动卡住，原因在 host KVM 的 in-kernel PIC（见剩余风险），这个组合不在本 suite 的用例里 | `make test unit split-irqchip`；`IRQCHIP=split make test`；`tests/mutate.py split-irqchip` |
| E34 | ACPI 固定电源按钮与 SCI（`acpi_pm_test`、`boot_tables_test` 和 `acpi-events` suite，判据是 ACPI 6.5 的 4.8.2.2.1、4.8.3.1、Table 4.11-4.13、5.2.9-5.2.10、5.2.12.5，guest 的 `/proc/interrupts`、`/sys/firmware/acpi/interrupts/ff_pwr_btn`、evdev 事件和 systemd-logind 日志）：寄存器层面，STS 只由硬件置位、写 1 清，SCI 是 `STS & EN` 的电平且只在变化时通知 irqchip，先按后使能也拉起 SCI，关使能或清状态都落下，按住期间再按不产生新边沿；未实现位读 0、写无效，GBL_EN 能写进去且不发 SCI；STS、EN、CNT 的高低字节可以单独读写；PM1_CNT 的 SCI_EN 恒 1、GBL_RLS 和 SLP_EN 只写、15:14 读 0；S5 经 PM1_CNT 字写、高字节单写和 SLEEP_CONTROL 三种写法都识别，其他 SLP_TYP 报告忽略；复位清寄存器并落下 SCI。表层面，FADT `PWR_BUTTON` 清 0，FACS 64 字节对齐、落在表区、FIRMWARE_CTRL 与 X_FIRMWARE_CTRL 一致、版本 3、hardware signature 等于前面全部表字节的 FNV-1a，MP table 的 IRQ 9 是 level/active low。guest 层面，两种 irqchip 下各一次：只有一个 `LNXPWRBN`，IRQ 9 是 `9-fasteoi acpi`，没有全局锁报错，monitor 两次按键各得到一对 `KEY_POWER` 1/0，SCI 计数和 `ff_pwr_btn` 每次正好加 1；`acpi=off` 时只锁存不发 SCI；Arch 的 logind 收到按键后正常关机经 S5 退出 | **PASS** — `acpi_pm_test` 54 checks / 11 cases，`boot_tables_test` 998 checks；`acpi-events` 4/4（Arch 7.2.11 内核从按键到 `reboot: Power down` 约 1.8 s）。对照：16 个源码 mutant 全部变红（`tests/mutate.py acpi-events`），寄存器与表的 12 个由单测挡住；接线的 4 个（SCI 接错 GSI、W1C 后 SCI 不落、monitor 绕过寄存器模型不拉 SCI、S5 被忽略）各让 e2e 变红。记录而不判定：guest 启动时 ACPICA 为 RTC 闹钟写 RTC_EN 读回 0，打印一次 `Could not enable RealTimeClock event`，pci-bus 的 ACPI Error 计数和 arch-distro 的 dmesg 清单都按预期排除这一条 | `make test unit acpi-events pci-bus`；`tests/mutate.py acpi-events` |
| E35 | pvpanic（`boot_tables_test`、`aml_test` 和 `pvpanic` suite，判据是 QEMU `docs/specs/pvpanic.rst`、Linux `include/uapi/misc/pvpanic.h`、Linux 7.x 的 `drivers/misc/pvpanic/`、`kernel/panic.c`、`include/linux/reboot.h`，ACPI 6.5 的 6.1.5、6.3.7、6.4.2.5、20.2.3）：表层面，挂设备时 DSDT 里有逐字节写出的 `Name(_HID, "QEMU0001")` 和 `IO(Decode16, 0x505, 0x505, 1, 1)`，不挂时不出现这个 ID，iasl 往返一致；`aml_string` 以 0x0D 开头、NUL 结尾。guest 层面，BusyBox guest 的内建驱动绑定 `QEMU0001:00`、capability/events 为 7，`/dev/port` 读出 0x07，未知位被忽略、只剩 `CRASH_LOADED` 时 guest 继续，sysrq-c 退出码 3 且没有复位请求；两个对照组（不挂设备、guest 把 events 清零）同一个 panic 走复位、退出码 0；`SHUTDOWN` 退出码 0，`PANICKED|SHUTDOWN` 退出码 3；重复的设备启动前被拒；Arch 7.x 的驱动模块自动加载、capability 7，poweroff 经 S5、不发 SHUTDOWN，sysrq-c 退出码 3 | **PASS** — `boot_tables_test` 1005 checks，`aml_test` 50 checks / 5 cases；`pvpanic` 8/8。对照：10 个源码 mutant 全部变红（`tests/mutate.py pvpanic`），DSDT 与 AML 的 4 个由单测挡住，设备与接线的 6 个各让 e2e 里对应的那一条用例变红。记录而不判定：一次启动加 panic，pvpanic 端口上只有 1 次读（驱动 probe）和 1 次写（panic 通知） | `make test unit tables pvpanic`；`tests/mutate.py pvpanic` |
| E12 | 三处修复与分发接口提取之后，真实 Linux 仍能正常启动关机 | **PASS** — `kvm-basics` 7/7、`linux-boot` 8/8、`legacy-devices` 3/3（vmlinux 直启、bzImage 解压、initramfs shell、ACPI S5 关机、i8042 reset、四个设备探测无超时、PIO trace 结构） | `make test kvm-basics linux-boot legacy-devices` |

### 复现命令

```sh
make test unit                     # E1 E2 E3 E7 E13 E14 E17 E18 E20 E22 E24 E26 E28 E32 E33 E34 E35
make test emu-net                  # E19 E21
make test emu-blk                  # E23 E25
make test pci-msi                  # E27
make test gdbstub                  # E29 E30（gdb 跨复位）
make test system-reset             # E30
make test monitor                  # E32
tests/mutate.py monitor            # E32 对照
make test split-irqchip            # E33
IRQCHIP=split make test            # E33（现有 e2e 跑在 split 模式上）
tests/mutate.py split-irqchip      # E33 对照
make test acpi-events              # E34（Arch 用例需要 tools/build_arch_image.sh 生成的镜像）
tests/mutate.py acpi-events        # E34 对照
make test pvpanic                  # E35（Arch 用例需要 tools/build_arch_image.sh 生成的镜像）
tests/mutate.py pvpanic            # E35 对照
make test arch-distro              # E31（需要 tools/build_arch_image.sh 生成的镜像）
ARCH_DISTRO_BASELINE=1 tests/e2e/arch-distro.sh -a   # E31 基线
ARCH_DISTRO_ONLINE=1 tests/e2e/arch-distro.sh -a     # E31 真实外网（需要 pasta）
TMPDIR=$PWD/build tests/mutate.py arch-distro       # E31 对照
make test negctl                   # E10 E26 E28 E32 E33 E34 E35（源码层 mutant）
make test tables                   # E8
FUZZ_TIME=300 make test fuzz       # E4 E5 E6
./build/fuzz/elf_loader_fuzz         build/fuzz/artifacts/elf-crash-50a488df453d94bf401917e701686c4e8ae6bed5
./build/fuzz/elf_loader_fuzz_noalign build/fuzz/artifacts/elf-noalign-crash-8d3e279f900d453ddeffdcb924b3c391a947c0e7
```

E5 的最小复现输入是一个 177 字节的 ELF：`e_phoff = 0xFFFFFFFFFFFFFFC0`、
`e_phnum = 2`、`e_phentsize = 56`。`0xFFFFFFFFFFFFFFC0 + 2*56` 在 64 位下回绕成
`0x30`，`0x30 > 177` 为假，于是 `check_ehdr()` 的截断检查放行；随后
`ph = img + e_phoff` 把指针推到 mmap 区之前 64 字节。对照：`src/mem.c`
的 `gpa_to_hva()` 对同一类运算特意写了 `if (gpa + len < gpa) return NULL;`
并注释了原因，`elf_loader.c` 的两处（第 37 行、第 87 行）没有。
第 87 行的 `p_offset + p_filesz > st.st_size` 是同一模式，本轮未单独触发
（fuzzer 在第一处就停了），属于**有根据的怀疑，尚未证实**。

---

## 没有验证的内容和剩余风险

- **SCI 是电平还是脉冲，e2e 分不出来。** guest 处理 SCI 时先清状态位、再调处理函数，按键之间隔着 shell 命令，SCI 拉起时 IOAPIC 引脚从没被屏蔽、也没有 remote IRR 挂着。把 SCI 改成“拉起后马上落下”的脉冲，这两次按键照样各进一次中断。区别只在第二次按键落进第一次中断处理窗口（或者引脚被屏蔽）时才出现：电平会在 EOI 后重投，脉冲会丢。寄存器层面的电平语义由 `acpi_pm_test` 断言；要在 guest 里看到差别，需要能在中断处理期间按键的手段（例如 gdbstub 停在 `acpi_ev_fixed_event_dispatch` 上，再从 monitor 按一次）。
- **PM1 的复位只有单测覆盖。** 按键后不让 guest 处理就整机复位，SCI 应当落下、状态位清零。Linux 启动时自己会清全部固定事件状态位，复位回调漏注册在 Linux guest 上看不出来，所以没有 e2e 用例。
- **pvpanic 的 `CRASH_LOADED` 和 `SHUTDOWN` 只由 guest 直接写端口送出。** 真实情形下，`CRASH_LOADED` 要 guest 装好 kdump 内核并带 `crash_kexec_post_notifiers`（否则 `panic()` 先跳进 kdump 内核，notifier 不会运行）；`SHUTDOWN` 只在 ACPI 关机没生效时才发，而 Linux 靠 ACPI 发现 pvpanic，`acpi=off` 时驱动也不绑定，在这台机器上造不出这种情形。VMM 对端口值的处理与它从哪来无关，端口写已经覆盖；没有验证的是 guest 在这两种情形下确实写出了对应的值。
- **monitor 在裸 payload 模式下的 `system_powerdown` 报错没有 e2e 用例。** 裸 payload 跑完就退出，等不到 monitor 发命令，要有一个不退出的 payload 才能测。
- **negative control 只覆盖清单里列出的错误。** `make test negctl` 证明的是“这套断言能区分正确实现与 `unit/*_mutants.c`、`mutants/*.py` 里的这些错误实现”，不证明“没有别的错误类型能溜过去”。它已经抓到过一次自己的漏洞（`len-truncated-32` 最初逃掉了，因为长度表里没有低 32 位小、高 32 位非零的值），这说明清单本身也需要随契约增长。
- **`kvm_run` 字段提取没有单元测试。** 见 O2。`io.data_offset` 那一条只有 e2e 覆盖。
- **fuzz 只跑了 5 分钟/target、单 worker。** 覆盖率见日志里的 `cov:`。修复前 fuzzer 在首次发现处就停（`-fno-sanitize-recover=all`），所以“修好了这两个”不等于“后面没有别的”——修复后的重跑结果见执行结果摘要。
- **`gpa_to_hva` 的模型对照有一个已知盲区**：结束地址正好等于 2^64 的 slot。实现里 `s->gpa + s->size` 会回绕成 0，该 slot 完全不可用。测试布局刻意避开，因为现实中不会出现；将来 MMIO hole 的划分变复杂时要重新评估。
- **`bzimage_fuzz_noalign` 编出来了但没跑过。** 理由：`bzimage_load()` 把 setup header `memcpy` 进对齐的局部变量再访问，严格版跑满 300 秒也没被 alignment 挡住。这是判断，不是证据。
- **P0 只做了一半。** 修复之后 `make test` 默认集合完整跑过一次并全绿（7 个 suite / 48 条用例 / 98 秒），所以“改完还能启动 Linux”是有证据的。但 P0 原本要的是**各跑 3 遍以分清 flaky**，那部分没做。已知的一个 flaky 来源是 guest 命令按固定延迟发送，已由 P4c 消除；其余用例的重复稳定性没有系统统计。
- **整机复位里有三项恢复没有测试能挡住**：MSR 不恢复、复位前不把 GSI 线拉低、串口复位后不重算中断线，三种错误实现都让 `system-reset` 照样通过。原因各不相同：kvmclock、async PF、steal time 这些 PV MSR 由新内核在同样的地址重新登记，残留的登记只在新内核登记之前那一小段时间里起作用；KVM 的 `irq_states` 按中断源分别记、取“或”，每根 GSI 上目前只有 VMM 这一个用户态中断源，设备下一次 `KVM_IRQ_LINE` 就改写了残留的位，边沿检测看的 `irr` 又随 `KVM_SET_IRQCHIP` 恢复，所以残留在现有拓扑下本来就观察不到；串口复位后的中断线由新内核初始化串口时重新算出。这三处的必要性来自 `include/reset.h` 里对 KVM 行为的推导（`kvm_vcpu_reset` 只在 INIT 路径复位 AP 的部分 MSR，BSP 收不到 INIT；同一 GSI 有第二个中断源后残留的高位会让线一直为高），不是测试证据。
- **arch-distro 的关机用例绕开了一个 guest 侧竞态，不覆盖“登录 shell 活到关机”这条路径。** bash 5.3 / readline 8.3 在 `rl_getc()` 里会丢掉落在“循环开头转交信号”和“进入 pselect”之间的 SIGHUP，串口上登录的 bash 不退出，systemd 等满 90 秒停止超时，关机超过 `SHUTDOWN_MAX`，几十次关机撞上一次。这是 guest 用户态的问题：卡住时 vCPU 全部空闲、ttyS0 没有新中断，signal tracepoint 显示 SIGHUP 已经送达、bash 的处理函数也跑了（机制见 `tests/e2e/arch-distro.sh` 里 `REBOOT_CMD` 的注释）。suite 用 `exec systemctl reboot/poweroff` 让 shell 在关机前退出。readline 开发分支已修，guest 镜像换到带修复的版本后去掉 `exec`，循环重启上百次看关机耗时来重新判断。
- **arch-distro 联网用例的一次失败原因未查明。** 有一次 pacman 那条用例没有 `PACMAN-RC-0` 和 `INSTALLED`，而 guest 正常关机、没有签名错误，与 pacman 返回非 0（例如下载失败）相符，但那次的完整输出没有保留，之后单独和整套连跑二十次都通过。用例失败时 `end_case` 会打印完整串口输出，再出现时从 pacman 的报错行查起。
- **arch-distro 的 dmesg 允许清单随 host 变化。** 清单里的 TSC P0、SRSO 两条取决于 host 内核和 CPU 微码，换一台 host 可能消失（允许而不要求出现，不会因此变红），也可能出现清单之外的新警告而变红，那时要逐条查明是不是 VMM 的问题再决定加不加进清单。
- **vCPU 工作队列有两处顺序要求只靠推导，没有测试能挡住。** 一处是 vCPU 被踢出后先清 `immediate_exit` 再看队列（反过来时，工作恰好在“看完队列”和“清零”之间排进来，这次踢的效果被清掉，空闲 vCPU 睡回 HLT，工作要等下一个中断）；另一处是 vCPU 线程退出时先关队列再执行剩下的工作（否则一个与停机同时到达的查询永远没有回复）。两者都要求一个请求恰好落进几条指令宽的窗口，e2e 无法稳定制造，理由写在 `src/smp.c` 的 `vcpu_kick()` 和 `vcpu_thread()` 旁。`quit` 恰好落在整机复位过程中的处理（`main.c` 复位循环前后都检查 `quit_requested`）同理没有测试。
- **exit 计数在运行中读取时不是同一时刻的快照。** 各格、各 vCPU 分别是 relaxed 原子读，`info exit-stats` 的分项之和可能和同一份报告里的 total 差几次 exit；测试只断言总数随时间增长，不断言运行中的分项和总数相等（停机后生成的 `--exit-stats` 报告仍由 `exit-stats` suite 逐项核对）。
- **split 模式加 `--pit-reinject on` 在开了 AVIC 的 AMD host 上没有验证。** IOAPIC 路径的 tick 确认靠 pin 0 向量的 EOI exit（路由里标成 level），AVIC 对 TMR=0 的中断由硬件完成 EOI、不退出；KVM 自己的 PIT 在补发打开时禁用 APICv（`APICV_INHIBIT_REASON_PIT_REINJ`），但它不知道用户态在补发。开发机 `kvm_amd avic=N`，这个组合跑不到；症状会是第一个 tick 之后积压只增不减，`info irqchip` 的 `acks` 停住。Intel APICv 的 EOI-exit bitmap 由 `ioapic_handled_vectors` 装载，不受影响。
- **“host 过载”用 SIGSTOP 一秒代替。** 里程碑的验收写的是 host 过载下比较补发策略；stress-ng 抢 CPU 欠下的 tick 数取决于核数和调度器，判据没法写成确定的数，所以 suite 停 VMM 一秒造出确定的 1000 个欠 tick。真实过载的步骤在 `tests/run.sh --manual split-irqchip`，只作人工观察。
- **内核 irqchip 加 `noapic noapictimer` 偶发卡死，原因在 host KVM。** KVM 的 in-kernel PIC 只在输出上升沿通知当时 LVT0 能接收 ExtINT 的 vCPU，而 LVT0 解除屏蔽、APIC 硬件关闭这两处都不重新检查 PIC 输出。Linux 的 `setup_local_APIC()` 先经 SPIV 软关闭 APIC（KVM 随之屏蔽所有 LVT），一串寄存器写完之后才把 LVT0 写成未屏蔽的 ExtINT。PIT tick 落进这段窗口时，通知被丢弃，PIC 输出一直保持 1，不会再出现上升沿，此后 tick 永远不再注入。现象是 guest 时间在 `Calibrating delay loop` 之后停住，串口输出停在驱动探测处，KVM `irq_injections` 停在个位数，`halt_exits` 每秒十几万。约五分之一的启动会卡住，取决于 tick 有没有落进窗口。split 模式和只加 `noapic` 时没有出现。最新上游 KVM（kvm-x86/next）同样存在，有一个 KVM selftest 能确定性复现，修复尚未进入上游。suite 不用这个组合，因为在未修复的 host 上它必然偶发失败；host 内核带上修复后，把它加进 `split-irqchip` 的内核模式用例，并连跑几十次确认不再卡住。
- **P1 完全没有触及设备状态机、并发、启动流程**，那些行为目前仍然只有整机启动这一条间接证据。
