# tests/ — 验收与回归

<!-- tests/README.md -->

```
make test                 默认集合：unit tables step1 step2 step6 step7
make test step2           只跑一个 suite
make test unit fuzz       跑多个
make test all             默认集合 + fuzz
make test-list            列出所有 suite
./tests/run.sh --manual step2    只看人工验收指引
```

退出码：`0` 全过，`1` 有 suite 失败，`2` 用法错。

验证设计（要确认什么、为什么、判据、预算、未决问题、剩余风险）在
[`TEST_DESIGN.md`](TEST_DESIGN.md)。改测试之前先看那份，尤其是 O1 和 O2 两个未决问题。

## 目录结构

```
tests/
├── run.sh          总入口，被 make test 调用；选 suite、跑、汇总
├── lib.sh          shell 断言框架，被各 suite source，不直接执行
├── TEST_DESIGN.md  验证设计与批准记录
├── offline/        不需要 /dev/kvm，秒级，可以在任何 Linux 上跑
│   ├── unit.sh         跑 build/tests/*_test，外加回归输入重放
│   ├── negctl.sh       negative control：每个 mutant 都必须让测试变红
│   ├── boot_tables.sh  把 ACPI 表交给 iasl 反汇编
│   └── fuzz.sh         跑 build/fuzz/*，按墙钟时间计费
├── e2e/            需要 /dev/kvm，多数还需要 guest 内核
│   ├── step1.sh  step2.sh  step6.sh  step7.sh
├── unit/           C 单元测试源码
│   ├── harness.h           最小断言框架
│   ├── mutants.h           negative control 的接口定义
│   ├── mem_test.c          内存 slot 与 GPA→HVA 边界契约
│   ├── mem_mutants.c       故意写错的 gpa_to_hva，共 7 份
│   ├── io_bus_test.c       PIO/MMIO 注册、查找、分发
│   ├── io_bus_mutants.c    故意写错的 dispatch，共 8 份
│   └── boot_tables_test.c  MP/ACPI/boot_params 字段检查
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
| `fuzz` | 否 | 否（有则用作种子） | ASan/UBSan + “必须有限时间内返回” |
| `step1` | 是 | 否 | VMM 日志 + payload 自检码 |
| `step2` | 是 | 是 | guest Linux 的启动日志 |
| `step6` | 是 | 是 | guest 的 `/proc/cpuinfo`、`/sys` 拓扑 |
| `step7` | 是 | 是 | guest 驱动探测结论 + PIO trace 结构 |

`step2` / `step6` / `step7` 需要 `VMLINUX=/path/to/vmlinux`（`step2` 的 2.4 另需 `BZIMAGE=`），
没设时相关用例 SKIP。Linux 模式必须带 `--initrd`，脚本在 `build/initramfs.cpio.gz`
不存在时会先调 `tools/build_initramfs.sh`。

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

`expect_not` 同样重要：`step1.sh` 里 `expect_not "SHUTDOWN"` 才能区分
“跑对了”和“跑飞了但恰好也打印了那行日志”。

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

`make test negctl`。一套断言全绿，只说明它在当前实现上没报警，不说明它**有能力**报警。
这个 suite 把被测函数换成 `unit/*_mutants.c` 里故意写错的完整实现，跑同一套用例，
要求每一个 mutant 都让测试非零退出——**退出码为 0 才是 FAIL**。

```sh
build/tests/mem_test --list-mutants      # 列出 mutant 及它违反的契约
build/tests/mem_test --mutant 3          # 单独跑一个，看它红在哪条用例
```

刻意没有用“改一行源码再撤销”那种做法：那是一次性的，行号会漂，重构一次就失效。
这里每个 mutant 是一份只依赖函数签名的完整实现，`src/` 内部怎么重构都不影响。
**加新契约时配一个对应的 mutant**，这份清单才一直有意义。

这套东西是有回报的：`mem_test` 最初漏掉了 `len-truncated-32` 这个 mutant——
长度表里没有“低 32 位小、高 32 位非零”的值，任何把 `len` 截成 32 位的实现都能溜过去。
是 negctl 逼出来的，不是想出来的。

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
