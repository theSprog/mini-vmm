/* include/devices/debug_port.h — 裸 payload 用的两个调试端口
 *
 * 端口 0x10（dbg-port）：guest 写一个字节，VMM 打印出来；读返回固定图案
 *   0x5A。串口跑起来之前，这是 guest 告诉 host"我走到这里了"唯一的手段。
 * 端口 0xF4（exit-port）：沿用 qemu isa-debug-exit 的约定，写入的字节成为
 *   VMM 进程的退出码，测试脚本靠它判断 payload 的结果。
 *
 * 所有模式都挂这两个端口，Linux guest 不访问它们。
 */
#ifndef DEVICES_DEBUG_PORT_H
#define DEVICES_DEBUG_PORT_H

struct vmm_vm;

#define DEBUG_PORT_PRINT 0x10
#define DEBUG_PORT_EXIT  0xF4

/* 先 dbg-port 后 exit-port：exit-stats 的设备表按注册顺序列出 */
int debug_ports_attach(struct vmm_vm *vm);

#endif
