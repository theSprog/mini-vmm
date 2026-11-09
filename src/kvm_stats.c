/* src/kvm_stats.c — KVM_GET_STATS_FD 二进制统计的解析，格式见 kvm_stats.h */
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/kvm.h>

#include "vmm.h"
#include "kvm_stats.h"

/* 合理性上限。内核的 name 是 KVM_STATS_NAME_SIZE（48）字节，x86 的 vCPU
 * 统计不到 100 项。上限只用来拒绝损坏的 header，免得按一个离谱的数去
 * malloc 和 pread。 */
#define KVM_STATS_MAX_NAME  256
#define KVM_STATS_MAX_DESC  4096
/* 数据块上限。desc.offset 是 u32，不设上限的话一个损坏的 offset 会让
 * 这里先 malloc 4 GiB 再发现文件没那么长；x86 的 vCPU 数据块实际不到
 * 4 KiB。 */
#define KVM_STATS_MAX_DATA  (1u << 20)

/* 读满 len 字节。文件比声明的短（返回 0）算结构错误，不是系统错误。 */
static int pread_full(int fd, void *buf, size_t len, uint64_t off)
{
    size_t done = 0;

    while (done < len) {
        ssize_t n = pread(fd, (char *)buf + done, len - done,
                          (off_t)(off + done));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            vmm_err("kvm stats: pread %zu bytes at 0x%llx: %s", len - done,
                    (unsigned long long)(off + done), strerror(errno));
            return VMM_ERR_SYS;
        }
        if (n == 0) {
            vmm_err("kvm stats: file ends at 0x%llx, expected %zu more bytes",
                    (unsigned long long)(off + done), len - done);
            return VMM_ERR_INVAL;
        }
        done += (size_t)n;
    }
    return VMM_OK;
}

/* 取第 i 个 desc 的定长部分，返回紧随其后的 name。name_size 不是 8 的
 * 倍数时 desc 在缓冲区里不对齐，所以拷出来用，而不是直接强转指针。 */
static const char *desc_at(const uint8_t *descs, size_t stride, unsigned i,
                           struct kvm_stats_desc *d)
{
    const uint8_t *p = descs + stride * i;

    memcpy(d, p, sizeof(*d));
    return (const char *)p + sizeof(*d);
}

void kvm_stats_free(struct kvm_stats *st)
{
    free(st->v);
    free(st->names);
    memset(st, 0, sizeof(*st));
}

int kvm_stats_read(int fd, struct kvm_stats *st)
{
    struct kvm_stats_header h;
    uint8_t *descs = NULL, *data = NULL;
    size_t stride;
    uint64_t data_len = 0;
    unsigned i, n_single = 0;
    int r;

    memset(st, 0, sizeof(*st));

    r = pread_full(fd, &h, sizeof(h), 0);
    if (r != VMM_OK)
        return r;
    if (h.name_size == 0 || h.name_size > KVM_STATS_MAX_NAME ||
        h.num_desc > KVM_STATS_MAX_DESC) {
        vmm_err("kvm stats: implausible header name_size=%u num_desc=%u",
                h.name_size, h.num_desc);
        return VMM_ERR_INVAL;
    }

    stride = sizeof(struct kvm_stats_desc) + h.name_size;
    descs = malloc(stride * h.num_desc + 1);
    if (!descs)
        return VMM_ERR_NOMEM;
    r = pread_full(fd, descs, stride * h.num_desc, h.desc_offset);
    if (r != VMM_OK)
        goto fail;

    /* 先过一遍：校验每个 desc，算出数据块要读多长 */
    for (i = 0; i < h.num_desc; i++) {
        struct kvm_stats_desc d;
        const char *name = desc_at(descs, stride, i, &d);
        uint64_t end = (uint64_t)d.offset + (uint64_t)d.size * 8;

        if (!memchr(name, '\0', h.name_size)) {
            vmm_err("kvm stats: descriptor %u name is not terminated", i);
            r = VMM_ERR_INVAL;
            goto fail;
        }
        if (d.size == 0) {
            vmm_err("kvm stats: %s has size 0", name);
            r = VMM_ERR_INVAL;
            goto fail;
        }
        if (end > KVM_STATS_MAX_DATA) {
            vmm_err("kvm stats: %s at data offset 0x%x size %u is past "
                    "the %u-byte limit", name, d.offset, d.size,
                    KVM_STATS_MAX_DATA);
            r = VMM_ERR_INVAL;
            goto fail;
        }
        if (end > data_len)
            data_len = end;
        if (d.size == 1)
            n_single++;
    }

    data = malloc(data_len + 1);
    st->v = calloc(n_single + 1, sizeof(*st->v));
    st->names = malloc((size_t)n_single * h.name_size + 1);
    if (!data || !st->v || !st->names) {
        r = VMM_ERR_NOMEM;
        goto fail;
    }
    r = pread_full(fd, data, data_len, h.data_offset);
    if (r != VMM_OK)
        goto fail;

    for (i = 0; i < h.num_desc; i++) {
        struct kvm_stats_desc d;
        const char *src = desc_at(descs, stride, i, &d);
        struct kvm_stat *s;
        char *name;

        if (d.size != 1) {
            st->skipped++;
            continue;
        }
        s = &st->v[st->n];
        name = st->names + (size_t)st->n * h.name_size;
        memcpy(name, src, h.name_size);
        s->name     = name;
        s->flags    = d.flags;
        s->exponent = d.exponent;
        memcpy(&s->value, data + d.offset, sizeof(s->value));
        st->n++;
    }

    free(descs);
    free(data);
    return VMM_OK;

fail:
    free(descs);
    free(data);
    kvm_stats_free(st);
    return r;
}

const struct kvm_stat *kvm_stats_find(const struct kvm_stats *st,
                                      const char *name)
{
    unsigned i;

    for (i = 0; i < st->n; i++)
        if (!strcmp(st->v[i].name, name))
            return &st->v[i];
    return NULL;
}
