/* include/console.h — host 终端 <-> guest 串口
 *
 * 把 stdin 注册到事件循环，I/O 线程读到的字节逐个塞进串口接收 FIFO。
 * stdin 是终端时切到 raw 模式，这样 Ctrl-C 之类的按键会原样送给 guest
 * 而不是杀掉 VMM。退出 VMM 用 Ctrl-A x（和 qemu 一致）。
 *
 * console_start 要在 evl_start 之前调用；console_stop 只恢复终端，
 * I/O 线程由调用方 evl_stop。
 */
#ifndef CONSOLE_H
#define CONSOLE_H

struct vmm_vm;
struct serial8250;
struct event_loop;

int  console_start(struct vmm_vm *vm, struct serial8250 *s,
                   struct event_loop *loop);
void console_stop(void);

#endif /* CONSOLE_H */
