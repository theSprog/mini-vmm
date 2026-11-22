/* include/devices/edu.h — edu 教学 PCI 设备的设备模型（--device edu）
 *
 * 寄存器、DMA 和中断的语义由 devices/edu_regs.h 规定，guest 驱动也按它写。
 * 这里只说明模型自己的结构。
 *
 * 异步完成：guest 写 DMA_CMD.START 时，vCPU 线程只取下参数、置“执行中”，
 * 然后写一个 eventfd；真正的搬运和中断在 I/O 线程的回调里做。这样 START 的
 * MMIO 写返回时命令确实还没完成，驱动必须等中断或轮询 START 位，按“写完
 * 寄存器数据就到位”写的驱动会读到旧数据而被测出来。如果在 vCPU 线程里
 * 同步做完，中断甚至会在写 START 的那条指令退休之前就挂上，驱动不等中断
 * 也总能读到正确结果，这一类驱动错误就永远暴露不出来。
 *
 * 并发：寄存器、缓冲区和执行状态只在 vm->io_lock 下访问。BAR 的读写经 exit
 * 分发进来时已经持着它；I/O 线程的完成回调先拿 io_lock 再动设备；整机复位
 * 也持着 io_lock，所以复位丢弃的命令不会在复位之后又被完成：回调拿到锁时
 * 看到的“执行中”已经被复位清掉，什么都不做。
 */
#ifndef DEVICES_EDU_H
#define DEVICES_EDU_H

#include <stdbool.h>
#include <stdint.h>

#include "pci/pci.h"
#include "devices/device.h"
#include "devices/edu_regs.h"

struct event_loop;

struct edu {
    struct pci_device pci;
    struct vmm_vm    *vm;
    int               wake_fd;        /* eventfd：vCPU 线程通知 I/O 线程执行命令 */

    uint32_t scratch;
    uint32_t int_status;
    uint32_t dma_addr_lo, dma_addr_hi;
    uint32_t dma_off, dma_len;
    uint32_t dma_cmd;                 /* START 位为 1 表示执行中 */
    uint32_t dma_result;
    uint32_t dma_done_count;

    /* 写 START 时取下的参数，完成回调按它执行 */
    uint64_t run_addr;
    uint32_t run_off, run_len, run_cmd;

    uint8_t  buf[EDU_BUF_SIZE];
};

/* 注册到 PCI 总线并把 eventfd 挂到事件循环上。e 由调用方分配，生命周期
 * 覆盖整个 VM 运行期。必须在 evl_start 之前调用。 */
int  edu_attach(struct edu *e, struct vmm_vm *vm, struct pci_bus *bus,
                struct event_loop *loop);

/* 整机复位回调（reset.h），opaque 是 struct edu * */
void edu_reset(void *opaque);

/* --device edu，没有 driver 以外的属性 */
extern const struct vmm_device_type edu_device_type;

#endif /* DEVICES_EDU_H */
