/* include/devices/serial8250.h — 16550A UART 模拟（Step 2.1）
 *
 * 只模拟 Linux 8250 驱动真正会碰到的那部分寄存器语义：
 *   - THR 写出字符 -> host 输出 fd
 *   - LSR 永远报告 THRE|TEMT（发送器永远空闲，guest 不需要等）
 *   - IIR/FCR 让驱动的 autoconfig 把我们识别成 16550A
 *   - MCR.LOOP 回环：autoconfig 会做回环自检，不实现的话端口被判为不存在
 *   - SCR 可读写：autoconfig 用它判断是不是 8250 以上的芯片
 *
 * 中断通过 irq_set 回调送出去，串口自己不关心中断控制器是谁。
 * Step 2.1 阶段回调可以为空（纯轮询输出）。
 */
#ifndef DEVICES_SERIAL8250_H
#define DEVICES_SERIAL8250_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

struct vmm_vm;

#define SERIAL_COM1_BASE 0x3f8
#define SERIAL_COM1_IRQ  4
#define SERIAL_NR_REGS   8

#define SERIAL_RX_BUF    256

struct serial8250 {
    /* 寄存器 */
    uint8_t ier;
    uint8_t iir;
    uint8_t fcr;
    uint8_t lcr;
    uint8_t mcr;
    uint8_t lsr;
    uint8_t msr;
    uint8_t scr;
    uint8_t dll;
    uint8_t dlm;

    /* 接收 FIFO：由 host 输入线程写入，guest 读 RBR 取出 */
    uint8_t  rx_buf[SERIAL_RX_BUF];
    uint32_t rx_head;
    uint32_t rx_tail;

    /* THRE 中断挂起：写 THR 后置位，读 IIR 拿到这个中断后清零 */
    bool thri_pending;
    int  irq_level;

    int out_fd;                /* guest 输出去哪儿，默认 STDOUT */

    /* 中断线电平回调，level=1 拉高、0 拉低 */
    void (*irq_set)(void *opaque, int irq, int level);
    void *irq_opaque;
    int   irq;

    pthread_mutex_t lock;      /* vCPU 线程与 stdin 线程共用 */
};

int  serial8250_init(struct serial8250 *s, int out_fd);

/* 注册到 PIO 总线 [base, base+8) */
int  serial8250_attach(struct vmm_vm *vm, struct serial8250 *s,
                       uint16_t base, int irq);

/* host 侧往接收 FIFO 塞一个字节；FIFO 满返回 false */
bool serial8250_push_rx(struct serial8250 *s, uint8_t c);

#endif /* DEVICES_SERIAL8250_H */
