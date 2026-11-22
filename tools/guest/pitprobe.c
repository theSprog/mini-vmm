/* tools/guest/pitprobe.c — 在 guest 用户态重放 Linux 的 PIT 慢路 TSC 校准
 *
 * 用法：pitprobe [ROUNDS]。tools/build_initramfs.sh 把它静态编译后放进
 * initramfs 的 /sbin。
 *
 * 为什么需要它：Linux 的 pit_calibrate_tsc（arch/x86/kernel/tsc.c）只告诉
 * 我们“成功”或“Unable to calibrate against PIT”，看不到它为什么判废。这里
 * 照抄同一个循环：开通道 2 的 GATE、关扬声器（端口 0x61 bit0=1、bit1=0），
 * 通道 2 设成 mode 0 装 CAL_LATCH（10ms），然后不停地读 0x61 直到 bit5
 * （OUT2）变 1，每读一次记一次 TSC 差值。每一轮打印读的次数、最小/最大
 * 间隔，以及按内核的两条判据（次数不少于 1000、最大间隔不超过最小间隔的
 * 10 倍）这一轮会不会被判废。
 *
 * 和内核的差别：内核在关中断时跑这个循环，用户态不能关中断（iopl(3) 从
 * Linux 5.5 起由 I/O 位图模拟，不再给 CLI/STI 权限），guest 自己的 tick
 * 会落进来。所以判废率只能在同一 guest 配置下横向比较 irqchip 模式，不能
 * 当成内核校准的成功率。
 *
 * 窗口长度是 PIT 的 10ms（11931 个 1.193182 MHz 时钟），用 TSC 量出来的
 * 周期数除以 10000 就是 TSC 的 MHz：和内核报的 "tsc: Detected" 对得上，
 * 说明 PIT 通道 2 的计时是准的。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <x86intrin.h>

enum { EXIT_OK = 0, EXIT_FAIL = 1, EXIT_USAGE = 2 };

/* arch/x86/kernel/tsc.c 和 include/linux/timex.h 里的同名常量 */
#define PIT_TICK_RATE   1193182u
#define CAL_MS          10u
#define CAL_LATCH       (PIT_TICK_RATE / (1000u / CAL_MS))
#define CAL_PIT_LOOPS   1000u

/* 一轮读不完说明 OUT2 永远不起来（GATE2 没拉高、mode 0 没到终点），不能
 * 一直转下去；正常一轮在几千次以内 */
#define MAX_READS       10000000u

static void usage(FILE *out)
{
    fprintf(out,
"Usage: pitprobe [ROUNDS]\n"
"\n"
"Replay Linux's slow-path TSC calibration loop (pit_calibrate_tsc) ROUNDS\n"
"times (default 10) from user space: PIT channel 2 in mode 0 counts 10 ms\n"
"while port 0x61 is polled for OUT2.\n"
"\n"
"Per round prints: reads, min/max TSC cycles between reads, window length\n"
"in TSC cycles, and whether the kernel's checks (reads >= 1000 and\n"
"max <= 10 * min) would accept it. The last line summarizes all rounds.\n"
"\n"
"EXAMPLE:\n"
"  pitprobe 20\n");
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

    return x < y ? -1 : x > y;
}

static uint64_t median(uint64_t *v, unsigned long n)
{
    qsort(v, n, sizeof(*v), cmp_u64);
    return v[n / 2];
}

struct round {
    uint32_t reads;
    uint64_t min, max, window;
    int ok;
};

static int one_round(struct round *r)
{
    uint64_t t1, t2, tsc, delta;

    outb((inb(0x61) & ~0x02) | 0x01, 0x61);
    outb(0xb0, 0x43);                   /* 通道 2，先低后高字节，mode 0，二进制 */
    outb(CAL_LATCH & 0xff, 0x42);
    outb(CAL_LATCH >> 8, 0x42);

    tsc = t1 = t2 = __rdtsc();
    r->reads = 0;
    r->min = UINT64_MAX;
    r->max = 0;
    while ((inb(0x61) & 0x20) == 0) {
        t2 = __rdtsc();
        delta = t2 - tsc;
        tsc = t2;
        if (delta < r->min)
            r->min = delta;
        if (delta > r->max)
            r->max = delta;
        if (++r->reads >= MAX_READS)
            return -1;
    }
    r->window = t2 - t1;
    r->ok = r->reads >= CAL_PIT_LOOPS && r->max <= 10 * r->min;
    return 0;
}

int main(int argc, char **argv)
{
    struct round r;
    unsigned long rounds = 10;
    unsigned long i, ok = 0;
    static uint64_t mins[1000], windows[1000];
    uint64_t win_med;
    char *end;

    if (argc > 2 || (argc == 2 && (!strcmp(argv[1], "-h") ||
                                   !strcmp(argv[1], "--help")))) {
        usage(argc > 2 ? stderr : stdout);
        return argc > 2 ? EXIT_USAGE : EXIT_OK;
    }
    if (argc == 2) {
        errno = 0;
        rounds = strtoul(argv[1], &end, 10);
        if (errno || *end || rounds == 0 || rounds > 1000) {
            fprintf(stderr, "pitprobe: ROUNDS must be 1..1000: %s\n", argv[1]);
            return EXIT_USAGE;
        }
    }
    if (ioperm(0x40, 4, 1) < 0 || ioperm(0x61, 1, 1) < 0) {
        fprintf(stderr, "pitprobe: ioperm: %s\n", strerror(errno));
        return EXIT_FAIL;
    }

    for (i = 0; i < rounds; i++) {
        if (one_round(&r) < 0) {
            printf("round %lu: OUT2 still low after %u reads of port 0x61\n",
                   i, r.reads);
            return EXIT_FAIL;
        }
        printf("round %lu: reads %u min %llu max %llu window %llu %s\n",
               i, r.reads, (unsigned long long)r.min,
               (unsigned long long)r.max, (unsigned long long)r.window,
               r.ok ? "accept" : "reject");
        ok += r.ok;
        mins[i] = r.min;
        windows[i] = r.window;
    }
    /* 取中位数：个别轮次在装数和第一次读 TSC 之间被打断，窗口会偏短。
     * 窗口是 10ms，周期数 / 10000 = TSC 的 MHz */
    win_med = median(windows, rounds);
    printf("pitprobe: %lu/%lu accepted, median min read %llu cycles, "
           "median window %llu cycles = %llu MHz\n",
           ok, rounds, (unsigned long long)median(mins, rounds),
           (unsigned long long)win_med,
           (unsigned long long)(win_med / (CAL_MS * 1000)));
    return EXIT_OK;
}
