/* tests/unit/kvm_msr_test.c — kvm_get_msr / kvm_set_msr 的返回值判断
 *
 * 判据来自 Documentation/virt/kvm/api.rst 对 KVM_GET_MSRS / KVM_SET_MSRS
 * 的规定：返回值是成功读写的 MSR 个数。遇到读不了或写不进的 MSR，KVM
 * 停在那一项并返回已处理的个数，不报 -1；只有整个调用出错（参数错、fd
 * 不对）才返回 -1。所以只处理一项时，返回 0 也是失败。
 *
 * 这里把 ioctl 换成桩：按测试设定返回 1、0 或 -1，读到的数据填一个特征
 * 值。用真 KVM 测不稳：内核参数 kvm.ignore_msrs=1 时，读不存在的 MSR 也
 * 返回成功，测试结果会随 host 配置变。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>

#include "vmm.h"
#include "kvm_wrappers.h"
#include "harness.h"

int vmm_log_level = 0;
void vmm_log(int level, const char *file, int line, const char *fmt, ...)
{
    (void)level; (void)file; (void)line; (void)fmt;
}

#define MSR_DATA 0x1122334455667788ULL

static int g_ret;                /* 桩 ioctl 的返回值 */

int ioctl(int fd, unsigned long req, ...)
{
    struct kvm_msrs *m;
    va_list ap;

    va_start(ap, req);
    m = va_arg(ap, struct kvm_msrs *);
    va_end(ap);
    if (g_ret < 0) {
        errno = EINVAL;
        return -1;
    }
    if (req == KVM_GET_MSRS && g_ret > 0)
        m->entries[0].data = MSR_DATA;
    return g_ret;
}

int main(int argc, char **argv)
{
    uint64_t v;

    T_MAIN_BEGIN();

    T_CASE("get: one MSR processed is success");
    g_ret = 1;
    v = 0;
    CHECK_U64(kvm_get_msr(3, 0x10, &v), VMM_OK, "return");
    CHECK_U64(v, MSR_DATA, "value");

    T_CASE("get: zero MSRs processed is failure, value untouched");
    g_ret = 0;
    v = 7;
    CHECK(kvm_get_msr(3, 0xdeadbeef, &v) != VMM_OK, "an unreadable MSR reported success");
    CHECK_U64(v, 7, "value written on failure");

    T_CASE("get: -1 is failure");
    g_ret = -1;
    CHECK(kvm_get_msr(3, 0x10, &v) != VMM_OK, "ioctl error reported success");

    T_CASE("set: one MSR processed is success");
    g_ret = 1;
    CHECK_U64(kvm_set_msr(3, 0x10, 1), VMM_OK, "return");

    T_CASE("set: zero MSRs processed is failure");
    g_ret = 0;
    CHECK(kvm_set_msr(3, 0xdeadbeef, 1) != VMM_OK, "a rejected MSR write reported success");

    T_CASE("set: -1 is failure");
    g_ret = -1;
    CHECK(kvm_set_msr(3, 0x10, 1) != VMM_OK, "ioctl error reported success");

    return T_SUMMARY();
}
