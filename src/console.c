/* src/console.c — stdin -> 串口接收 FIFO，Ctrl-A x 退出
 *
 * stdin 是事件循环的一个后端 fd，读取发生在 I/O 线程的回调里。 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <sys/epoll.h>

#include "vmm.h"
#include "console.h"
#include "smp.h"
#include "io/event_loop.h"
#include "devices/serial8250.h"

#define CTRL_A 0x01

static struct termios g_saved_tio;
static bool g_tio_saved;
static bool g_esc;          /* 上一个字节是 Ctrl-A */
static struct vmm_vm *g_vm;
static struct serial8250 *g_serial;

static void restore_tty(void)
{
    if (g_tio_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_tio);
        g_tio_saved = false;
    }
}

/* 一个字节按 Ctrl-A 转义规则处理；返回 false 表示用户要求退出 */
static bool console_byte(uint8_t c)
{
    if (g_esc) {
        g_esc = false;
        if (c == 'x' || c == 'X')
            return false;
        /* Ctrl-A Ctrl-A 发送一个 Ctrl-A，Ctrl-A 加其它字节两个都原样送 */
        if (c != CTRL_A)
            serial8250_push_rx(g_serial, CTRL_A);
    } else if (c == CTRL_A) {
        g_esc = true;
        return true;
    }
    serial8250_push_rx(g_serial, c);
    return true;
}

/*
 * I/O 线程上的回调。只拿串口自己的锁（serial8250_push_rx 内部），
 * 不拿 io_lock，符合 io/event_loop.h 里的加锁顺序。
 *
 * stdin 保持阻塞模式：O_NONBLOCK 是打开文件描述的属性，在终端里启动时
 * stdin 和 stdout 通常是 dup 出来的同一个打开文件描述，设了它串口输出的 write 也会变成非阻塞，
 * 终端一忙就丢 guest 输出。水平触发报可读之后只 read 一次，raw 模式下
 * （VMIN=1, VTIME=0）和管道上这次 read 都会立即返回已有的字节，不会阻塞。
 */
static void console_on_input(void *opaque, int fd, uint32_t events)
{
    struct event_loop *loop = opaque;
    uint8_t buf[64];
    ssize_t n, i;

    n = read(fd, buf, sizeof(buf));
    if (n < 0 && (errno == EINTR || errno == EAGAIN))
        return;
    if (n <= 0) {
        /* EOF（管道写端关闭）或 pty 主端关闭后的 EIO：不再有输入，但 guest
         * 继续跑。不摘掉的话水平触发会一直报这个事件 */
        vmm_dbg("console: stdin %s (events 0x%x), input stops",
                n == 0 ? "EOF" : strerror(errno), events);
        evl_del(loop, fd);
        return;
    }
    for (i = 0; i < n; i++) {
        if (!console_byte(buf[i])) {
            fprintf(stderr, "\r\n[vmm] Ctrl-A x, terminating\r\n");
            g_vm->user_stop = true;
            vm_request_stop(g_vm);
            evl_del(loop, fd);
            return;
        }
    }
}

int console_start(struct vmm_vm *vm, struct serial8250 *s,
                  struct event_loop *loop)
{
    struct termios tio;
    int r;

    g_vm = vm;
    g_serial = s;

    /* stdin 是终端、但我们不在前台进程组时（典型场景：被
     * `timeout` 包着跑，它会把子进程放进单独的进程组），碰终端就会
     * 被内核发 SIGTTOU/SIGTTIN 停住：tcsetattr 触发前者，read 触发
     * 后者。表现为 VMM 静止不动、Ctrl-C 也没反应。
     * 这种情况下既不切 raw 模式也不读输入，guest 照常运行。 */
    if (isatty(STDIN_FILENO) && tcgetpgrp(STDIN_FILENO) != getpgrp()) {
        vmm_warn("console: stdin is a terminal but vmm is not in the "
                 "foreground process group, guest input is disabled");
        vmm_warn("  (running under timeout? use 'timeout --foreground')");
        return VMM_OK;
    }

    /* 先确认 stdin 能被 epoll 监听，再切 raw 模式：监听不了时终端保持原样 */
    r = evl_add(loop, STDIN_FILENO, EPOLLIN, console_on_input, loop);
    if (r == VMM_ERR_UNSUPPORTED) {
        vmm_info("console: stdin cannot be polled (regular file or "
                 "/dev/null), guest input is disabled");
        return VMM_OK;
    }
    if (r != VMM_OK)
        return r;

    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &g_saved_tio) == 0) {
        g_tio_saved = true;
        atexit(restore_tty);
        tio = g_saved_tio;
        cfmakeraw(&tio);
        /* 保留输出处理：guest 自己会发 \r\n，但 VMM 日志只有 \n */
        tio.c_oflag |= OPOST | ONLCR;
        tcsetattr(STDIN_FILENO, TCSANOW, &tio);
        vmm_info("console: raw mode, press Ctrl-A x to quit");
    }
    return VMM_OK;
}

void console_stop(void)
{
    restore_tty();
}
