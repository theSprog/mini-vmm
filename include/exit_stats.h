/* include/exit_stats.h — VM 停下后输出 exit 统计（--exit-stats）
 *
 * 数据有三个来源，报告里分三段：
 *   1. 每个 vCPU 回到用户态的 exit，按 exit_reason 分（vcpu->exits[]）
 *   2. KVM_EXIT_IO / KVM_EXIT_MMIO 落在哪个设备上，按读写分
 *      （vm->pio_stats[] 等，未注册地址单列一行）
 *   3. KVM 自己的计数器（KVM_GET_STATS_FD），包括在内核里就地处理、
 *      根本没回用户态的 exit
 * 1、2 在 exit 路径上一直在累加，开销是持锁路径上的一次自增；
 * 本模块只在所有 vCPU 线程 join 之后读它们，所以全程不需要额外的同步，
 * 打开 --exit-stats 也不会改变 guest 的执行路径。
 */
#ifndef EXIT_STATS_H
#define EXIT_STATS_H

#include <stdint.h>
#include <stdio.h>

struct vmm_vm;

/* 写出完整报告。seconds 是 guest 从开始运行到全部 vCPU 停下的时长。
 * 必须在 vm_run() 返回之后、vm_destroy() 之前调用：vCPU fd 要还开着，
 * vCPU 线程要已经停下（理由见 kvm_get_stats_fd()）。 */
void exit_stats_report(FILE *out, struct vmm_vm *vm, double seconds);

#endif /* EXIT_STATS_H */
