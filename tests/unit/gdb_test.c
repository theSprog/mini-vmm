/* tests/unit/gdb_test.c — gdbstub 的协议层和 x86 纯函数
 *
 * 协议层接一个纯内存的假 target，逐字节喂入 gdb 会发的包，断言应答的
 * 字节。判据来源：
 *   - 包格式、ack、转义、各条命令的应答：GDB 手册附录 E
 *   - g 包的寄存器顺序和宽度：target.xml，也就是 gdb 源码
 *     gdb/features/i386/64bit-{core,sse,segments}.xml
 *   - DR7/DR6 位：Intel SDM Vol.3 Debug Registers 一节，取值与 asm/debugreg.h、
 *     kvm_host.h 的 DR7_FIXED_1 一致；kvm_regs 字段：linux/kvm.h
 * e2e（tests/e2e/gdbstub.sh）用真实 gdb 证明两边对得上，这里负责把每个
 * 编码细节钉住，出错时直接指出是哪个字段。
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gdb/rsp.h"
#include "gdb/x86.h"
#include "harness.h"

/* ------------------------------------------------------------------ */
/* 假 target                                                           */
/* ------------------------------------------------------------------ */

#define NCPU      2
#define MEM_BASE  0x1000u
#define MEM_SIZE  0x2000u     /* [0x1000, 0x3000) 可访问，其余地址翻译失败 */

struct fake {
    struct rsp_regs regs[NCPU];
    uint8_t mem[MEM_SIZE];
    struct gdb_x86_bps bps;     /* GVA 与 GPA 相同的恒等映射 */
    enum rsp_action act[RSP_MAX_CPUS];
    int resumed, interrupted, detached, killed;
    int last_cpu;               /* 最近一次回调用到的 cpu */
};

static struct fake F;

static int f_get_regs(void *t, int cpu, struct rsp_regs *r)
{
    (void)t;
    F.last_cpu = cpu;
    *r = F.regs[cpu];
    return 0;
}

static int f_set_regs(void *t, int cpu, const struct rsp_regs *r)
{
    (void)t;
    F.regs[cpu] = *r;
    return 0;
}

static size_t f_access(uint64_t addr, size_t len)
{
    if (addr < MEM_BASE || addr >= MEM_BASE + MEM_SIZE)
        return 0;
    if (len > MEM_BASE + MEM_SIZE - addr)
        len = MEM_BASE + MEM_SIZE - addr;
    return len;
}

static size_t f_read_mem(void *t, int cpu, uint64_t addr, void *buf, size_t len)
{
    size_t n = f_access(addr, len);

    (void)t;
    F.last_cpu = cpu;
    memcpy(buf, F.mem + (addr - MEM_BASE) * (n != 0), n);
    gdb_x86_sw_hide(&F.bps, addr, buf, n);
    return n;
}

static size_t f_write_mem(void *t, int cpu, uint64_t addr, const void *buf,
                          size_t len)
{
    size_t n = f_access(addr, len);
    uint8_t tmp[RSP_PACKET_SIZE];

    (void)t; (void)cpu;
    memcpy(tmp, buf, n);
    gdb_x86_sw_shadow_write(&F.bps, addr, tmp, n);
    memcpy(F.mem + (addr - MEM_BASE) * (n != 0), tmp, n);
    return n;
}

static int f_insert_bp(void *t, int cpu, enum rsp_bp_type type, uint64_t addr)
{
    int r;

    (void)t; (void)cpu;
    if (type == RSP_BP_HW)
        return gdb_x86_hw_add(&F.bps, addr) < 0 ? -ENOSPC : 0;
    if (!f_access(addr, 1))
        return -EFAULT;
    r = gdb_x86_sw_add(&F.bps, addr, addr, F.mem[addr - MEM_BASE]);
    if (r == 0)
        F.mem[addr - MEM_BASE] = GDB_X86_INT3;
    return r;
}

static int f_remove_bp(void *t, int cpu, enum rsp_bp_type type, uint64_t addr)
{
    struct gdb_x86_sw_bp bp;

    (void)t; (void)cpu;
    if (type == RSP_BP_HW)
        return gdb_x86_hw_del(&F.bps, addr);
    if (gdb_x86_sw_del(&F.bps, addr, &bp) < 0)
        return -ENOENT;
    F.mem[bp.gpa - MEM_BASE] = bp.orig;
    return 0;
}

static void f_resume(void *t, const enum rsp_action *act)
{
    (void)t;
    memcpy(F.act, act, sizeof(F.act));
    F.resumed++;
}

static void f_interrupt(void *t) { (void)t; F.interrupted++; }
static void f_detach(void *t)    { (void)t; F.detached++; }
static void f_kill(void *t)      { (void)t; F.killed++; }

static const struct rsp_target_ops fake_ops = {
    .get_regs  = f_get_regs,
    .set_regs  = f_set_regs,
    .read_mem  = f_read_mem,
    .write_mem = f_write_mem,
    .insert_bp = f_insert_bp,
    .remove_bp = f_remove_bp,
    .resume    = f_resume,
    .interrupt = f_interrupt,
    .detach    = f_detach,
    .kill      = f_kill,
};

/* ------------------------------------------------------------------ */
/* 会话驱动                                                            */
/* ------------------------------------------------------------------ */

static char   g_out[3 * RSP_PACKET_SIZE];
static size_t g_out_len;
static struct rsp_session S;

static int cap_write(void *io, const void *buf, size_t len)
{
    (void)io;
    if (g_out_len + len > sizeof(g_out))
        return -1;
    memcpy(g_out + g_out_len, buf, len);
    g_out_len += len;
    return 0;
}

static void reset(void)
{
    int i;

    memset(&F, 0, sizeof(F));
    for (i = 0; i < (int)MEM_SIZE; i++)
        F.mem[i] = (uint8_t)i;
    rsp_session_init(&S, &fake_ops, NULL, NCPU, cap_write, NULL);
    g_out_len = 0;
}

static enum rsp_status feed(const char *bytes, size_t len)
{
    g_out_len = 0;
    return rsp_feed(&S, bytes, len);
}

/* 按协议组一个包喂进去：$data#cs */
static enum rsp_status send_pkt(const char *data)
{
    char buf[2 * RSP_PACKET_SIZE];
    unsigned sum = 0;
    size_t i, n = strlen(data);

    for (i = 0; i < n; i++)
        sum += (uint8_t)data[i];
    snprintf(buf, sizeof(buf), "$%s#%02x", data, sum & 0xff);
    return feed(buf, strlen(buf));
}

/* 校验 stub 发出的 "+$...#cs"，返回数据部分（静态缓冲区）。
 * 没有包或校验和不对时返回 NULL。 */
static const char *reply_of_out(bool expect_ack)
{
    static char data[2 * RSP_PACKET_SIZE];
    const char *p = g_out, *end = g_out + g_out_len;
    unsigned sum = 0, cs;
    size_t n = 0;

    if (expect_ack) {
        if (p >= end || *p != '+')
            return NULL;
        p++;
    }
    if (p >= end || *p++ != '$')
        return NULL;
    while (p < end && *p != '#') {
        sum += (uint8_t)*p;
        data[n++] = *p++;
    }
    if (end - p != 3 || sscanf(p + 1, "%2x", &cs) != 1 || cs != (sum & 0xff))
        return NULL;
    data[n] = '\0';
    return data;
}

static const char *roundtrip(const char *pkt)
{
    send_pkt(pkt);
    return reply_of_out(true);
}

#define CHECK_REPLY(pkt, want) do {                                      \
        const char *got_ = roundtrip(pkt);                              \
        CHECK(got_ && !strcmp(got_, want),                              \
              "%s -> \"%s\", want \"%s\"", pkt, got_ ? got_ : "(none)", \
              want);                                                    \
    } while (0)

static void le_hex(char *dst, uint64_t v, int bytes)
{
    int i;

    for (i = 0; i < bytes; i++)
        sprintf(dst + 2 * i, "%02x", (unsigned)((v >> (8 * i)) & 0xff));
}

/* ------------------------------------------------------------------ */
/* 协议层                                                              */
/* ------------------------------------------------------------------ */

static void case_framing(void)
{
    T_CASE("framing: ack, bad checksum, retransmit, escapes, overflow");
    reset();

    CHECK_REPLY("?", "T05thread:1;");

    feed("$?#00", 5);
    CHECK(g_out_len == 1 && g_out[0] == '-',
          "bad checksum must be NAKed and not answered, got %zu bytes",
          g_out_len);

    send_pkt("qC");
    feed("-", 1);
    CHECK(g_out_len > 0 && !strncmp(g_out, "$QC1#", 5),
          "'-' from gdb must resend the last packet, got \"%.*s\"",
          (int)g_out_len, g_out);

    feed("+", 1);
    CHECK(g_out_len == 0, "'+' from gdb produces no output");

    /* "}]" 是转义后的 '}'。校验和按线上的原始字节算：0x7d + 0x5d + 'q'。
     * 去转义后的包是未知命令，回空包，关键是 ack 为 '+' */
    {
        char buf[16];
        unsigned sum = 'q' + '}' + ']';

        snprintf(buf, sizeof(buf), "$q}]#%02x", sum & 0xff);
        feed(buf, strlen(buf));
        CHECK(reply_of_out(true) && !strcmp(reply_of_out(true), ""),
              "escaped packet: checksum over raw bytes, got \"%.*s\"",
              (int)g_out_len, g_out);
    }

    /* 去转义的结果决定执行哪条命令："}Q" 是 0x51 ^ 0x20 = 'q'，包就是 qC */
    {
        char buf[16];
        unsigned sum = '}' + 'Q' + 'C';

        snprintf(buf, sizeof(buf), "$}QC#%02x", sum & 0xff);
        feed(buf, strlen(buf));
        CHECK(reply_of_out(true) && !strcmp(reply_of_out(true), "QC1"),
              "escaped 'q' must decode to qC, got \"%.*s\"",
              (int)g_out_len, g_out);
    }

    /* 超过 PacketSize 的包：回 '-'，不截断执行 */
    {
        static char big[RSP_PACKET_SIZE + 64];

        memset(big, '0', sizeof(big) - 1);
        big[0] = 'M';
        big[sizeof(big) - 1] = '\0';
        send_pkt(big);
        CHECK(g_out_len == 1 && g_out[0] == '-',
              "oversized packet must be NAKed, got %zu bytes", g_out_len);
    }

    /* 包和包可以在一次 read 里背靠背到达 */
    {
        char two[32];

        snprintf(two, sizeof(two), "$qC#b4$qAttached#8f");
        feed(two, strlen(two));
        CHECK(g_out_len > 0 && strstr(g_out, "$QC1#") &&
              strstr(g_out, "$1#31"),
              "two packets in one read, got \"%.*s\"", (int)g_out_len, g_out);
    }

    CHECK_REPLY("vMustReplyEmpty", "");
    CHECK_REPLY("X1000,0:", "");    /* 不支持 X，gdb 退回 M */
    CHECK_REPLY("G00", "");         /* 不支持 G，gdb 用 P */
}

/* 现有应答里没有要转义的字符，stub 方向的转义只能直接测 rsp_frame。
 * 校验和是线上字节之和：'}' 0x7d、转义后的字节、普通字节都算。 */
static void case_frame_escape(void)
{
    char out[64];
    size_t n;

    T_CASE("rsp_frame: stub escapes # $ } * and sums the escaped bytes");

    n = rsp_frame(out, "OK", 2);
    out[n] = '\0';
    CHECK(!strcmp(out, "$OK#9a"), "plain packet: %s", out);

    /* '#'^0x20 = 0x03，'$'^0x20 = 0x04，'}'^0x20 = ']'，'*'^0x20 = 0x0a */
    n = rsp_frame(out, "a#$}*b", 6);
    {
        static const char want_data[] = "a}\x03}\x04}]}\x0a" "b";
        unsigned sum = 0;
        char want[64];
        size_t i, wl = sizeof(want_data) - 1;

        for (i = 0; i < wl; i++)
            sum += (uint8_t)want_data[i];
        want[0] = '$';
        memcpy(want + 1, want_data, wl);
        snprintf(want + 1 + wl, sizeof(want) - 1 - wl, "#%02x", sum & 0xff);
        CHECK(n == wl + 4 && !memcmp(out, want, n),
              "escaped packet: got %zu bytes, want %zu (\"%s\")", n, wl + 4,
              want);
    }
}

static void case_qsupported_and_xml(void)
{
    const char *r;
    char xml[16384] = "", req[64];
    size_t off = 0;
    int regs = 0, rounds = 0;
    const char *p;

    T_CASE("qSupported and qXfer:features:read:target.xml");
    reset();

    r = roundtrip("qSupported:multiprocess+;swbreak+;hwbreak+;xmlRegisters=i386");
    CHECK(r && strstr(r, "PacketSize=1000") &&
          strstr(r, "qXfer:features:read+") && strstr(r, "swbreak+") &&
          strstr(r, "hwbreak+"), "qSupported reply: %s", r ? r : "(none)");
    CHECK(S.gdb_swbreak && S.gdb_hwbreak, "gdb features recorded");

    /* 按 0x200 一段读完，'m' 表示还有、'l' 表示结束 */
    for (;;) {
        snprintf(req, sizeof(req), "qXfer:features:read:target.xml:%zx,200", off);
        r = roundtrip(req);
        if (!r || (r[0] != 'm' && r[0] != 'l'))
            break;
        CHECK(strlen(r + 1) <= 0x200, "chunk longer than requested");
        memcpy(xml + off, r + 1, strlen(r + 1));
        off += strlen(r + 1);
        rounds++;
        if (r[0] == 'l' || rounds > 64)
            break;
    }
    /* 带着最后一段数据的那次应答就要是 'l'，不能先回 'm' 再让 gdb 多读
     * 一次空的 'l' */
    CHECK(r && r[0] == 'l' && r[1], "the chunk that reaches the end is 'l'");
    CHECK(rounds > 1, "target.xml took %d chunk(s); expected several", rounds);
    CHECK(!strncmp(xml, "<?xml", 5) && strstr(xml, "</target>"),
          "target.xml is complete");
    CHECK(strstr(xml, "<architecture>i386:x86-64</architecture>") != NULL,
          "architecture element");
    for (p = xml; (p = strstr(p, "<reg ")); p++)
        regs++;
    CHECK_U64(regs, 59, "register count in target.xml");
    CHECK(strstr(xml, "name=\"xmm0\" bitsize=\"128\" type=\"vec128\" regnum=\"40\"")
          != NULL, "xmm0 is register 40");

    snprintf(req, sizeof(req), "qXfer:features:read:target.xml:%zx,200", off);
    CHECK_REPLY(req, "l");
    CHECK_REPLY("qXfer:features:read:other.xml:0,200", "E00");
}

static void case_registers(void)
{
    const char *r;
    char want[64];
    int i;

    T_CASE("g/p/P: layout, unavailable x87 fields, writable range");
    reset();
    for (i = 0; i < 16; i++)
        F.regs[0].gpr[i] = 0x0101010101010101ull * (uint64_t)(i + 1);
    F.regs[0].rip = 0xffffffff81000000ull;
    F.regs[0].eflags = 0x246;
    F.regs[0].seg[0] = 0x10;
    F.regs[0].fctrl = 0x37f;
    F.regs[0].fop = 0x1234;
    F.regs[0].xmm[0][0] = 0xaa;
    F.regs[0].xmm[15][15] = 0xbb;
    F.regs[0].mxcsr = 0x1f80;
    F.regs[0].fs_base = 0x1122334455667788ull;
    F.regs[0].gs_base = 0xffff888000000000ull;
    F.regs[1].rip = 0x2222;

    r = roundtrip("g");
    CHECK(r != NULL, "g reply");
    if (!r)
        return;
    /* 字节偏移：gpr 0..127，rip 128，eflags 136，seg 140..163，
     * st 164..243，fctrl 244，fstat 248，ftag/fiseg/fioff/foseg/fooff
     * 252..271，fop 272，xmm 276..531，mxcsr 532，fs_base 536，gs_base 544 */
    CHECK_U64(strlen(r), 2 * 552, "g reply length in hex chars");
    CHECK(!strncmp(r, "0101010101010101", 16), "rax first");
    CHECK(!strncmp(r + 2 * 48, "0707070707070707", 16),
          "gdb order: rbp (6th) then rsp (7th)");
    le_hex(want, F.regs[0].rip, 8);
    CHECK(!strncmp(r + 2 * 128, want, 16), "rip at byte 128");
    CHECK(!strncmp(r + 2 * 136, "46020000", 8), "eflags is 32-bit LE");
    CHECK(!strncmp(r + 2 * 140, "10000000", 8), "cs selector");
    CHECK(!strncmp(r + 2 * 244, "7f030000", 8), "fctrl");
    CHECK(!strncmp(r + 2 * 252, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", 40),
          "ftag..fooff unavailable");
    CHECK(!strncmp(r + 2 * 272, "34120000", 8), "fop");
    CHECK(!strncmp(r + 2 * 276, "aa", 2), "xmm0 byte 0");
    CHECK(!strncmp(r + 2 * 531, "bb", 2), "xmm15 byte 15");
    CHECK(!strncmp(r + 2 * 532, "801f0000", 8), "mxcsr");
    CHECK(!strncmp(r + 2 * 536, "8877665544332211", 16), "fs_base");
    CHECK(!strncmp(r + 2 * 544, "000000008088ffff", 16), "gs_base last");

    le_hex(want, F.regs[0].rip, 8);
    CHECK_REPLY("p10", want);
    CHECK_REPLY("p22", "xxxxxxxx");          /* 34 = ftag */
    CHECK_REPLY("p3a", "000000008088ffff");  /* 58 = gs_base */
    CHECK_REPLY("p3b", "E01");
    CHECK_REPLY("pzz", "E01");

    CHECK_REPLY("P0=efbeadde00000000", "OK");
    CHECK_U64(F.regs[0].gpr[0], 0xdeadbeef, "P0 wrote rax");
    CHECK_REPLY("P11=02000000", "OK");
    CHECK_U64(F.regs[0].eflags, 0x2, "P11 wrote eflags");
    CHECK_REPLY("P12=18000000", "E01");      /* cs 不可写 */
    CHECK_REPLY("P10=0011", "E01");          /* 长度不对 */
    CHECK_U64(F.regs[0].rip, 0xffffffff81000000ull, "short P left rip alone");

    CHECK_REPLY("Hg2", "OK");
    CHECK_REPLY("p10", "2222000000000000");
    CHECK_REPLY("Hg0", "OK");                /* “任意”保持原选择 */
    CHECK_U64(S.g_cpu, 1, "Hg0 keeps the selected thread");
    CHECK_REPLY("Hg3", "E01");
    CHECK_REPLY("Hg-1", "OK");
    CHECK_REPLY("Hc-1", "OK");
    CHECK_REPLY("Hx1", "E01");
}

static void case_memory(void)
{
    const char *r;

    T_CASE("m/M: partial reads, unmapped, round trip");
    reset();

    CHECK_REPLY("m1000,4", "00010203");
    /* 跨越可访问区的末尾：只回能读的部分 */
    CHECK_REPLY("m2ffe,4", "feff");
    CHECK_REPLY("m3000,4", "E0e");
    CHECK_REPLY("m0,1", "E0e");
    CHECK_REPLY("m1000", "E01");

    /* 请求超过一个包能装下的长度时截短，gdb 会接着读 */
    r = roundtrip("m1000,1000");
    CHECK(r && strlen(r) == RSP_PACKET_SIZE, "long read capped at %d hex chars, got %zu",
          RSP_PACKET_SIZE, r ? strlen(r) : 0);

    CHECK_REPLY("M1100,3:aabbcc", "OK");
    CHECK_REPLY("m1100,3", "aabbcc");
    CHECK_REPLY("M1100,3:aabb", "E01");      /* 数据比长度短 */
    CHECK_REPLY("M1100,1:aabb", "E01");      /* 数据比长度长 */
    CHECK_REPLY("M2fff,2:0102", "E0e");      /* 只写进去一半 */
}

static void case_breakpoints(void)
{
    T_CASE("Z0/z0 hide the int3 from reads, writes keep it; Z1; Z2 unsupported");
    reset();

    CHECK_REPLY("Z0,1010,1", "OK");
    CHECK_U64(F.mem[0x10], GDB_X86_INT3, "int3 is in memory");
    CHECK_REPLY("m100f,3", "0f1011");        /* 读到的是原字节 */
    CHECK_REPLY("Z0,1010,1", "OK");          /* 重复插入不改表 */
    CHECK_U64(F.bps.nr_sw, 1, "one entry");

    CHECK_REPLY("M1010,1:99", "OK");
    CHECK_U64(F.mem[0x10], GDB_X86_INT3, "write over a breakpoint keeps int3");
    CHECK_REPLY("m1010,1", "99");
    CHECK_REPLY("z0,1010,1", "OK");
    CHECK_U64(F.mem[0x10], 0x99, "removal restores the value gdb wrote");
    CHECK_REPLY("z0,1010,1", "E01");

    CHECK_REPLY("Z0,5000,1", "E01");         /* 翻译不了的地址 */

    /* 删掉表里不是最后一项的断点，剩下的那个还在、还被隐藏 */
    CHECK_REPLY("Z0,1020,1", "OK");
    CHECK_REPLY("Z0,1030,1", "OK");
    CHECK_REPLY("z0,1020,1", "OK");
    CHECK_U64(F.bps.nr_sw, 1, "one entry left");
    CHECK(gdb_x86_sw_find(&F.bps, 0x1030) >= 0 &&
          gdb_x86_sw_find(&F.bps, 0x1020) < 0, "the removed entry is 0x1020");
    CHECK_REPLY("m1030,1", "30");
    CHECK_REPLY("z0,1030,1", "OK");
    CHECK_U64(F.mem[0x30], 0x30, "0x1030 restored");

    CHECK_REPLY("Z1,ffffffff81000000,1", "OK");
    CHECK_REPLY("Z1,ffffffff81000001,1", "OK");
    CHECK_REPLY("Z1,ffffffff81000002,1", "OK");
    CHECK_REPLY("Z1,ffffffff81000003,1", "OK");
    CHECK_REPLY("Z1,ffffffff81000004,1", "E01"); /* 只有 4 个 DR */
    CHECK_REPLY("z1,ffffffff81000002,1", "OK");
    CHECK_REPLY("Z1,ffffffff81000004,1", "OK");

    CHECK_REPLY("Z2,1000,4", "");            /* watchpoint 不支持 */
    CHECK_REPLY("Z0,1000", "E01");
}

static void case_vcont_and_stop(void)
{
    struct rsp_stop st;
    enum rsp_status rs;

    T_CASE("vCont actions, running state, Ctrl-C, stop replies");
    reset();
    roundtrip("qSupported:swbreak+;hwbreak+");

    CHECK_REPLY("vCont?", "vCont;c;C;s;S");

    send_pkt("vCont;s:2;c");
    CHECK(g_out_len == 1 && g_out[0] == '+', "vCont has no immediate reply");
    CHECK(F.resumed == 1 && F.act[0] == RSP_ACT_CONT && F.act[1] == RSP_ACT_STEP,
          "leftmost matching action wins: act = %d %d", F.act[0], F.act[1]);
    CHECK(S.running, "session is running");

    send_pkt("g");
    CHECK(g_out_len == 1 && g_out[0] == '+',
          "packets while running are acked but not executed");
    feed("\x03", 1);
    CHECK_U64(F.interrupted, 1, "Ctrl-C while running -> interrupt");

    st.kind = RSP_STOP_SWBREAK; st.cpu = 1; st.exit_code = 0;
    g_out_len = 0;
    rs = rsp_report_stop(&S, &st);
    CHECK(rs == RSP_CONTINUE, "stop keeps the session");
    CHECK(reply_of_out(false) && !strcmp(reply_of_out(false),
                                         "T05thread:2;swbreak:;"),
          "swbreak stop reply: %.*s", (int)g_out_len, g_out);
    CHECK_U64(S.g_cpu, 1, "stopped thread becomes current");
    CHECK_REPLY("?", "T05thread:2;swbreak:;");
    CHECK_REPLY("qC", "QC2");

    feed("\x03", 1);
    CHECK_U64(F.interrupted, 1, "Ctrl-C while stopped is ignored");

    send_pkt("vCont;s:1");
    CHECK(F.act[0] == RSP_ACT_STEP && F.act[1] == RSP_ACT_NONE,
          "unmatched thread stays stopped");
    st.kind = RSP_STOP_TRAP; st.cpu = 0;
    g_out_len = 0;
    rsp_report_stop(&S, &st);
    CHECK(reply_of_out(false) && !strcmp(reply_of_out(false), "T05thread:1;"),
          "step stop reply");

    send_pkt("vCont;C02");
    CHECK(F.act[0] == RSP_ACT_CONT && F.act[1] == RSP_ACT_CONT,
          "C with signal continues every thread");
    st.kind = RSP_STOP_HWBREAK; st.cpu = 0;
    g_out_len = 0;
    rsp_report_stop(&S, &st);
    CHECK(reply_of_out(false) && !strcmp(reply_of_out(false),
                                         "T05thread:1;hwbreak:;"),
          "hwbreak stop reply");

    F.resumed = 0;
    CHECK_REPLY("vCont;c:5", "E01");         /* 没有这个线程 */
    CHECK_REPLY("vCont;x", "E01");
    CHECK_REPLY("vCont", "E01");
    CHECK_REPLY("vCont;c:0", "E01");
    CHECK_U64(F.resumed, 0, "rejected vCont does not resume");

    /* gdb 没声明 swbreak/hwbreak 时只发 T05 */
    reset();
    send_pkt("vCont;c");
    st.kind = RSP_STOP_SWBREAK; st.cpu = 0;
    g_out_len = 0;
    rsp_report_stop(&S, &st);
    CHECK(reply_of_out(false) && !strcmp(reply_of_out(false), "T05thread:1;"),
          "no swbreak without gdb support");

    send_pkt("vCont;c");
    st.kind = RSP_STOP_INTERRUPT; st.cpu = 1;
    g_out_len = 0;
    rsp_report_stop(&S, &st);
    CHECK(reply_of_out(false) && !strcmp(reply_of_out(false), "T02thread:2;"),
          "interrupt is SIGINT");

    send_pkt("vCont;c");
    st.kind = RSP_STOP_EXITED; st.cpu = 0; st.exit_code = 3;
    g_out_len = 0;
    rs = rsp_report_stop(&S, &st);
    CHECK(reply_of_out(false) && !strcmp(reply_of_out(false), "W03"),
          "exit reply");
    CHECK(rs == RSP_CLOSED, "W closes the session");
}

static void case_threads_and_session_end(void)
{
    char want[32];

    T_CASE("thread queries, D and k");
    reset();

    CHECK_REPLY("qfThreadInfo", "m1,2");
    CHECK_REPLY("qsThreadInfo", "l");
    CHECK_REPLY("qAttached", "1");
    /* "vCPU 1" 的十六进制 */
    snprintf(want, sizeof(want), "%s", "7643505520" "31");
    CHECK_REPLY("qThreadExtraInfo,2", want);
    CHECK_REPLY("qThreadExtraInfo,3", "E01");
    CHECK_REPLY("T2", "OK");
    CHECK_REPLY("T3", "E01");
    CHECK_REPLY("T0", "E01");
    CHECK_REPLY("qOffsets", "");

    CHECK(send_pkt("D") == RSP_CLOSED, "D closes the session");
    CHECK(reply_of_out(true) && !strcmp(reply_of_out(true), "OK"), "D -> OK");
    CHECK_U64(F.detached, 1, "detach called");
    CHECK(send_pkt("?") == RSP_CLOSED && g_out_len == 0,
          "closed session ignores input");

    reset();
    CHECK(send_pkt("k") == RSP_CLOSED, "k closes the session");
    CHECK(g_out_len == 1 && g_out[0] == '+', "k has no reply");
    CHECK_U64(F.killed, 1, "kill called");
}

/* ------------------------------------------------------------------ */
/* x86 纯函数                                                          */
/* ------------------------------------------------------------------ */

static void case_x86_tables(void)
{
    struct gdb_x86_bps b;
    struct gdb_x86_sw_bp out;
    uint8_t buf[4] = { 1, 2, 3, 4 };
    int i;

    T_CASE("x86: breakpoint tables, DR7, hide across range start");
    memset(&b, 0, sizeof(b));

    CHECK(gdb_x86_sw_add(&b, 0x10, 0x2000, 0xaa) == 0, "add");
    CHECK(gdb_x86_sw_add(&b, 0x10, 0x9999, 0xbb) == 0 && b.nr_sw == 1 &&
          b.sw[0].gpa == 0x2000, "re-add keeps the first entry");
    CHECK(gdb_x86_sw_del(&b, 0x11, &out) == -ENOENT, "del unknown");
    for (i = 1; i < GDB_X86_MAX_SW_BP; i++)
        gdb_x86_sw_add(&b, 0x100 + (uint64_t)i, 0x3000 + (uint64_t)i, 0);
    CHECK(gdb_x86_sw_add(&b, 0x5000, 0x5000, 0) == -ENOSPC, "table full");

    memset(&b, 0, sizeof(b));
    gdb_x86_sw_add(&b, 0x10, 0x1fff, 0xaa);     /* 在区间之前一个字节 */
    gdb_x86_sw_add(&b, 0x11, 0x2003, 0xbb);     /* 区间最后一个字节 */
    gdb_x86_sw_add(&b, 0x12, 0x2004, 0xcc);     /* 区间之后 */
    gdb_x86_sw_hide(&b, 0x2000, buf, sizeof(buf));
    CHECK(buf[0] == 1 && buf[3] == 0xbb, "hide only inside [gpa, gpa+len)");
    buf[3] = 0x77;
    gdb_x86_sw_shadow_write(&b, 0x2000, buf, sizeof(buf));
    CHECK(buf[3] == GDB_X86_INT3 && b.sw[1].orig == 0x77,
          "shadow write records the new byte and keeps int3");
    CHECK(gdb_x86_sw_del(&b, 0x10, &out) == 0 && out.gpa == 0x1fff &&
          out.orig == 0xaa, "del returns the entry");

    memset(&b, 0, sizeof(b));
    CHECK_U64(gdb_x86_dr7(&b), 0x400, "DR7 with no slots: only the fixed bit");
    CHECK(gdb_x86_hw_add(&b, 0xa) == 0 && gdb_x86_hw_add(&b, 0xb) == 1 &&
          gdb_x86_hw_add(&b, 0xc) == 2, "slots in order");
    CHECK(gdb_x86_hw_add(&b, 0xb) == 1, "same address, same slot");
    gdb_x86_hw_del(&b, 0xb);
    /* L0 (bit 0) 和 L2 (bit 4)；R/W 和 LEN 字段全 0 = 1 字节指令断点 */
    CHECK_U64(gdb_x86_dr7(&b), 0x400 | 0x1 | 0x10, "DR7 for slots 0 and 2");
    CHECK(gdb_x86_hw_add(&b, 0xd) == 1, "freed slot reused");
    CHECK(gdb_x86_hw_add(&b, 0xe) == 3 && gdb_x86_hw_add(&b, 0xf) == -ENOSPC,
          "four slots only");
    CHECK(gdb_x86_hw_del(&b, 0x99) == -ENOENT, "del unknown hw");
}

static void case_x86_guest_debug(void)
{
    struct gdb_x86_bps b;
    struct kvm_guest_debug d;

    T_CASE("x86: KVM_SET_GUEST_DEBUG control word and debug registers");
    memset(&b, 0, sizeof(b));

    gdb_x86_guest_debug(&b, false, KVM_GUESTDBG_BLOCKIRQ, &d);
    CHECK_U64(d.control, 0, "nothing to debug -> guest debug off");

    gdb_x86_guest_debug(&b, true, KVM_GUESTDBG_BLOCKIRQ, &d);
    CHECK_U64(d.control, KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP |
                         KVM_GUESTDBG_BLOCKIRQ, "step only");

    gdb_x86_sw_add(&b, 0x10, 0x10, 0);
    gdb_x86_guest_debug(&b, false, KVM_GUESTDBG_BLOCKIRQ, &d);
    CHECK_U64(d.control, KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_SW_BP,
              "sw breakpoints, no step: BLOCKIRQ must not leak in");

    gdb_x86_hw_add(&b, 0xffffffff81000000ull);
    gdb_x86_hw_add(&b, 0xffffffff82000000ull);
    gdb_x86_hw_del(&b, 0xffffffff81000000ull);
    gdb_x86_guest_debug(&b, false, 0, &d);
    CHECK_U64(d.control, KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_SW_BP |
                         KVM_GUESTDBG_USE_HW_BP, "sw + hw");
    CHECK_U64(d.arch.debugreg[0], 0, "freed slot cleared");
    CHECK_U64(d.arch.debugreg[1], 0xffffffff82000000ull, "DR1");
    CHECK_U64(d.arch.debugreg[7], 0x400 | 0x4, "DR7 enables slot 1 only");
}

static void case_x86_classify(void)
{
    struct gdb_x86_bps b;
    struct kvm_debug_exit_arch ex;

    T_CASE("x86: KVM_EXIT_DEBUG classification");
    memset(&b, 0, sizeof(b));
    gdb_x86_sw_add(&b, 0xffffffff81000010ull, 0x1000010, 0x55);
    gdb_x86_hw_add(&b, 0xffffffff81000020ull);      /* slot 0 */

    memset(&ex, 0, sizeof(ex));
    ex.exception = GDB_X86_BP_VECTOR;
    ex.pc = 0xffffffff81000010ull;
    CHECK(gdb_x86_classify(&b, &ex, false) == GDB_X86_EV_SWBREAK,
          "#BP at our int3");
    ex.pc = 0xffffffff81000011ull;
    CHECK(gdb_x86_classify(&b, &ex, false) == GDB_X86_EV_REINJECT_BP,
          "#BP elsewhere is the guest's own int3");

    memset(&ex, 0, sizeof(ex));
    ex.exception = GDB_X86_DB_VECTOR;
    ex.dr6 = 0xffff0ff0 | 0x1;
    CHECK(gdb_x86_classify(&b, &ex, false) == GDB_X86_EV_HWBREAK,
          "#DB with B0 and slot 0 in use");
    ex.dr6 = 0xffff0ff0 | 0x2;
    CHECK(gdb_x86_classify(&b, &ex, false) == GDB_X86_EV_REINJECT_DB,
          "#DB with B1 but slot 1 unused");
    ex.dr6 = 0xffff0ff0 | GDB_X86_DR6_BS;
    CHECK(gdb_x86_classify(&b, &ex, true) == GDB_X86_EV_STEP,
          "BS while stepping");
    CHECK(gdb_x86_classify(&b, &ex, false) == GDB_X86_EV_REINJECT_DB,
          "BS while not stepping is the guest's own TF");
    ex.dr6 = 0xffff0ff0 | 0x2;
    CHECK(gdb_x86_classify(&b, &ex, true) == GDB_X86_EV_REINJECT_DB,
          "#DB without BS while stepping is the guest's own");
    ex.dr6 = 0xffff0ff0 | GDB_X86_DR6_BS | 0x1;
    CHECK(gdb_x86_classify(&b, &ex, true) == GDB_X86_EV_HWBREAK,
          "breakpoint hit reported over the step");
}

static void case_x86_regs(void)
{
    struct kvm_regs kr, back;
    struct kvm_sregs ks;
    struct kvm_fpu kf;
    struct rsp_regs r;

    T_CASE("x86: kvm_regs <-> gdb layout");
    memset(&kr, 0, sizeof(kr));
    memset(&ks, 0, sizeof(ks));
    memset(&kf, 0, sizeof(kf));
    kr.rax = 1; kr.rbx = 2; kr.rcx = 3; kr.rdx = 4; kr.rsi = 5; kr.rdi = 6;
    kr.rsp = 7; kr.rbp = 8; kr.r8 = 9; kr.r15 = 16;
    kr.rip = 0xffffffff81000000ull;
    kr.rflags = 0x246;
    ks.cs.selector = 0x10; ks.gs.selector = 0x18;
    ks.fs.base = 0x7f0000001000ull; ks.gs.base = 0xffff888000000000ull;
    kf.fpr[1][9] = 0x40;
    kf.fcw = 0x37f; kf.fsw = 0x1; kf.last_opcode = 0x5ab;
    kf.xmm[3][0] = 0x99;
    kf.mxcsr = 0x1f80;

    gdb_x86_regs_from_kvm(&r, &kr, &ks, &kf);
    CHECK(r.gpr[6] == 8 && r.gpr[7] == 7, "gpr[6] = rbp, gpr[7] = rsp");
    CHECK(r.gpr[0] == 1 && r.gpr[5] == 6 && r.gpr[8] == 9 && r.gpr[15] == 16,
          "other gprs");
    CHECK(r.rip == kr.rip && r.eflags == 0x246, "rip, eflags");
    CHECK(r.seg[0] == 0x10 && r.seg[5] == 0x18, "cs and gs selectors");
    CHECK(r.fs_base == ks.fs.base && r.gs_base == ks.gs.base, "fs/gs base");
    CHECK(r.st[1][9] == 0x40 && r.fctrl == 0x37f && r.fstat == 1 &&
          r.fop == 0x5ab, "x87");
    CHECK(r.xmm[3][0] == 0x99 && r.mxcsr == 0x1f80, "sse");

    r.gpr[6] = 0x66; r.gpr[7] = 0x77; r.rip = 0x1234; r.eflags = 0x2;
    back = kr;
    gdb_x86_regs_to_kvm(&r, &back);
    CHECK(back.rbp == 0x66 && back.rsp == 0x77, "rbp/rsp written back");
    CHECK(back.rip == 0x1234 && back.rflags == 0x2, "rip/rflags written back");
    CHECK(back.rax == 1 && back.r15 == 16, "untouched gprs survive");
}

int main(int argc, char **argv)
{
    T_MAIN_BEGIN();
    case_framing();
    case_frame_escape();
    case_qsupported_and_xml();
    case_registers();
    case_memory();
    case_breakpoints();
    case_vcont_and_stop();
    case_threads_and_session_end();
    case_x86_tables();
    case_x86_guest_debug();
    case_x86_classify();
    case_x86_regs();
    return T_SUMMARY();
}
