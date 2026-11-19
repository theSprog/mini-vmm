/* include/devices/edu_regs.h — edu 教学 PCI 设备的寄存器规范
 *
 * 这份文件就是设备的规范：VMM 的设备模型（src/devices/edu.c）和 guest 的
 * 驱动（tools/guest/edu/）都只依据这里的定义，两边共用这一个头文件，常量
 * 不会各写一份而对不上。它只有宏，不依赖任何头文件，所以内核模块和用户态
 * 程序都能直接包含。
 *
 * 设备做三件事：一组可读写的寄存器，一个在 guest 内存和设备内部缓冲区之间
 * 搬数据的 DMA 引擎，一个由软件或 DMA 完成触发的中断。
 *
 * ---- PCI 身份 ----
 *
 *   vendor 0x1234  device 0x0003  class 0xFF0000（不属于任何已定义类别）
 *   INTA 引脚，一个 MSI capability（单向量、64 位地址）
 *   BAR0  32 位、不可预取的 MMIO，8 KiB
 *
 * 0x1234 不在 PCI-SIG 的 vendor 分配表里。Linux 里按这个 vendor 匹配的只有
 * bochs 显卡驱动（device 0x1111），不会和本设备冲突。
 *
 * ---- BAR0 布局 ----
 *
 *   0x0000-0x0FFF  寄存器，只接受对齐的 4 字节访问；其他宽度或不对齐的访问
 *                  读回全 1、写被丢弃，设备记一条日志
 *   0x1000-0x1FFF  设备内部缓冲区（4 KiB），任意宽度、任意偏移都能读写
 *
 * 缓冲区经 MMIO 直接可见，驱动因此有两条互相独立的路去读写同一块数据：
 * CPU 经 MMIO 访问，或者让设备 DMA。拿一条路写、另一条路读，就能验证
 * DMA 引擎搬的方向和内容，而不必相信 DMA 自己的结果。
 *
 *   偏移    名字            访问  含义
 *   0x000   ID              RO    EDU_ID_VALUE
 *   0x004   SCRATCH         RW    写什么读回什么，复位为 0
 *   0x008   INT_STATUS      RO    待处理的中断原因，位定义见 EDU_INT_*
 *   0x00C   INT_RAISE       WO    写 1 的位置进 INT_STATUS，只认 EDU_INT_SOFT_MASK
 *                                 里的位（读回 0）
 *   0x010   INT_ACK         WO    写 1 的位从 INT_STATUS 清掉（读回 0）
 *   0x020   DMA_ADDR_LO     RW    DMA 的 guest 一侧：总线地址的低 32 位
 *   0x024   DMA_ADDR_HI     RW                     高 32 位
 *   0x028   DMA_OFF         RW    DMA 的设备一侧：缓冲区内的字节偏移
 *   0x02C   DMA_LEN         RW    字节数
 *   0x030   DMA_CMD         RW    见 EDU_DMA_CMD_*
 *   0x034   DMA_RESULT      RO    最近一次完成的 DMA 的结果，见 EDU_DMA_RES_*
 *   0x038   DMA_DONE_COUNT  RO    复位以来完成的 DMA 命令数（含失败的）
 *   其余                    RO    读 0，写丢弃
 *
 * ---- DMA 引擎 ----
 *
 * 软件填好 DMA_ADDR、DMA_OFF、DMA_LEN，再向 DMA_CMD 写入带 START 的命令。
 * 设备在写 START 的那一刻取下这几个寄存器的值，之后再改它们不影响这条命令。
 * DMA_CMD 读回时 START 位表示“正在执行”，命令完成时设备清掉它。
 *
 * 命令是异步完成的：写 START 的 MMIO 访问返回时，搬运通常还没做。驱动要么
 * 等中断，要么轮询 START 位；不能假设写完寄存器数据就到位了。真实设备的
 * DMA 都是这样，按同步写的驱动在真硬件上会读到旧数据。
 *
 * 执行中（START 仍为 1）对 DMA_ADDR、DMA_OFF、DMA_LEN、DMA_CMD 的写被丢弃，
 * 设备记一条日志：一个引擎同一时间只执行一条命令。
 *
 * 方向由 EDU_DMA_CMD_TO_RAM 决定：0 表示从 guest 内存读进缓冲区，1 表示从
 * 缓冲区写到 guest 内存。命令完成时 DMA_RESULT 给出结果：
 *   - DMA_LEN 为 0，或 DMA_OFF + DMA_LEN 超出 4 KiB 缓冲区：BAD_RANGE，
 *     一个字节都不搬；
 *   - 配置空间 command 寄存器的 Bus Master Enable 关着，或 guest 地址范围不完整地落在 RAM 里：
 *     BUS_ERROR，一个字节都不搬；
 *   - 否则搬完全部字节，结果 OK。
 * 不论结果如何，DMA_DONE_COUNT 加 1，START 清掉；命令带 IRQ 位时置
 * INT_STATUS 的 EDU_INT_DMA。
 *
 * ---- 中断 ----
 *
 * INT_STATUS 的每一位是一个待处理的中断原因，由设备（DMA 完成）或软件
 * （INT_RAISE）置位，只由软件写 INT_ACK 清除。
 *   - INTx：INTA 的电平等于 INT_STATUS != 0。电平触发，驱动不清 INT_STATUS
 *     就会一直收到中断。
 *   - MSI：guest 打开 MSI 后，INT_STATUS 每多出一个原来为 0 的位，设备发一条
 *     消息。已经置位、还没被确认的原因再次发生不再发消息，驱动处理时读到
 *     的 INT_STATUS 里已经有它。驱动在中断处理里只清自己读到的那些位，处理
 *     过程中新到的原因留在 INT_STATUS 里，不会丢。
 * 打开 MSI 之后 INTA 不再拉起，这是 PCI 规范对 MSI 的要求。
 *
 * ---- 复位 ----
 *
 * 整机复位把全部寄存器和缓冲区清 0，执行中的命令被丢弃，不产生中断。
 */
#ifndef DEVICES_EDU_REGS_H
#define DEVICES_EDU_REGS_H

#define EDU_VENDOR_ID           0x1234
#define EDU_DEVICE_ID           0x0003
#define EDU_CLASS_CODE          0xFF0000

#define EDU_BAR0_SIZE           0x2000
#define EDU_BUF_OFFSET          0x1000
#define EDU_BUF_SIZE            0x1000

#define EDU_REG_ID              0x000
#define EDU_REG_SCRATCH         0x004
#define EDU_REG_INT_STATUS      0x008
#define EDU_REG_INT_RAISE       0x00C
#define EDU_REG_INT_ACK         0x010
#define EDU_REG_DMA_ADDR_LO     0x020
#define EDU_REG_DMA_ADDR_HI     0x024
#define EDU_REG_DMA_OFF         0x028
#define EDU_REG_DMA_LEN         0x02C
#define EDU_REG_DMA_CMD         0x030
#define EDU_REG_DMA_RESULT      0x034
#define EDU_REG_DMA_DONE_COUNT  0x038

/* 按字节读是 "EDU1" */
#define EDU_ID_VALUE            0x31554445

#define EDU_INT_DMA             0x00000001   /* DMA 命令完成（命令带 IRQ 位时） */
#define EDU_INT_SOFT_MASK       0xFFFFFF00   /* 留给软件经 INT_RAISE 使用的位 */

#define EDU_DMA_CMD_START       0x1
#define EDU_DMA_CMD_TO_RAM      0x2
#define EDU_DMA_CMD_IRQ         0x4

#define EDU_DMA_RES_OK          0
#define EDU_DMA_RES_BAD_RANGE   1
#define EDU_DMA_RES_BUS_ERROR   2

#endif /* DEVICES_EDU_REGS_H */
