/* tests/unit/opts_test.c — QEMU 风格参数值解析（src/opts.c）
 *
 * 判据来源：
 *   - 隐含键、",," 转义、键里不转义：QEMU util/qemu-option.c 的
 *     get_opt_name_value / get_opt_value。合法输入的解析结果和 QEMU 一致
 *     才能把 QEMU 命令行原样搬过来对照。
 *   - 重复键、短格式布尔、空元素报错：include/opts.h 写明的、比 QEMU
 *     更严格的三处。
 *
 * 错误路径同时检查返回值和日志：日志是用户唯一能看到的东西，报错信息里
 * 必须带出原始参数和出错的那个键。
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "vmm.h"
#include "opts.h"
#include "harness.h"

/* vmm_log 桩：最后一条日志留下来给用例比对 */
int vmm_log_level = -1;
static char g_last_log[512];

void vmm_log(int level, const char *file, int line, const char *fmt, ...)
{
    va_list ap;

    (void)level; (void)file; (void)line;
    va_start(ap, fmt);
    vsnprintf(g_last_log, sizeof(g_last_log), fmt, ap);
    va_end(ap);
    if (vmm_log_level >= 0)
        fprintf(stderr, "       [log] %s\n", g_last_log);
}

/* 按顺序列出所有键值对："k=v|k=v"，一次比对整个解析结果 */
static const char *dump(const struct opts *o)
{
    static char s[512];
    size_t n = 0;
    int i;

    s[0] = '\0';
    for (i = 0; i < o->n; i++)
        n += (size_t)snprintf(s + n, sizeof(s) - n, "%s%s=%s", i ? "|" : "",
                              o->pairs[i].key, o->pairs[i].val);
    return s;
}

static void expect_ok(const char *arg, const char *implied, const char *want)
{
    struct opts o;
    int r = opts_parse(&o, "--device", arg, implied);

    CHECK_U64(r, VMM_OK, "parse \"%s\"", arg);
    if (r == VMM_OK) {
        CHECK(!strcmp(dump(&o), want), "\"%s\" -> \"%s\", want \"%s\"",
              arg, dump(&o), want);
        opts_free(&o);
    }
}

static void expect_err(const char *arg, const char *implied, const char *msg)
{
    struct opts o;
    char head[256];

    g_last_log[0] = '\0';
    CHECK_U64(opts_parse(&o, "--device", arg, implied), VMM_ERR_INVAL,
              "\"%s\" must be rejected", arg);
    snprintf(head, sizeof(head), "--device %s: ", arg);
    CHECK(!strncmp(g_last_log, head, strlen(head)),
          "error names the option and the raw value: \"%s\"", g_last_log);
    CHECK(strstr(g_last_log, msg) != NULL, "error for \"%s\" mentions \"%s\": \"%s\"",
          arg, msg, g_last_log);
    CHECK(o.buf == NULL && o.n == 0, "rejected parse leaves nothing to free");
}

static void case_implied(void)
{
    T_CASE("隐含键：第一个元素没有 '=' 时是 driver 的值，也可以显式写");
    expect_ok("e1000", "driver", "driver=e1000");
    expect_ok("e1000,netdev=n0,mac=52:54:00:12:34:56", "driver",
              "driver=e1000|netdev=n0|mac=52:54:00:12:34:56");
    expect_ok("driver=e1000,netdev=n0", "driver", "driver=e1000|netdev=n0");
    /* 第一个元素带 '=' 时就不是隐含键，driver 由调用方判缺 */
    expect_ok("netdev=n0", "driver", "netdev=n0");
    expect_ok("tap,id=n0,ifname=tap0", "type", "type=tap|id=n0|ifname=tap0");
    expect_ok("e1000,", "driver", "driver=e1000");
    expect_ok("id=n0", NULL, "id=n0");
}

static void case_escape(void)
{
    T_CASE("转义：值里 ',,' 是一个逗号，可以出现在开头、结尾、连续出现");
    expect_ok("tap,ifname=a,,b", "type", "type=tap|ifname=a,b");
    expect_ok("tap,ifname=,,a", "type", "type=tap|ifname=,a");
    expect_ok("tap,ifname=a,,", "type", "type=tap|ifname=a,");
    expect_ok("tap,ifname=a,,,,b,id=x", "type", "type=tap|ifname=a,,b|id=x");
    expect_ok("a,,b,id=x", "type", "type=a,b|id=x");
    /* '=' 只有第一个分隔键和值，值里的 '=' 原样保留 */
    expect_ok("tap,ifname=a=b", "type", "type=tap|ifname=a=b");
    /* 空值合法，由取值方决定能不能接受 */
    expect_ok("tap,ifname=", "type", "type=tap|ifname=");
}

static void case_errors(void)
{
    T_CASE("语法错误：在启动前报出，信息里带原始参数和出错的键");
    expect_err("", "driver", "empty value");
    expect_err(",a=1", "driver", "empty driver");
    expect_err("e1000,flag", "driver", "'flag' has no value");
    expect_err("e1000,noflag", "driver", "write noflag=<value>");
    expect_err("e1000,=1", "driver", "empty property name");
    expect_err("e1000,a=1,,,b", "driver", "'b' has no value");
    expect_err("e1000,mac=1,mac=2", "driver", "property 'mac' given twice");
    expect_err("e1000,driver=x", "driver", "property 'driver' given twice");
    expect_err("e1000", NULL, "'e1000' has no value");
    expect_err("e1000,a=1,b=2,c=3,d=4,e=5,f=6,g=7,h=8,i=9,j=10,k=11,l=12,"
               "m=13,n=14,o=15,p=16", "driver", "more than 16 properties");
}

static void case_escape_first(void)
{
    /* "e1000,," 是 driver="e1000," 而不是空元素：转义先于分隔 */
    T_CASE("第一个元素里的 ',,' 也是转义");
    expect_ok("e1000,,", "driver", "driver=e1000,");
}

static void case_check_keys(void)
{
    static const char *const allowed[] = { "driver", "netdev", "mac", NULL };
    struct opts o;

    T_CASE("opts_check_keys：未知键报错并列出合法键；opts_get/require");
    CHECK_U64(opts_parse(&o, "--device", "e1000,netdev=n0", "driver"), VMM_OK, "parse");
    CHECK_U64(opts_check_keys(&o, allowed), VMM_OK, "known keys pass");
    CHECK(!strcmp(opts_get(&o, "netdev"), "n0"), "opts_get netdev");
    CHECK(opts_get(&o, "mac") == NULL, "absent key reads NULL");
    g_last_log[0] = '\0';
    CHECK(opts_require(&o, "mac") == NULL, "require of an absent key fails");
    CHECK(strstr(g_last_log, "missing property 'mac'") != NULL,
          "missing property logged: \"%s\"", g_last_log);
    opts_free(&o);

    CHECK_U64(opts_parse(&o, "--device", "e1000,netdve=n0", "driver"), VMM_OK, "parse typo");
    g_last_log[0] = '\0';
    CHECK_U64(opts_check_keys(&o, allowed), VMM_ERR_INVAL, "typo rejected");
    CHECK(!strcmp(g_last_log, "--device e1000,netdve=n0: unknown property "
                  "'netdve' (valid: driver, netdev, mac)"),
          "unknown key message: \"%s\"", g_last_log);
    opts_free(&o);
}

int main(int argc, char **argv)
{
    int i;

    T_MAIN_BEGIN();
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-v"))
            vmm_log_level = 3;

    case_implied();
    case_escape();
    case_escape_first();
    case_errors();
    case_check_keys();
    return T_SUMMARY();
}
