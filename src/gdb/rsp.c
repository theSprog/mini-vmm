/* src/gdb/rsp.c — GDB Remote Serial Protocol 协议层，接口说明见 gdb/rsp.h
 *
 * 支持的包（GDB 手册 E.2 Packets）：
 *   ?  g  p  P  m  M  Z0/z0  Z1/z1  H  T  D  k
 *   qSupported  qXfer:features:read  qfThreadInfo/qsThreadInfo  qC
 *   qAttached  qThreadExtraInfo  vCont?  vCont
 * 其余包回空应答，这是协议规定的“不支持”。gdb 据此退回替代方案，例如 X
 * 不支持就改用 M，Z2..Z4（watchpoint）不支持就报告无法设置。
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "gdb/rsp.h"

/* ------------------------------------------------------------------ */
/* target.xml                                                          */
/* ------------------------------------------------------------------ */
/*
 * 寄存器描述取自 gdb 源码 gdb/features/i386/64bit-core.xml、64bit-sse.xml、
 * 64bit-segments.xml（原文件的许可：Copying and distribution of this file,
 * with or without modification, are permitted in any medium without royalty
 * provided the copyright notice and this notice are preserved. Copyright (C)
 * 2010-2024 Free Software Foundation, Inc.）。三个 feature 合进一个
 * target.xml 直接给出：用 xi:include 引用时 gdb 会再用 qXfer 向 stub 要
 * 被引用的文件，并不会用它内置的副本。
 *
 * 寄存器编号按出现顺序从 0 起算，xmm0 = 40 和 64bit-sse.xml 里显式写的
 * regnum 一致；struct rsp_regs 与下面 reg_table 的顺序都要和这里对齐。
 */
static const char target_xml[] =
"<?xml version=\"1.0\"?>"
"<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
"<target version=\"1.0\">"
"<architecture>i386:x86-64</architecture>"
"<feature name=\"org.gnu.gdb.i386.core\">"
  "<flags id=\"i386_eflags\" size=\"4\">"
    "<field name=\"CF\" start=\"0\" end=\"0\"/>"
    "<field name=\"\" start=\"1\" end=\"1\"/>"
    "<field name=\"PF\" start=\"2\" end=\"2\"/>"
    "<field name=\"AF\" start=\"4\" end=\"4\"/>"
    "<field name=\"ZF\" start=\"6\" end=\"6\"/>"
    "<field name=\"SF\" start=\"7\" end=\"7\"/>"
    "<field name=\"TF\" start=\"8\" end=\"8\"/>"
    "<field name=\"IF\" start=\"9\" end=\"9\"/>"
    "<field name=\"DF\" start=\"10\" end=\"10\"/>"
    "<field name=\"OF\" start=\"11\" end=\"11\"/>"
    "<field name=\"NT\" start=\"14\" end=\"14\"/>"
    "<field name=\"RF\" start=\"16\" end=\"16\"/>"
    "<field name=\"VM\" start=\"17\" end=\"17\"/>"
    "<field name=\"AC\" start=\"18\" end=\"18\"/>"
    "<field name=\"VIF\" start=\"19\" end=\"19\"/>"
    "<field name=\"VIP\" start=\"20\" end=\"20\"/>"
    "<field name=\"ID\" start=\"21\" end=\"21\"/>"
  "</flags>"
  "<reg name=\"rax\" bitsize=\"64\" type=\"int64\"/>"
  "<reg name=\"rbx\" bitsize=\"64\" type=\"int64\"/>"
  "<reg name=\"rcx\" bitsize=\"64\" type=\"int64\"/>"
  "<reg name=\"rdx\" bitsize=\"64\" type=\"int64\"/>"
  "<reg name=\"rsi\" bitsize=\"64\" type=\"int64\"/>"
  "<reg name=\"rdi\" bitsize=\"64\" type=\"int64\"/>"
  "<reg name=\"rbp\" bitsize=\"64\" type=\"data_ptr\"/>"
  "<reg name=\"rsp\" bitsize=\"64\" type=\"data_ptr\"/>"
  "<reg name=\"r8\" bitsize=\"64\" type=\"int64\"/>"
  "<reg name=\"r9\" bitsize=\"64\" type=\"int64\"/>"
  "<reg name=\"r10\" bitsize=\"64\" type=\"int64\"/>"
  "<reg name=\"r11\" bitsize=\"64\" type=\"int64\"/>"
  "<reg name=\"r12\" bitsize=\"64\" type=\"int64\"/>"
  "<reg name=\"r13\" bitsize=\"64\" type=\"int64\"/>"
  "<reg name=\"r14\" bitsize=\"64\" type=\"int64\"/>"
  "<reg name=\"r15\" bitsize=\"64\" type=\"int64\"/>"
  "<reg name=\"rip\" bitsize=\"64\" type=\"code_ptr\"/>"
  "<reg name=\"eflags\" bitsize=\"32\" type=\"i386_eflags\"/>"
  "<reg name=\"cs\" bitsize=\"32\" type=\"int32\"/>"
  "<reg name=\"ss\" bitsize=\"32\" type=\"int32\"/>"
  "<reg name=\"ds\" bitsize=\"32\" type=\"int32\"/>"
  "<reg name=\"es\" bitsize=\"32\" type=\"int32\"/>"
  "<reg name=\"fs\" bitsize=\"32\" type=\"int32\"/>"
  "<reg name=\"gs\" bitsize=\"32\" type=\"int32\"/>"
  "<reg name=\"st0\" bitsize=\"80\" type=\"i387_ext\"/>"
  "<reg name=\"st1\" bitsize=\"80\" type=\"i387_ext\"/>"
  "<reg name=\"st2\" bitsize=\"80\" type=\"i387_ext\"/>"
  "<reg name=\"st3\" bitsize=\"80\" type=\"i387_ext\"/>"
  "<reg name=\"st4\" bitsize=\"80\" type=\"i387_ext\"/>"
  "<reg name=\"st5\" bitsize=\"80\" type=\"i387_ext\"/>"
  "<reg name=\"st6\" bitsize=\"80\" type=\"i387_ext\"/>"
  "<reg name=\"st7\" bitsize=\"80\" type=\"i387_ext\"/>"
  "<reg name=\"fctrl\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
  "<reg name=\"fstat\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
  "<reg name=\"ftag\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
  "<reg name=\"fiseg\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
  "<reg name=\"fioff\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
  "<reg name=\"foseg\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
  "<reg name=\"fooff\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
  "<reg name=\"fop\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
"</feature>"
"<feature name=\"org.gnu.gdb.i386.sse\">"
  "<vector id=\"v8bf16\" type=\"bfloat16\" count=\"8\"/>"
  "<vector id=\"v8h\" type=\"ieee_half\" count=\"8\"/>"
  "<vector id=\"v4f\" type=\"ieee_single\" count=\"4\"/>"
  "<vector id=\"v2d\" type=\"ieee_double\" count=\"2\"/>"
  "<vector id=\"v16i8\" type=\"int8\" count=\"16\"/>"
  "<vector id=\"v8i16\" type=\"int16\" count=\"8\"/>"
  "<vector id=\"v4i32\" type=\"int32\" count=\"4\"/>"
  "<vector id=\"v2i64\" type=\"int64\" count=\"2\"/>"
  "<union id=\"vec128\">"
    "<field name=\"v8_bfloat16\" type=\"v8bf16\"/>"
    "<field name=\"v8_half\" type=\"v8h\"/>"
    "<field name=\"v4_float\" type=\"v4f\"/>"
    "<field name=\"v2_double\" type=\"v2d\"/>"
    "<field name=\"v16_int8\" type=\"v16i8\"/>"
    "<field name=\"v8_int16\" type=\"v8i16\"/>"
    "<field name=\"v4_int32\" type=\"v4i32\"/>"
    "<field name=\"v2_int64\" type=\"v2i64\"/>"
    "<field name=\"uint128\" type=\"uint128\"/>"
  "</union>"
  "<flags id=\"i386_mxcsr\" size=\"4\">"
    "<field name=\"IE\" start=\"0\" end=\"0\"/>"
    "<field name=\"DE\" start=\"1\" end=\"1\"/>"
    "<field name=\"ZE\" start=\"2\" end=\"2\"/>"
    "<field name=\"OE\" start=\"3\" end=\"3\"/>"
    "<field name=\"UE\" start=\"4\" end=\"4\"/>"
    "<field name=\"PE\" start=\"5\" end=\"5\"/>"
    "<field name=\"DAZ\" start=\"6\" end=\"6\"/>"
    "<field name=\"IM\" start=\"7\" end=\"7\"/>"
    "<field name=\"DM\" start=\"8\" end=\"8\"/>"
    "<field name=\"ZM\" start=\"9\" end=\"9\"/>"
    "<field name=\"OM\" start=\"10\" end=\"10\"/>"
    "<field name=\"UM\" start=\"11\" end=\"11\"/>"
    "<field name=\"PM\" start=\"12\" end=\"12\"/>"
    "<field name=\"FZ\" start=\"15\" end=\"15\"/>"
  "</flags>"
  "<reg name=\"xmm0\" bitsize=\"128\" type=\"vec128\" regnum=\"40\"/>"
  "<reg name=\"xmm1\" bitsize=\"128\" type=\"vec128\"/>"
  "<reg name=\"xmm2\" bitsize=\"128\" type=\"vec128\"/>"
  "<reg name=\"xmm3\" bitsize=\"128\" type=\"vec128\"/>"
  "<reg name=\"xmm4\" bitsize=\"128\" type=\"vec128\"/>"
  "<reg name=\"xmm5\" bitsize=\"128\" type=\"vec128\"/>"
  "<reg name=\"xmm6\" bitsize=\"128\" type=\"vec128\"/>"
  "<reg name=\"xmm7\" bitsize=\"128\" type=\"vec128\"/>"
  "<reg name=\"xmm8\" bitsize=\"128\" type=\"vec128\"/>"
  "<reg name=\"xmm9\" bitsize=\"128\" type=\"vec128\"/>"
  "<reg name=\"xmm10\" bitsize=\"128\" type=\"vec128\"/>"
  "<reg name=\"xmm11\" bitsize=\"128\" type=\"vec128\"/>"
  "<reg name=\"xmm12\" bitsize=\"128\" type=\"vec128\"/>"
  "<reg name=\"xmm13\" bitsize=\"128\" type=\"vec128\"/>"
  "<reg name=\"xmm14\" bitsize=\"128\" type=\"vec128\"/>"
  "<reg name=\"xmm15\" bitsize=\"128\" type=\"vec128\"/>"
  "<reg name=\"mxcsr\" bitsize=\"32\" type=\"i386_mxcsr\" group=\"vector\"/>"
"</feature>"
"<feature name=\"org.gnu.gdb.i386.segments\">"
  "<reg name=\"fs_base\" bitsize=\"64\" type=\"int\"/>"
  "<reg name=\"gs_base\" bitsize=\"64\" type=\"int\"/>"
"</feature>"
"</target>";

/* ------------------------------------------------------------------ */
/* 寄存器表                                                            */
/* ------------------------------------------------------------------ */
/*
 * 每个编号在 struct rsp_regs 里的偏移和字节数。REG_NA 的五个 x87 寄存器
 * 在 g/p 应答里报成 'x'（附录 E 'g' 包：stub 无法取得的寄存器值）：
 * KVM_GET_FPU 返回的是 FXSAVE 格式，标记字是 8 位的简写形式，而 gdb 的
 * ftag 是完整的 16 位标记字，要按每个物理寄存器的内容重新推算；64 位模式
 * 下 FPU 指令/数据指针是不带段选择子的 64 位偏移，拆不成 fiseg:fioff /
 * foseg:fooff 这样的 32 位字段。报不可用比编一个值诚实。
 */
#define REG_NA  0xffff

struct reg_desc {
    uint16_t off;
    uint8_t  size;
};

#define REG(f, n)  { (uint16_t)offsetof(struct rsp_regs, f), n }
#define NA(n)      { REG_NA, n }

static const struct reg_desc reg_table[] = {
    REG(gpr[0], 8),  REG(gpr[1], 8),  REG(gpr[2], 8),  REG(gpr[3], 8),
    REG(gpr[4], 8),  REG(gpr[5], 8),  REG(gpr[6], 8),  REG(gpr[7], 8),
    REG(gpr[8], 8),  REG(gpr[9], 8),  REG(gpr[10], 8), REG(gpr[11], 8),
    REG(gpr[12], 8), REG(gpr[13], 8), REG(gpr[14], 8), REG(gpr[15], 8),
    REG(rip, 8),     REG(eflags, 4),
    REG(seg[0], 4),  REG(seg[1], 4),  REG(seg[2], 4),
    REG(seg[3], 4),  REG(seg[4], 4),  REG(seg[5], 4),
    REG(st[0], 10),  REG(st[1], 10),  REG(st[2], 10), REG(st[3], 10),
    REG(st[4], 10),  REG(st[5], 10),  REG(st[6], 10), REG(st[7], 10),
    REG(fctrl, 4),   REG(fstat, 4),
    NA(4) /* ftag */, NA(4) /* fiseg */, NA(4) /* fioff */,
    NA(4) /* foseg */, NA(4) /* fooff */,
    REG(fop, 4),
    REG(xmm[0], 16),  REG(xmm[1], 16),  REG(xmm[2], 16),  REG(xmm[3], 16),
    REG(xmm[4], 16),  REG(xmm[5], 16),  REG(xmm[6], 16),  REG(xmm[7], 16),
    REG(xmm[8], 16),  REG(xmm[9], 16),  REG(xmm[10], 16), REG(xmm[11], 16),
    REG(xmm[12], 16), REG(xmm[13], 16), REG(xmm[14], 16), REG(xmm[15], 16),
    REG(mxcsr, 4),
    REG(fs_base, 8), REG(gs_base, 8),
};

#define NR_REGS  ((int)(sizeof(reg_table) / sizeof(reg_table[0])))

_Static_assert(sizeof(reg_table) / sizeof(reg_table[0]) == 59,
               "register count must match target.xml");

/* ------------------------------------------------------------------ */
/* 编解码                                                              */
/* ------------------------------------------------------------------ */

static const char hexdig[] = "0123456789abcdef";

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 解析一个十六进制数，*pp 前进到第一个非十六进制字符。至少要有一位，
 * 超过 64 位视为格式错误。 */
static bool parse_hex(const char **pp, uint64_t *out)
{
    const char *p = *pp;
    uint64_t v = 0;
    int n = 0, d;

    while ((d = hexval(*p)) >= 0) {
        if (++n > 16)
            return false;
        v = (v << 4) | (uint64_t)d;
        p++;
    }
    if (!n)
        return false;
    *pp = p;
    *out = v;
    return true;
}

/* 线程号："-1"（所有线程）或十六进制数 */
static bool parse_tid(const char **pp, int64_t *tid)
{
    uint64_t v;

    if ((*pp)[0] == '-' && (*pp)[1] == '1') {
        *pp += 2;
        *tid = -1;
        return true;
    }
    if (!parse_hex(pp, &v) || v > RSP_MAX_CPUS)
        return false;
    *tid = (int64_t)v;
    return true;
}

static char *hex_encode(char *dst, const void *src, size_t len)
{
    const uint8_t *s = src;
    size_t i;

    for (i = 0; i < len; i++) {
        *dst++ = hexdig[s[i] >> 4];
        *dst++ = hexdig[s[i] & 0xf];
    }
    return dst;
}

/* 恰好解出 len 字节，src 里多出或缺少字符都算错 */
static bool hex_decode(void *dst, const char *src, size_t len)
{
    uint8_t *d = dst;
    size_t i;

    for (i = 0; i < len; i++) {
        int hi = hexval(src[2 * i]), lo;

        if (hi < 0)
            return false;
        lo = hexval(src[2 * i + 1]);
        if (lo < 0)
            return false;
        d[i] = (uint8_t)(hi << 4 | lo);
    }
    return src[2 * len] == '\0';
}

/* ------------------------------------------------------------------ */
/* 发送                                                                */
/* ------------------------------------------------------------------ */

static void raw_write(struct rsp_session *s, const void *buf, size_t len)
{
    if (s->write(s->io, buf, len) != 0)
        s->io_err = true;
}

/*
 * 数据里的 '#' '$' '}' 必须转义成 '}' 加原字节异或 0x20；stub 发出的数据
 * 还要转义 '*'，否则 gdb 会把它当成游程编码的开头（附录 E.1）。校验和按
 * 转义后实际发出的字节计算。
 */
size_t rsp_frame(char *out, const char *data, size_t len)
{
    char *o = out;
    uint8_t sum = 0;
    size_t i;

    *o++ = '$';
    for (i = 0; i < len; i++) {
        char c = data[i];

        if (c == '#' || c == '$' || c == '}' || c == '*') {
            *o++ = '}';
            sum += '}';
            c ^= 0x20;
        }
        *o++ = c;
        sum += (uint8_t)c;
    }
    *o++ = '#';
    *o++ = hexdig[sum >> 4];
    *o++ = hexdig[sum & 0xf];
    return (size_t)(o - out);
}

/* 应答最长是 m 包的 RSP_PACKET_SIZE 个十六进制字符，last 按最坏的转义
 * 留足了 2 * RSP_PACKET_SIZE + 8 */
static void send_packet(struct rsp_session *s, const char *data, size_t len)
{
    s->last_len = rsp_frame(s->last, data, len);
    raw_write(s, s->last, s->last_len);
}

static void reply(struct rsp_session *s, const char *str)
{
    send_packet(s, str, strlen(str));
}

#define REPLY_OK        "OK"
#define REPLY_EMPTY     ""          /* 不支持的包 */
#define REPLY_EINVAL    "E01"       /* 包格式错误或参数不被接受 */
#define REPLY_EFAULT    "E0e"       /* 内存不可访问（EFAULT = 14） */

/* ------------------------------------------------------------------ */
/* 停止应答                                                            */
/* ------------------------------------------------------------------ */

static void reply_stop(struct rsp_session *s, const struct rsp_stop *st)
{
    char buf[64];
    const char *reason = "";
    int sig = 5;            /* SIGTRAP */

    switch (st->kind) {
    case RSP_STOP_EXITED:
        snprintf(buf, sizeof(buf), "W%02x", st->exit_code & 0xff);
        reply(s, buf);
        return;
    case RSP_STOP_INTERRUPT:
        sig = 2;            /* SIGINT */
        break;
    /* swbreak/hwbreak 只在 gdb 声明支持时才发（附录 E.3 Stop Reply
     * Packets）。swbreak 告诉 gdb pc 已经指向断点地址本身：x86 上 gdb 默认
     * 认为 int3 执行后 pc 停在断点地址 + 1，没有这个原因时它会自己判断要不
     * 要把 pc 减一，相邻两个断点时会判断错。KVM 报告的 rip 正是 int3 的
     * 地址，见 gdb/x86.h。 */
    case RSP_STOP_SWBREAK:
        if (s->gdb_swbreak)
            reason = "swbreak:;";
        break;
    case RSP_STOP_HWBREAK:
        if (s->gdb_hwbreak)
            reason = "hwbreak:;";
        break;
    case RSP_STOP_TRAP:
        break;
    }
    snprintf(buf, sizeof(buf), "T%02xthread:%x;%s", sig, st->cpu + 1, reason);
    reply(s, buf);
}

/* ------------------------------------------------------------------ */
/* 命令                                                                */
/* ------------------------------------------------------------------ */

static void encode_reg(char **o, const struct rsp_regs *r, int n)
{
    const struct reg_desc *d = &reg_table[n];

    if (d->off == REG_NA) {
        memset(*o, 'x', 2 * d->size);
        *o += 2 * d->size;
    } else {
        *o = hex_encode(*o, (const uint8_t *)r + d->off, d->size);
    }
}

static void handle_g(struct rsp_session *s)
{
    struct rsp_regs r;
    char buf[2 * 1024], *o = buf;   /* 59 个寄存器共 552 字节 */
    int n;

    if (s->ops->get_regs(s->target, s->g_cpu, &r) < 0) {
        reply(s, REPLY_EINVAL);
        return;
    }
    for (n = 0; n < NR_REGS; n++)
        encode_reg(&o, &r, n);
    send_packet(s, buf, (size_t)(o - buf));
}

static void handle_p(struct rsp_session *s, const char *p)
{
    struct rsp_regs r;
    char buf[64], *o = buf;
    uint64_t n;

    if (!parse_hex(&p, &n) || *p || n >= (uint64_t)NR_REGS) {
        reply(s, REPLY_EINVAL);
        return;
    }
    if (s->ops->get_regs(s->target, s->g_cpu, &r) < 0) {
        reply(s, REPLY_EINVAL);
        return;
    }
    encode_reg(&o, &r, (int)n);
    send_packet(s, buf, (size_t)(o - buf));
}

/*
 * 只接受 gpr、rip、eflags 的写入。段寄存器在 gdb 里只是一个选择子，而
 * CPU 实际使用的是 KVM sregs 里的影子描述符（base/limit/属性），单改
 * 选择子不改影子，两者不一致，guest 下一次重载段寄存器时的行为无法预料；
 * x87/SSE 寄存器和 fs_base/gs_base 在内核调试里没有改写的需要。
 * 不支持 G（一次写全部寄存器）：gdb 只在 stub 不支持 P 时才用 G。
 */
static void handle_P(struct rsp_session *s, const char *p)
{
    struct rsp_regs r;
    const struct reg_desc *d;
    uint64_t n;

    if (!parse_hex(&p, &n) || *p++ != '=' || n >= (uint64_t)NR_REGS) {
        reply(s, REPLY_EINVAL);
        return;
    }
    if (n >= RSP_NR_WRITABLE) {
        reply(s, REPLY_EINVAL);
        return;
    }
    d = &reg_table[n];
    if (s->ops->get_regs(s->target, s->g_cpu, &r) < 0 ||
        !hex_decode((uint8_t *)&r + d->off, p, d->size) ||
        s->ops->set_regs(s->target, s->g_cpu, &r) < 0) {
        reply(s, REPLY_EINVAL);
        return;
    }
    reply(s, REPLY_OK);
}

/* "addr,len" */
static bool parse_addr_len(const char **pp, uint64_t *addr, uint64_t *len)
{
    return parse_hex(pp, addr) && *(*pp)++ == ',' && parse_hex(pp, len);
}

static void handle_m(struct rsp_session *s, const char *p)
{
    uint8_t data[RSP_PACKET_SIZE / 2];
    char buf[RSP_PACKET_SIZE];
    uint64_t addr, len;
    size_t n;

    if (!parse_addr_len(&p, &addr, &len) || *p) {
        reply(s, REPLY_EINVAL);
        return;
    }
    /* 应答可以比请求短（附录 E 'm' 包），gdb 会接着读剩下的部分 */
    if (len > sizeof(data))
        len = sizeof(data);
    n = s->ops->read_mem(s->target, s->g_cpu, addr, data, (size_t)len);
    if (!n && len) {
        reply(s, REPLY_EFAULT);
        return;
    }
    send_packet(s, buf, (size_t)(hex_encode(buf, data, n) - buf));
}

static void handle_M(struct rsp_session *s, const char *p)
{
    uint8_t data[RSP_PACKET_SIZE / 2];
    uint64_t addr, len;

    if (!parse_addr_len(&p, &addr, &len) || *p++ != ':' ||
        len > sizeof(data) || !hex_decode(data, p, (size_t)len)) {
        reply(s, REPLY_EINVAL);
        return;
    }
    if (s->ops->write_mem(s->target, s->g_cpu, addr, data, (size_t)len) != len) {
        reply(s, REPLY_EFAULT);
        return;
    }
    reply(s, REPLY_OK);
}

/* Z/z TYPE,ADDR,KIND。KIND 在 x86 上是断点指令的长度，int3 只有 1 字节，
 * 硬件指令断点也只能是 1 字节，所以忽略。 */
static void handle_Z(struct rsp_session *s, const char *p, bool insert)
{
    uint64_t type, addr, kind;
    int r;

    if (!parse_hex(&p, &type) || *p++ != ',' ||
        !parse_addr_len(&p, &addr, &kind)) {
        reply(s, REPLY_EINVAL);
        return;
    }
    if (type != RSP_BP_SW && type != RSP_BP_HW) {
        reply(s, REPLY_EMPTY);
        return;
    }
    r = insert ? s->ops->insert_bp(s->target, s->g_cpu,
                                   (enum rsp_bp_type)type, addr)
               : s->ops->remove_bp(s->target, s->g_cpu,
                                   (enum rsp_bp_type)type, addr);
    reply(s, r < 0 ? REPLY_EINVAL : REPLY_OK);
}

/* Hg 选择 g/p/P/m/M/Z 作用的线程；0 和 -1 表示“任意”，保持原选择。
 * Hc 只对老式 c/s 包有意义，这里用 vCont，接受但不记录。 */
static void handle_H(struct rsp_session *s, const char *p)
{
    char op = *p++;
    int64_t tid;

    if ((op != 'g' && op != 'c') || !parse_tid(&p, &tid) || *p ||
        tid > s->nr_cpus) {
        reply(s, REPLY_EINVAL);
        return;
    }
    if (op == 'g' && tid > 0)
        s->g_cpu = (int)tid - 1;
    reply(s, REPLY_OK);
}

static void handle_T(struct rsp_session *s, const char *p)
{
    int64_t tid;

    if (!parse_tid(&p, &tid) || *p || tid < 1 || tid > s->nr_cpus)
        reply(s, REPLY_EINVAL);
    else
        reply(s, REPLY_OK);
}

static void handle_qsupported(struct rsp_session *s, const char *p)
{
    char buf[128];

    /* gdb 的特性列表形如 "multiprocess+;swbreak+;hwbreak+;..." */
    s->gdb_swbreak = strstr(p, "swbreak+") != NULL;
    s->gdb_hwbreak = strstr(p, "hwbreak+") != NULL;
    snprintf(buf, sizeof(buf),
             "PacketSize=%x;qXfer:features:read+;swbreak+;hwbreak+",
             RSP_PACKET_SIZE);
    reply(s, buf);
}

static void handle_qxfer_features(struct rsp_session *s, const char *p)
{
    static const char annex[] = "target.xml:";
    char buf[RSP_PACKET_SIZE];
    uint64_t off, len;
    size_t total = sizeof(target_xml) - 1, n;

    if (strncmp(p, annex, sizeof(annex) - 1)) {
        reply(s, "E00");        /* 没有这个 annex */
        return;
    }
    p += sizeof(annex) - 1;
    if (!parse_addr_len(&p, &off, &len) || *p) {
        reply(s, REPLY_EINVAL);
        return;
    }
    if (off >= total) {
        reply(s, "l");
        return;
    }
    /* 留出 'm'/'l' 前缀和转义的余量：target.xml 里没有需要转义的字符，
     * 但 send_packet 照样按最坏情况一字节变两字节 */
    n = total - (size_t)off;
    if (n > len)
        n = (size_t)len;
    if (n > (sizeof(s->last) - 8) / 2 - 1)
        n = (sizeof(s->last) - 8) / 2 - 1;
    if (n > sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    buf[0] = (off + n < total) ? 'm' : 'l';
    memcpy(buf + 1, target_xml + off, n);
    send_packet(s, buf, n + 1);
}

static void handle_q(struct rsp_session *s, const char *p)
{
    char buf[64];
    int i, n;

    if (!strncmp(p, "qSupported", 10)) {
        handle_qsupported(s, p + 10);
    } else if (!strncmp(p, "qXfer:features:read:", 20)) {
        handle_qxfer_features(s, p + 20);
    } else if (!strcmp(p, "qfThreadInfo")) {
        n = snprintf(buf, sizeof(buf), "m");
        for (i = 0; i < s->nr_cpus && n < (int)sizeof(buf) - 4; i++)
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%x",
                          i ? "," : "", i + 1);
        reply(s, buf);
    } else if (!strcmp(p, "qsThreadInfo")) {
        reply(s, "l");          /* qfThreadInfo 已经给出全部线程 */
    } else if (!strcmp(p, "qC")) {
        snprintf(buf, sizeof(buf), "QC%x", s->last_stop.cpu + 1);
        reply(s, buf);
    } else if (!strcmp(p, "qAttached")) {
        /* 1：附加到一个已经存在的进程。gdb 退出时据此发 D（让 VM 继续跑）
         * 而不是 k（结束 VM） */
        reply(s, "1");
    } else if (!strncmp(p, "qThreadExtraInfo,", 17)) {
        const char *q = p + 17;
        char text[16];
        int64_t tid;

        if (!parse_tid(&q, &tid) || *q || tid < 1 || tid > s->nr_cpus) {
            reply(s, REPLY_EINVAL);
            return;
        }
        n = snprintf(text, sizeof(text), "vCPU %d", (int)tid - 1);
        send_packet(s, buf, (size_t)(hex_encode(buf, text, (size_t)n) - buf));
    } else {
        reply(s, REPLY_EMPTY);
    }
}

/*
 * vCont;ACTION[:TID]...：每个线程取最左边匹配它的动作，不带 TID 的动作
 * 匹配所有线程，没有匹配的线程保持停止（附录 E 'vCont' 包）。
 * C/S 带的信号号被忽略：VM 里没有 POSIX 信号可以投递，gdb 只在用户显式
 * 要求把信号传给被调试程序时才发 C/S。
 */
static void handle_vcont(struct rsp_session *s, const char *p)
{
    enum rsp_action act[RSP_MAX_CPUS] = { RSP_ACT_NONE };
    bool any = false;
    int i;

    if (!*p) {
        reply(s, REPLY_EINVAL);     /* 至少要有一个动作 */
        return;
    }
    while (*p) {
        enum rsp_action a;
        int64_t tid = -1;
        uint64_t sig;

        if (*p++ != ';') {
            reply(s, REPLY_EINVAL);
            return;
        }
        switch (*p++) {
        case 'c': a = RSP_ACT_CONT; break;
        case 's': a = RSP_ACT_STEP; break;
        case 'C': a = RSP_ACT_CONT; if (!parse_hex(&p, &sig)) goto bad; break;
        case 'S': a = RSP_ACT_STEP; if (!parse_hex(&p, &sig)) goto bad; break;
        default:  goto bad;
        }
        if (*p == ':') {
            p++;
            if (!parse_tid(&p, &tid) || tid == 0)
                goto bad;
        }
        for (i = 0; i < s->nr_cpus; i++) {
            if ((tid == -1 || tid == i + 1) && act[i] == RSP_ACT_NONE)
                act[i] = a;
        }
    }
    for (i = 0; i < s->nr_cpus; i++)
        any |= act[i] != RSP_ACT_NONE;
    if (!any)
        goto bad;           /* 只点了不存在的线程，没有谁会停下来报告 */

    s->running = true;
    s->ops->resume(s->target, act);
    return;
bad:
    reply(s, REPLY_EINVAL);
}

static void handle_packet(struct rsp_session *s)
{
    const char *p = s->pkt;

    switch (p[0]) {
    case '?': reply_stop(s, &s->last_stop);  break;
    case 'g': handle_g(s);                   break;
    case 'p': handle_p(s, p + 1);            break;
    case 'P': handle_P(s, p + 1);            break;
    case 'm': handle_m(s, p + 1);            break;
    case 'M': handle_M(s, p + 1);            break;
    case 'Z': handle_Z(s, p + 1, true);      break;
    case 'z': handle_Z(s, p + 1, false);     break;
    case 'H': handle_H(s, p + 1);            break;
    case 'T': handle_T(s, p + 1);            break;
    case 'q': handle_q(s, p);                break;
    case 'D':
        s->ops->detach(s->target);
        reply(s, REPLY_OK);
        s->closed = true;
        break;
    case 'k':                                /* k 没有应答 */
        s->ops->kill(s->target);
        s->closed = true;
        break;
    case 'v':
        if (!strcmp(p, "vCont?"))
            reply(s, "vCont;c;C;s;S");
        else if (!strncmp(p, "vCont", 5) && (p[5] == ';' || !p[5]))
            handle_vcont(s, p + 5);
        else
            reply(s, REPLY_EMPTY);
        break;
    default:
        reply(s, REPLY_EMPTY);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 会话                                                                */
/* ------------------------------------------------------------------ */

void rsp_session_init(struct rsp_session *s, const struct rsp_target_ops *ops,
                      void *target, int nr_cpus,
                      int (*write)(void *io, const void *buf, size_t len),
                      void *io)
{
    memset(s, 0, sizeof(*s));
    s->ops = ops;
    s->target = target;
    s->nr_cpus = nr_cpus > RSP_MAX_CPUS ? RSP_MAX_CPUS : nr_cpus;
    s->write = write;
    s->io = io;
    s->rx = RSP_RX_IDLE;
    s->last_stop.kind = RSP_STOP_TRAP;
    s->last_stop.cpu = 0;
}

static void pkt_put(struct rsp_session *s, char c)
{
    if (s->pkt_len < RSP_PACKET_SIZE)
        s->pkt[s->pkt_len++] = c;
    else
        s->pkt_overflow = true;
}

/*
 * 解包状态机。ack 规则（附录 E.1）：收到完整的包回 '+'，校验和不对或包
 * 超长回 '-' 让 gdb 重发；gdb 对我们的包回 '-' 时重发上一个包。
 * 不支持 QStartNoAckMode：ack 在 TCP 上只多一个字节，关掉它要多维护一种
 * 会话状态。
 *
 * 目标机在跑时只认 0x03（Ctrl-C，在包之外单独发送）。all-stop 模式下 gdb
 * 在等停止应答期间不发别的包；万一收到，不交给 handle_packet，因为那时
 * vCPU 在跑，target 回调的前提（目标机已停止）不成立。
 */
enum rsp_status rsp_feed(struct rsp_session *s, const void *buf, size_t len)
{
    const uint8_t *in = buf;
    size_t i;

    s->io_err = false;
    for (i = 0; i < len && !s->closed; i++) {
        uint8_t c = in[i];
        int d;

        switch (s->rx) {
        case RSP_RX_IDLE:
            if (c == '$') {
                s->rx = RSP_RX_DATA;
                s->pkt_len = 0;
                s->pkt_overflow = false;
                s->sum = 0;
            } else if (c == '-' && s->last_len) {
                raw_write(s, s->last, s->last_len);
            } else if (c == 0x03 && s->running) {
                s->ops->interrupt(s->target);
            }
            break;              /* '+' 和其他杂散字节直接丢弃 */
        case RSP_RX_DATA:
            if (c == '#') {
                s->rx = RSP_RX_CS1;
                break;
            }
            s->sum += c;
            if (c == '}')
                s->rx = RSP_RX_ESC;
            else
                pkt_put(s, (char)c);
            break;
        case RSP_RX_ESC:
            s->sum += c;
            pkt_put(s, (char)(c ^ 0x20));
            s->rx = RSP_RX_DATA;
            break;
        case RSP_RX_CS1:
            d = hexval((char)c);
            s->cs_hi = (uint8_t)(d < 0 ? 0xff : d);
            s->rx = RSP_RX_CS2;
            break;
        case RSP_RX_CS2:
            d = hexval((char)c);
            s->rx = RSP_RX_IDLE;
            if (d < 0 || s->cs_hi == 0xff ||
                (uint8_t)(s->cs_hi << 4 | d) != s->sum || s->pkt_overflow) {
                raw_write(s, "-", 1);
                break;
            }
            raw_write(s, "+", 1);
            if (s->running)
                break;
            s->pkt[s->pkt_len] = '\0';
            handle_packet(s);
            break;
        }
    }
    if (s->io_err)
        return RSP_IO_ERROR;
    return s->closed ? RSP_CLOSED : RSP_CONTINUE;
}

enum rsp_status rsp_report_stop(struct rsp_session *s,
                                const struct rsp_stop *st)
{
    s->io_err = false;
    s->running = false;
    s->last_stop = *st;
    if (st->kind != RSP_STOP_EXITED)
        s->g_cpu = st->cpu;
    reply_stop(s, st);
    if (st->kind == RSP_STOP_EXITED)
        s->closed = true;
    if (s->io_err)
        return RSP_IO_ERROR;
    return s->closed ? RSP_CLOSED : RSP_CONTINUE;
}
