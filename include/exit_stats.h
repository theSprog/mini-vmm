/* include/exit_stats.h — exit 统计报告（--exit-stats、monitor 的 info exit-stats）
 *
 * 数据有三个来源，报告里分三段：
 *   1. 每个 vCPU 回到用户态的 exit，按 exit_reason 分（vcpu->exits[]）
 *   2. KVM_EXIT_IO / KVM_EXIT_MMIO 落在哪个设备上，按读写分
 *      （vm->pio_stats[] 等，未注册地址单列一行）
 *   3. KVM 自己的计数器（KVM_GET_STATS_FD），包括在内核里就地处理、
 *      根本没回用户态的 exit
 * 1、2 在 exit 路径上一直在累加，开销是一次自增；打开统计不改变 guest
 * 的执行路径。报告可以在 guest 运行中生成：1 用 relaxed 原子读（理由见
 * struct vmm_vcpu.exits），2 持 io_lock 读，3 读创建 vCPU 时就打开的统计
 * fd（理由见 struct vmm_vcpu.stats_fd）。运行中生成时各段、各 vCPU 之间
 * 不是同一时刻的快照，数字只保证各自单调。
 */
#ifndef EXIT_STATS_H
#define EXIT_STATS_H

#include <stdint.h>
#include <stdio.h>

struct vmm_vm;

/* 写出完整报告。seconds 是 guest 已经运行的时长。vm_destroy() 之前的
 * 任何时刻都可以调用，运行中调用时不能持有 io_lock（这里要拿它） */
void exit_stats_report(FILE *out, struct vmm_vm *vm, double seconds);

#endif /* EXIT_STATS_H */
