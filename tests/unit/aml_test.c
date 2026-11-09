/* tests/unit/aml_test.c — AML 编码器的边界：PkgLength、整数前缀、EisaId、溢出
 *
 * DSDT 整体的正确性由 tables suite 的 iasl 往返检查（反汇编再编译后逐字节
 * 一致）兜底，但那只覆盖当前 DSDT 实际用到的长度：PkgLength 只出现了 1 字节
 * 和 2 字节两种编码。这里补上 3、4 字节编码的切换点和上限，以及 EisaId。
 *
 * 判据来源：ACPI 6.4 第 20.2.4 节（PkgLength），PkgLength 的值包含它自己；
 * 解码按 ACPICA acpi_ps_get_next_package_length() 的算法写在 decode_pkglen；
 * EisaId 的解码照抄 ACPICA acpi_ex_eisa_id_to_string()。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot/aml.h"
#include "harness.h"

/* ACPICA 的解码：byte0 bit 7:6 = 后续字节数；0 时 bit 5:0 是长度，
 * 否则 bit 3:0 是低 4 位，后续字节依次是 bit 11:4、19:12、27:20 */
static uint32_t decode_pkglen(const uint8_t *p, unsigned *nbytes)
{
    unsigned n = p[0] >> 6, i;
    uint32_t len;

    if (!n) {
        *nbytes = 1;
        return p[0] & 0x3F;
    }
    len = p[0] & 0x0F;
    for (i = 1; i <= n; i++)
        len |= (uint32_t)p[i] << (4 + 8 * (i - 1));
    *nbytes = n + 1;
    return len;
}

static void eisa_to_string(uint32_t id, char out[8])
{
    static const char hex[] = "0123456789ABCDEF";
    uint32_t s = __builtin_bswap32(id);

    out[0] = (char)(0x40 + ((s >> 26) & 0x1F));
    out[1] = (char)(0x40 + ((s >> 21) & 0x1F));
    out[2] = (char)(0x40 + ((s >> 16) & 0x1F));
    out[3] = hex[(s >> 12) & 0xF];
    out[4] = hex[(s >> 8) & 0xF];
    out[5] = hex[(s >> 4) & 0xF];
    out[6] = hex[s & 0xF];
    out[7] = 0;
}

static uint8_t g_buf[1 << 21];

/* open/close 一个装着 body 个字节的结构，检查编码长度和解码值 */
static void pkglen_case(size_t body, unsigned want_bytes)
{
    struct aml a;
    size_t at;
    unsigned n = 0;
    uint32_t v;

    aml_init(&a, g_buf, sizeof(g_buf));
    at = aml_open(&a);
    memset(g_buf + a.len, 0xAB, body);
    a.len += body;
    aml_close(&a, at);
    CHECK(!a.overflow, "body %zu: overflow", body);
    v = decode_pkglen(g_buf + at, &n);
    CHECK_U64(n, want_bytes, "body %zu: PkgLength bytes", body);
    CHECK_U64(v, body + n, "body %zu: PkgLength counts itself", body);
    CHECK_U64(a.len, body + n, "body %zu: content moved right after PkgLength", body);
    if (body)
        CHECK(g_buf[at + n] == 0xAB && g_buf[at + n + body - 1] == 0xAB,
              "body %zu: content intact", body);
}

int main(int argc, char **argv)
{
    struct aml a;
    uint8_t small[8];
    char s[8];

    T_MAIN_BEGIN();

    T_CASE("PkgLength：每种编码长度的切换点");
    pkglen_case(0, 1);
    pkglen_case(62, 1);          /* 62 + 1 = 63，1 字节编码的上限 */
    pkglen_case(63, 2);          /* 63 + 1 = 64 放不下，换 2 字节 */
    pkglen_case(0xFFD, 2);       /* 0xFFD + 2 = 0xFFF */
    pkglen_case(0xFFE, 3);
    pkglen_case(0xFFFFC, 3);     /* + 3 = 0xFFFFF */
    pkglen_case(0xFFFFD, 4);

    T_CASE("整数：按数值选最短的前缀");
    {
        static const struct { uint64_t v; uint8_t bytes[9]; size_t n; } t[] = {
            { 0,           { 0x00 }, 1 },
            { 1,           { 0x01 }, 1 },
            { 2,           { 0x0A, 0x02 }, 2 },
            { 0xFF,        { 0x0A, 0xFF }, 2 },
            { 0x100,       { 0x0B, 0x00, 0x01 }, 3 },
            { 0x10000,     { 0x0C, 0x00, 0x00, 0x01, 0x00 }, 5 },
            { 0x100000000ULL, { 0x0E, 0, 0, 0, 0, 1, 0, 0, 0 }, 9 },
        };
        size_t i;

        for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
            aml_init(&a, g_buf, sizeof(g_buf));
            aml_int(&a, t[i].v);
            CHECK(a.len == t[i].n && !memcmp(g_buf, t[i].bytes, t[i].n),
                  "integer 0x%llx", (unsigned long long)t[i].v);
        }
    }

    T_CASE("EisaId：PNP0A03 的压缩值，以及按 ACPICA 的算法解回原文");
    CHECK_U64(aml_eisaid("PNP0A03"), 0x030AD041u, "PNP0A03");
    eisa_to_string(aml_eisaid("PNP0A08"), s);
    CHECK(!strcmp(s, "PNP0A08"), "PNP0A08 round trip: %s", s);
    eisa_to_string(aml_eisaid("XYZ9AF0"), s);
    CHECK(!strcmp(s, "XYZ9AF0"), "letters Z and hex digits A-F round trip: %s", s);

    T_CASE("溢出：写满后置 overflow，不越界");
    aml_init(&a, small, sizeof(small));
    aml_name_decl(&a, "_HID");            /* 5 字节 */
    aml_int(&a, 0x12345678);              /* 5 字节，放不下 */
    /* 放不下的那个对象可能写进去一部分；调用方看到 overflow 会丢弃整张表，
     * 这里只要求不越过缓冲区（越界由 ASan 报） */
    CHECK(a.overflow, "overflow flag set");
    CHECK(a.len <= sizeof(small), "len %zu stays within the buffer", a.len);
    aml_byte(&a, 0);
    CHECK(a.len <= sizeof(small), "writes after overflow are dropped");
    aml_init(&a, g_buf, sizeof(g_buf));
    aml_name(&a, "ABC");
    CHECK(a.overflow, "a NameSeg must be exactly 4 characters");

    return T_SUMMARY();
}
