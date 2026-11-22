/* src/io_bus.c — PIO/MMIO 地址表、访问分发、exit 计数和 --trace-pio/--trace-mmio
 *
 * KVM_EXIT_IO / KVM_EXIT_MMIO 交到用户态之后，这里按地址找到设备、调它的
 * 回调。地址表、按设备的 exit 计数和 trace 文件都只在持有 vm->io_lock 时
 * 读写，见 include/io_bus.h。 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>

#include "vmm.h"

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

void io_trace_open(struct vmm_vm *vm)
{
    g_pio_trace = trace_open(vm->cfg.trace_pio_path, "PIO");
    g_mmio_trace = trace_open(vm->cfg.trace_mmio_path, "MMIO");
}

void io_trace_close(void)
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
