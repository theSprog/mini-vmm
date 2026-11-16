/* src/monitor_cmds.c — monitor 的 VM 相关命令，命令列表见 monitor_cmds.h
 *
 * 处理函数都在 I/O 线程上运行。它们读的 VM/vCPU 字段由别的线程写，读法
 * 各有依据：exit 计数、vCPU 状态、线程号、复位次数是 relaxed 原子（只要
 * 单个值完整，不要求彼此是同一时刻的快照）；设备计数持 io_lock；寄存器
 * 只能在 vCPU 自己的线程上读，经 vcpu_queue_work() 异步完成。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vmm.h"
#include "smp.h"
#include "monitor.h"
#include "monitor_cmds.h"
#include "exit_stats.h"

static double uptime(struct vmm_vm *vm)
{
    struct timespec now;

    if (!vm->started.tv_sec && !vm->started.tv_nsec)
        return 0;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - vm->started.tv_sec) +
           (double)(now.tv_nsec - vm->started.tv_nsec) / 1e9;
}

/* ---- info status ---- */

static int info_status(struct mon_req *req, int argc, char **argv)
{
    struct vmm_vm *vm = mon_req_opaque(req);
    const char *st;

    if (__atomic_load_n(&vm->quit_requested, __ATOMIC_SEQ_CST))
        st = "shutting down";
    else if (__atomic_load_n(&vm->reset_requested, __ATOMIC_SEQ_CST))
        st = "resetting";
    else if (__atomic_load_n(&vm->running, __ATOMIC_SEQ_CST))
        st = "running";
    else
        st = "shutting down";
    fprintf(mon_out(req), "VM status: %s\n", st);
    fprintf(mon_out(req), "vcpus: %d, memory: %llu MiB, uptime: %.3f s, resets: %u\n",
            vm->nr_vcpus, (unsigned long long)(vm->cfg.ram_size >> 20),
            uptime(vm), __atomic_load_n(&vm->reset_count, __ATOMIC_RELAXED));
    return MON_OK;
}

/* ---- info cpus ---- */

static const char *state_str(int st)
{
    switch (st) {
    case VCPU_GUEST:  return "guest";
    case VCPU_VMM:    return "vmm";
    case VCPU_PAUSED: return "paused";
    default:          return "stopped";
    }
}

/* host 调度器眼里这个线程的状态（proc(5) 的 stat 第 3 个字段）：R 正在
 * 运行或可运行，S 睡眠。guest 执行 HLT 时 vCPU 线程在 KVM 内部睡眠，显示 S；
 * 读不到时返回 '-' */
static char host_state(int tid)
{
    char path[64], buf[512], *p;
    FILE *f;
    size_t n;

    snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
    f = fopen(path, "r");
    if (!f)
        return '-';
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    /* 第 2 个字段 comm 带括号且可能含空格，从最后一个 ')' 往后找 */
    p = strrchr(buf, ')');
    return (p && p[1] == ' ' && p[2]) ? p[2] : '-';
}

static int info_cpus(struct mon_req *req, int argc, char **argv)
{
    struct vmm_vm *vm = mon_req_opaque(req);
    int i;

    for (i = 0; i < vm->nr_vcpus; i++) {
        struct vmm_vcpu *v = &vm->vcpus[i];
        int st = __atomic_load_n(&v->state, __ATOMIC_RELAXED);
        int tid = __atomic_load_n(&v->tid, __ATOMIC_RELAXED);
        uint64_t exits = vcpu_exit_total(v);
        uint32_t last = __atomic_load_n(&v->last_exit, __ATOMIC_RELAXED);

        /* 线程已退出时 tid 可能已被别的线程复用，不去读 /proc */
        fprintf(mon_out(req), "%s CPU #%d: thread_id=%d state=%s host=%c "
                "exits=%llu last=%s\n",
                i == mon_req_cpu(req) ? "*" : " ", i, tid, state_str(st),
                st == VCPU_STOPPED || !tid ? '-' : host_state(tid),
                (unsigned long long)exits,
                exits ? kvm_exit_reason_str(last) : "none");
    }
    return MON_OK;
}

/* ---- cpu ---- */

static int cmd_cpu(struct mon_req *req, int argc, char **argv)
{
    struct vmm_vm *vm = mon_req_opaque(req);
    char *end;
    long n;

    errno = 0;
    n = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end || errno)
        return mon_err(req, "invalid CPU index '%s'", argv[1]);
    if (n < 0 || n >= vm->nr_vcpus)
        return mon_err(req, "no such CPU %ld (this VM has %d)", n, vm->nr_vcpus);
    mon_req_set_cpu(req, (int)n);
    return MON_OK;
}

/* ---- info registers ----
 *
 * 每个目标 vCPU 排一个 vcpu_work，在 vCPU 线程上把寄存器格式化进各自的
 * 缓冲区。remaining 的初值是目标数加 1，多出的 1 属于处理函数自己：工作
 * 可能在处理函数还没排完时就已经执行完，没有这一份的话计数会提前归零。
 * 谁把它减到 0 谁就 post；finish 在 I/O 线程上按 vCPU 顺序拼出回复。
 */
struct regs_part {
    struct vcpu_work  w;
    struct regs_job  *job;
    int               cpu;
    bool              queued;
    char             *text;
    size_t            len;
};

struct regs_job {
    struct mon_req   *req;
    int               remaining;
    int               n;
    struct regs_part  parts[VMM_MAX_VCPUS];
};

static void regs_put(struct regs_job *job)
{
    if (__atomic_sub_fetch(&job->remaining, 1, __ATOMIC_ACQ_REL) == 0)
        mon_req_post(job->req);
}

/* vCPU 线程上运行 */
static void regs_work(struct vmm_vcpu *v, struct vcpu_work *w)
{
    struct regs_part *p = (struct regs_part *)w;
    FILE *f = open_memstream(&p->text, &p->len);

    if (f) {
        vcpu_print_state(f, v);
        fclose(f);
    }
    regs_put(p->job);
}

static int regs_finish(struct mon_req *req)
{
    struct regs_job *job = mon_req_defer_opaque(req);
    int i, st = MON_OK;

    for (i = 0; i < job->n; i++) {
        struct regs_part *p = &job->parts[i];

        if (st == MON_OK && !p->queued)
            st = mon_err(req, "CPU %d is not running (VM stopping or resetting)",
                         p->cpu);
        else if (st == MON_OK)
            fwrite(p->text ? p->text : "", 1, p->text ? p->len : 0, mon_out(req));
        free(p->text);
    }
    free(job);
    return st;
}

static int info_registers(struct mon_req *req, int argc, char **argv)
{
    struct vmm_vm *vm = mon_req_opaque(req);
    struct regs_job *job;
    int i, first, n;

    if (argc == 2 && strcmp(argv[1], "-a"))
        return mon_err(req, "info registers: unknown option '%s' "
                       "(usage: info registers [-a])", argv[1]);
    if (argc == 2) {
        first = 0;
        n = vm->nr_vcpus;
    } else {
        first = mon_req_cpu(req);
        n = 1;
    }

    job = calloc(1, sizeof(*job));
    if (!job)
        return mon_err(req, "out of memory");
    job->req = req;
    job->n = n;
    job->remaining = n + 1;
    /* 必须先于排队：第一个工作可能马上完成并 post */
    mon_req_defer(req, regs_finish, job);
    for (i = 0; i < n; i++) {
        struct regs_part *p = &job->parts[i];

        p->job = job;
        p->cpu = first + i;
        p->w.fn = regs_work;
        p->queued = vcpu_queue_work(&vm->vcpus[p->cpu], &p->w) == VMM_OK;
        if (!p->queued)
            regs_put(job);
    }
    regs_put(job);
    return MON_PENDING;
}

/* ---- info exit-stats ---- */

static int info_exit_stats(struct mon_req *req, int argc, char **argv)
{
    struct vmm_vm *vm = mon_req_opaque(req);

    exit_stats_report(mon_out(req), vm, uptime(vm));
    return MON_OK;
}

/* ---- quit ---- */

static int cmd_quit(struct mon_req *req, int argc, char **argv)
{
    struct vmm_vm *vm = mon_req_opaque(req);

    vmm_info("monitor: quit requested");
    vm->exit_code = 0;
    /* 先置 quit_requested 再停机：main 的复位循环据此不再复位，理由见
     * struct vmm_vm.quit_requested */
    __atomic_store_n(&vm->quit_requested, true, __ATOMIC_SEQ_CST);
    vm_request_stop(vm);
    return MON_OK;
}

int monitor_cmds_register(struct monitor *m, struct vmm_vm *vm)
{
    const struct mon_cmd top[] = {
        { "cpu", "<index>", "set the current CPU for info registers",
          1, 1, cmd_cpu, vm },
        { "quit", "", "stop the guest and exit the VMM",
          0, 0, cmd_quit, vm },
    };
    const struct mon_cmd info[] = {
        { "status", "", "VM run state, vCPUs, memory, uptime, resets",
          0, 0, info_status, vm },
        { "cpus", "", "per-vCPU thread, state and exit counts",
          0, 0, info_cpus, vm },
        { "registers", "[-a]", "show the current CPU's registers (-a: all CPUs)",
          0, 1, info_registers, vm },
        { "exit-stats", "", "VM exit counts so far, same report as --exit-stats",
          0, 0, info_exit_stats, vm },
    };
    size_t i;
    int r;

    for (i = 0; i < sizeof(top) / sizeof(top[0]); i++)
        if ((r = monitor_register(m, MON_TOP, &top[i])) != VMM_OK)
            return r;
    for (i = 0; i < sizeof(info) / sizeof(info[0]); i++)
        if ((r = monitor_register(m, MON_INFO, &info[i])) != VMM_OK)
            return r;
    return VMM_OK;
}
