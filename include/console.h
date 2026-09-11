/* include/console.h — host 终端 <-> guest 串口
 *
 * 起一个线程读 stdin，逐字节塞进串口接收 FIFO。stdin 是终端时切到
 * raw 模式，这样 Ctrl-C 之类的按键会原样送给 guest 而不是杀掉 VMM。
 * 退出 VMM 用 Ctrl-A x（和 qemu 一致）。
 */
#ifndef CONSOLE_H
#define CONSOLE_H

struct vmm_vm;
struct serial8250;

int  console_start(struct vmm_vm *vm, struct serial8250 *s);
void console_stop(void);

#endif /* CONSOLE_H */
