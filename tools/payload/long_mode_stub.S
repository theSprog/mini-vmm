/* tools/payload/long_mode_stub.S — Step 1.2 的 64 位长模式测试载荷
 *
 * 载入到 GPA 0x300000，vCPU 由 VMM 预先设置成长模式后从这里开始执行。
 *
 * 这段代码能跑起来，本身就证明了三件事：
 *   1. CR0.PG / CR4.PAE / EFER.LME|LMA 设置正确（否则取指就崩）
 *   2. 恒等映射页表正确（否则取指 #PF -> triple fault -> SHUTDOWN）
 *   3. CS 描述符的 L 位正确（否则 movabs 这种 REX 前缀指令会被
 *      当成 32 位指令解码，结果不可预期）
 */
    .code64
    .section .text
    .globl _start
_start:
    /* 64 位立即数只能靠 movabs 装进寄存器 */
    movabs    $0x1122334455667788, %rax

    /* 绝对地址写内存。编码是 48 89 04 25 00 00 50 00，
     * ModRM.mod=00 rm=100 + SIB(base=101,index=100) = disp32 绝对寻址，
     * 不是 RIP 相对 —— 这正是我们要的，因为要写的是固定 GPA。 */
    movq    %rax, 0x500000

    /* 再写第二个 8 字节，方便一眼看出写入长度对不对 */
    movabs    $0xDEADBEEFCAFEBABE, %rbx
    movq    %rbx, 0x500008

    /* 读回来验证一次，确认这块内存确实可读可写 */
    movq    0x500000, %rcx
    cmpq    %rax, %rcx
    jne    fail

    movb    $0x64, %al        /* 'd'，表示 64 位路径成功 */
    outb    %al, $0x10
    hlt

fail:
    movb    $0x21, %al        /* '!' */
    outb    %al, $0x10
    hlt

1:    jmp    1b
