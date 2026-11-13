/* src/devices/irqchip.c — 内置 PIC/IOAPIC/LAPIC + PIT 初始化、中断线 */
#define _GNU_SOURCE
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>

#include "vmm.h"
#include "kvm_wrappers.h"
#include "devices/irqchip.h"

int irqchip_create(struct vmm_vm *vm)
{
    struct kvm_pit_config pit;

    if (!kvm_check_extension(vm->kvm_fd, KVM_CAP_IRQCHIP)) {
        vmm_err("kernel does not support KVM_CAP_IRQCHIP");
        return VMM_ERR_UNSUPPORTED;
    }
    if (!kvm_check_extension(vm->kvm_fd, KVM_CAP_SIGNAL_MSI)) {
        vmm_err("kernel does not support KVM_CAP_SIGNAL_MSI");
        return VMM_ERR_UNSUPPORTED;
    }

    /* 一次性创建 2 片 8259 PIC + 1 个 IOAPIC，并让之后创建的每个
     * vCPU 都带一个内核态 LAPIC。GSI 0-15 默认同时路由到 PIC 和
     * IOAPIC，guest 用哪个都行。 */
    if (ioctl(vm->vm_fd, KVM_CREATE_IRQCHIP, 0) < 0) {
        vmm_err("ioctl(KVM_CREATE_IRQCHIP) failed: %s", strerror(errno));
        return VMM_ERR_SYS;
    }

    /* 8254 PIT，挂在 GSI 0。Linux 用它校准 TSC / LAPIC timer。
     *
     * KVM_PIT_SPEAKER_DUMMY 名字有误导性：它的作用是让 KVM 在内核里
     * 接管端口 0x61。0x61 bit0 是 PIT 通道 2 的 GATE，bit5 是通道 2
     * 的 OUT。Linux 的 quick_pit_calibrate / pit_calibrate_tsc 靠
     * 拉 GATE、读 OUT 来测 TSC 频率。不带这个 flag，0x61 会落到用户态，
     * 我们不处理的话 GATE 永远拉不起来，校准间歇性失败，表现为
     * jiffies 停在 0.001000 不动（见 host i8254.c kvm_create_pit）。 */
    memset(&pit, 0, sizeof(pit));
    pit.flags = KVM_PIT_SPEAKER_DUMMY;
    if (ioctl(vm->vm_fd, KVM_CREATE_PIT2, &pit) < 0) {
        vmm_err("ioctl(KVM_CREATE_PIT2) failed: %s", strerror(errno));
        return VMM_ERR_SYS;
    }

    vm->has_irqchip = true;
    vmm_info("in-kernel irqchip: PIC + IOAPIC + LAPIC, PIT on GSI 0");
    return VMM_OK;
}

int irqchip_set_irq(struct vmm_vm *vm, int gsi, int level)
{
    struct kvm_irq_level il;

    if (!vm->has_irqchip || gsi < 0)
        return VMM_OK;

    il.irq   = (uint32_t)gsi;
    il.level = level;
    if (ioctl(vm->vm_fd, KVM_IRQ_LINE, &il) < 0) {
        vmm_err("ioctl(KVM_IRQ_LINE gsi=%d level=%d) failed: %s",
                gsi, level, strerror(errno));
        return VMM_ERR_SYS;
    }
    return VMM_OK;
}

void irqchip_irq_cb(void *opaque, int gsi, int level)
{
    irqchip_set_irq(opaque, gsi, level);
}

/* KVM_SIGNAL_MSI 的返回值（kvm_send_userspace_msi -> kvm_set_msi ->
 * kvm_irq_delivery_to_apic，arch/x86/kvm/irq_comm.c、lapic.c）：
 *   > 0  收下这条消息的 vCPU 个数；
 *   = 0  有匹配的路径但没有 vCPU 收下：快速路径里目标 APIC ID 不存在
 *        （kvm_apic_map_get_dest_lapic 返回空位图），或者目标 LAPIC 被
 *        软件禁用（__apic_accept_irq 里 apic_enabled 为假）；
 *   -1   慢速路径里没有任何 LAPIC 匹配目标。内核把 -1 原样当 ioctl 的
 *        返回值，用户态看到的是 errno == EPERM，不是一个真正的权限错误；
 *   其他负值是真正的错误（例如没有内核态 irqchip 时 EINVAL）。
 * api.rst 把 0 写成“guest 屏蔽了这条 MSI”，上面是对照源码得到的具体情形。
 * 这两种“没人收”对 guest 来说都是中断丢了，原因要报出来。 */
int irqchip_signal_msi(struct vmm_vm *vm, uint64_t addr, uint32_t data,
                       const char **why)
{
    struct kvm_msi msi;
    int r;

    if (!vm->has_irqchip) {
        *why = "no in-kernel irqchip";
        return VMM_ERR_SYS;
    }
    memset(&msi, 0, sizeof(msi));
    msi.address_lo = (uint32_t)addr;
    msi.address_hi = (uint32_t)(addr >> 32);
    msi.data       = data;
    r = ioctl(vm->vm_fd, KVM_SIGNAL_MSI, &msi);
    if (r > 0)
        return VMM_OK;
    if (r == 0) {
        *why = "no vCPU accepted it (no LAPIC with that destination ID, or it is software-disabled)";
        return VMM_ERR_GUEST;
    }
    if (errno == EPERM) {
        *why = "no LAPIC matches the destination";
        return VMM_ERR_GUEST;
    }
    vmm_err("ioctl(KVM_SIGNAL_MSI addr=0x%llx data=0x%x) failed: %s",
            (unsigned long long)addr, data, strerror(errno));
    *why = "KVM_SIGNAL_MSI failed";
    return VMM_ERR_SYS;
}
