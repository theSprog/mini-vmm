/* tools/payload/serial_stub.S — Step 2.1 串口测试载荷
 *
 * 长模式下，载入 GPA 0x300000。按 Linux early_serial 的方式轮询 LSR.THRE
 * 再写 THR，把一行字符串打到 COM1。然后顺手做一次 SCR 读写回环和
 * MCR.LOOP 回环自检，结果通过调试端口 0x10 报告：
 *   'S' = 两项自检都过   'x' = 有失败
 */
    .code64
    .section .text
    .globl _start

#define COM1     0x3f8
#define COM1_LSR (COM1 + 5)
#define COM1_MCR (COM1 + 4)
#define COM1_MSR (COM1 + 6)
#define COM1_SCR (COM1 + 7)

_start:
    lea     msg(%rip), %rsi
1:
    movb    (%rsi), %bl
    testb   %bl, %bl
    jz      2f
    /* 等发送器空 */
    mov     $COM1_LSR, %dx
3:  inb     %dx, %al
    testb   $0x20, %al
    jz      3b
    mov     $COM1, %dx
    movb    %bl, %al
    outb    %al, %dx
    inc     %rsi
    jmp     1b

2:
    /* SCR 回环 */
    mov     $COM1_SCR, %dx
    movb    $0xa5, %al
    outb    %al, %dx
    inb     %dx, %al
    cmpb    $0xa5, %al
    jne     fail

    /* MCR.LOOP|RTS|OUT2 -> MSR 应有 CTS|DCD = 0x90 */
    mov     $COM1_MCR, %dx
    movb    $0x1a, %al
    outb    %al, %dx
    mov     $COM1_MSR, %dx
    inb     %dx, %al
    andb    $0xf0, %al
    cmpb    $0x90, %al
    jne     fail
    mov     $COM1_MCR, %dx
    xorb    %al, %al
    outb    %al, %dx

    movb    $0x53, %al          /* 'S' */
    outb    %al, $0x10
    hlt

fail:
    movb    $0x78, %al          /* 'x' */
    outb    %al, $0x10
    hlt

msg:
    .asciz  "Hello from guest via 8250 UART\n"
