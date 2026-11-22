/* include/boot/payload.h — 裸 payload 装载器（--mode real16 / long64）
 *
 * payload 是一段平坦二进制，原样拷到 --load 指定的 GPA，vCPU 从 --entry
 * 开始执行：
 *   real16  vCPU 停在实模式，CS:IP 指向入口，不需要任何内存里的结构。
 *   long64  VMM 在 guest 内存里建 GDT 和恒等映射页表（粒度和覆盖范围由
 *           --gran/--map 决定），vCPU 直接进 64 位长模式。
 *
 * 只装载一次：裸机没有复位来源，不需要 reload。
 */
#ifndef BOOT_PAYLOAD_H
#define BOOT_PAYLOAD_H

struct vmm_vm;
struct vmm_config;

int payload_boot(struct vmm_vm *vm, const struct vmm_config *cfg);

#endif
