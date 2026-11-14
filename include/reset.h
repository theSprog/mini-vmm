/* include/reset.h — 整机复位：guest 请求复位时在同一个进程里回到上电状态
 *
 * 真机按下 reset 键时，CPU、中断控制器和设备都回到上电状态，内存内容不变，
 * 固件重新把 OS 装进来。VMM 做同样的事，不重建 VM：磁盘镜像、TAP、gdb 连接、
 * --exit-stats 的计数都跨越复位保留，和真机上 reset 不断电的行为一致。
 *
 * "上电状态"由四部分组成，各自由谁负责：
 *   - vCPU 和内核态 PIC/IOAPIC/PIT 的状态：KVM 持有，用户态看不到也算不出
 *     复位值（例如 MSR 的复位值和 CPU 型号、KVM 版本有关），所以第一次
 *     KVM_RUN 之前把它们 GET 出来存好（vm_power_on_save），复位时原样 SET
 *     回去。AP 尤其要恢复：guest 不一定先让 AP 停下再复位（Linux 的 sysrq-b
 *     直接复位），AP 不回到 UNINITIALIZED 就会在新装载的内核上接着执行上一次
 *     启动的指令。MSR 要全部存：kvmclock、steal time、async PF 这些 PV 功能
 *     的 MSR 里是 guest 内存地址，KVM 会持续往登记的地址写。AP 收到新内核的
 *     INIT 时 KVM 自己会清掉它们（host 内核 arch/x86/kvm/x86.c 的
 *     kvm_vcpu_reset），BSP 收不到 INIT，只能靠这里恢复，否则新内核重新登记
 *     之前，KVM 还在往上一次启动登记的地址写；
 *   - 中断线电平：KVM 按中断源（source id）分别记在 IOAPIC/PIC 的
 *     irq_states 里，一根 GSI 的电平是各中断源的"或"（host 内核
 *     arch/x86/include/asm/kvm_host.h 的 __kvm_irq_line_state）。它不在
 *     KVM_GET/SET_IRQCHIP 的结构体中（arch/x86/kvm/ioapic.c 的 kvm_set_ioapic
 *     只拷贝 kvm_ioapic_state），复位前拉高着的线，复位后 irq_states 里还是高。
 *     vm_reset 在 SET_IRQCHIP 之前用 KVM_IRQ_LINE 把每根线拉低，让 KVM 记的
 *     电平和设备复位后的 0 一致。目前每根 GSI 上只有 VMM 这一个用户态中断源
 *     （PIT 在 GSI 0 上用自己的 source id），设备下一次 KVM_IRQ_LINE 就会改写
 *     这一位，边沿检测看的是随 SET_IRQCHIP 恢复的 irr，所以残留的位不会造成
 *     可观察的错误；一根 GSI 上有了第二个中断源后，残留的高位会通过"或"让
 *     这根线一直为高，电平触发的引脚在每次 EOI 之后都会被重新送达；
 *   - 用户态设备的寄存器：各设备注册一个复位回调（vmm_register_reset）；
 *   - 装进内存的启动镜像：由调用方的 reload 回调重新装载（内核、initrd、
 *     固件表、页表、BSP 的入口寄存器），因为 guest 运行时会改写这些内存。
 *
 * 时序（vm_reset 的调用条件）：所有 vCPU 线程都已 join。vm_reset 持 io_lock
 * 完成全部步骤，I/O 线程上的设备回调（收包、控制台输入）被挡在外面，
 * 不会看到一半复位的设备，也不会在镜像装载期间 DMA 进 guest 内存。
 */
#ifndef RESET_H
#define RESET_H

#include <stdbool.h>

struct vmm_vm;

#define VMM_MAX_RESET_HANDLERS 32

typedef void (*vmm_reset_fn)(void *opaque);
typedef int  (*vmm_reload_fn)(void *opaque);

struct vmm_reset_handler {
    const char  *name;
    vmm_reset_fn fn;
    void        *opaque;
};

/* 设备 attach 时登记复位回调。复位时持 io_lock 按登记顺序调用，所以 PCI
 * 总线（配置空间）先于挂在它上面的设备复位。回调运行时 KVM 里的中断线
 * 已经全部拉低，设备自己记着的电平也要回到 0，否则它以为线还高着，下一次
 * 拉高时不会调 KVM_IRQ_LINE，中断就丢了。 */
int  vmm_register_reset(struct vmm_vm *vm, const char *name, vmm_reset_fn fn,
                        void *opaque);

/* 第一次 vm_run 之前调用，把每个 vCPU 和内核态 irqchip 的状态存下来 */
int  vm_power_on_save(struct vmm_vm *vm);

/* 设备在 exit 分发路径上（持 io_lock）报告 guest 请求复位。返回
 * VMM_ERR_EXIT，让 vCPU 按停机路径退出。--no-reboot 时整机停下、VMM 退出；
 * 否则置 vm->reset_requested，vm_run 返回后由 main 调 vm_reset 再重新运行。
 * source 进日志，例如 "i8042"。 */
int  vm_request_reset(struct vmm_vm *vm, const char *source);

/* 把整机带回上电状态，见文件头的时序要求。成功后可以直接再调 vm_run。 */
int  vm_reset(struct vmm_vm *vm, vmm_reload_fn reload, void *opaque);

/* vm_destroy 调用，释放 vm_power_on_save 分配的内存 */
void vm_power_on_free(struct vmm_vm *vm);

#endif /* RESET_H */
