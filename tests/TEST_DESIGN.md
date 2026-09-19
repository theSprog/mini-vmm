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

- **当前准备验收的改动**：不是某一次改动，而是 Step 1/2/6 完成后的既有实现。触发点是仓库里的测试方法过于单一，见下节。
- **希望确认的行为**：
  1. `gpa_to_hva()` 对越界、跨 slot、零长度和整数回绕的地址范围一律拒绝（`AGENT_GUIDE` 第 4 节的头号 invariant）；
  2. PIO/MMIO 总线的注册、查找、分发语义符合 `include/vmm.h` 与 `HANDOFF_STEP2_CN.md` 第 8 节写明的约定；
  3. `elf_load_vmlinux()` 与 `bzimage_load()` 面对任意畸形文件都不会越界访问或崩溃。
- **为什么这些行为重要**：这三处是 Step 3（VirtIO）唯一会直接依赖的地基。virtqueue 的每一个 descriptor segment 都要过 `gpa_to_hva()`；virtio-mmio 的配置空间会按各种宽度访问总线；内核镜像是这个 VMM 唯一解析的外部文件。三者出错的后果都是 VMM 进程内的内存破坏，而不是“guest 跑不起来”这种一眼能看见的失败。
- **用来比较的旧行为、规范或其他正确标准**：
  - `include/memory.h` 对 `gpa_to_hva()` 的语义注释（契约本身）；
  - `include/vmm.h` 对 `vmm_io_dev` 回调的语义注释、`HANDOFF_STEP2_CN.md` 第 5.6 / 第 8 节；
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
- 换一份 guest 内核 config 才会走到的探测分支（例如 `CONFIG_SERIAL_8250_16550A_VARIANTS=y` 才会执行的 EFR 路径）。

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
| P3-full | 按 `HANDOFF_STEP2_CN.md` 第 5 节对四个传统设备逐条注入已知缺陷 | 需要先有 P2 的设备单元测试 | 同上 | 同上 | — | 未批准（依赖 P2） |
| P4a | ASan+UBSan 与 TSan 构建下各跑一次完整启动关机 | 需要 `/dev/kvm`；TSan 下明显变慢 | 无 sanitizer 报告 | 报告全文 | 单次超过 300s 即中止 | 未批准 |
| P4b | 把 `pio_decode.py --summary` 变成带容差的回归断言 | 扩 `tests/e2e/step7.sh` | 总数 30449 ±5%、PIT 端口恒为 0 | 每次的统计输出 | — | 未批准 |
| P4c | guest 用例各跑 10 遍，统计 `feed_shell` 的时序稳定性 | 只跑不改 | 失败率为 0 | 10 次的退出码 | — | 未批准 |

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
  make test                      # 默认集合（unit tables step1 step2 step6 step7）
  make test step2                # 单个 suite
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
- **O2 — 分发接口（已解决）。**
  已把分发提成 `io_bus_dispatch_pio()` / `io_bus_dispatch_mmio()`（`include/vmm.h` 声明，`src/vm.c` 实现），`handle_io()` / `handle_mmio()` 退化成从 `kvm_run` 取字段的薄封装。`io_bus_test.c` 不再 `#include src/vm.c`，改为正常链接并通过公开接口调用。
  代价要记下来：**KVM_EXIT 路径上从 `kvm_run` 取字段那几行现在没有单元测试覆盖**，尤其是“必须按 `io.data_offset` 定位缓冲区、不能假设它紧跟结构体”这一条。它只由 e2e 覆盖（任何一次 guest IO 都会走到）。

---

## 执行结果摘要

环境：soc63（hygon-srv1），Ubuntu 22.04.5，kernel 5.15.0-190，gcc 11.4.0，clang 14.0.0。代码版本 `6eedbd5`，`src/` 无改动。

| 证据编号 | 确认的行为 | 实际结果 | 证据位置 |
|---|---|---|---|
| E1 | `gpa_to_hva` 与独立 128 位模型在边界网格、整数回绕、跨 slot、20 万轮随机输入上逐点一致 | **PASS** — 201666 checks / 8 cases / 0 failed；seed `0x2545f4914f6cdd1d` | `make test unit` → `mem_test` |
| E2 | IO 总线的注册重叠检测、表满、查找边界、offset 计算、串操作步进、`VMM_ERR_EXIT` 中止、未注册端口读回 0xFF | **PASS** — 1139 checks / 10 cases / 0 failed | `make test unit` → `io_bus_test` |
| E3 | 跨越设备末端的访问当前不做校验：`off=7, size=4` 照常交给回调（`dev->len=8`） | **CHARACTERIZED** — 记录现状，语义未定，见 O1 | `io_bus_test` 最后一个用例 |
| E4 | `bzimage_load()` 面对任意畸形文件无内存错误、无未定义行为 | **PASS** — 2346815 runs / 301s，0 发现 | `build/fuzz/logs/bz_strict.log` |
| E5 | `elf_loader.c` 的截断检查被 64 位回绕绕过，导致映射区之前的越界读 | **FAIL → 已修复** — 修复前 290271 runs 内触发；修复后 7074730 runs / 301s 零发现 | 修复前 `build/fuzz/logs/elf_noalign.log`；修复后 `elf_noalign_postfix.log` |
| E6 | `elf_loader.c` 以 `Elf64_Phdr*` 解引用文件里任意偏移，未对齐即未定义行为 | **FAIL → 已修复** — 修复前 9313 runs / 30s 内触发；修复后 6909083 runs / 301s 零发现 | 修复前 `elf_noalign.log`；修复后 `elf_strict_postfix.log` |
| E7 | 固件表生成过程无未定义行为 | **FAIL → 已修复** — `acpi.c:275` 未对齐 `uint64_t` 存储；修复后严格版退出码 0 | `build/tests/boot_tables_test` |
| E8 | 生成的 ACPI 表结构本身合法 | **PASS** — iasl 反汇编 APIC/DSDT/FACP/XSDT，无 Incorrect/Invalid/Error/Warning | `make test tables`，5/5 |
| E9 | E5/E6 的最小复现输入在修复后被干净拒绝（RED→GREEN） | **PASS** — 两个样本 × 两个 target，全部退出码 0 无 sanitizer 报告；已沉淀为 `tests/fuzz/regress/*.bin`，每次 `make test unit` 重放 | `make test unit` → regress 段 |
| E10 | 单元测试有能力报警（negative control） | **PASS** — 15 个接口层 mutant 全部被抓住，且都是因断言失败而红 | `make test negctl`，15/15 |
| E11 | negctl 反过来查出了 `mem_test` 自身的覆盖漏洞 | **已修复** — 长度表缺“低 32 位小、高 32 位非零”一类，`len-truncated-32` 最初逃逸；补齐取值后 15/15 | `mem_test.c` 的 `g_lens` |
| E12 | 三处修复与分发接口提取之后，真实 Linux 仍能正常启动关机 | **PASS** — `step1` 7/7、`step2` 8/8、`step7` 3/3（vmlinux 直启、bzImage 解压、initramfs shell、ACPI S5 关机、i8042 reset、四个设备探测无超时、PIO trace 结构） | `make test step1 step2 step7` |

### 复现命令

```sh
make test unit                     # E1 E2 E3 E7
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

- **negative control 只覆盖两个被测函数的 15 类错误。** `make test negctl` 证明的是“这套断言能区分正确实现与这 15 份错误实现”，不证明“没有别的错误类型能溜过去”。它已经抓到过一次自己的漏洞（`len-truncated-32` 最初逃掉了，因为长度表里没有低 32 位小、高 32 位非零的值），这说明清单本身也需要随契约增长。
- **`kvm_run` 字段提取没有单元测试。** 见 O2。`io.data_offset` 那一条只有 e2e 覆盖。
- **fuzz 只跑了 5 分钟/target、单 worker。** 覆盖率见日志里的 `cov:`。修复前 fuzzer 在首次发现处就停（`-fno-sanitize-recover=all`），所以“修好了这两个”不等于“后面没有别的”——修复后的重跑结果见执行结果摘要。
- **`gpa_to_hva` 的模型对照有一个已知盲区**：结束地址正好等于 2^64 的 slot。实现里 `s->gpa + s->size` 会回绕成 0，该 slot 完全不可用。测试布局刻意避开，因为现实中不会出现；将来 MMIO hole 的划分变复杂时要重新评估。
- **`bzimage_fuzz_noalign` 编出来了但没跑过。** 理由：`bzimage_load()` 把 setup header `memcpy` 进对齐的局部变量再访问，严格版跑满 300 秒也没被 alignment 挡住。这是判断，不是证据。
- **P0 只做了一半。** 修复之后 `make test` 默认集合完整跑过一次并全绿（7 个 suite / 48 条用例 / 98 秒），所以“改完还能启动 Linux”是有证据的。但 P0 原本要的是**各跑 3 遍以分清 flaky**，那部分没做——`feed_shell` 用 `sleep` 定时喂命令，时序稳定性仍然未知（P4c）。
- **P1 完全没有触及设备状态机、并发、启动流程**，那些行为目前仍然只有整机启动这一条间接证据。
