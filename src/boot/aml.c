/* src/boot/aml.c — 生成 DSDT 用的最小 AML 编码器                 [pci-bus]
 *
 * 接口和参考见 include/boot/aml.h。
 */
#include <string.h>

#include "boot/aml.h"

/* 操作码（ACPI 6.4 第 20.3 节；ACPICA amlcode.h 同名常量） */
#define AML_ZERO_OP          0x00
#define AML_ONE_OP           0x01
#define AML_NAME_OP          0x08
#define AML_BYTE_PREFIX      0x0A
#define AML_WORD_PREFIX      0x0B
#define AML_DWORD_PREFIX     0x0C
#define AML_QWORD_PREFIX     0x0E
#define AML_SCOPE_OP         0x10
#define AML_BUFFER_OP        0x11
#define AML_PACKAGE_OP       0x12
#define AML_EXT_OP_PREFIX    0x5B
#define AML_DEVICE_OP        0x82      /* 0x5B 0x82 */
#define AML_ROOT_PREFIX      0x5C      /* '\' */

/* PkgLength 最多 4 字节，能表示的最大长度是 2^28 - 1 */
#define PKGLEN_MAX_BYTES     4
#define PKGLEN_LIMIT         (1u << 28)

void aml_init(struct aml *a, uint8_t *buf, size_t cap)
{
    a->buf      = buf;
    a->cap      = cap;
    a->len      = 0;
    a->overflow = false;
}

void aml_bytes(struct aml *a, const void *p, size_t n)
{
    if (a->overflow || n > a->cap - a->len) {
        a->overflow = true;
        return;
    }
    memcpy(a->buf + a->len, p, n);
    a->len += n;
}

void aml_byte(struct aml *a, uint8_t v)
{
    aml_bytes(a, &v, 1);
}

void aml_word(struct aml *a, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };

    aml_bytes(a, b, 2);
}

void aml_dword(struct aml *a, uint32_t v)
{
    aml_word(a, (uint16_t)v);
    aml_word(a, (uint16_t)(v >> 16));
}

void aml_int(struct aml *a, uint64_t v)
{
    if (v == 0) {
        aml_byte(a, AML_ZERO_OP);
    } else if (v == 1) {
        aml_byte(a, AML_ONE_OP);
    } else if (v <= 0xFF) {
        aml_byte(a, AML_BYTE_PREFIX);
        aml_byte(a, (uint8_t)v);
    } else if (v <= 0xFFFF) {
        aml_byte(a, AML_WORD_PREFIX);
        aml_word(a, (uint16_t)v);
    } else if (v <= 0xFFFFFFFFu) {
        aml_byte(a, AML_DWORD_PREFIX);
        aml_dword(a, (uint32_t)v);
    } else {
        aml_byte(a, AML_QWORD_PREFIX);
        aml_dword(a, (uint32_t)v);
        aml_dword(a, (uint32_t)(v >> 32));
    }
}

/* NameSeg 恒为 4 个字符（不足的用 '_' 补齐，规范第 20.2.2 节），这里
 * 要求调用方直接给出完整的 4 个字符，写错长度时宁可让表坏得显眼。 */
void aml_name(struct aml *a, const char *name)
{
    if (name[0] == '\\') {
        aml_byte(a, AML_ROOT_PREFIX);
        name++;
    }
    if (strlen(name) != 4) {
        a->overflow = true;
        return;
    }
    aml_bytes(a, name, 4);
}

size_t aml_open(struct aml *a)
{
    size_t at = a->len;
    uint8_t zero[PKGLEN_MAX_BYTES] = { 0 };

    aml_bytes(a, zero, sizeof(zero));
    return at;
}

/* PkgLength 的值包含它自己占的字节数（第 20.2.4 节）。
 *   1 字节：bit 7:6 = 0，bit 5:0 = 长度（< 64）
 *   n 字节：bit 7:6 = n - 1，bit 3:0 = 长度的低 4 位，后续每字节依次放
 *           bit 11:4、19:12、27:20
 * 用最短编码：ACPICA 两种都能解析，但 iasl 输出的是最短的，按它写，
 * 反汇编结果才能和 ASL 原文逐字节对应。 */
void aml_close(struct aml *a, size_t at)
{
    size_t body, total;
    unsigned n, i;

    if (a->overflow)
        return;
    body = a->len - at - PKGLEN_MAX_BYTES;
    for (n = 1; n <= PKGLEN_MAX_BYTES; n++) {
        total = body + n;
        if (n == 1 ? total < 0x40 : total < (1u << (4 + 8 * (n - 1))))
            break;
    }
    if (n > PKGLEN_MAX_BYTES || total >= PKGLEN_LIMIT) {
        a->overflow = true;
        return;
    }
    memmove(a->buf + at + n, a->buf + at + PKGLEN_MAX_BYTES, body);
    a->len -= PKGLEN_MAX_BYTES - n;
    if (n == 1) {
        a->buf[at] = (uint8_t)total;
        return;
    }
    a->buf[at] = (uint8_t)(((n - 1) << 6) | (total & 0xF));
    for (i = 1; i < n; i++)
        a->buf[at + i] = (uint8_t)(total >> (4 + 8 * (i - 1)));
}

size_t aml_scope(struct aml *a, const char *name)
{
    size_t at;

    aml_byte(a, AML_SCOPE_OP);
    at = aml_open(a);
    aml_name(a, name);
    return at;
}

size_t aml_device(struct aml *a, const char *name)
{
    size_t at;

    aml_byte(a, AML_EXT_OP_PREFIX);
    aml_byte(a, AML_DEVICE_OP);
    at = aml_open(a);
    aml_name(a, name);
    return at;
}

void aml_name_decl(struct aml *a, const char *name)
{
    aml_byte(a, AML_NAME_OP);
    aml_name(a, name);
}

size_t aml_package(struct aml *a, uint8_t n)
{
    size_t at;

    aml_byte(a, AML_PACKAGE_OP);
    at = aml_open(a);
    aml_byte(a, n);
    return at;
}

void aml_buffer(struct aml *a, const void *bytes, size_t n)
{
    size_t at;

    aml_byte(a, AML_BUFFER_OP);
    at = aml_open(a);
    aml_int(a, n);
    aml_bytes(a, bytes, n);
    aml_close(a, at);
}

/* 规范第 6.1.5 节：字母按 'A' = 1 编成 5 位，三个字母共 15 位放在
 * 第一个 16 位的 bit 14:0（大端），接着是 4 个十六进制数字。ACPICA 的
 * acpi_ex_eisa_id_to_string() 是逆过程：先把整数按字节反转成大端再取位。 */
uint32_t aml_eisaid(const char *id)
{
    uint32_t be = 0;
    int i;

    for (i = 0; i < 3; i++)
        be |= (uint32_t)(id[i] - 0x40) << (26 - 5 * i);
    for (i = 0; i < 4; i++) {
        char c = id[3 + i];
        uint32_t d = (c >= '0' && c <= '9') ? (uint32_t)(c - '0')
                                            : (uint32_t)(c - 'A' + 10);
        be |= d << (12 - 4 * i);
    }
    return __builtin_bswap32(be);
}
