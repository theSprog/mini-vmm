/* src/exit_stats.c — exit 统计报告，数据来源见 exit_stats.h
 *
 * 报告是给人对照着看的文本表，每段一个标题行加一个表头行，数字右对齐。
 * 只打印非零的行（设备表除外：注册了却一次没被访问，本身就是信息）。
 */
#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>

#include "vmm.h"
#include "kvm_wrappers.h"
#include "kvm_stats.h"
#include "exit_stats.h"

#define NAME_W  26      /* 第一列宽度，放得下 KVM 最长的单值统计名 */
#define NUM_W   12

static void row_label(FILE *out, const char *name)
{
    fprintf(out, "  %-*s", NAME_W, name);
}

static void header_vcpus(FILE *out, const char *first, int nr_vcpus)
{
    char col[16];
    int i;

    row_label(out, first);
    fprintf(out, " %*s", NUM_W, "total");
    for (i = 0; i < nr_vcpus; i++) {
        snprintf(col, sizeof(col), "vcpu%d", i);
        fprintf(out, " %*s", NUM_W, col);
    }
    fputc('\n', out);
}

static void row_vcpus(FILE *out, const char *name, const uint64_t *v,
                      int nr_vcpus)
{
    uint64_t total = 0;
    int i;

    for (i = 0; i < nr_vcpus; i++)
        total += v[i];
    row_label(out, name);
    fprintf(out, " %*llu", NUM_W, (unsigned long long)total);
    for (i = 0; i < nr_vcpus; i++)
        fprintf(out, " %*llu", NUM_W, (unsigned long long)v[i]);
    fputc('\n', out);
}

static void report_reasons(FILE *out, struct vmm_vm *vm)
{
    uint64_t v[VMM_MAX_VCPUS];
    char buf[32];
    int r, i;

    fprintf(out, "user-space exits by reason (KVM_RUN returned to the VMM)\n");
    header_vcpus(out, "reason", vm->nr_vcpus);
    for (r = 0; r < VMM_EXIT_REASON_SLOTS; r++) {
        const char *name = kvm_exit_reason_str((uint32_t)r);
        uint64_t any = 0;

        for (i = 0; i < vm->nr_vcpus; i++)
            any |= v[i] = vm->vcpus[i].exits[r];
        if (!any)
            continue;
        if (r == VMM_EXIT_REASON_SLOTS - 1)
            name = "out-of-range";
        else if (!strcmp(name, "?")) {
            snprintf(buf, sizeof(buf), "reason-%d", r);
            name = buf;
        }
        row_vcpus(out, name, v, vm->nr_vcpus);
    }
    for (i = 0; i < vm->nr_vcpus; i++)
        v[i] = vcpu_exit_total(&vm->vcpus[i]);
    row_vcpus(out, "total", v, vm->nr_vcpus);
}

static void row_dev(FILE *out, const char *name, const char *space,
                    const struct vmm_io_stats *st)
{
    row_label(out, name);
    fprintf(out, " %-6s %*llu %*llu\n", space,
            NUM_W, (unsigned long long)st->reads,
            NUM_W, (unsigned long long)st->writes);
}

static void report_devices(FILE *out, struct vmm_vm *vm)
{
    int i;

    fprintf(out, "\nuser-space exits by device (string I/O counts once per exit)\n");
    row_label(out, "device");
    fprintf(out, " %-6s %*s %*s\n", "space", NUM_W, "reads", NUM_W, "writes");
    for (i = 0; i < vm->nr_pio_devs; i++)
        row_dev(out, vm->pio_devs[i].name, "PIO", &vm->pio_stats[i]);
    for (i = 0; i < vm->nr_mmio_devs; i++)
        row_dev(out, vm->mmio_devs[i].name, "MMIO", &vm->mmio_stats[i]);
    row_dev(out, "unhandled", "PIO", &vm->pio_unhandled);
    row_dev(out, "unhandled", "MMIO", &vm->mmio_unhandled);
}

/* 打开 fd 上的统计并读出来；失败时在报告里写明原因 */
static int read_kvm_stats(FILE *out, int fd, const char *what,
                          struct kvm_stats *st)
{
    int sfd = kvm_get_stats_fd(fd);
    int r;

    if (sfd < 0) {
        fprintf(out, "  %s stats unavailable: KVM_GET_STATS_FD failed "
                "(needs host kernel 5.14+)\n", what);
        return VMM_ERR_SYS;
    }
    r = kvm_stats_read(sfd, st);
    close(sfd);
    if (r != VMM_OK)
        fprintf(out, "  %s stats unreadable: malformed stats file "
                "(error %d, details in the log)\n", what, r);
    return r;
}

static void report_kvm_vcpu(FILE *out, struct vmm_vm *vm)
{
    struct kvm_stats st[VMM_MAX_VCPUS];
    uint64_t v[VMM_MAX_VCPUS];
    unsigned k;
    int i, n = 0;

    fprintf(out, "\nKVM vCPU stats (KVM_GET_STATS_FD, nonzero single values; "
            "includes exits handled inside KVM)\n");
    for (; n < vm->nr_vcpus; n++)
        if (read_kvm_stats(out, vm->vcpus[n].fd, "vCPU", &st[n]) != VMM_OK)
            goto out;

    header_vcpus(out, "name", vm->nr_vcpus);
    /* 每个 vCPU 的统计项相同，按 vcpu0 的顺序列，其他 vCPU 按名字取值 */
    for (k = 0; k < st[0].n; k++) {
        uint64_t any = 0;

        for (i = 0; i < vm->nr_vcpus; i++) {
            const struct kvm_stat *s = kvm_stats_find(&st[i], st[0].v[k].name);
            any |= v[i] = s ? s->value : 0;
        }
        if (any)
            row_vcpus(out, st[0].v[k].name, v, vm->nr_vcpus);
    }
out:
    for (i = 0; i < n; i++)
        kvm_stats_free(&st[i]);
}

static void report_kvm_vm(FILE *out, struct vmm_vm *vm)
{
    struct kvm_stats st;
    unsigned k;

    fprintf(out, "\nKVM VM stats (KVM_GET_STATS_FD, nonzero single values)\n");
    if (read_kvm_stats(out, vm->vm_fd, "VM", &st) != VMM_OK)
        return;
    row_label(out, "name");
    fprintf(out, " %*s\n", NUM_W, "value");
    for (k = 0; k < st.n; k++) {
        if (!st.v[k].value)
            continue;
        row_label(out, st.v[k].name);
        fprintf(out, " %*llu\n", NUM_W, (unsigned long long)st.v[k].value);
    }
    kvm_stats_free(&st);
}

void exit_stats_report(FILE *out, struct vmm_vm *vm, double seconds)
{
    fprintf(out, "exit-stats: %d vCPU(s), %.3f s in guest\n",
            vm->nr_vcpus, seconds);
    report_reasons(out, vm);
    report_devices(out, vm);
    report_kvm_vcpu(out, vm);
    report_kvm_vm(out, vm);
    fflush(out);
}
