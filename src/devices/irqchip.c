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
