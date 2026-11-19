/* tools/guest/edu/edu_ioctl.h — edu 驱动的字符设备接口（驱动和 eductl 共用）
 *
 * /dev/eduN 上有两条互相独立的路访问设备内部的 4 KiB 缓冲区：
 *   read/write/lseek  CPU 经 BAR0 的 MMIO 窗口直接读写缓冲区，文件偏移就是
 *                     缓冲区内的偏移
 *   EDU_IOC_DMA       让设备的 DMA 引擎在 guest 内存和缓冲区之间搬运，
 *                     驱动等到完成中断才返回
 * 用一条路写、另一条路读，验证的是 DMA 引擎搬运的方向和内容。
 *
 * 只用 linux/ 下的 UAPI 头文件，内核模块和用户态程序都能包含。
 */
#ifndef EDU_IOCTL_H
#define EDU_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define EDU_DMA_DIR_TO_DEVICE    0   /* 用户缓冲区 → 设备缓冲区 */
#define EDU_DMA_DIR_FROM_DEVICE  1   /* 设备缓冲区 → 用户缓冲区 */

struct edu_dma {
	__u32 dir;          /* EDU_DMA_DIR_* */
	__u32 off;          /* 设备缓冲区内的偏移，原样交给设备 */
	__u32 len;          /* 字节数，1..4096；超出缓冲区的组合由设备判定 */
	__u32 pad;          /* 必须为 0 */
	__u64 user_addr;    /* 用户缓冲区地址 */
};

/* 设备当前状态和驱动的计数 */
struct edu_info {
	__u32 irq_mode;     /* 0 = INTx，1 = MSI */
	__u32 pad;
	__u64 irq_count;    /* 驱动的中断处理函数认领的中断数 */
	__u32 dma_done;     /* 设备的 DMA_DONE_COUNT 寄存器 */
	__u32 int_status;   /* 设备的 INT_STATUS 寄存器 */
};

#define EDU_IOC_MAGIC  'E'
/* 执行一次 DMA，等完成中断后返回。设备报 BAD_RANGE 时返回 -EINVAL，
 * BUS_ERROR 时返回 -EIO，等不到完成中断时返回 -ETIMEDOUT */
#define EDU_IOC_DMA    _IOW(EDU_IOC_MAGIC, 1, struct edu_dma)
/* 经 INT_RAISE 置软件中断位（只认 EDU_INT_SOFT_MASK），等中断处理函数
 * 看到这些位之后返回 */
#define EDU_IOC_RAISE  _IOW(EDU_IOC_MAGIC, 2, __u32)
#define EDU_IOC_INFO   _IOR(EDU_IOC_MAGIC, 3, struct edu_info)

#endif /* EDU_IOCTL_H */
