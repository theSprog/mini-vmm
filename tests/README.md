# tests/ — 分 Step 验收

<!-- tests/README.md -->

每个 Step 一个脚本，自成一体，可以单独跑：

```
./tests/step1.sh          # 自动用例 + 人工验收指引
./tests/step1.sh -a       # 只跑自动用例（CI / 回归用）
./tests/step1.sh -m       # 只看人工验收指引
```

退出码：`0` 全过，`1` 有 FAIL，`2` 参数错。

## 为什么不放进 Makefile

`make` 负责的是"把源码变成产物"，验收负责的是"判断产物对不对"。
两者混在一起会有几个具体问题：

- 验收需要断言、需要比对输出、需要区分 PASS/FAIL/SKIP，
  这些在 Makefile 的 recipe 里写会变成一堆 `@if ... grep -q ... || exit 1`，
  出错时看不到实际输出是什么
- 有些验收步骤需要 `sudo`（perf tracepoint）、需要人眼判断（串口日志），
  这类东西不该出现在构建系统里
- 后续 Step 的验收会越来越重（起 guest 内核、挂 rootfs、跑 iperf），
  跟编译解耦之后才好单独重跑某一项

## 组织约定

- `lib.sh`：公共断言框架，被各 `stepN.sh` source，不直接执行
- `stepN.sh`：分成两块
  - `auto_tests()`：能机器判定的，用 `run_case` / `expect_*` / `end_case`
  - `manual_guide()`：判定不了的，用 `guide "标题" "命令" "期望现象"`，
    只打印指导，不做断言

判不了就老实写进 `manual_guide`，不要为了凑自动化去断言一个
其实证明不了什么的字符串。

## 断言框架

```sh
run_case "用例名" 命令 参数...   # 执行，stdout+stderr 合并进 $CUR_OUT
expect_rc 0                      # 退出码
expect_has "子串"                # 必须出现（固定字符串匹配，非正则）
expect_not "子串"                # 必须不出现
end_case                         # 判定；失败时自动打印完整实际输出
```

`expect_not` 同样重要：Step 1 里 `expect_not "SHUTDOWN"` 才能区分
"跑对了"和"跑飞了但恰好也打印了那行日志"。

## 各 Step 覆盖情况

| Step | 脚本 | 自动用例 | 人工项 |
|------|------|---------|--------|
| 1.1 实模式 + KVM_EXIT_IO/HLT | `step1.sh` | 2 | — |
| 1.2 4 级页表 + GDT + 长模式  | `step1.sh` | 2 | — |
| 错误路径诊断信息             | `step1.sh` | 3 | — |
| SVM 能力 / ioctl 序列 / tracepoint | `step1.sh` | — | 5 |