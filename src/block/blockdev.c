/* src/block/blockdev.c — --drive 的解析、前后端连接和同步的 raw 镜像读写
 *
 * 接口约定和生命周期见 include/block/blockdev.h。
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>

#include "vmm.h"
#include "opts.h"
#include "block/blockdev.h"
#include "devices/device.h"

static const char *const drive_props[] = {
    "id", "file", "format", "readonly", NULL
};

int blockdev_parse(const char *arg, struct blockdev **out)
{
    struct blockdev *bd = NULL;
    const char *id, *file, *format;
    bool read_only;
    struct opts o;
    int r;

    /* 没有隐含键：QEMU 的 -drive 也不接受 "disk.img,..." 这种省略写法
     * （drive_def 调 qemu_opts_parse_noisily 时 permit_abbrev 为 false） */
    r = opts_parse(&o, "--drive", arg, NULL);
    if (r != VMM_OK)
        return r;

    r = VMM_ERR_INVAL;
    if (opts_check_keys(&o, drive_props) != VMM_OK)
        goto out;
    id = opts_require(&o, "id");
    file = opts_require(&o, "file");
    if (!id || !file)
        goto out;
    if (!opts_id_wellformed(id) || strlen(id) >= BLOCKDEV_ID_MAX) {
        vmm_err("--drive %s: invalid id '%s' (a letter followed by letters, "
                "digits, '-', '.' or '_', at most %d characters)",
                arg, id, BLOCKDEV_ID_MAX - 1);
        goto out;
    }
    if (!file[0]) {
        vmm_err("--drive %s: file must not be empty", arg);
        goto out;
    }
    /* format 可以省略，因为只有 raw 一种。QEMU 省略时会探测格式，探测
     * 结果取决于镜像开头的字节，guest 写坏开头就可能下次被认成别的格式；
     * 这里没有探测，省略就是 raw */
    format = opts_get(&o, "format");
    if (format && strcmp(format, "raw")) {
        vmm_err("--drive %s: unsupported format '%s' (only raw)", arg, format);
        goto out;
    }
    if (opts_get_bool(&o, "readonly", false, &read_only) != VMM_OK)
        goto out;

    bd = calloc(1, sizeof(*bd));
    if (!bd || !(bd->filename = strdup(file))) {
        free(bd);
        r = VMM_ERR_NOMEM;
        goto out;
    }
    memcpy(bd->id, id, strlen(id) + 1);
    bd->read_only = read_only;
    bd->arg = arg;
    bd->fd = -1;
    *out = bd;
    r = VMM_OK;
out:
    opts_free(&o);
    return r;
}

int blockdev_link(struct vmm_device *devs, int nr_devs,
                  struct blockdev *const *drives, int nr_drives)
{
    int r = VMM_OK;
    int i, j;

    for (i = 0; i < nr_drives; i++)
        for (j = 0; j < i; j++)
            if (!strcmp(drives[i]->id, drives[j]->id)) {
                vmm_err("--drive %s: duplicate id '%s' (also used by --drive %s)",
                        drives[i]->arg, drives[i]->id, drives[j]->arg);
                r = VMM_ERR_INVAL;
            }
    if (r != VMM_OK)
        return r;

    for (i = 0; i < nr_devs; i++) {
        struct vmm_device *d = &devs[i];
        struct blockdev *bd = NULL;

        if (!d->drive_id)
            continue;
        for (j = 0; j < nr_drives; j++)
            if (!strcmp(drives[j]->id, d->drive_id))
                bd = drives[j];
        if (!bd) {
            vmm_err("--device %s: no --drive with id '%s'", d->arg, d->drive_id);
            r = VMM_ERR_INVAL;
            continue;
        }
        /* 一个镜像只接一个控制器：两个控制器各自缓存、各自写，guest 里
         * 会出现两块内容互相覆盖的“不同”磁盘 */
        if (bd->user) {
            vmm_err("--device %s: drive '%s' is already used by --device %s",
                    d->arg, bd->id, bd->user);
            r = VMM_ERR_INVAL;
            continue;
        }
        bd->user = d->arg;
        d->blk = bd;
    }

    /* 没有前端的 --drive 是写漏了 --device：镜像照样会被打开、加锁，
     * guest 里却没有这块盘 */
    for (i = 0; i < nr_drives; i++)
        if (!drives[i]->user) {
            vmm_err("--drive %s: not used by any --device (a disk controller "
                    "needs drive=%s)", drives[i]->arg, drives[i]->id);
            r = VMM_ERR_INVAL;
        }
    return r;
}

int blockdev_open(struct blockdev *bd)
{
    off_t end;

    bd->fd = open(bd->filename, (bd->read_only ? O_RDONLY : O_RDWR) | O_CLOEXEC);
    if (bd->fd < 0) {
        vmm_err("--drive %s: open %s: %s", bd->arg, bd->filename, strerror(errno));
        return VMM_ERR_SYS;
    }
    if (flock(bd->fd, (bd->read_only ? LOCK_SH : LOCK_EX) | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK)
            vmm_err("--drive %s: %s is locked by another process (another VM "
                    "using it?)", bd->arg, bd->filename);
        else
            vmm_err("--drive %s: flock %s: %s", bd->arg, bd->filename,
                    strerror(errno));
        goto fail;
    }
    /* lseek 对普通文件和块设备都给出字节数，fstat 的 st_size 对块设备是 0 */
    end = lseek(bd->fd, 0, SEEK_END);
    if (end < 0) {
        vmm_err("--drive %s: seek %s: %s", bd->arg, bd->filename, strerror(errno));
        goto fail;
    }
    if (end == 0) {
        vmm_err("--drive %s: %s is empty (create it first, e.g. "
                "truncate -s 64M %s)", bd->arg, bd->filename, bd->filename);
        goto fail;
    }
    bd->size = (uint64_t)end;
    vmm_info("drive %s: %s, %llu bytes%s", bd->id, bd->filename,
             (unsigned long long)bd->size, bd->read_only ? ", read-only" : "");
    return VMM_OK;

fail:
    close(bd->fd);
    bd->fd = -1;
    return VMM_ERR_SYS;
}

/* iov 的总长度；溢出时返回 false */
static bool iov_total(const struct iovec *iov, int iovcnt, uint64_t *total)
{
    uint64_t t = 0;
    int i;

    for (i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len > UINT64_MAX - t)
            return false;
        t += iov[i].iov_len;
    }
    *total = t;
    return true;
}

/* 范围检查。guest 给的 LBA 和长度先由前端换算成字节，这里不信任换算结果：
 * offset + len 的加法本身可能溢出回绕 */
static int check_range(struct blockdev *bd, const char *op,
                       const struct iovec *iov, int iovcnt, uint64_t offset,
                       uint64_t *len)
{
    if (iovcnt < 0 || iovcnt > BLK_MAX_IOV || !iov_total(iov, iovcnt, len) ||
        offset > bd->size || *len > bd->size - offset) {
        bd->stats.errors++;
        vmm_err("drive %s: %s of %d segments at offset 0x%llx is outside the "
                "%llu-byte image", bd->id, op, iovcnt,
                (unsigned long long)offset, (unsigned long long)bd->size);
        return VMM_ERR_INVAL;
    }
    return VMM_OK;
}

/* preadv/pwritev 可能只传一部分（信号打断、文件系统的单次上限），剩下的
 * 继续传。本地拷一份 iovec 才能在原地推进。 */
static int xfer(struct blockdev *bd, bool write, const struct iovec *iov,
                int iovcnt, uint64_t offset, uint64_t len)
{
    struct iovec v[BLK_MAX_IOV];
    struct iovec *cur = v;
    int cnt = iovcnt;
    uint64_t done = 0;

    if (iovcnt > 0)
        memcpy(v, iov, sizeof(v[0]) * (size_t)iovcnt);
    while (done < len) {
        ssize_t n;

        /* 跳过长度为 0 的段，并保证 cur[0] 至少还有 1 字节 */
        while (cnt > 0 && cur->iov_len == 0) {
            cur++;
            cnt--;
        }
        n = write ? pwritev(bd->fd, cur, cnt, (off_t)(offset + done))
                  : preadv(bd->fd, cur, cnt, (off_t)(offset + done));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            /* n == 0：范围检查过了却读到文件尾，镜像在运行中被别人截短了 */
            if (n == 0)
                errno = EIO;
            bd->stats.errors++;
            vmm_err("drive %s: %s %llu bytes at offset 0x%llx: %s", bd->id,
                    write ? "write" : "read", (unsigned long long)(len - done),
                    (unsigned long long)(offset + done), strerror(errno));
            return VMM_ERR_SYS;
        }
        done += (uint64_t)n;
        while (n > 0) {
            if ((size_t)n >= cur->iov_len) {
                n -= (ssize_t)cur->iov_len;
                cur++;
                cnt--;
            } else {
                cur->iov_base = (char *)cur->iov_base + n;
                cur->iov_len -= (size_t)n;
                n = 0;
            }
        }
    }
    return VMM_OK;
}

int blk_preadv(struct blockdev *bd, const struct iovec *iov, int iovcnt,
               uint64_t offset)
{
    uint64_t len;
    int r;

    r = check_range(bd, "read", iov, iovcnt, offset, &len);
    if (r != VMM_OK)
        return r;
    r = xfer(bd, false, iov, iovcnt, offset, len);
    if (r == VMM_OK) {
        bd->stats.rd_ops++;
        bd->stats.rd_bytes += len;
    }
    return r;
}

int blk_pwritev(struct blockdev *bd, const struct iovec *iov, int iovcnt,
                uint64_t offset)
{
    uint64_t len;
    int r;

    if (bd->read_only) {
        bd->stats.errors++;
        vmm_err("drive %s: write at offset 0x%llx to a read-only drive",
                bd->id, (unsigned long long)offset);
        return VMM_ERR_INVAL;
    }
    r = check_range(bd, "write", iov, iovcnt, offset, &len);
    if (r != VMM_OK)
        return r;
    r = xfer(bd, true, iov, iovcnt, offset, len);
    if (r == VMM_OK) {
        bd->stats.wr_ops++;
        bd->stats.wr_bytes += len;
    }
    return r;
}

int blk_flush(struct blockdev *bd)
{
    if (bd->read_only)
        return VMM_OK;
    /* 只需要数据和找回数据所需的元数据（文件长度不变），不需要 mtime 这类
     * 属性落盘，所以是 fdatasync 而不是 fsync */
    while (fdatasync(bd->fd) < 0) {
        if (errno == EINTR)
            continue;
        bd->stats.errors++;
        vmm_err("drive %s: fdatasync: %s", bd->id, strerror(errno));
        return VMM_ERR_SYS;
    }
    bd->stats.flush_ops++;
    return VMM_OK;
}

void blockdev_close(struct blockdev *bd)
{
    const struct blockdev_stats *s = &bd->stats;

    if (bd->fd < 0)
        return;
    vmm_info("drive %s: read %llu ops %llu bytes, write %llu ops %llu bytes, "
             "%llu flushes, %llu errors", bd->id,
             (unsigned long long)s->rd_ops, (unsigned long long)s->rd_bytes,
             (unsigned long long)s->wr_ops, (unsigned long long)s->wr_bytes,
             (unsigned long long)s->flush_ops, (unsigned long long)s->errors);
    close(bd->fd);
    bd->fd = -1;
}
