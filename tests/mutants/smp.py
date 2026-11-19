# tests/mutants/smp.py — 里程碑 smp 的源码变异体，由 tests/mutate.py 读取
#
# CPUID 拓扑只有真实 guest 看得出来，全部是 e2e mutant（suite=），要跑它们：
#   tests/mutate.py smp
# 每个变异体改错一个字段。guest 内核解析 CPUID 时会用后读的叶子覆盖先读的，
# 有的字段填错了 /proc/cpuinfo 和 sysfs 都看不出来，挡住它们的是 smp suite
# 里 cpuidprobe 的逐字段比对；其余的同时会被 guest 侧的拓扑断言或
# "APIC ID mismatch" 告警挡住。
#
# 没有列进来的两类改动在当前开发机上是等价变异体，跑了也只会 SURVIVED：
#   - 不清 0x1F：host 的最大基本叶是 0xD，KVM 不给 0x1F，guest 也不读它；
#   - smp_warn_llc_split() 永不警告：host 是 Zen3，本来就不该警告。
# 换到最大基本叶 >= 0x1F 的 host、或者海光 / Zen1 host 上，它们才有意义。

SMP = "src/smp.c"

MUTANTS = [
    dict(
        name="leaf1-apicid-zero",
        breaks="CPUID.1:EBX[31:24] is each vCPU's initial APIC ID",
        file=SMP,
        old="(id << 24)",
        new="(0u << 24)",
        suite="smp",
    ),
    dict(
        name="leaf1-nproc-one",
        breaks="CPUID.1:EBX[23:16] counts the logical processors in the package",
        file=SMP,
        old="((n > 255 ? 255 : n) << 16)",
        new="(1u << 16)",
        suite="smp",
    ),
    dict(
        name="leafb-core-shift-zero",
        breaks="0xB core level shift is ceil(log2 n) for 1 socket x n cores",
        file=SMP,
        old="        { bits, n, CPUID_TOPO_TYPE_CORE  },",
        new="        { 0,    n, CPUID_TOPO_TYPE_CORE  },",
        suite="smp",
    ),
    dict(
        name="leafb-edx-differs-per-subleaf",
        breaks="every 0xB subleaf reports the same x2APIC ID in EDX",
        file=SMP,
        old="                      level[sub].type, id);",
        new="                      level[sub].type, id + sub);",
        suite="smp",
    ),
    dict(
        name="leaf80000008-apicidsize-zero",
        breaks="0x80000008:ECX[15:12] carries the core ID width",
        file=SMP,
        old="(order_base2(n) << 12)",
        new="(0u << 12)",
        suite="smp",
    ),
    dict(
        name="leaf80000008-nc-off-by-one",
        breaks="0x80000008:ECX[7:0] is the thread count minus one",
        file=SMP,
        old="(order_base2(n) << 12) | (n - 1);",
        new="(order_base2(n) << 12) | n;",
        suite="smp",
    ),
    dict(
        name="leaf8000001e-apicid-zero",
        breaks="0x8000001E:EAX is each vCPU's extended APIC ID",
        file=SMP,
        old="            e->eax = id;\n            e->ebx = id & 0xff;",
        new="            e->eax = 0;\n            e->ebx = id & 0xff;",
        suite="smp",
    ),
    dict(
        name="leaf8000001e-two-threads",
        breaks="0x8000001E:EBX[15:8] is threads per core minus one (0: no SMT)",
        file=SMP,
        old="            e->ebx = id & 0xff;",
        new="            e->ebx = (id & 0xff) | (1u << 8);",
        suite="smp",
    ),
    dict(
        name="leaf8000001d-l3-private",
        breaks="all vCPUs share one L3 in the 1-socket model",
        file=SMP,
        old="uint32_t share = (level >= 3) ? n - 1 : 0;",
        new="uint32_t share = (level >= 3) ? 0 : 0;",
        suite="smp",
    ),
    dict(
        name="leaf8000001d-l2-shared",
        breaks="L1 and L2 are private to each core",
        file=SMP,
        old="uint32_t share = (level >= 3) ? n - 1 : 0;",
        new="uint32_t share = n - 1;",
        suite="smp",
    ),
    dict(
        name="llc-warn-every-amd",
        breaks="only Hygon and AMD Zen1 guests derive the LLC id from APIC ID bit 3",
        file=SMP,
        old="    if (v == ARCH_VENDOR_AMD && !(family == 0x17 && model <= 0x1f))\n        return;\n",
        new="",
        suite="smp",
    ),
]
