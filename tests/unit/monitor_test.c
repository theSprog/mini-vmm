/* tests/unit/monitor_test.c — monitor 协议核心（src/monitor.c）的契约
 *
 * 判据来自 include/monitor.h 写明的协议，逐条对应：
 *   - 回复是若干以两个空格开头的正文行，加一行 OK 或 ERR <原因>；
 *   - 一行命令被拆成任意多段到达、一段里有多行、CRLF 行尾，结果都一样；
 *   - 空行和只有空白的行不产生回复；
 *   - 未知命令、未知 info 子命令、参数个数不对、参数过多、行内有 NUL、
 *     超长行各自返回 ERR，之后的命令照常执行；
 *   - 超长行的边界：MON_LINE_MAX 字节的行照常解析，多一个字节整行丢弃，
 *     拆成多段到达时也一样，而且只回一次 ERR；
 *   - 错误原因里的换行不能破坏回复格式；
 *   - help 和 info 从注册表生成，注册的命令出现在里面；
 *   - 异步命令完成前，同一连接后面的命令不执行，完成后按请求顺序回复；
 *   - 连接断开后完成的异步请求仍然调用 finish 释放资源，回复被丢弃；
 *   - 当前 CPU 是每个连接自己的；
 *   - 回复积压超过 MON_OUT_MAX 时置 overflow。
 *
 * 不建 socket：直接用 mon_conn_input() 喂字节、读 conn.out。socket 层的
 * 接受连接、断开、连接数上限由 e2e suite monitor 验证。
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmm.h"
#include "monitor.h"
#include "harness.h"

int vmm_log_level = -1;

void vmm_log(int level, const char *file, int line, const char *fmt, ...)
{
    (void)level; (void)file; (void)line; (void)fmt;
}

/* ---- 测试用命令 ---- */

static int cmd_echo(struct mon_req *req, int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++)
        fprintf(mon_out(req), "arg%d=%s\n", i, argv[i]);
    return MON_OK;
}

static int cmd_lines(struct mon_req *req, int argc, char **argv)
{
    /* 最后一行故意不带换行，核心要补上 */
    fprintf(mon_out(req), "first\n\nOK\nERR not an end marker");
    return MON_OK;
}

static int cmd_fail(struct mon_req *req, int argc, char **argv)
{
    return mon_err(req, "bad thing\nsecond line");
}

static int cmd_big(struct mon_req *req, int argc, char **argv)
{
    size_t i;

    for (i = 0; i < MON_OUT_MAX / 64 + 1; i++)
        fprintf(mon_out(req), "%063zu\n", i);
    return MON_OK;
}

static int cmd_setcpu(struct mon_req *req, int argc, char **argv)
{
    mon_req_set_cpu(req, atoi(argv[1]));
    return MON_OK;
}

static int cmd_getcpu(struct mon_req *req, int argc, char **argv)
{
    fprintf(mon_out(req), "cpu=%d\n", mon_req_cpu(req));
    return MON_OK;
}

static int info_thing(struct mon_req *req, int argc, char **argv)
{
    fprintf(mon_out(req), "thing is %s\n", (const char *)mon_req_opaque(req));
    return MON_OK;
}

/* 异步命令：把请求存起来，由测试决定什么时候 post */
static struct mon_req *g_pending;
static int g_finish_calls, g_finish_orphaned;

static int async_finish(struct mon_req *req)
{
    g_finish_calls++;
    if (mon_req_orphaned(req))
        g_finish_orphaned++;
    fprintf(mon_out(req), "async done %s\n",
            (const char *)mon_req_defer_opaque(req));
    return MON_OK;
}

static int cmd_async(struct mon_req *req, int argc, char **argv)
{
    g_pending = req;
    mon_req_defer(req, async_finish, (void *)"tag");
    return MON_PENDING;
}

static const struct mon_cmd test_cmds[] = {
    { "echo",   "[args...]", "print the arguments", 0, 3, cmd_echo, NULL },
    { "lines",  "",          "multi-line body",     0, 0, cmd_lines, NULL },
    { "fail",   "",          "always fails",        0, 0, cmd_fail, NULL },
    { "big",    "",          "overflows output",    0, 0, cmd_big, NULL },
    { "setcpu", "<n>",       "set current cpu",     1, 1, cmd_setcpu, NULL },
    { "getcpu", "",          "show current cpu",    0, 0, cmd_getcpu, NULL },
    { "async",  "",          "completes later",     0, 0, cmd_async, NULL },
};

static struct monitor g_mon;

static void setup(void)
{
    static const struct mon_cmd thing = {
        "thing", "", "show the thing", 0, 0, info_thing, (void *)"shiny"
    };
    size_t i;

    if (monitor_init(&g_mon) != VMM_OK) {
        printf("monitor_init failed\n");
        exit(1);
    }
    for (i = 0; i < sizeof(test_cmds) / sizeof(test_cmds[0]); i++)
        monitor_register(&g_mon, MON_TOP, &test_cmds[i]);
    monitor_register(&g_mon, MON_INFO, &thing);
}

/* 喂一段字节，返回这次新增的输出（静态缓冲） */
static const char *feed(struct mon_conn *c, const char *s, size_t n)
{
    static char buf[1 << 16];
    size_t before = c->out_len;
    size_t got;

    mon_conn_input(&g_mon, c, s, n);
    got = c->out_len - before;
    if (got >= sizeof(buf))
        got = sizeof(buf) - 1;
    if (got)
        memcpy(buf, c->out + before, got);
    buf[got] = '\0';
    return buf;
}

static const char *feeds(struct mon_conn *c, const char *s)
{
    return feed(c, s, strlen(s));
}

#define CHECK_STR(got, want, what)                                      \
    CHECK(strcmp((got), (want)) == 0, "%s:\n--- got ---\n%s--- want ---\n%s", \
          (what), (got), (want))

static void case_register(void)
{
    static const struct mon_cmd dup = { "echo", "", "dup", 0, 0, cmd_echo, NULL };
    static const struct mon_cmd spaced = { "a b", "", "x", 0, 0, cmd_echo, NULL };
    static const struct mon_cmd info_dup_ok = { "echo", "", "info echo", 0, 0, cmd_echo, NULL };

    T_CASE("register: duplicate and malformed names rejected");
    CHECK(monitor_register(&g_mon, MON_TOP, &dup) == VMM_ERR_INVAL,
          "duplicate top-level name");
    CHECK(monitor_register(&g_mon, MON_TOP, &spaced) == VMM_ERR_INVAL,
          "name with a space");
    /* 两张表是独立的命名空间，和 HMP 的 info 子命令一样 */
    CHECK(monitor_register(&g_mon, MON_INFO, &info_dup_ok) == VMM_OK,
          "same name in the info table");
}

static void case_basic(void)
{
    struct mon_conn c;

    T_CASE("reply: indented body then OK; multi-line body; missing newline");
    mon_conn_init(&c, -1);
    CHECK_STR(feeds(&c, "echo a b\n"), "  arg1=a\n  arg2=b\nOK\n", "echo");
    CHECK_STR(feeds(&c, "echo\n"), "OK\n", "empty body");
    CHECK_STR(feeds(&c, "lines\n"),
              "  first\n  \n  OK\n  ERR not an end marker\nOK\n",
              "body lines that look like end markers are indented");
    CHECK_STR(feeds(&c, "  \techo   x\t \n"), "  arg1=x\nOK\n",
              "spaces and tabs around arguments");
    mon_conn_release(&g_mon, &c);
}

static void case_framing(void)
{
    struct mon_conn c;

    T_CASE("framing: split input, several lines per chunk, CRLF, empty lines");
    mon_conn_init(&c, -1);
    CHECK_STR(feeds(&c, "ec"), "", "partial line: no reply yet");
    CHECK_STR(feeds(&c, "ho "), "", "still partial");
    CHECK_STR(feeds(&c, "z\n"), "  arg1=z\nOK\n", "completed line");
    CHECK_STR(feeds(&c, "echo 1\necho 2\n"),
              "  arg1=1\nOK\n  arg1=2\nOK\n", "two lines in one chunk");
    CHECK_STR(feeds(&c, "echo crlf\r\n"), "  arg1=crlf\nOK\n", "CRLF");
    CHECK_STR(feeds(&c, "\n\r\n   \n\t\n"), "", "empty lines produce nothing");
    mon_conn_release(&g_mon, &c);
}

static void case_errors(void)
{
    struct mon_conn c;
    char line[MON_MAX_ARGS * 3 + 16];
    int i, n;

    T_CASE("errors: unknown command, info, argument counts, NUL");
    mon_conn_init(&c, -1);
    CHECK_STR(feeds(&c, "frob\n"), "ERR unknown command 'frob' (try 'help')\n",
              "unknown command");
    CHECK_STR(feeds(&c, "info frob\n"),
              "ERR unknown info command 'frob' (try 'info')\n",
              "unknown info subcommand");
    CHECK_STR(feeds(&c, "lines extra\n"),
              "ERR lines: too many arguments (usage: lines)\n",
              "too many arguments");
    CHECK_STR(feeds(&c, "setcpu\n"),
              "ERR setcpu: missing argument (usage: setcpu <n>)\n",
              "missing argument");
    CHECK_STR(feeds(&c, "info thing extra\n"),
              "ERR info thing: too many arguments (usage: info thing)\n",
              "info subcommand argument count");

    n = snprintf(line, sizeof(line), "echo");
    for (i = 0; i < MON_MAX_ARGS; i++)
        n += snprintf(line + n, sizeof(line) - n, " a");
    snprintf(line + n, sizeof(line) - n, "\n");
    CHECK_STR(feeds(&c, line),
              "ERR too many arguments (max 16 including the command)\n",
              "more tokens than MON_MAX_ARGS");

    CHECK_STR(feed(&c, "echo a\0b\n", 9),
              "ERR line contains a NUL byte\n", "NUL byte");
    CHECK_STR(feeds(&c, "fail\n"), "ERR bad thing second line\n",
              "newline in the reason is flattened");
    CHECK_STR(feeds(&c, "echo after\n"), "  arg1=after\nOK\n",
              "commands still work after errors");
    mon_conn_release(&g_mon, &c);
}

static void case_long_lines(void)
{
    static char line[MON_LINE_MAX * 3];
    struct mon_conn c;
    const char *got;

    T_CASE("long lines: MON_LINE_MAX accepted, one more byte rejected once");
    mon_conn_init(&c, -1);

    /* 恰好 MON_LINE_MAX 字节：照常解析（这里是一个未知命令） */
    memset(line, 'a', MON_LINE_MAX);
    line[MON_LINE_MAX] = '\n';
    got = feed(&c, line, MON_LINE_MAX + 1);
    CHECK(strncmp(got, "ERR unknown command 'aaa", 24) == 0,
          "line of exactly MON_LINE_MAX bytes is parsed: %.60s", got);

    /* 多一个字节：整行丢弃，只回一次 ERR */
    memset(line, 'b', MON_LINE_MAX + 1);
    line[MON_LINE_MAX + 1] = '\n';
    CHECK_STR(feed(&c, line, MON_LINE_MAX + 2),
              "ERR line too long (max 1024 bytes)\n", "one byte over");

    /* 拆成多段到达：超长部分在中间几段，换行在最后一段 */
    memset(line, 'c', sizeof(line));
    CHECK_STR(feed(&c, line, MON_LINE_MAX), "", "first chunk, at the limit");
    CHECK_STR(feed(&c, line, MON_LINE_MAX), "", "over the limit: discarding");
    CHECK_STR(feed(&c, line, MON_LINE_MAX), "", "still discarding");
    CHECK_STR(feeds(&c, "ccc\necho next\n"),
              "ERR line too long (max 1024 bytes)\n  arg1=next\nOK\n",
              "one ERR at the newline, next line parsed normally");
    CHECK(c.in_len == 0, "nothing buffered after the newline (in_len=%zu)",
          c.in_len);
    mon_conn_release(&g_mon, &c);
}

static void case_help_info(void)
{
    struct mon_conn c;
    const char *got;

    T_CASE("help and info are generated from the registry");
    mon_conn_init(&c, -1);
    got = feeds(&c, "help\n");
    CHECK(strstr(got, "  help [command]") != NULL, "help lists itself:\n%s", got);
    CHECK(strstr(got, "  setcpu <n>") != NULL, "help lists setcpu:\n%s", got);
    CHECK(strstr(got, "set current cpu") != NULL, "help shows the description");
    CHECK(strstr(got, "  info thing") != NULL, "help lists info subcommands:\n%s", got);
    CHECK(strlen(got) >= 3 && !strcmp(got + strlen(got) - 3, "OK\n"),
          "help ends with OK");

    CHECK_STR(feeds(&c, "help setcpu\n"), "  setcpu <n>  set current cpu\nOK\n",
              "help for one command");
    CHECK_STR(feeds(&c, "help info thing\n"),
              "  info thing  show the thing\nOK\n", "help for an info subcommand");
    CHECK_STR(feeds(&c, "help frob\n"), "ERR unknown command 'frob' (try 'help')\n",
              "help for an unknown command");

    got = feeds(&c, "info\n");
    CHECK(strstr(got, "  info thing") != NULL && strstr(got, "  info echo") != NULL,
          "bare info lists the info subcommands:\n%s", got);
    CHECK_STR(feeds(&c, "info thing\n"), "  thing is shiny\nOK\n",
              "info subcommand gets its opaque");
    mon_conn_release(&g_mon, &c);
}

static void case_async(void)
{
    struct mon_conn c;

    T_CASE("async: later commands wait, replies stay in request order");
    mon_conn_init(&c, -1);
    g_pending = NULL;
    g_finish_calls = g_finish_orphaned = 0;
    CHECK_STR(feeds(&c, "async\necho second\n"), "", "nothing until completion");
    CHECK(g_pending != NULL && c.pending == g_pending, "request is pending");
    CHECK_STR(feeds(&c, "echo third\n"), "", "input while pending is buffered");
    monitor_run_completions(&g_mon);
    CHECK(c.out_len == 0, "running completions before post does nothing");

    mon_req_post(g_pending);
    monitor_run_completions(&g_mon);
    CHECK(c.out != NULL &&
          !strcmp(c.out, "  async done tag\nOK\n  arg1=second\nOK\n  arg1=third\nOK\n"),
          "async reply first, then the buffered commands:\n%.*s",
          (int)c.out_len, c.out ? c.out : "");
    CHECK(c.pending == NULL, "nothing pending afterwards");
    CHECK(g_finish_calls == 1 && g_finish_orphaned == 0, "finish called once");
    mon_conn_release(&g_mon, &c);

    T_CASE("async: connection closed before completion");
    mon_conn_init(&c, -1);
    g_pending = NULL;
    g_finish_calls = g_finish_orphaned = 0;
    feeds(&c, "async\n");
    mon_conn_release(&g_mon, &c);
    CHECK(g_pending != NULL, "request was pending");
    mon_req_post(g_pending);
    monitor_run_completions(&g_mon);
    CHECK(g_finish_calls == 1 && g_finish_orphaned == 1,
          "finish still called, marked orphaned (calls=%d orphaned=%d)",
          g_finish_calls, g_finish_orphaned);
}

static void case_cpu_and_overflow(void)
{
    struct mon_conn a, b;

    T_CASE("current cpu is per connection");
    mon_conn_init(&a, -1);
    mon_conn_init(&b, -1);
    CHECK_STR(feeds(&a, "getcpu\n"), "  cpu=0\nOK\n", "default is cpu 0");
    feeds(&a, "setcpu 3\n");
    CHECK_STR(feeds(&a, "getcpu\n"), "  cpu=3\nOK\n", "a sees its change");
    CHECK_STR(feeds(&b, "getcpu\n"), "  cpu=0\nOK\n", "b is unaffected");

    T_CASE("output backlog over MON_OUT_MAX sets overflow");
    CHECK(!a.overflow, "no overflow yet");
    feeds(&a, "big\n");
    CHECK(a.overflow, "overflow after a huge reply (out_len=%zu)", a.out_len);
    mon_conn_release(&g_mon, &a);
    mon_conn_release(&g_mon, &b);
}

int main(int argc, char **argv)
{
    T_MAIN_BEGIN();
    setup();
    case_register();
    case_basic();
    case_framing();
    case_errors();
    case_long_lines();
    case_help_info();
    case_async();
    case_cpu_and_overflow();
    monitor_close(&g_mon);
    return T_SUMMARY();
}
