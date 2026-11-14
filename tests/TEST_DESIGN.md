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
| E25 | 真实 Linux ahci 驱动按类代码绑定控制器：`AHCI 0001.0301 32 slots 6 ports 6 Gbps 0x3f impl SATA mode`，`flags: 64bit clo only pio`；ata1 `SATA link up 6.0 Gbps (SStatus 133 SControl 300)`，ata2–ata6 `SATA link down (SStatus 0 SControl 300)`；`/proc/interrupts` 上 ahci 的 IRQ 计数大于 0；`--exit-stats` 有 `ahci.bar5` 的 MMIO 行。两个控制器各一块盘：IDENTIFY 出 `ATA-8: MINIVMM AHCI DISK, 1.0, max UDMA/133`、`131072 sectors, multi 0: LBA48`、`configured for UDMA/133`，`[sda]` 131072、`[sdb]` 65536 个扇区，写缓存开；host 用 `mke2fs -d` 做的镜像，guest `dd iflag=direct` 整盘读与只读挂载后读文件的 sha256 都与 host 一致，镜像运行后不变；guest 在空盘上 `mkfs.ext2`、写 8 MiB 随机文件、卸载，host 用 `debugfs` 取出的文件 sha256 与 guest 报的一致，`e2fsck -fn` 无错；后端统计 d1 有写有 flush、两盘 0 错误；NCQ：`flags` 行有 `ncq`，IDENTIFY 行 `LBA48 NCQ (depth 32)`，`queue_depth` 为 32，`--trace-mmio` 里两个控制器端口 0 都有 PxSACT 写；`libata.force=noncq` 时 `NCQ (not used)`，整盘读与 2 MiB 直写的内容由 host 核对，没有 PxSACT 写；host 在 guest 运行中把镜像截短一半，guest 读后半段时 VMM 报 `Input/output error`，Linux 报 `failed command: READ FPDMA QUEUED`、`error: { UNC }` 并 `EH complete`，没有日志 10h 读取失败、校验和错误或 tag 不符的警告，之后读前半段仍正常；`root=/dev/sda rdinit=/nonexistent` 时内核从这块盘挂根（`VFS: Mounted root (ext2 filesystem) on device 8:0.`），shell 读到盘上的标记文件 | **PASS** — `emu-blk` 后 16 条；没有 ahci 设备的二进制报 unknown driver，这 16 条全部变红；只有 HBA 外观、所有 ATA 命令都中止的第 2 步二进制上，读写与启动的用例变红；没有 NCQ 的第 3 步二进制上，NCQ 相关的 6 条变红、其余 10 条通过 | `make test emu-blk` |
| E26 | MSI（`pci_bus_test`、`ahci_test`）：capability 在 0x40、是链表上唯一一项，控制字只有 Enable 与 MME 可写、报 64 位、MMC=0，地址 bit 1:0 读 0，数据只有 16 位，设备声明 MSI 时 Bus Master 可写；MSI Enable 打开时撤下 INTx 线上的请求、关掉时挂着的请求回到线上，status.INT 照旧跟随设备，字节写也能清 Enable；发消息时高低地址拼成 64 位、数据取 16 位，Bus Master 关着不发；地址不在 0xFEE 窗口（含高 32 位非 0）、投递模式 011/110、Fixed/Lowest 的向量不在 0x10-0xFE 都报原因不投递，NMI 不看向量，地址保留位、RH/DM、电平触发原样交给 KVM，KVM 没收下时报它给的原因；丢弃日志对新的地址/数据立刻记，同一对按第 1、2、4、8… 次记，中间成功一次后重新计数。AHCI 什么时候发消息按第 10.7.2.1 节图 22：IS 有新位置上发一条，同一端口再置已置的位不发，软件清掉一部分还剩别的位发一条，清掉了但虚拟线仍为 1、位立刻重新置上发一条，写 IS 不清任何位不发，全清不发，GHC.IE=0 时不发、0→1 时有挂着的位补发一条、重写 1 不发，MSI 关着时只走 INTx | **PASS** — `pci_bus_test` 257 checks / 19 cases，`ahci_test` 300 checks / 25 cases；十八种错误实现（`tests/mutants/pci-msi.py`）各自变红（AHCI 用例每一步单独比较消息数的增量，一处错不会带红后面的行）：AHCI 只看有没有新置位（软件部分清除、GHC.IE 0→1 漏发）、按 INTx 电平的上升沿发（另一个端口置位、软件部分清除、清后重新置上漏发）、每次同步都发、GHC.IE=0 也发、重写 GHC.IE=1 也发、GHC.IE 0→1 不补发、写 IS 不清位也发、不申请 MSI capability 八种由 `ahci_test` 抓到；MSI 打开仍驱动 INTx、Bus Master 关着照发、改 Enable 不重算 INTx、不查地址高 32 位、不查 0xFEE 窗口、放行保留投递模式、放行 0x00-0x0F 向量、每次丢弃都记日志、只记第一次丢弃、Enable 不可写十种由 `pci_bus_test` 抓到 | `make test unit` → `pci_bus_test`、`ahci_test`；`make test negctl` → 源码层 mutant 18/18 |
| E27 | 真实 Linux ahci 驱动用上 MSI：VMM 报 `pci 00:01.0 ahci [1234:0003] class 010601, MSI capable`，驱动打开时记下 `MSI enabled, address 0xfee….. data 0x00..`，`/proc/interrupts` 上 ahci 是 `PCI-MSI-0000:00:01.0 0-edge`，256 次 4 KiB 直读中断计数增量不少于 256、读到的内容与镜像一致；`msi_bus` 写 0 后解绑再绑定驱动，VMM 报 `MSI disabled, interrupts go to INTA`，ahci 回到 `IO-APIC 17-fasteoi`，读盘照常；`msi_bus` 写 1 再绑定后回到 PCI-MSI，读盘照常；全程没有 `not delivered`、正常关机。guest 经 sysfs 的 config 文件把向量写成 0x05、把地址最高字节写成 0、把目标 ID 写成 0，各启动一次：发起一次读后 VMM 报出带 BDF、地址、数据和原因（异常向量、不在中断窗口、KVM 没收下）的 `not delivered` 行 | **PASS** — `pci-msi` 7/7；没有 MSI 的上一版二进制（883bd1b）上 6 条变红，只剩“两次重新绑定后正常关机”这条通过（它走 INTx，本来就能工作）。emu-blk 的第一次启动加了 `pci=nomsi`，INTx 的用例继续守住 INTx 路径。按 INTx 电平上升沿发 MSI 的错误实现在真实 guest 上不会丢中断（单核、4 核 4 路并发直读都通过）：命令在发起它的 vCPU 上同步完成，而 Linux 发命令和整段中断处理持着同一把 `host->lock`，完成落不进“清 PxIS 之后、写 IS 之前”的窗口，所以这一类只由 `ahci_test` 挡住 | `make test pci-msi` |
| E28 | gdb 远程协议与 x86 部分（`gdb_test`，判据是 GDB 手册附录 E、gdb 的 64bit-core/sse/segments.xml、`asm/debugreg.h` 与 KVM `DR7_FIXED_1` 的 DR6/DR7 位、`linux/kvm.h`）：包按线上字节算校验和，错的回 '-' 不执行，超长包回 '-'，gdb 回 '-' 时重发上一个包，去转义的结果决定执行哪条命令；stub 发出的 `# $ } *` 都转义且校验和含转义字节；vCPU 在跑时收到的包只 ack 不执行，0x03 只在跑时打断；停止应答的线程号是 vCPU 下标 + 1，swbreak/hwbreak 只在 gdb 声明支持时带上，停下的线程成为 g/m 的当前线程，Hg0 不改选择；g 包 552 字节、rbp 在 rsp 前、拿不到的 x87 字段是 `xx`，P 只接受 gpr/rip/eflags；m 可以短读、一个字节都读不到才报 E0e，M 的数据长度必须和 LENGTH 一致；target.xml 59 个寄存器，带着最后一段数据的应答就是 'l'；vCont 每个线程取最左边匹配的动作；软件断点读回原字节、写入时保住 int3 并更新原字节、删除表里任意一项不影响其余项、重复插入不重复记录；硬件断点 4 个槽、同地址复用槽、DR7 的 Ln 在 bit 2n；只有软件断点时也带 ENABLE，单步带上 BLOCKIRQ；不在断点表里的 #BP、DR6 里不是我们槽位的 Bn、单步时没有 BS 的 #DB 都还给 guest | **PASS** — `gdb_test` 188 checks / 12 cases；三十二种错误实现（`tests/mutants/gdbstub.py`）各自变红：stub 不转义 '*'、校验和漏掉转义字节、收包不异或 0x20、收包校验和漏掉转义后的字节、放行错的校验和、放行超长包、'-' 不重发、跑着时执行包、停着时响应 Ctrl-C、不看 gdb 是否支持就发 swbreak、线程号从 0 起、停止后不切换当前线程、拿不到的寄存器填 0、允许写段寄存器、rbp/rsp 对调、M 忽略多余数据、短读报错、qXfer 永远回 'm'、vCont 最右边的动作生效、Hg0 改了选择、读不隐藏 int3、写覆盖掉 int3、写不更新原字节、删除时丢掉最后一项、重复插入软件断点、DR7 使能位错位、同地址硬件断点占两个槽、只设 USE_SW_BP 不带 ENABLE、单步丢掉 BLOCKIRQ、吞掉 guest 自己的 int3、单步时不看 BS、DR6 里没用的槽也当断点 | `make test unit` → `gdb_test`；`make test negctl` → 源码层 mutant 32/32 |
| E29 | 没改过的 gdb 经 TCP 调试 2 vCPU 的 Linux guest：`--gdb 70000` 报 `--gdb must be a TCP port in 0..65535: 70000`；连上后 2 个线程，`hbreak start_kernel` 停在 vCPU 0，`stepi` 后 pc 前进的正好是 gdb 反汇编给出的那条指令的长度（先确认它不是跳转），改写 rax 再还原（刷新 gdb 的寄存器缓存后读回），读写 `linux_banner` 再还原，读没有映射的地址报 MemoryError，`break kernel_init` 停在 kernel_init，读另一个线程的寄存器；guest 在 shell 提示符处运行时 Ctrl-C，之前 host 上两个 vCPU 线程的 syscall 是 `ioctl`（或正在运行），之后都是 `futex`；detach 后 guest 继续跑，经串口执行 `poweroff -f` 正常关机，VMM 报 `gdbstub: passed 1 guest #BP and 0 guest #DB back to the guest`，全程没有 kernel BUG；`rdinit=/nonexistent` 让 guest panic 后重启，gdb 收到退出事件、退出码 0 | **PASS** — `gdbstub` 3/3。把不在断点表里的 #BP 报给 gdb 而不交还的 VMM 上：guest 在 `int3_selftest_ip` 处 kernel BUG 后 panic，`break kernel_init` 停在错误的 pc，2 vCPU 用例变红；暂停时不发 SIGUSR1 只置 immediate_exit 的 VMM 上：第一次停止（`hbreak start_kernel`）就永远等不齐，因为 vCPU 1 还是等 SIPI 的 AP，阻塞在 KVM_RUN 里不会自己出来，2 vCPU 用例超时变红 | `make test gdbstub` || E12 | 三处修复与分发接口提取之后，真实 Linux 仍能正常启动关机 | **PASS** — `kvm-basics` 7/7、`linux-boot` 8/8、`legacy-devices` 3/3（vmlinux 直启、bzImage 解压、initramfs shell、ACPI S5 关机、i8042 reset、四个设备探测无超时、PIO trace 结构） | `make test kvm-basics linux-boot legacy-devices` |

### 复现命令

```sh
make test unit                     # E1 E2 E3 E7 E13 E14 E17 E18 E20 E22 E24 E26 E28
make test emu-net                  # E19 E21
make test emu-blk                  # E23 E25
make test pci-msi                  # E27
make test gdbstub                  # E29
make test negctl                   # E10 E26 E28（源码层 mutant）
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

- **negative control 只覆盖清单里列出的错误。** `make test negctl` 证明的是“这套断言能区分正确实现与 `unit/*_mutants.c`、`mutants/*.py` 里的这些错误实现”，不证明“没有别的错误类型能溜过去”。它已经抓到过一次自己的漏洞（`len-truncated-32` 最初逃掉了，因为长度表里没有低 32 位小、高 32 位非零的值），这说明清单本身也需要随契约增长。
- **`kvm_run` 字段提取没有单元测试。** 见 O2。`io.data_offset` 那一条只有 e2e 覆盖。
- **fuzz 只跑了 5 分钟/target、单 worker。** 覆盖率见日志里的 `cov:`。修复前 fuzzer 在首次发现处就停（`-fno-sanitize-recover=all`），所以“修好了这两个”不等于“后面没有别的”——修复后的重跑结果见执行结果摘要。
- **`gpa_to_hva` 的模型对照有一个已知盲区**：结束地址正好等于 2^64 的 slot。实现里 `s->gpa + s->size` 会回绕成 0，该 slot 完全不可用。测试布局刻意避开，因为现实中不会出现；将来 MMIO hole 的划分变复杂时要重新评估。
- **`bzimage_fuzz_noalign` 编出来了但没跑过。** 理由：`bzimage_load()` 把 setup header `memcpy` 进对齐的局部变量再访问，严格版跑满 300 秒也没被 alignment 挡住。这是判断，不是证据。
- **P0 只做了一半。** 修复之后 `make test` 默认集合完整跑过一次并全绿（7 个 suite / 48 条用例 / 98 秒），所以“改完还能启动 Linux”是有证据的。但 P0 原本要的是**各跑 3 遍以分清 flaky**，那部分没做。已知的一个 flaky 来源是 guest 命令按固定延迟发送，已由 P4c 消除；其余用例的重复稳定性没有系统统计。
- **P1 完全没有触及设备状态机、并发、启动流程**，那些行为目前仍然只有整机启动这一条间接证据。
