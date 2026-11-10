/* src/opts.c — QEMU 风格参数值解析，语法和与 QEMU 的差异见 include/opts.h */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmm.h"
#include "opts.h"

static int opts_fail(const struct opts *o, const char *fmt, ...)
{
    char msg[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    vmm_err("%s %s: %s", o->what, o->arg, msg);
    return VMM_ERR_INVAL;
}

/* 从 p 拷一个值到 *out：",," 变成 ","，遇到单个 ',' 或串尾停下。
 * 返回停下的位置（指向 ',' 或 '\0'），*out 前进到写入的 '\0' 之后。 */
static const char *copy_value(const char *p, char **out)
{
    char *d = *out;

    for (;;) {
        if (*p == ',' && p[1] == ',') {
            *d++ = ',';
            p += 2;
        } else if (*p == ',' || *p == '\0') {
            break;
        } else {
            *d++ = *p++;
        }
    }
    *d++ = '\0';
    *out = d;
    return p;
}

int opts_parse(struct opts *o, const char *what, const char *arg,
               const char *implied_key)
{
    const char *p = arg;
    char *out;
    int i;

    memset(o, 0, sizeof(*o));
    o->what = what;
    o->arg  = arg;
    if (!*arg)
        return opts_fail(o, "empty value");

    /* 去转义只会变短，每个键和值各多一个 '\0'，正好顶替 '=' 和 ','；
     * 隐含键不拷贝。所以原长度 + 1 一定够用。 */
    o->buf = malloc(strlen(arg) + 1);
    if (!o->buf)
        return VMM_ERR_NOMEM;
    out = o->buf;

    for (;;) {
        size_t len = strcspn(p, "=,");
        struct opts_pair *kv;

        if (o->n == OPTS_MAX_PAIRS) {
            opts_fail(o, "more than %d properties", OPTS_MAX_PAIRS);
            goto fail;
        }
        kv = &o->pairs[o->n];

        if (p[len] != '=') {
            if (o->n == 0 && implied_key) {
                kv->key = implied_key;
                kv->val = out;
                p = copy_value(p, &out);
                if (!*kv->val) {
                    opts_fail(o, "empty %s", implied_key);
                    goto fail;
                }
            } else if (len == 0) {
                opts_fail(o, "empty element at offset %zu", (size_t)(p - arg));
                goto fail;
            } else {
                opts_fail(o, "'%.*s' has no value; write %.*s=<value> "
                          "(short-form flags are not supported)",
                          (int)len, p, (int)len, p);
                goto fail;
            }
        } else {
            if (len == 0) {
                opts_fail(o, "empty property name at offset %zu",
                          (size_t)(p - arg));
                goto fail;
            }
            memcpy(out, p, len);
            out[len] = '\0';
            kv->key = out;
            out += len + 1;
            kv->val = out;
            p = copy_value(p + len + 1, &out);
        }

        for (i = 0; i < o->n; i++)
            if (!strcmp(o->pairs[i].key, kv->key)) {
                opts_fail(o, "property '%s' given twice", kv->key);
                goto fail;
            }
        o->n++;

        if (*p == ',')
            p++;
        if (!*p)
            return VMM_OK;
    }

fail:
    opts_free(o);
    return VMM_ERR_INVAL;
}

const char *opts_get(const struct opts *o, const char *key)
{
    int i;

    for (i = 0; i < o->n; i++)
        if (!strcmp(o->pairs[i].key, key))
            return o->pairs[i].val;
    return NULL;
}

const char *opts_require(const struct opts *o, const char *key)
{
    const char *v = opts_get(o, key);

    if (!v)
        opts_fail(o, "missing property '%s'", key);
    return v;
}

int opts_check_keys(const struct opts *o, const char *const allowed[])
{
    char list[256];
    size_t n = 0;
    int i, j;

    for (i = 0; i < o->n; i++) {
        for (j = 0; allowed[j]; j++)
            if (!strcmp(o->pairs[i].key, allowed[j]))
                break;
        if (allowed[j])
            continue;

        list[0] = '\0';
        for (j = 0; allowed[j] && n < sizeof(list); j++)
            n += (size_t)snprintf(list + n, sizeof(list) - n, "%s%s",
                                  j ? ", " : "", allowed[j]);
        return opts_fail(o, "unknown property '%s' (valid: %s)",
                         o->pairs[i].key, list);
    }
    return VMM_OK;
}

void opts_free(struct opts *o)
{
    free(o->buf);
    o->buf = NULL;
    o->n   = 0;
}
