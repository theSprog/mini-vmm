/* tools/guest/edu/edu_ioctl.h — edu 驱动的字符设备接口（驱动和 eductl 共用）
 *
 * /dev/eduN 上有两条互相独立的路访问设备内部的 4 KiB 缓冲区：
 *   read/write/lseek  CPU 经 BAR0 的 MMIO 窗口直接读写缓冲区，文件偏移就是
 *                     缓冲区内的偏移
 *   EDU_IOC_DMA       让设备的 DMA 引擎在 guest 内存和缓冲区之间搬运，
 *                     驱动等到完成中断才返回
 * 用一条路写、另一条路读，验证的是 DMA 引擎搬运的方向和内容。
 *
 * 只用 linux/ 下的 UAPI 头文件和只有宏的 edu_regs.h，内核模块和用户态程序
 * 都能包含。
 */
#ifndef EDU_IOCTL_H
#define EDU_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#include "edu_regs.h"

#define EDU_DMA_DIR_TO_DEVICE    0   /* 用户缓冲区 → 设备缓冲区 */
#define EDU_DMA_DIR_FROM_DEVICE  1   /* 设备缓冲区 → 用户缓冲区 */

struct edu_dma {
	__u32 dir;          /* EDU_DMA_DIR_* */
	__u32 off;          /* 设备缓冲区内的偏移，原样交给设备 */
	__u32 len;          /* 字节数，1..4096；超出缓冲区的组合由设备判定 */
	__u32 pad;          /* 必须为 0 */
	__u64 user_addr;    /* 用户缓冲区地址 */
};

#define EDU_MODE_INTX  0
#define EDU_MODE_MSI   1
#define EDU_MODE_MSIX  2

/* 设备当前状态和驱动的计数。INTx 和 MSI 只有向量 0，它负责 INT_STATUS 的
 * 全部位；MSI-X 有 EDU_MSIX_VECTORS 个向量，第 n 个负责
 * EDU_INT_VEC_MASK(n) 那个字节。 */
struct edu_info {
	__u32 irq_mode;     /* EDU_MODE_* */
	__u32 nvec;         /* 申请到的向量数 */
	__u64 irq_count;    /* 各向量认领的中断数之和 */
	__u32 dma_done;     /* 设备的 DMA_DONE_COUNT 寄存器 */
	__u32 int_status;   /* 设备的 INT_STATUS 寄存器 */
	__u64 vec_handled[EDU_MSIX_VECTORS];  /* 每个向量认领（IRQ_HANDLED）的次数 */
	__u64 vec_calls[EDU_MSIX_VECTORS];    /* 每个向量的处理函数被调用的次数，含 IRQ_NONE */
};

/* EDU_IOC_MASKTEST 的三种做法，只在 MSI-X 下可用。vector 取 1..3（软件
 * 中断所在的字节）。驱动在内核里按下面的次序做完，只记事实不下结论，
 * 判断由用户态做：
 *   VECTOR    屏蔽第 vector 项（disable_irq，立即写表项的 Mask 位），置它
 *             负责的一个软件中断位，读 PBA；解除屏蔽（enable_irq），等处理
 *             函数看到那一位，再读 PBA
 *   FUNCTION  置 MSI-X Function Mask，在第 1 到 3 项各置一个软件中断位，
 *             读 PBA；清 Function Mask，等三位都被看到，再读 PBA
 *   ACKED     屏蔽第 vector 项，置位，读 PBA；直接写 INT_ACK 把它清掉，读
 *             PBA；解除屏蔽，再经正常路径置同一位并等它被看到，读 PBA */
#define EDU_MASKTEST_VECTOR    0
#define EDU_MASKTEST_FUNCTION  1
#define EDU_MASKTEST_ACKED     2

struct edu_masktest {
	__u32 kind;         /* in：EDU_MASKTEST_* */
	__u32 vector;       /* in：VECTOR 和 ACKED 用 */
	__u64 pba_masked;   /* out：屏蔽期间、中断原因出现之后的 PBA */
	__u64 pba_acked;    /* out：ACKED 里写完 INT_ACK 之后的 PBA，其余为 0 */
	__u64 pba_after;    /* out：结束时的 PBA */
	__u64 calls_masked[EDU_MSIX_VECTORS];  /* out：开始到读 pba_masked 之间各向量处理函数被调用的次数 */
	__u64 calls_after[EDU_MSIX_VECTORS];   /* out：整个过程里各向量处理函数被调用的次数 */
};

#define EDU_IOC_MAGIC  'E'
/* 执行一次 DMA，等完成中断后返回。设备报 BAD_RANGE 时返回 -EINVAL，
 * BUS_ERROR 时返回 -EIO，等不到完成中断时返回 -ETIMEDOUT */
#define EDU_IOC_DMA    _IOW(EDU_IOC_MAGIC, 1, struct edu_dma)
/* 经 INT_RAISE 置软件中断位（只认 EDU_INT_SOFT_MASK），等中断处理函数
 * 看到这些位之后返回 */
#define EDU_IOC_RAISE  _IOW(EDU_IOC_MAGIC, 2, __u32)
#define EDU_IOC_INFO   _IOR(EDU_IOC_MAGIC, 3, struct edu_info)
/* 屏蔽与 pending 位的测试，见 EDU_MASKTEST_*。不在 MSI-X 下返回
 * -EOPNOTSUPP，参数不对返回 -EINVAL，等不到中断返回 -ETIMEDOUT */
#define EDU_IOC_MASKTEST _IOWR(EDU_IOC_MAGIC, 4, struct edu_masktest)

#endif /* EDU_IOCTL_H */
