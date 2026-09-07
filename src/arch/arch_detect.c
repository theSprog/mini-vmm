/* src/arch/arch_detect.c — 运行时 CPU 厂商探测 */
#include <string.h>
#include "vmm.h"
#include "arch/arch.h"

void arch_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4])
{
    uint32_t a, b, c, d;

    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(leaf), "c"(subleaf));
    out[0] = a; out[1] = b; out[2] = c; out[3] = d;
}

/* CPUID leaf 0 返回的 vendor string 顺序是 EBX:EDX:ECX，
 * 不是直觉上的 EBX:ECX:EDX —— 这是 x86 的历史遗留，抄错会导致
 * "GenuntelineI" 这种经典乱序结果。 */
enum arch_vendor arch_detect_vendor(char buf[13])
{
    uint32_t r[4];

    arch_cpuid(0, 0, r);
    memcpy(buf + 0, &r[1], 4);   /* EBX */
    memcpy(buf + 4, &r[3], 4);   /* EDX */
    memcpy(buf + 8, &r[2], 4);   /* ECX */
    buf[12] = '\0';

    if (!strcmp(buf, "AuthenticAMD"))
        return ARCH_VENDOR_AMD;
    if (!strcmp(buf, "HygonGenuine"))
        return ARCH_VENDOR_HYGON;
    if (!strcmp(buf, "GenuineIntel"))
        return ARCH_VENDOR_INTEL;
    return ARCH_VENDOR_UNKNOWN;
}

const char *arch_vendor_str(enum arch_vendor v)
{
    switch (v) {
    case ARCH_VENDOR_AMD:   return "AMD";
    case ARCH_VENDOR_HYGON: return "Hygon";
    case ARCH_VENDOR_INTEL: return "Intel";
    default:                return "Unknown";
    }
}

static const struct arch_cpu_ops *const all_ops[] = {
    &arch_ops_amd,
    /* &arch_ops_intel, */
};

const struct arch_cpu_ops *arch_probe(void)
{
    char vendor[13];
    enum arch_vendor v = arch_detect_vendor(vendor);
    size_t i;

    vmm_info("CPU vendor: %s (%s)", vendor, arch_vendor_str(v));

    for (i = 0; i < sizeof(all_ops) / sizeof(all_ops[0]); i++)
        if (all_ops[i]->match(v))
            return all_ops[i];

    vmm_err("no matching arch backend (vendor=%s)", vendor);
    if (v == ARCH_VENDOR_INTEL)
        vmm_err("  Intel/VMX backend is not implemented yet, see src/arch/intel/");
    return NULL;
}
