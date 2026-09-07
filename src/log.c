/* src/log.c — 日志实现 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "vmm.h"

int vmm_log_level = 2;

static const char *lvl_str[] = { "ERR ", "WARN", "INFO", "DBG " };

void vmm_log(int level, const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    const char *base;

    if (level > vmm_log_level)
        return;

    base = strrchr(file, '/');
    base = base ? base + 1 : file;

    /* DBG 才带文件行号，INFO 以上保持输出干净，
     * 因为 Step 2 之后 guest 串口日志会和 VMM 日志混在一起。 */
    if (level >= 3)
        fprintf(stderr, "[%s] %s:%d: ", lvl_str[level], base, line);
    else
        fprintf(stderr, "[%s] ", lvl_str[level]);

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}