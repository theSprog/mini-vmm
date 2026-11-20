/* src/main.c — 进程入口：解析命令行，按阶段装配并运行一台 VM
 *
 * 阶段（每一步的前提是上一步成功）：
 *   1. 解析命令行（config.h），打开统计输出文件
 *   2. vm_create：KVM VM、guest 内存、中断控制器、vCPU
 *   3. machine_init / machine_boot：挂设备、装载镜像（machine.h）
 *   4. 注册 monitor，启动 I/O 线程和 gdbstub，运行 guest；guest 请求复位时
 *      vm_reset 之后再运行
 *   5. 报告：vCPU 现场、exit 统计、内存 dump
 *   6. 按依赖的逆序释放
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include "vmm.h"
#include "memory.h"
#include "machine.h"
#include "net/netdev.h"
#include "block/blockdev.h"
#include "smp.h"
#include "io/event_loop.h"
#include "exit_stats.h"
#include "monitor.h"
#include "monitor_cmds.h"
#include "power.h"
#include "gdb/gdbstub.h"

/* guest 请求复位时 vm_run 照常返回（vCPU 线程都已 join），复位后重新
 * 运行。I/O 线程和控制台在复位期间继续工作，vm_reset 持 io_lock 把它们
 * 挡在设备外面 */
static int run_guest(struct vmm_vm *vm, struct machine *m)
{
    int r;

    for (;;) {
        r = vm_run(vm);
        if (r != VMM_OK || !vm->reset_requested)
            break;
        /* quit 可能和 guest 的复位请求同时发生，也可能落在 vm_reset 的
         * 过程中；vm_reset 会清掉 should_stop，所以复位前后都要看一次 */
        if (__atomic_load_n(&vm->quit_requested, __ATOMIC_SEQ_CST))
            break;
        r = vm_reset(vm, machine_reload, m);
        if (r != VMM_OK || __atomic_load_n(&vm->quit_requested, __ATOMIC_SEQ_CST))
            break;
    }
    /* 因 quit 放弃复位时，gdbstub 要看到 running 和 reset_requested 都为
     * false 才认为 VM 结束了 */
    if (vm->reset_requested && __atomic_load_n(&vm->quit_requested, __ATOMIC_SEQ_CST))
        __atomic_store_n(&vm->reset_requested, false, __ATOMIC_SEQ_CST);
    return r;
}

/* 统计文件在启动前就打开：路径写错要在 guest 跑之前报出来，
 * 而不是跑完一整次之后才发现结果没处写 */
static int open_exit_stats(const struct vmm_config *cfg, FILE **out)
{
    *out = NULL;
    if (!cfg->exit_stats_path)
        return VMM_OK;
    *out = strcmp(cfg->exit_stats_path, "-")
         ? fopen(cfg->exit_stats_path, "w") : stderr;
    if (!*out) {
        vmm_err("--exit-stats %s: %s", cfg->exit_stats_path, strerror(errno));
        return VMM_ERR_SYS;
    }
    return VMM_OK;
}

/* 已经 monitor_init 过的 monitor：登记命令，开始监听。socket 和完成通知
 * eventfd 要在 evl_start 之前注册（处理器表只在单线程阶段由主线程写） */
static int monitor_setup(struct monitor *mon, struct vmm_vm *vm,
                         const char *path, struct event_loop *loop)
{
    int r = monitor_cmds_register(mon, vm);

    if (r == VMM_OK)
        r = power_monitor_register(mon);
    if (r == VMM_OK)
        r = monitor_listen(mon, path, loop);
    return r;
}

static double seconds_between(const struct timespec *t0,
                              const struct timespec *t1)
{
    return (double)(t1->tv_sec - t0->tv_sec) +
           (double)(t1->tv_nsec - t0->tv_nsec) / 1e9;
}

/* 运行 guest 直到它停下，然后停掉 I/O 线程并输出报告 */
static int run_and_report(struct vmm_vm *vm, struct machine *m,
                          struct event_loop *loop, FILE *stats_out)
{
    const struct vmm_config *cfg = &vm->cfg;
    struct timespec t0, t1;
    int i, r;

    vmm_info("--- entering guest ---");
    clock_gettime(CLOCK_MONOTONIC, &t0);
    r = run_guest(vm, m);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    /* vCPU 线程都已 join，停掉 I/O 线程之后再没有谁会碰设备状态 */
    evl_stop(loop);
    machine_stop(m);
    vmm_info("--- left guest (%s) ---", r == VMM_OK ? "ok" : "error");

    /* Ctrl-A x 打断的现场一定要看：guest 卡住时这是唯一的线索 */
    if (r == VMM_OK && (vmm_log_level >= 3 || vm->user_stop)) {
        for (i = 0; i < vm->nr_vcpus; i++)
            vcpu_dump_state(&vm->vcpus[i]);
    }

    /* 出错停下的那一次同样要输出：exit 落在哪里正是排错线索 */
    if (stats_out)
        exit_stats_report(stats_out, vm, seconds_between(&t0, &t1));

    if (cfg->dump_on_exit) {
        fprintf(stderr, "guest memory at GPA 0x%llx, %zu bytes:\n",
                (unsigned long long)cfg->dump_gpa, cfg->dump_len);
        mem_hexdump(vm, cfg->dump_gpa, cfg->dump_len);
    }
    return r;
}

int main(int argc, char **argv)
{
    struct vmm_config cfg;
    struct vmm_vm vm;
    static struct machine machine;
    static struct event_loop loop;
    static struct monitor mon;
    bool mon_inited = false;
    FILE *stats_out;
    int i, r;

    vmm_config_default(&cfg);
    if (argc < 2) {
        vmm_usage(argv[0]);
        return 1;
    }
    if (vmm_config_parse_args(&cfg, argc, argv) != VMM_OK)
        return 1;
    vmm_log_level = cfg.log_level;
    if (open_exit_stats(&cfg, &stats_out) != VMM_OK)
        return 1;

    r = vm_create(&vm, &cfg);
    if (r != VMM_OK)
        goto close_stats;

    /* 后端 fd 在 guest 启动前注册（console_start 等），evl_start 放在
     * vm_run 之前，这样注册阶段处理器表只有主线程在写 */
    r = evl_init(&loop);
    if (r == VMM_OK)
        r = machine_init(&machine, &vm, &loop);
    if (r == VMM_OK)
        r = machine_boot(&machine);
    if (r == VMM_OK && cfg.monitor_path) {
        r = monitor_init(&mon);
        mon_inited = r == VMM_OK;
        if (r == VMM_OK)
            r = monitor_setup(&mon, &vm, cfg.monitor_path, &loop);
    }
    if (r != VMM_OK)
        goto out;

    clock_gettime(CLOCK_MONOTONIC, &vm.started);
    r = evl_start(&loop);
    if (r == VMM_OK && cfg.gdb_port >= 0)
        r = gdbstub_init(&vm, (uint16_t)cfg.gdb_port);
    if (r == VMM_OK)
        r = run_and_report(&vm, &machine, &loop, stats_out);

out:
    /* monitor 的回调在 I/O 线程上运行，先停 I/O 线程再关 monitor。
     * 正常路径上 I/O 线程已经停了，evl_stop 在未启动或已停止时直接返回 */
    evl_stop(&loop);
    if (mon_inited)
        monitor_close(&mon);
    /* 先于 vm_destroy：gdbstub 线程可能还在用 vCPU fd 和 guest 内存 */
    gdbstub_destroy(&vm);
    evl_destroy(&loop);
    for (i = 0; i < cfg.nr_netdevs; i++)
        netdev_close(cfg.netdevs[i]);
    for (i = 0; i < cfg.nr_drives; i++)
        blockdev_close(cfg.drives[i]);
    vm_destroy(&vm);
close_stats:
    if (stats_out && stats_out != stderr && fclose(stats_out) != 0) {
        vmm_err("--exit-stats %s: %s", cfg.exit_stats_path, strerror(errno));
        return 1;
    }
    return (r == VMM_OK) ? vm.exit_code : 1;
}
