/* tests/fuzz/fuzz_common.h — 两个内核加载器 fuzz target 的公共部分  [P1a]
 *
 * 被测对象是 src/boot/{elf_loader,bzimage}.c：它们是这个 VMM 里唯一
 * 解析外部文件的地方，而且解析的是完全由文件内容控制的 64 位偏移与长度。
 *
 * oracle 是 sanitizer 加一条结构性契约：
 *   1. 任何输入都不得触发 ASan/UBSan 报告或段错误；
 *   2. 任何输入都必须在有限时间内返回，返回 VMM_OK 或某个 VMM_ERR_*，
 *      不得越界读写 guest RAM 缓冲区。
 * 这不需要知道“正确的解析结果是什么”，因此不存在自产自销的 oracle 问题。
 *
 * 加载器的接口吃的是路径（内部自己 open + mmap），所以每一轮把输入写进
 * 一个 memfd，再用 /proc/self/fd/N 当路径喂进去。这样既不落盘，也保留了
 * 真实的 mmap 行为（页对齐、尾部补零），不改动生产代码。
 */
#ifndef FUZZ_COMMON_H
#define FUZZ_COMMON_H

#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "vmm.h"
#include "memory.h"

/* guest RAM 的桩：一块真实的堆内存，越界写会被 ASan 抓住。
 * 64 MiB 足够放下正常的装载地址（1 MiB / 16 MiB），又小到能让
 * 大 init_size / 大 p_memsz 走进“超出 guest RAM”的拒绝路径。 */
#define FUZZ_RAM_SIZE (64ULL << 20)

static uint8_t *fuzz_ram;

int vmm_log_level = -1;         /* 全程静音：fuzz 时日志只会拖慢速度 */

void vmm_log(int level, const char *file, int line, const char *fmt, ...)
{
    (void)level; (void)file; (void)line; (void)fmt;
}

/* 和 src/mem.c 的 gpa_to_hva() 同语义（含溢出检查），但只有一个
 * 从 0 起的 slot。故意不复用 mem.c：这里要测的是加载器怎么用这个
 * 接口，桩本身必须简单到一眼能看出它是对的。 */
void *gpa_to_hva(struct vmm_vm *vm, uint64_t gpa, uint64_t len)
{
    (void)vm;
    if (len == 0)
        return NULL;
    if (gpa + len < gpa)                /* 回绕 */
        return NULL;
    if (gpa >= FUZZ_RAM_SIZE || gpa + len > FUZZ_RAM_SIZE)
        return NULL;
    return fuzz_ram + gpa;
}

int mem_write(struct vmm_vm *vm, uint64_t gpa, const void *src, size_t len)
{
    void *p = gpa_to_hva(vm, gpa, len);

    if (!p)
        return VMM_ERR_GUEST;
    memcpy(p, src, len);
    return VMM_OK;
}

static void fuzz_ram_init(void)
{
    if (!fuzz_ram) {
        fuzz_ram = malloc(FUZZ_RAM_SIZE);
        if (!fuzz_ram) {
            fprintf(stderr, "fuzz: cannot allocate %llu bytes of guest RAM\n",
                    (unsigned long long)FUZZ_RAM_SIZE);
            abort();
        }
    }
}

/* 把这一轮的输入变成一个可 open/mmap 的路径。
 * 返回 0 成功；调用方用完必须 fuzz_input_close()。 */
struct fuzz_input {
    int  fd;
    char path[64];
};

static int fuzz_input_open(struct fuzz_input *in, const uint8_t *data,
                           size_t size)
{
    ssize_t n;
    size_t done = 0;

    in->fd = memfd_create("fuzz-kernel", MFD_CLOEXEC);
    if (in->fd < 0)
        return -1;
    while (done < size) {
        n = write(in->fd, data + done, size - done);
        if (n <= 0) {
            close(in->fd);
            return -1;
        }
        done += (size_t)n;
    }
    snprintf(in->path, sizeof(in->path), "/proc/self/fd/%d", in->fd);
    return 0;
}

static void fuzz_input_close(struct fuzz_input *in)
{
    close(in->fd);
}

#endif /* FUZZ_COMMON_H */
