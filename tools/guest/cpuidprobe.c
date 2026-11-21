/* tools/guest/cpuidprobe.c — 在 guest 里逐个 CPU 直接执行 CPUID，打印拓扑字段
 *
 * 用法：cpuidprobe
 *   对每个在线 CPU 绑核后执行 CPUID，每个 CPU 打印一行：
 *     CPUID-<cpu> L1=<apic>,<nproc> B0=<eax>,<ebx>,<ecx>,<edx> B1=... B2=...
 *       E8=<ecx> E1E=<eax>,<ebx>,<ecx> L3=<threads>
 *   L1   CPUID.1:EBX[31:24] initial APIC ID，EBX[23:16] 逻辑处理器数
 *   Bk   CPUID.0xB 子叶 k 的 EAX[4:0]、EBX[15:0]、ECX[15:0]、EDX
 *   E8   CPUID.0x80000008:ECX 的 [15:12] 和 [7:0]（ApicIdSize、NC），其余位清零
 *   E1E  CPUID.0x8000001E 的 EAX、EBX[15:0]、ECX[10:0]
 *   L3   CPUID.0x8000001D 里 level 3 那一级的共享线程数（EAX[25:14] + 1），
 *        没有 L3 时为 0
 *   全部十进制。叶子超出 CPUID.0 / 0x80000000 报告的最大值时，该项打印 "-"。
 *
 * 为什么要有它：guest 内核解析 CPUID 时会用后读到的叶子覆盖先读到的
 * （0xB 子叶 0 的 EDX 覆盖 CPUID.1 的 initial APIC ID），/proc/cpuinfo 只
 * 反映最后胜出的那一个，VMM 填错的字段可能被盖住而看不出来。这里绕过内核，
 * 直接读 VMM 经 KVM_SET_CPUID2 交给每颗 vCPU 的原始值，由测试按规范算出
 * 期望值逐项比对。
 */
#define _GNU_SOURCE
#include <cpuid.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>

enum { EXIT_OK = 0, EXIT_FAIL = 1 };

static void cpuid_sub(unsigned leaf, unsigned sub, unsigned r[4])
{
    __cpuid_count(leaf, sub, r[0], r[1], r[2], r[3]);
}

static void probe(int cpu)
{
    unsigned r[4], max_basic, max_ext, k;

    cpuid_sub(0, 0, r);
    max_basic = r[0];
    cpuid_sub(0x80000000, 0, r);
    max_ext = r[0];

    printf("CPUID-%d", cpu);

    cpuid_sub(1, 0, r);
    printf(" L1=%u,%u", r[1] >> 24, (r[1] >> 16) & 0xff);

    for (k = 0; k < 3; k++) {
        if (max_basic < 0xb) {
            printf(" B%u=-", k);
            continue;
        }
        cpuid_sub(0xb, k, r);
        printf(" B%u=%u,%u,%u,%u", k, r[0] & 0x1f, r[1] & 0xffff,
               r[2] & 0xffff, r[3]);
    }

    if (max_ext >= 0x80000008) {
        cpuid_sub(0x80000008, 0, r);
        printf(" E8=%u", r[2] & 0xf0ff);
    } else {
        printf(" E8=-");
    }

    if (max_ext >= 0x8000001e) {
        cpuid_sub(0x8000001e, 0, r);
        printf(" E1E=%u,%u,%u", r[0], r[1] & 0xffff, r[2] & 0x7ff);
    } else {
        printf(" E1E=-");
    }

    if (max_ext >= 0x8000001d) {
        unsigned l3 = 0;

        for (k = 0; k < 16; k++) {
            cpuid_sub(0x8000001d, k, r);
            if ((r[0] & 0x1f) == 0)
                break;
            if (((r[0] >> 5) & 0x7) == 3)
                l3 = ((r[0] >> 14) & 0xfff) + 1;
        }
        printf(" L3=%u", l3);
    } else {
        printf(" L3=-");
    }

    printf("\n");
}

int main(void)
{
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int cpu;

    if (ncpu <= 0) {
        perror("cpuidprobe: sysconf");
        return EXIT_FAIL;
    }
    for (cpu = 0; cpu < ncpu; cpu++) {
        cpu_set_t set;

        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof(set), &set) != 0) {
            fprintf(stderr, "cpuidprobe: sched_setaffinity cpu %d: ", cpu);
            perror(NULL);
            return EXIT_FAIL;
        }
        probe(cpu);
    }
    return EXIT_OK;
}
