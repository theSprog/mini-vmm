/* include/monitor_cmds.h — monitor 的 VM 相关命令
 *
 * 命令名和输出格式沿用 QEMU HMP：
 *   info status        运行状态、vCPU 数、内存、运行时长、复位次数
 *   info cpus          每个 vCPU 的 host 线程号、VMM 侧状态、host 线程状态、
 *                      exit 总数、最近一次 exit 原因；当前 CPU 前面标 *
 *   info registers     当前 CPU 的寄存器，-a 列出全部 vCPU
 *   info exit-stats    和 --exit-stats 相同的报告，运行中实时生成
 *   cpu <index>        设置这个连接的当前 CPU
 *   quit               让 VMM 退出，退出码 0
 * 协议核心（help、info 的分发、错误格式）在 monitor.c；以后的里程碑在
 * 各自的模块里用 monitor_register() 加命令，不改这个文件。
 */
#ifndef MONITOR_CMDS_H
#define MONITOR_CMDS_H

struct monitor;
struct vmm_vm;

int monitor_cmds_register(struct monitor *m, struct vmm_vm *vm);

#endif /* MONITOR_CMDS_H */
