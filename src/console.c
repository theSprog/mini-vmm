/* src/console.c — stdin -> 串口接收 FIFO，Ctrl-A x 退出 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <pthread.h>

#include "vmm.h"
#include "console.h"
#include "smp.h"
#include "devices/serial8250.h"

#define CTRL_A 0x01

static struct termios g_saved_tio;
static bool g_tio_saved;
static pthread_t g_thread;
static bool g_thread_started;
static struct vmm_vm *g_vm;
static struct serial8250 *g_serial;

static void restore_tty(void)
{
    if (g_tio_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_tio);
        g_tio_saved = false;
    }
}

static void *console_thread(void *arg)
{
    bool esc = false;
    uint8_t c;
    ssize_t n;

    (void)arg;
    for (;;) {
        n = read(STDIN_FILENO, &c, 1);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;          /* EOF：不再有输入，但 guest 继续跑 */

        if (esc) {
            esc = false;
            if (c == 'x' || c == 'X') {
                fprintf(stderr, "\r\n[vmm] Ctrl-A x, terminating\r\n");
                g_vm->user_stop = true;
                vm_request_stop(g_vm);
                break;
            }
            if (c != CTRL_A)
                serial8250_push_rx(g_serial, CTRL_A);
            /* Ctrl-A Ctrl-A 发送一个 Ctrl-A */
        } else if (c == CTRL_A) {
            esc = true;
            continue;
        }
        serial8250_push_rx(g_serial, c);
    }
    return NULL;
}

int console_start(struct vmm_vm *vm, struct serial8250 *s)
{
    struct termios tio;

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

    if (pthread_create(&g_thread, NULL, console_thread, NULL) != 0) {
        vmm_err("pthread_create console thread failed");
        restore_tty();
        return VMM_ERR_SYS;
    }
    g_thread_started = true;
    return VMM_OK;
}

void console_stop(void)
{
    restore_tty();
    /* 读线程阻塞在 read(stdin) 上，没法优雅唤醒；进程退出时一并回收 */
    if (g_thread_started)
        pthread_detach(g_thread);
    g_thread_started = false;
}
