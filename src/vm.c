/* src/vm.c — VM/vCPU 生命周期、KVM_RUN 主循环、exit 分发 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>

#include "vmm.h"
#include "memory.h"
#include "kvm_wrappers.h"
#include "arch/arch.h"
#include "devices/irqchip.h"
#include "gdb/gdbstub.h"

/* ------------------------------------------------------------------ */
/* IO 总线                                                             */
/* ------------------------------------------------------------------ */

static int io_register(struct vmm_io_dev *tbl, int *n, int max,
                       const struct vmm_io_dev *dev, const char *kind)
{
    int i;

    if (*n >= max) {
        vmm_err("%s device table is full", kind);
        return VMM_ERR_NOMEM;
    }
    for (i = 0; i < *n; i++) {
        if (tbl[i].len == 0)            /* 暂不响应的 BAR 表项，见 vmm_io_reserve */
            continue;
        if (dev->base < tbl[i].base + tbl[i].len &&
            tbl[i].base < dev->base + dev->len) {
            vmm_err("%s address conflict: %s [0x%llx,+0x%llx) overlaps %s",
                    kind, dev->name,
                    (unsigned long long)dev->base,
                    (unsigned long long)dev->len, tbl[i].name);
            return VMM_ERR_INVAL;
        }
    }
    tbl[(*n)++] = *dev;
    vmm_dbg("registered %s device %s @ [0x%llx, 0x%llx)", kind, dev->name,
            (unsigned long long)dev->base,
            (unsigned long long)(dev->base + dev->len));
    return VMM_OK;
}

static const struct vmm_io_dev *io_lookup(const struct vmm_io_dev *tbl, int n,
                                          uint64_t addr)
{
    int i;

    for (i = 0; i < n; i++)
        if (addr >= tbl[i].base && addr < tbl[i].base + tbl[i].len)
            return &tbl[i];
    return NULL;
}

int vmm_register_pio(struct vmm_vm *vm, const struct vmm_io_dev *dev)
{
    return io_register(vm->pio_devs, &vm->nr_pio_devs,
                       VMM_MAX_PIO_DEVS, dev, "PIO");
}

int vmm_register_mmio(struct vmm_vm *vm, const struct vmm_io_dev *dev)
{
    return io_register(vm->mmio_devs, &vm->nr_mmio_devs,
                       VMM_MAX_MMIO_DEVS, dev, "MMIO");
}

static void io_table(struct vmm_vm *vm, enum vmm_io_space space,
                     struct vmm_io_dev **tbl, int **n, int *max)
{
    if (space == VMM_IO_PIO) {
        *tbl = vm->pio_devs;
        *n   = &vm->nr_pio_devs;
        *max = VMM_MAX_PIO_DEVS;
    } else {
        *tbl = vm->mmio_devs;
        *n   = &vm->nr_mmio_devs;
        *max = VMM_MAX_MMIO_DEVS;
    }
}

int vmm_io_reserve(struct vmm_vm *vm, enum vmm_io_space space,
                   const struct vmm_io_dev *dev, int *slot)
{
    struct vmm_io_dev *tbl, d = *dev;
    int *n, max, r;

    io_table(vm, space, &tbl, &n, &max);
    d.base = 0;
    d.len  = 0;
    r = io_register(tbl, n, max, &d, space == VMM_IO_PIO ? "PIO" : "MMIO");
    if (r == VMM_OK)
        *slot = *n - 1;
    return r;
}

void vmm_io_move(struct vmm_vm *vm, enum vmm_io_space space, int slot,
                 uint64_t base, uint64_t len)
{
    const char *kind = space == VMM_IO_PIO ? "PIO" : "MMIO";
    struct vmm_io_dev *tbl, *e;
    int *n, max, i;

    io_table(vm, space, &tbl, &n, &max);
    e = &tbl[slot];
    e->base = len ? base : 0;
    e->len  = len;
    if (!len) {
        vmm_dbg("%s %s: no longer decoded", kind, e->name);
        return;
    }
    vmm_dbg("%s %s: decoded at [0x%llx, 0x%llx)", kind, e->name,
            (unsigned long long)base, (unsigned long long)(base + len));
    for (i = 0; i < *n; i++) {
        if (i == slot || !tbl[i].len)
            continue;
        if (base < tbl[i].base + tbl[i].len && tbl[i].base < base + len)
            vmm_warn("%s %s at [0x%llx, 0x%llx) overlaps %s; %s wins",
                     kind, e->name, (unsigned long long)base,
                     (unsigned long long)(base + len), tbl[i].name,
                     i < slot ? tbl[i].name : e->name);
    }
}

const struct vmm_io_dev *vmm_lookup_pio(struct vmm_vm *vm, uint64_t port)
{
    return io_lookup(vm->pio_devs, vm->nr_pio_devs, port);
}

const struct vmm_io_dev *vmm_lookup_mmio(struct vmm_vm *vm, uint64_t addr)
{
    return io_lookup(vm->mmio_devs, vm->nr_mmio_devs, addr);
}

/* ------------------------------------------------------------------ */
/* 调试输出                                                            */
/* ------------------------------------------------------------------ */

const char *kvm_exit_reason_str(uint32_t r)
{
    switch (r) {
    case KVM_EXIT_UNKNOWN:        return "UNKNOWN";
    case KVM_EXIT_EXCEPTION:      return "EXCEPTION";
    case KVM_EXIT_IO:             return "IO";
    case KVM_EXIT_HYPERCALL:      return "HYPERCALL";
    case KVM_EXIT_DEBUG:          return "DEBUG";
    case KVM_EXIT_HLT:            return "HLT";
    case KVM_EXIT_MMIO:           return "MMIO";
    case KVM_EXIT_IRQ_WINDOW_OPEN:return "IRQ_WINDOW_OPEN";
    case KVM_EXIT_SHUTDOWN:       return "SHUTDOWN";
    case KVM_EXIT_FAIL_ENTRY:     return "FAIL_ENTRY";
    case KVM_EXIT_INTR:           return "INTR";
    case KVM_EXIT_SET_TPR:        return "SET_TPR";
    case KVM_EXIT_TPR_ACCESS:     return "TPR_ACCESS";
    case KVM_EXIT_NMI:            return "NMI";
    case KVM_EXIT_INTERNAL_ERROR: return "INTERNAL_ERROR";
    case KVM_EXIT_SYSTEM_EVENT:   return "SYSTEM_EVENT";
    case KVM_EXIT_IOAPIC_EOI:     return "IOAPIC_EOI";
    case KVM_EXIT_HYPERV:         return "HYPERV";
    case KVM_EXIT_X86_RDMSR:      return "X86_RDMSR";
    case KVM_EXIT_X86_WRMSR:      return "X86_WRMSR";
    case KVM_EXIT_DIRTY_RING_FULL:return "DIRTY_RING_FULL";
    case KVM_EXIT_AP_RESET_HOLD:  return "AP_RESET_HOLD";
    case KVM_EXIT_X86_BUS_LOCK:   return "X86_BUS_LOCK";
    default:                      return "?";
    }
}

static void dump_seg(const char *name, const struct kvm_segment *s)
{
    fprintf(stderr, "  %-3s sel=%04x base=%016llx limit=%08x "
                    "type=%x s=%u dpl=%u p=%u l=%u db=%u g=%u\n",
            name, s->selector, (unsigned long long)s->base, s->limit,
            s->type, s->s, s->dpl, s->present, s->l, s->db, s->g);
}

void vcpu_dump_state(struct vmm_vcpu *vcpu)
{
    struct kvm_regs r;
    struct kvm_sregs s;

    if (kvm_get_regs(vcpu->fd, &r) != VMM_OK ||
        kvm_get_sregs(vcpu->fd, &s) != VMM_OK) {
        fprintf(stderr, "cannot read vCPU %d state\n", vcpu->id);
        return;
    }

    fprintf(stderr, "=== vCPU %d state ===\n", vcpu->id);
    fprintf(stderr, "  rip=%016llx rflags=%016llx\n",
            (unsigned long long)r.rip, (unsigned long long)r.rflags);
    fprintf(stderr, "  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n",
            (unsigned long long)r.rax, (unsigned long long)r.rbx,
            (unsigned long long)r.rcx, (unsigned long long)r.rdx);
    fprintf(stderr, "  rsi=%016llx rdi=%016llx rsp=%016llx rbp=%016llx\n",
            (unsigned long long)r.rsi, (unsigned long long)r.rdi,
            (unsigned long long)r.rsp, (unsigned long long)r.rbp);
    fprintf(stderr, "  cr0=%016llx cr2=%016llx cr3=%016llx cr4=%016llx\n",
            (unsigned long long)s.cr0, (unsigned long long)s.cr2,
            (unsigned long long)s.cr3, (unsigned long long)s.cr4);
    fprintf(stderr, "  efer=%016llx  (LME=%d LMA=%d)  apic_base=%016llx\n",
            (unsigned long long)s.efer,
            !!(s.efer & (1ULL << 8)), !!(s.efer & (1ULL << 10)),
            (unsigned long long)s.apic_base);
    dump_seg("cs", &s.cs);
    dump_seg("ss", &s.ss);
    dump_seg("ds", &s.ds);
    fprintf(stderr, "  gdt base=%016llx limit=%04x  idt base=%016llx limit=%04x\n",
            (unsigned long long)s.gdt.base, s.gdt.limit,
            (unsigned long long)s.idt.base, s.idt.limit);
    fprintf(stderr, "  exits=%llu (io=%llu mmio=%llu hlt=%llu)\n",
            (unsigned long long)vcpu_exit_total(vcpu),
            (unsigned long long)vcpu->exits[KVM_EXIT_IO],
            (unsigned long long)vcpu->exits[KVM_EXIT_MMIO],
            (unsigned long long)vcpu->exits[KVM_EXIT_HLT]);
}

uint64_t vcpu_exit_total(const struct vmm_vcpu *vcpu)
{
    uint64_t sum = 0;
    int i;

    for (i = 0; i < VMM_EXIT_REASON_SLOTS; i++)
        sum += vcpu->exits[i];
    return sum;
}

/* ------------------------------------------------------------------ */
/* exit 处理                                                           */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* I/O 追踪（--trace-pio、--trace-mmio）                                */
/* ------------------------------------------------------------------ */
/*
 * 设备模拟到什么程度，由驱动的探测逻辑决定，而不是由芯片手册决定——
 * 想知道驱动到底要什么，最直接的办法是把它读写寄存器的顺序原样记下来。
 *
 * PIO 和 MMIO 各一个文件，每行一次访问，格式相同：
 *   <单调时钟秒>.<微秒> <设备名> <端口或 GPA> R|W<字节数> <值> [rip=<guest RIP>]
 * 端口按 4 位十六进制、GPA 按至少 8 位十六进制打印；MMIO 的设备名是 BAR
 * 的名字（如 e1000.bar0），寄存器偏移 = GPA - BAR 基址，BAR 基址在启动日志
 * 的 "pci ... BAR0 [mem ...]" 一行。两种文件都用 tools/trace_decode.py 解读。
 *
 * rip 只对没有设备的地址输出：已注册设备的名字已经回答了“谁在访问”，
 * 而没有设备的地址恰恰是需要反查调用者的那一类。拿 RIP 到 guest 的
 * vmlinux 上 `nm -n` 一查就知道是哪个函数在盲探，例如 0x87 读回 0xFF
 * 正是 i8237A_init_ops() 判定“这台机器没有 8237 DMA 控制器”的依据。
 *
 * 只在给出对应选项时才打开。写文件本身不加锁——分发路径全程持有
 * vm->io_lock，多 vCPU 下也是串行的。
 */
static FILE *g_pio_trace;
static FILE *g_mmio_trace;

static FILE *trace_open(const char *path, const char *what)
{
    FILE *f;

    if (!path)
        return NULL;
    f = fopen(path, "w");
    if (!f)
        vmm_warn("cannot open %s trace file %s: %s", what, path, strerror(errno));
    else
        vmm_info("tracing %s to %s", what, path);
    return f;
}

static void trace_open_all(struct vmm_vm *vm)
{
    g_pio_trace = trace_open(vm->cfg.trace_pio_path, "PIO");
    g_mmio_trace = trace_open(vm->cfg.trace_mmio_path, "MMIO");
}

static void trace_close_all(void)
{
    if (g_pio_trace)
        fclose(g_pio_trace);
    if (g_mmio_trace)
        fclose(g_mmio_trace);
    g_pio_trace = g_mmio_trace = NULL;
}

/* addr_width 是地址的最少十六进制位数：PIO 4、MMIO 8 */
static void trace_line(FILE *f, const char *name, uint64_t addr, int addr_width,
                       bool is_write, uint32_t size, const uint8_t *val,
                       uint64_t rip)
{
    unsigned long long v = 0;
    struct timespec ts;
    uint32_t i;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    for (i = 0; i < size && i < sizeof(v); i++)
        v |= (unsigned long long)val[i] << (8 * i);

    fprintf(f, "%lld.%06ld %-11s %0*llx %c%u %0*llx",
            (long long)ts.tv_sec, ts.tv_nsec / 1000, name, addr_width,
            (unsigned long long)addr, is_write ? 'W' : 'R', size,
            (int)(size * 2), v);
    if (rip)
        fprintf(f, " rip=%016llx", (unsigned long long)rip);
    fputc('\n', f);
}

/* 没有设备的地址才去取一次 RIP。这是一次 vCPU ioctl，只发生在
 * 对应的追踪打开、且访问落在没有设备的地址上的时候。 */
static uint64_t trace_rip(struct vmm_vcpu *vcpu)
{
    struct kvm_regs regs;

    memset(&regs, 0, sizeof(regs));
    if (ioctl(vcpu->fd, KVM_GET_REGS, &regs) < 0)
        return 0;
    return regs.rip;
}

/* 调用方持有 io_lock，见 struct vmm_io_stats */
static void io_stats_count(struct vmm_io_stats *st, bool is_write)
{
    if (is_write)
        st->writes++;
    else
        st->reads++;
}

int io_bus_dispatch_pio(struct vmm_vcpu *vcpu, uint16_t port, bool is_write,
                        uint32_t size, uint32_t count, uint8_t *data)
{
    struct vmm_vm *vm = vcpu->vm;
    const struct vmm_io_dev *dev;
    uint32_t i;

    dev = vmm_lookup_pio(vm, port);
    io_stats_count(dev ? &vm->pio_stats[dev - vm->pio_devs]
                       : &vm->pio_unhandled, is_write);

    if (!dev) {
        /* 未注册端口：写丢弃，读返回全 1（真机上空闲总线的行为）。
         * 不能直接报错退出——Linux 内核启动时会盲探一堆遗留端口。 */
        if (!is_write)
            memset(data, 0xFF, (size_t)size * count);
        vmm_dbg("unhandled PIO %s port=0x%04x size=%u count=%u",
                is_write ? "OUT" : "IN", port, size, count);
        if (g_pio_trace)
            trace_line(g_pio_trace, "unhandled", port, 4, is_write, size,
                       data, trace_rip(vcpu));
        return VMM_OK;
    }

    /* count > 1 出现在 ins/outs 串操作指令上。逐次调设备回调，
     * 每次前进 size 字节。串口载荷用 outsb 批量输出时会走到。 */
    for (i = 0; i < count; i++) {
        uint8_t *p = data + (size_t)i * size;
        uint64_t off = port - dev->base;
        int r;

        if (is_write)
            r = dev->write ? dev->write(dev->opaque, off, size, p) : VMM_OK;
        else
            r = dev->read ? dev->read(dev->opaque, off, size, p) : VMM_OK;
        /* 先记录再判错：设备回调用返回值表示“guest 请求关机/复位”，
         * 触发退出的那一次写恰恰是最值得留在 trace 里的一行。 */
        if (g_pio_trace)
            trace_line(g_pio_trace, dev->name, port, 4, is_write, size, p, 0);
        if (r != VMM_OK)
            return r;
    }
    return VMM_OK;
}

int io_bus_dispatch_mmio(struct vmm_vcpu *vcpu, uint64_t addr, bool is_write,
                         uint32_t len, uint8_t *data)
{
    const struct vmm_io_dev *dev;
    uint64_t off;
    int r;

    dev = vmm_lookup_mmio(vcpu->vm, addr);
    io_stats_count(dev ? &vcpu->vm->mmio_stats[dev - vcpu->vm->mmio_devs]
                       : &vcpu->vm->mmio_unhandled, is_write);
    if (!dev) {
        if (!is_write)
            memset(data, 0xFF, len);
        vmm_dbg("unhandled MMIO %s addr=0x%llx len=%u",
                is_write ? "W" : "R", (unsigned long long)addr, len);
        if (g_mmio_trace)
            trace_line(g_mmio_trace, "unhandled", addr, 8, is_write, len,
                       data, trace_rip(vcpu));
        return VMM_OK;
    }

    off = addr - dev->base;
    if (is_write)
        r = dev->write ? dev->write(dev->opaque, off, len, data) : VMM_OK;
    else
        r = dev->read ? dev->read(dev->opaque, off, len, data) : VMM_OK;
    /* 先记录再返回，理由同 PIO：触发停机的那一次写也要留在 trace 里 */
    if (g_mmio_trace)
        trace_line(g_mmio_trace, dev->name, addr, 8, is_write, len, data, 0);
    return r;
}

/* 以下两个是 KVM_EXIT 路径上的薄封装：只负责从 kvm_run 里把字段取出来。 */

static int handle_io(struct vmm_vcpu *vcpu)
{
    struct kvm_run *run = vcpu->run;
    uint8_t *data;

    /* 数据缓冲区在 kvm_run 页内，偏移由内核填在 io.data_offset。
     * 不能假设它紧跟在结构体后面——必须按 data_offset 算。 */
    data = (uint8_t *)run + run->io.data_offset;

    return io_bus_dispatch_pio(vcpu, (uint16_t)run->io.port,
                               run->io.direction == KVM_EXIT_IO_OUT,
                               run->io.size, run->io.count, data);
}

static int handle_mmio(struct vmm_vcpu *vcpu)
{
    struct kvm_run *run = vcpu->run;

    return io_bus_dispatch_mmio(vcpu, run->mmio.phys_addr,
                                run->mmio.is_write != 0,
                                run->mmio.len, run->mmio.data);
}

static void report_internal_error(struct vmm_vcpu *vcpu)
{
    struct kvm_run *run = vcpu->run;
    uint32_t i;

    vmm_err("KVM_EXIT_INTERNAL_ERROR suberror=%u ndata=%u",
            run->internal.suberror, run->internal.ndata);

    switch (run->internal.suberror) {
    case KVM_INTERNAL_ERROR_EMULATION:
        vmm_err("  KVM tried to emulate an instruction in software and failed.");
        vmm_err("  likely causes: RIP does not point at a valid "
                "instruction (payload not loaded? wrong entry "
                "address? broken page table?)");
        if (run->emulation_failure.flags &
            KVM_INTERNAL_ERROR_EMULATION_FLAG_INSTRUCTION_BYTES) {
            fprintf(stderr, "  instruction bytes:");
            for (i = 0; i < run->emulation_failure.insn_size; i++)
                fprintf(stderr, " %02x",
                        run->emulation_failure.insn_bytes[i]);
            fputc('\n', stderr);
        }
        break;
    case KVM_INTERNAL_ERROR_SIMUL_EX:
        vmm_err("  multiple simultaneous exceptions");
        break;
    case KVM_INTERNAL_ERROR_DELIVERY_EV:
        vmm_err("  unexpected VM exit while delivering an event");
        break;
    default:
        for (i = 0; i < run->internal.ndata && i < 16; i++)
            vmm_err("  data[%u] = 0x%llx", i,
                    (unsigned long long)run->internal.data[i]);
        break;
    }
}

int vcpu_handle_exit(struct vmm_vcpu *vcpu)
{
    struct kvm_run *run = vcpu->run;

    vcpu->exits[run->exit_reason < VMM_EXIT_REASON_SLOTS - 1
                ? run->exit_reason : VMM_EXIT_REASON_SLOTS - 1]++;

    switch (run->exit_reason) {
    case KVM_EXIT_IO: {
        int r;

        pthread_mutex_lock(&vcpu->vm->io_lock);
        r = handle_io(vcpu);
        pthread_mutex_unlock(&vcpu->vm->io_lock);
        return r;
    }

    case KVM_EXIT_MMIO: {
        int r;

        pthread_mutex_lock(&vcpu->vm->io_lock);
        r = handle_mmio(vcpu);
        pthread_mutex_unlock(&vcpu->vm->io_lock);
        return r;
    }

    case KVM_EXIT_HLT:
        vmm_info("vCPU %d: HLT, stopping", vcpu->id);
        return VMM_ERR_EXIT;

    case KVM_EXIT_INTR:
        /* 被信号打断，正常继续 */
        return VMM_OK;

    case KVM_EXIT_SHUTDOWN:
        /* x86 上这基本等同于 triple fault。没装 IDT 时，任何异常
         * 都会走到这里，是长模式 payload 最常见的失败形式。 */
        vmm_err("vCPU %d: SHUTDOWN (usually a triple fault)", vcpu->id);
        vcpu_dump_state(vcpu);
        return VMM_ERR_GUEST;

    case KVM_EXIT_FAIL_ENTRY:
        vmm_err("vCPU %d: VM entry failed, reason=0x%llx cpu=%u",
                vcpu->id,
                (unsigned long long)run->fail_entry.hardware_entry_failure_reason,
                run->fail_entry.cpu);
        vcpu_dump_state(vcpu);
        return VMM_ERR_GUEST;

    case KVM_EXIT_INTERNAL_ERROR:
        report_internal_error(vcpu);
        vcpu_dump_state(vcpu);
        return VMM_ERR_GUEST;

    case KVM_EXIT_DEBUG:
        /* 只有 gdbstub 打开 guest debug 后才会出现 */
        if (vcpu->vm->gdb)
            return gdbstub_vcpu_debug_exit(vcpu);
        vmm_err("vCPU %d: KVM_EXIT_DEBUG without gdbstub: exception %u "
                "pc 0x%llx", vcpu->id, run->debug.arch.exception,
                (unsigned long long)run->debug.arch.pc);
        vcpu_dump_state(vcpu);
        return VMM_ERR_GUEST;

    case KVM_EXIT_SYSTEM_EVENT:
        vmm_info("vCPU %d: SYSTEM_EVENT type=%u", vcpu->id,
                 run->system_event.type);
        return VMM_ERR_EXIT;

    default:
        vmm_err("vCPU %d: unhandled exit_reason=%u (%s)", vcpu->id,
                run->exit_reason,
                kvm_exit_reason_str(run->exit_reason));
        vcpu_dump_state(vcpu);
        return VMM_ERR_GUEST;
    }
}

int vcpu_run_loop(struct vmm_vcpu *vcpu)
{
    int r;

    while (!vcpu->should_stop) {
        r = kvm_run(vcpu->fd);
        /* 被打断时 kvm_run 页里的 exit_reason 不一定有效（immediate_exit
         * 路径下 KVM 不会改写它，还是上一次的值），不能交给
         * vcpu_handle_exit，否则会把上一次的 IO 再执行一遍 */
        if (r == VMM_ERR_INTR) {
            /* AP 唤醒（EAGAIN）和停机（EINTR）都走这条路，每个 vCPU 整个
             * 生命周期里只会走几次，所以在这里查一次 mp_state 的代价可以
             * 忽略，而它是判断 AP 卡在哪一步的决定性证据：
             *   UNINITIALIZED   还没收到 INIT，查 MADT 里这颗 CPU 的登记
             *   INIT_RECEIVED   收到 INIT 在等 SIPI
             *   RUNNABLE        已经在跑或正要跑
             * 必须在本 vCPU 的线程里调，见 kvm_get_mp_state() 的说明。 */
            if (vmm_log_level >= 3) {
                uint32_t st;

                if (kvm_get_mp_state(vcpu->fd, &st) == VMM_OK)
                    vmm_dbg("vCPU %d: KVM_RUN interrupted, mp_state=%u (%s)",
                            vcpu->id, st, kvm_mp_state_str(st));
            }
            /* gdb 要求暂停时停在这里。这一刻挂起的 IO 已经被 KVM 完成，
             * 寄存器是一致的，见 gdb/gdbstub.h */
            if (vcpu->vm->gdb)
                gdbstub_vcpu_interrupted(vcpu);
            continue;
        }
        if (r != VMM_OK)
            return r;

        r = vcpu_handle_exit(vcpu);
        if (r == VMM_ERR_EXIT)
            return VMM_OK;      /* 正常停机 */
        if (r != VMM_OK)
            return r;
    }
    return VMM_OK;
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

int vm_create_vcpu(struct vmm_vm *vm, int id)
{
    struct vmm_vcpu *v;
    int r;

    if (id < 0 || id >= VMM_MAX_VCPUS)
        return VMM_ERR_INVAL;

    v = &vm->vcpus[id];
    memset(v, 0, sizeof(*v));
    v->vm = vm;
    v->id = id;

    v->fd = kvm_create_vcpu(vm->vm_fd, id);
    if (v->fd < 0)
        return VMM_ERR_SYS;

    v->run_size = vm->vcpu_mmap_size;
    v->run = kvm_mmap_run(v->fd, v->run_size);
    if (!v->run) {
        close(v->fd);
        v->fd = -1;
        return VMM_ERR_SYS;
    }

    if (vm->arch->init_vcpu) {
        r = vm->arch->init_vcpu(v);
        if (r != VMM_OK)
            return r;
    }

    if (id + 1 > vm->nr_vcpus)
        vm->nr_vcpus = id + 1;

    vmm_dbg("vCPU %d created (fd=%d, kvm_run=%p, %zu bytes)",
            id, v->fd, (void *)v->run, v->run_size);
    return VMM_OK;
}

int vm_create(struct vmm_vm *vm, const struct vmm_config *cfg)
{
    int r, i;

    memset(vm, 0, sizeof(*vm));
    vm->kvm_fd = vm->vm_fd = -1;
    vm->cfg = *cfg;
    pthread_mutex_init(&vm->io_lock, NULL);
    trace_open_all(vm);

    /* 1. 打开 /dev/kvm */
    vm->kvm_fd = kvm_open();
    if (vm->kvm_fd < 0)
        return VMM_ERR_SYS;

    /* 2. API 版本必须精确等于 12。KVM 从 2007 年定下这个数字后再没
     *    改过——新功能全部通过 KVM_CHECK_EXTENSION 增量协商，
     *    而不是升版本号。所以不等于 12 就说明环境根本不对。 */
    vm->api_version = kvm_get_api_version(vm->kvm_fd);
    if (vm->api_version != KVM_API_VERSION) {
        vmm_err("KVM API version %d, expected %d",
                vm->api_version, KVM_API_VERSION);
        goto fail;
    }

    if (!kvm_check_extension(vm->kvm_fd, KVM_CAP_USER_MEMORY)) {
        vmm_err("kernel does not support KVM_CAP_USER_MEMORY");
        goto fail;
    }

    vm->vcpu_mmap_size = (size_t)kvm_get_vcpu_mmap_size(vm->kvm_fd);
    if ((ssize_t)vm->vcpu_mmap_size < (ssize_t)sizeof(struct kvm_run)) {
        vmm_err("KVM_GET_VCPU_MMAP_SIZE=%zu is smaller than "
                "sizeof(kvm_run)=%zu; the linux/kvm.h used at build "
                "time is probably newer than the running kernel",
                vm->vcpu_mmap_size, sizeof(struct kvm_run));
        goto fail;
    }

    r = kvm_check_extension(vm->kvm_fd, KVM_CAP_MAX_VCPUS);
    vmm_info("KVM API v%d, kvm_run size %zu bytes, max vCPUs %d",
             vm->api_version, vm->vcpu_mmap_size, r);
    if (r > 0 && cfg->nr_vcpus > r) {
        vmm_err("%d vCPUs requested, KVM allows at most %d",
                cfg->nr_vcpus, r);
        goto fail;
    }

    /* 停机协调依赖 kvm_run->immediate_exit，见 smp.c vm_request_stop() */
    if (!kvm_check_extension(vm->kvm_fd, KVM_CAP_IMMEDIATE_EXIT)) {
        vmm_err("kernel does not support KVM_CAP_IMMEDIATE_EXIT (needs 4.11+)");
        goto fail;
    }

    /* 3. 探测 CPU 厂商并校验虚拟化能力 */
    vm->arch = arch_probe();
    if (!vm->arch)
        goto fail;
    if (vm->arch->check_features) {
        r = vm->arch->check_features(vm);
        if (r != VMM_OK)
            goto fail;
    }

    /* 4. 创建 VM */
    vm->vm_fd = kvm_create_vm(vm->kvm_fd);
    if (vm->vm_fd < 0)
        goto fail;

    /* 5. 厂商相关的 VM 级准备（Intel 的 TSS/identity map 在这里） */
    if (vm->arch->prepare_vm) {
        r = vm->arch->prepare_vm(vm);
        if (r != VMM_OK)
            goto fail;
    }

    /* 6. 分配并注册 guest 物理内存 */
    r = mem_init(vm, cfg->ram_size);
    if (r != VMM_OK)
        goto fail;

    /* 7. Linux 需要中断控制器和 PIT，且必须在创建 vCPU 之前建好：
     *    KVM 在 KVM_CREATE_VCPU 时才决定是否给 vCPU 挂内核态 LAPIC。 */
    if (cfg->mode == VMM_BOOT_LINUX) {
        r = irqchip_create(vm);
        if (r != VMM_OK)
            goto fail;
    }

    /* 8. 创建 vCPU */
    for (i = 0; i < cfg->nr_vcpus; i++) {
        r = vm_create_vcpu(vm, i);
        if (r != VMM_OK)
            goto fail;
    }

    vm->running = true;
    return VMM_OK;

fail:
    vm_destroy(vm);
    return VMM_ERR_SYS;
}

void vm_destroy(struct vmm_vm *vm)
{
    int i;

    for (i = 0; i < VMM_MAX_VCPUS; i++) {
        struct vmm_vcpu *v = &vm->vcpus[i];
        if (v->run)
            munmap(v->run, v->run_size);
        if (v->fd > 0)
            close(v->fd);
        v->run = NULL;
        v->fd = -1;
    }

    vm_power_on_free(vm);
    mem_destroy(vm);

    if (vm->vm_fd >= 0)
        close(vm->vm_fd);
    if (vm->kvm_fd >= 0)
        close(vm->kvm_fd);
    vm->vm_fd = vm->kvm_fd = -1;
    vm->running = false;
    trace_close_all();
}
