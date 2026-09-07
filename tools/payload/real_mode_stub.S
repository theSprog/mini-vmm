/* tools/payload/real_mode_stub.S — Step 1.1 的 16 位实模式测试载荷
 *
 * 载入到 GPA 0x0，vCPU 以 CS:IP = 0000:0000 开始执行。
 *
 * 注意这段代码里没有任何 BIOS 调用、没有 IVT、没有栈上的返回地址依赖 ——
 * 因为 guest 里除了这几个字节以外什么都没有。
 */
    .code16
    .section .text
    .globl _start
_start:
    movb    $0x42, %al
    outb    %al, $0x10        /* -> KVM_EXIT_IO, port=0x10, OUT */

    inb    $0x10, %al        /* -> KVM_EXIT_IO, port=0x10, IN  */
    outb    %al, $0x10        /* 回显读到的值，验证读通路 */

    hlt                /* -> KVM_EXIT_HLT，主循环退出 */

    /* hlt 之后理论上不会再执行。万一被中断唤醒，死循环兜住，
     * 避免 RIP 跑到未初始化内存里产生迷惑性的报错。 */
1:    jmp    1b
