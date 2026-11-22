/* include/opts.h — QEMU 风格的 "type,key=val,..." 参数值解析
 *
 * --device、--netdev、--drive 的参数值都是这种格式，语法沿用 QEMU 的 QemuOpts
 * （util/qemu-option.c 的 get_opt_name_value、get_opt_value），这样 QEMU
 * 的命令行可以几乎原样搬过来对照：
 *
 *   e1000,netdev=n0,mac=52:54:00:12:34:56
 *   ^^^^^ 第一个元素里没有 '=' 时，它是“隐含键”的值（--device 的隐含键
 *         是 driver），写成 driver=e1000 也一样
 *
 *   ifname=a,,b      值里的 ",," 表示一个逗号，值是 "a,b"；键里没有转义
 *
 * 和 QEMU 不同、更严格的三处，都是为了让写错的参数在启动前报错，而不是
 * 静默生效成另一个意思：
 *   - 同一个键出现两次是错误。QEMU 取最后一个，长命令行里改了一处却
 *     忘了删另一处时，看不出生效的是哪个。
 *   - 第一个元素之外不带 '=' 的元素是错误。QEMU 把 "foo" 当 foo=on、
 *     "nofoo" 当 foo=off，这种短格式在 QEMU 里已经废弃（会打印警告），
 *     而且拼错的键名会被当成另一个布尔开关。
 *   - 空元素、空键是错误。
 *
 * 解析只管语法。哪些键合法由使用方在知道类型之后用 opts_check_keys 检查，
 * 因为合法键集合取决于隐含键的值（e1000 和 pci-test 的属性不同）。
 */
#ifndef OPTS_H
#define OPTS_H

#include <stdbool.h>

/* 一个参数值里最多的键值对数。目前最多的 e1000 只有 driver/netdev/mac 三个 */
#define OPTS_MAX_PAIRS  16

struct opts_pair {
    const char *key;
    const char *val;               /* 已去掉转义，可以是空串 */
};

struct opts {
    const char *what;              /* 选项名，比如 "--device"，用于报错 */
    const char *arg;               /* 原始参数值，用于报错 */
    char       *buf;               /* 所有 key/val 字符串的存储，opts_free 释放 */
    int         n;
    struct opts_pair pairs[OPTS_MAX_PAIRS];
};

/* 解析 arg。implied_key 为 NULL 表示没有隐含键，第一个元素也必须带 '='。
 * 失败时打印带 what 和原始 arg 的错误，返回 VMM_ERR_INVAL，o 不需要释放。 */
int  opts_parse(struct opts *o, const char *what, const char *arg,
                const char *implied_key);

/* 取值，键不存在返回 NULL */
const char *opts_get(const struct opts *o, const char *key);

/* 键不存在时报 "missing property"，返回 NULL */
const char *opts_require(const struct opts *o, const char *key);

/* 布尔属性。键不存在时 *out = def。取值集合沿用 QEMU 的 qapi_bool_parse
 * （qapi/qapi-util.c）：on/yes/true/y 与 off/no/false/n，其余报错。 */
int  opts_get_bool(const struct opts *o, const char *key, bool def, bool *out);

/* id= 的取值规则，沿用 QEMU 的 id_wellformed（util/id.c）：字母开头，其后
 * 是字母、数字或 "-._"，这样从 QEMU 搬过来的命令行里的 id 都能直接用 */
bool opts_id_wellformed(const char *id);

/* 每个键都必须在 allowed（NULL 结尾）里，否则报错并列出合法的键 */
int  opts_check_keys(const struct opts *o, const char *const allowed[]);

void opts_free(struct opts *o);

#endif /* OPTS_H */
