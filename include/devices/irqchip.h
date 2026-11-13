/* include/devices/irqchip.h — 内核态中断控制器（PIC/IOAPIC/LAPIC + PIT）
 *
 * Linux 启动离不开两样东西：一个会产生周期中断的时钟源（PIT），
 * 一条能把设备中断送进 vCPU 的通路（PIC/IOAPIC/LAPIC）。
 * 这两样都交给 KVM 在内核里模拟，用户态只做两件事：
 *   1) KVM_CREATE_IRQCHIP / KVM_CREATE_PIT2 —— 必须在创建 vCPU 之前
 *   2) KVM_IRQ_LINE 拉高/拉低某根 GSI —— 设备（如串口）要中断时调用
 *
 * 副作用：有了内核态 LAPIC 之后，guest 的 HLT 由 KVM 就地处理
 * （阻塞到有中断为止），不再以 KVM_EXIT_HLT 返回用户态。
 */
#ifndef DEVICES_IRQCHIP_H
#define DEVICES_IRQCHIP_H

#include <stddef.h>
#include <stdint.h>

struct vmm_vm;

int  irqchip_create(struct vmm_vm *vm);

/* 设置 GSI 电平；level=1 拉高，0 拉低 */
int  irqchip_set_irq(struct vmm_vm *vm, int gsi, int level);

/* 给串口等设备用的回调形式，opaque 是 struct vmm_vm * */
void irqchip_irq_cb(void *opaque, int gsi, int level);

/* ---- MSI ----
 *
 * MSI 在总线上只是一次 DWORD 内存写（PCI 3.0 第 6.8 节）。x86 上，写进
 * 0xFEE00000-0xFEEFFFFF 这 1 MiB 的才是中断消息，地址里是目标 APIC，
 * 数据里是向量和投递模式（Intel SDM Vol.3 325384-093US 第 13.11.1、
 * 13.11.2 节）。KVM_SIGNAL_MSI 把一条这样的消息交给内核态 LAPIC。
 *
 * KVM 不检查消息合不合法：它只取地址的 bit 19:12 当目标、数据的 bit 7:0
 * 当向量（arch/x86/kvm/irq_comm.c kvm_set_msi_irq），地址高位是什么都照样
 * 投递。所以格式检查要在 VMM 里做，否则一个写坏的 MSI 地址会变成一次
 * 打到 APIC 0 的中断，guest 看到的是莫名其妙的向量。
 *
 * irqchip_msi_invalid 只看消息本身，返回 NULL 表示合法，否则返回原因。
 * 写成 inline 是为了让 PCI 层的单元测试不链接 KVM 代码也能走到真实的检查。
 * 只拒绝 SDM 明确规定不能出现的值：
 *   - 地址不在中断窗口里：高 32 位非 0，或 bit 31:20 不是 0xFEE。这样的
 *     写在真实机器上落到内存或别的设备，不是中断；
 *   - 投递模式 011、110：保留编码；
 *   - Fixed / Lowest Priority 的向量不在 0x10-0xFE：0x00-0x0F 是异常向量，
 *     SDM 要求软件保证不编这些值，投递进去 guest 会把它当成异常处理。
 * 地址 bit 11:4 和数据的保留位不检查：SDM 说保留位“不假定为任何值”，
 * 而且 KVM 的扩展目标 ID 正好借用了地址 bit 11:5，拒绝它们会误伤合法 guest。 */
#define MSI_ADDR_WINDOW          0xFEE00000u
#define MSI_ADDR_WINDOW_MASK     0xFFF00000u
#define MSI_DATA_VECTOR(d)       ((d) & 0xFFu)
#define MSI_DATA_DELIVERY(d)     (((d) >> 8) & 0x7u)
#define MSI_DELIVERY_FIXED       0
#define MSI_DELIVERY_LOWEST      1

static inline const char *irqchip_msi_invalid(uint64_t addr, uint32_t data)
{
    uint32_t dm = MSI_DATA_DELIVERY(data);
    uint32_t vec = MSI_DATA_VECTOR(data);

    if ((addr >> 32) || ((uint32_t)addr & MSI_ADDR_WINDOW_MASK) != MSI_ADDR_WINDOW)
        return "address is outside the interrupt window 0xfee00000-0xfeefffff";
    if (dm == 3 || dm == 6)
        return "reserved delivery mode";
    if ((dm == MSI_DELIVERY_FIXED || dm == MSI_DELIVERY_LOWEST) &&
        (vec < 0x10 || vec > 0xFE))
        return "vector outside 0x10-0xfe";
    return NULL;
}

/* 投递一条已经通过格式检查的消息。投递成功返回 VMM_OK；没有 vCPU 收下时
 * 返回 VMM_ERR_GUEST，失败返回 VMM_ERR_SYS，两种情况 *why 都指向原因。
 * 这里不打日志：原因要和设备的 BDF 一起记，由调用方（PCI 层）来记。 */
int  irqchip_signal_msi(struct vmm_vm *vm, uint64_t addr, uint32_t data,
                        const char **why);

#endif /* DEVICES_IRQCHIP_H */
