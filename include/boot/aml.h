/* include/boot/aml.h — 生成 DSDT 用的最小 AML 编码器
 *
 * DSDT 的内容是 AML 字节码。_S5 这种十几个字节的对象可以手写，host bridge
 * 的 _CRS（资源描述符）和 _PRT（32 个 slot × 4 根线 = 128 项的路由表）
 * 手写既易错又改不动，所以用一组按语法结构生成字节的函数。
 *
 * 只覆盖 DSDT 实际用到的那几种构造：Scope、Device、Name、Package、Buffer、
 * 整数和单段名字。结构嵌套用 open/close 配对：open 先预留 4 字节的
 * PkgLength，close 时按实际长度改写成最短编码并把内容往前挪。
 *
 * 缓冲区由调用方提供，写满后后续写入全部丢弃并置 overflow，调用方在最后
 * 检查一次即可，不需要每一步都判断返回值。
 *
 * 参考：ACPI 6.4 第 20 章（AML 语法：20.2.4 PkgLength、20.2.2 NameSeg、
 * 20.2.3 整数常量、20.2.5 各对象的编码）、第 6.4 节（资源描述符）、
 * 第 6.1.5 节（_HID 的 EISA ID 压缩格式）；Linux drivers/acpi/acpica/
 * amlcode.h（操作码）、amlresrc.h（资源描述符布局）。
 */
#ifndef BOOT_AML_H
#define BOOT_AML_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct aml {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    bool     overflow;
};

void   aml_init(struct aml *a, uint8_t *buf, size_t cap);
void   aml_byte(struct aml *a, uint8_t v);
void   aml_bytes(struct aml *a, const void *p, size_t n);
void   aml_word(struct aml *a, uint16_t v);     /* 小端裸字节，不带前缀 */
void   aml_dword(struct aml *a, uint32_t v);

/* 整数常量：按数值选 ZeroOp/OneOp/BytePrefix/WordPrefix/DWordPrefix/QWordPrefix */
void   aml_int(struct aml *a, uint64_t v);

/* 名字：可选的根前缀 '\' 加一个 4 字符的 NameSeg，例如 "\\_SB_"、"PCI0" */
void   aml_name(struct aml *a, const char *name);

/* 带 PkgLength 的结构：返回 open 时的位置，传给对应的 close */
size_t aml_open(struct aml *a);
void   aml_close(struct aml *a, size_t at);

/* Scope (name) { ... }、Device (name) { ... }：open 之后写内容再 close */
size_t aml_scope(struct aml *a, const char *name);
size_t aml_device(struct aml *a, const char *name);

/* Name (name, ...) 的前半部分：NameOp + 名字，后面紧跟一个数据对象 */
void   aml_name_decl(struct aml *a, const char *name);

/* Package (n) { ... }：返回位置，写完 n 个元素后 aml_close */
size_t aml_package(struct aml *a, uint8_t n);

/* Buffer () { bytes }：一次写完 */
void   aml_buffer(struct aml *a, const void *bytes, size_t n);

/* _HID 的 EISA ID 压缩：3 个大写字母各 5 位 + 4 位十六进制产品号，
 * 按字节序存成 32 位整数，例如 "PNP0A03" -> 0x030AD041 */
uint32_t aml_eisaid(const char *id);

#endif /* BOOT_AML_H */
