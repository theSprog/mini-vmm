/* tests/unit/blockdev_test.c — 磁盘后端（src/block/blockdev.c）
 *
 * 判据来源：
 *   - 参数和引用规则：include/block/blockdev.h 写明的约定（id 规则、
 *     format 只有 raw、每个 --drive 恰好被一个设备引用）。
 *   - 读写结果：用独立的 pread/pwrite 在另一个 fd 上核对镜像内容，而不是
 *     拿被测函数自己读回来比较，否则读写两边犯同一个错误时会互相掩盖。
 *   - 部分传输：本文件提供自己的 preadv/pwritev，覆盖 libc 的同名符号，
 *     可以让每次调用只传几个字节或先返回一次 EINTR。普通文件上真实的
 *     preadv 几乎从不部分返回，不这样做，xfer 里推进 iovec 的那段代码
 *     就永远测不到。
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/uio.h>

#include "vmm.h"
#include "block/blockdev.h"
#include "devices/device.h"
#include "harness.h"

int vmm_log_level = -1;
static char g_last_log[512];

void vmm_log(int level, const char *file, int line, const char *fmt, ...)
{
    va_list ap;

    (void)level; (void)file; (void)line;
    va_start(ap, fmt);
    vsnprintf(g_last_log, sizeof(g_last_log), fmt, ap);
    va_end(ap);
    if (vmm_log_level >= 0)
        fprintf(stderr, "       [log] %s\n", g_last_log);
}

/* ---- preadv/pwritev 替身 ----
 *
 * g_chunk > 0 时每次最多传 g_chunk 字节，并且只碰第一个 iovec 段，模拟
 * 内核的部分传输；g_eintr > 0 时先返回 g_eintr 次 EINTR。g_calls 记调用
 * 次数，用来确认部分传输真的发生了。 */
static size_t g_chunk;
static int    g_eintr;
static int    g_calls;

static ssize_t fake_io(bool write, int fd, const struct iovec *iov, int cnt,
                       off_t off)
{
    ssize_t total = 0;
    int i;

    g_calls++;
    if (g_eintr > 0) {
        g_eintr--;
        errno = EINTR;
        return -1;
    }
    for (i = 0; i < cnt; i++) {
        size_t len = iov[i].iov_len;
        ssize_t n;

        if (g_chunk && len > g_chunk)
            len = g_chunk;
        n = write ? pwrite(fd, iov[i].iov_base, len, off + total)
                  : pread(fd, iov[i].iov_base, len, off + total);
        if (n < 0)
            return total ? total : -1;
        total += n;
        if (g_chunk || (size_t)n < iov[i].iov_len)
            break;
    }
    return total;
}

ssize_t preadv(int fd, const struct iovec *iov, int cnt, off_t off)
{
    return fake_io(false, fd, iov, cnt, off);
}

ssize_t pwritev(int fd, const struct iovec *iov, int cnt, off_t off)
{
    return fake_io(true, fd, iov, cnt, off);
}

/* ---- 辅助 ---- */

static char g_dir[256];

/* 被测对象按设计从不释放（见 blockdev.h），测试里解析出来的对象和它们
 * 引用的参数字符串挂在这里，保持可达，LeakSanitizer 就不会把它们当成
 * 泄漏；进程退出时一起回收 */
static void *g_keep[256];
static int   g_nkeep;

static void *keep(void *p)
{
    if (p && g_nkeep < (int)(sizeof(g_keep) / sizeof(g_keep[0])))
        g_keep[g_nkeep++] = p;
    return p;
}

/* 参数字符串要活得和对象一样久（bd->arg 指向它） */
static const char *arg_of(const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return keep(strdup(buf));
}

/* 建一个 size 字节的镜像，第 i 字节是 pattern(i) */
static uint8_t pattern(uint64_t i)
{
    return (uint8_t)(i * 7 + (i >> 8));
}

static char *make_image(const char *name, size_t size)
{
    static char path[4][512];
    static int next;
    char *p = path[next++ & 3];
    uint8_t *buf = malloc(size ? size : 1);
    size_t i;
    int fd;

    snprintf(p, 512, "%s/%s", g_dir, name);
    for (i = 0; i < size; i++)
        buf[i] = pattern(i);
    fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || (size && write(fd, buf, size) != (ssize_t)size)) {
        perror(p);
        exit(1);
    }
    close(fd);
    free(buf);
    return p;
}

static struct blockdev *parse_ok(const char *arg)
{
    struct blockdev *bd = NULL;

    CHECK_U64(blockdev_parse(arg, &bd), VMM_OK, "parse \"%s\" (%s)", arg, g_last_log);
    if (!bd)
        exit(1);
    keep(bd);
    keep(bd->filename);
    return bd;
}

static void expect_parse_err(const char *arg, const char *msg)
{
    struct blockdev *bd = NULL;

    g_last_log[0] = '\0';
    CHECK_U64(blockdev_parse(arg, &bd), VMM_ERR_INVAL, "\"%s\" rejected", arg);
    CHECK(bd == NULL, "\"%s\" allocates nothing", arg);
    CHECK(strstr(g_last_log, msg) != NULL, "error for \"%s\" mentions \"%s\": \"%s\"",
          arg, msg, g_last_log);
}

static struct blockdev *open_ok(const char *file, bool ro)
{
    const char *arg = arg_of("id=d0,file=%s%s", file, ro ? ",readonly=on" : "");
    struct blockdev *bd = parse_ok(arg);

    CHECK_U64(blockdev_open(bd), VMM_OK, "open %s (%s)", arg, g_last_log);
    return bd;
}

/* 用另一个 fd 读镜像，和被测代码无关 */
static void read_back(const char *file, uint64_t off, void *buf, size_t len)
{
    int fd = open(file, O_RDONLY);

    if (fd < 0 || pread(fd, buf, len, (off_t)off) != (ssize_t)len) {
        perror(file);
        exit(1);
    }
    close(fd);
}

/* ---- 用例 ---- */

static void case_parse(void)
{
    struct blockdev *bd;
    char longid[64];

    T_CASE("--drive 参数：必填 id/file，format 只有 raw，readonly 是布尔");
    bd = parse_ok("id=d0,file=a.img");
    if (bd) {
        CHECK(!strcmp(bd->id, "d0") && !strcmp(bd->filename, "a.img"), "id and file kept");
        CHECK(!bd->read_only && bd->fd == -1, "writable, not opened yet");
    }
    bd = parse_ok("file=a,,b.img,format=raw,readonly=on,id=d1");
    if (bd) {
        CHECK(!strcmp(bd->filename, "a,b.img"), "',,' is a comma in the file name: %s",
              bd->filename);
        CHECK(bd->read_only, "readonly=on");
    }

    expect_parse_err("file=a.img", "missing property 'id'");
    expect_parse_err("id=d0", "missing property 'file'");
    expect_parse_err("id=d0,file=", "file must not be empty");
    expect_parse_err("id=0d,file=a", "invalid id '0d'");
    /* 31 个字符是上限（BLOCKDEV_ID_MAX 含结尾的 0），32 个被拒绝 */
    snprintf(longid, sizeof(longid), "id=%.31s,file=a", "abcdefghijklmnopqrstuvwxyz0123456789");
    parse_ok(arg_of("%s", longid));
    snprintf(longid, sizeof(longid), "id=%.32s,file=a", "abcdefghijklmnopqrstuvwxyz0123456789");
    expect_parse_err(longid, "at most 31 characters");
    expect_parse_err("id=d0,file=a,format=qcow2", "unsupported format 'qcow2'");
    expect_parse_err("id=d0,file=a,readonly=1", "must be 'on' or 'off'");
    expect_parse_err("id=d0,file=a,if=ide", "unknown property 'if'");
    /* 没有隐含键：QEMU 的 -drive 也不接受省略 file= 的写法 */
    expect_parse_err("a.img,id=d0", "'a.img' has no value");
}

static void case_link(void)
{
    struct vmm_device devs[3];
    struct blockdev *d[3];

    T_CASE("drive= 引用：id 存在、一一对应，没有被引用的 --drive 报错");
    memset(devs, 0, sizeof(devs));
    devs[0].arg = "ahci,drive=d0";
    devs[0].drive_id = "d0";
    devs[1].arg = "e1000";                     /* 没有 drive= 的设备不参与 */
    devs[2].arg = "ahci,drive=d1";
    devs[2].drive_id = "d1";
    d[0] = parse_ok("id=d1,file=b");
    d[1] = parse_ok("id=d0,file=a");
    CHECK_U64(blockdev_link(devs, 3, d, 2), VMM_OK, "link (%s)", g_last_log);
    CHECK(devs[0].blk == d[1] && devs[2].blk == d[0], "each device got its drive");
    CHECK(devs[1].blk == NULL, "device without drive= untouched");
    CHECK(d[1]->user && !strcmp(d[1]->user, "ahci,drive=d0"), "user recorded");

    memset(devs, 0, sizeof(devs));
    devs[0].arg = "ahci,drive=dx";
    devs[0].drive_id = "dx";
    d[0] = parse_ok("id=d0,file=a");
    g_last_log[0] = '\0';
    CHECK_U64(blockdev_link(devs, 1, d, 1), VMM_ERR_INVAL, "unknown id rejected");
    /* 最后一条是“d0 没人用”，未知 id 那条在它之前，两条都要报 */
    CHECK(strstr(g_last_log, "not used by any --device") != NULL,
          "unreferenced drive reported: \"%s\"", g_last_log);

    memset(devs, 0, sizeof(devs));
    devs[0].arg = "ahci,drive=d0";
    devs[0].drive_id = "d0";
    devs[1].arg = "ahci,drive=d0,x";
    devs[1].drive_id = "d0";
    d[0] = parse_ok("id=d0,file=a");
    g_last_log[0] = '\0';
    CHECK_U64(blockdev_link(devs, 2, d, 1), VMM_ERR_INVAL, "shared drive rejected");
    CHECK(strstr(g_last_log, "drive 'd0' is already used by --device ahci,drive=d0"),
          "double use names the first user: \"%s\"", g_last_log);

    d[0] = parse_ok("id=d0,file=a");
    d[1] = parse_ok("id=d0,file=b");
    g_last_log[0] = '\0';
    CHECK_U64(blockdev_link(NULL, 0, d, 2), VMM_ERR_INVAL, "duplicate id rejected");
    CHECK(strstr(g_last_log, "duplicate id 'd0'") != NULL, "duplicate id: \"%s\"",
          g_last_log);
}

static void case_open(void)
{
    struct blockdev *a, *b, *c;
    char *img = make_image("open.img", 65536 + 100);
    char *empty = make_image("empty.img", 0);

    T_CASE("打开：容量取文件长度，空镜像和不存在的文件报错，镜像加锁");
    a = open_ok(img, false);
    CHECK_U64(a->size, 65536 + 100, "size is the file length, not rounded to sectors");

    /* 可写盘是排他锁：第二个打开者不管读写都失败 */
    b = parse_ok(arg_of("id=d1,file=%s", img));
    g_last_log[0] = '\0';
    CHECK_U64(blockdev_open(b), VMM_ERR_SYS, "second writer refused");
    CHECK(strstr(g_last_log, "is locked by another process") != NULL,
          "lock conflict explained: \"%s\"", g_last_log);
    CHECK(b->fd == -1, "failed open leaves fd -1");
    b = parse_ok(arg_of("id=d1,file=%s,readonly=on", img));
    CHECK_U64(blockdev_open(b), VMM_ERR_SYS, "reader refused while a writer holds it");
    blockdev_close(a);
    CHECK(a->fd == -1, "close resets fd");

    /* 只读盘是共享锁：多个只读者可以共存，但挡住写者 */
    b = open_ok(img, true);
    c = open_ok(img, true);
    a = parse_ok(arg_of("id=d2,file=%s", img));
    CHECK_U64(blockdev_open(a), VMM_ERR_SYS, "writer refused while readers hold it");
    blockdev_close(b);
    blockdev_close(c);

    a = parse_ok(arg_of("id=d0,file=%s", empty));
    g_last_log[0] = '\0';
    CHECK_U64(blockdev_open(a), VMM_ERR_SYS, "empty image refused");
    CHECK(strstr(g_last_log, "is empty") != NULL, "empty image: \"%s\"", g_last_log);

    a = parse_ok(arg_of("id=d0,file=%s/missing.img", g_dir));
    g_last_log[0] = '\0';
    CHECK_U64(blockdev_open(a), VMM_ERR_SYS, "missing file refused");
    CHECK(strstr(g_last_log, "No such file") != NULL, "errno text: \"%s\"", g_last_log);
}

static void case_rw(void)
{
    char *img = make_image("rw.img", 8192);
    struct blockdev *bd = open_ok(img, false);
    uint8_t a[100], b[1], c[300], back[401];
    struct iovec iov[4];
    int i;

    T_CASE("读写：多段 iovec 按顺序连续落在 offset 上，长度为 0 的段被跳过");
    iov[0] = (struct iovec){ a, sizeof(a) };
    iov[1] = (struct iovec){ NULL, 0 };
    iov[2] = (struct iovec){ b, sizeof(b) };
    iov[3] = (struct iovec){ c, sizeof(c) };
    CHECK_U64(blk_preadv(bd, iov, 4, 1000), VMM_OK, "read 401 bytes at 1000");
    for (i = 0; i < 100; i++)
        CHECK(a[i] == pattern(1000 + i), "a[%d]", i);
    CHECK(b[0] == pattern(1100), "b[0]");
    for (i = 0; i < 300; i++)
        CHECK(c[i] == pattern(1101 + i), "c[%d]", i);

    memset(a, 0xA1, sizeof(a));
    b[0] = 0xB2;
    memset(c, 0xC3, sizeof(c));
    CHECK_U64(blk_pwritev(bd, iov, 4, 7791), VMM_OK, "write 401 bytes ending at the last byte");
    read_back(img, 7791, back, sizeof(back));
    for (i = 0; i < 401; i++)
        CHECK(back[i] == (i < 100 ? 0xA1 : i == 100 ? 0xB2 : 0xC3), "back[%d] = %#x", i, back[i]);
    read_back(img, 7790, back, 1);
    CHECK(back[0] == pattern(7790), "byte before the write untouched");

    CHECK_U64(blk_flush(bd), VMM_OK, "flush");
    CHECK_U64(bd->stats.rd_ops, 1, "rd_ops");
    CHECK_U64(bd->stats.rd_bytes, 401, "rd_bytes");
    CHECK_U64(bd->stats.wr_ops, 1, "wr_ops");
    CHECK_U64(bd->stats.wr_bytes, 401, "wr_bytes");
    CHECK_U64(bd->stats.flush_ops, 1, "flush_ops");
    CHECK_U64(bd->stats.errors, 0, "no errors");
    blockdev_close(bd);
}

static void case_range(void)
{
    char *img = make_image("range.img", 4096);
    struct blockdev *bd = open_ok(img, false);
    static struct iovec many[BLK_MAX_IOV + 1];
    uint8_t buf[16], back[16];
    struct iovec iov[2];
    int i;

    T_CASE("越界：offset 和长度的任何组合都不能碰到镜像以外，加法溢出也不行");
    iov[0] = (struct iovec){ buf, 16 };
    CHECK_U64(blk_preadv(bd, iov, 1, 4080), VMM_OK, "last 16 bytes readable");
    CHECK_U64(blk_preadv(bd, iov, 1, 4081), VMM_ERR_INVAL, "one byte past the end");
    CHECK_U64(blk_preadv(bd, iov, 1, 4096), VMM_ERR_INVAL, "starting at the end");
    CHECK_U64(blk_pwritev(bd, iov, 1, UINT64_MAX - 7), VMM_ERR_INVAL,
              "offset + len wraps around");
    CHECK(strstr(g_last_log, "outside the 4096-byte image") != NULL,
          "range error names the image size: \"%s\"", g_last_log);

    /* 两段长度之和溢出回绕成很小的数，不能被当成 15 字节 */
    iov[0] = (struct iovec){ buf, 16 };
    iov[1] = (struct iovec){ buf, SIZE_MAX };
    CHECK_U64(blk_pwritev(bd, iov, 2, 0), VMM_ERR_INVAL, "iovec total overflows");

    for (i = 0; i <= BLK_MAX_IOV; i++)
        many[i] = (struct iovec){ buf, 1 };
    CHECK_U64(blk_preadv(bd, many, BLK_MAX_IOV, 0), VMM_OK, "BLK_MAX_IOV segments allowed");
    CHECK_U64(blk_preadv(bd, many, BLK_MAX_IOV + 1, 0), VMM_ERR_INVAL,
              "one segment more refused");
    CHECK_U64(blk_preadv(bd, iov, -1, 0), VMM_ERR_INVAL, "negative count refused");

    CHECK_U64(bd->stats.errors, 6, "every rejection counted");
    read_back(img, 0, back, sizeof(back));
    for (i = 0; i < 16; i++)
        CHECK(back[i] == pattern(i), "rejected writes left byte %d alone", i);
    blockdev_close(bd);
}

static void case_readonly(void)
{
    char *img = make_image("ro.img", 4096);
    struct blockdev *bd = open_ok(img, true);
    uint8_t buf[16], back[16];
    struct iovec iov = { buf, sizeof(buf) };
    int i;

    T_CASE("只读盘：读正常，写被拒绝且不改镜像，flush 是空操作");
    CHECK_U64(blk_preadv(bd, &iov, 1, 0), VMM_OK, "read");
    memset(buf, 0xEE, sizeof(buf));
    g_last_log[0] = '\0';
    CHECK_U64(blk_pwritev(bd, &iov, 1, 0), VMM_ERR_INVAL, "write refused");
    CHECK(strstr(g_last_log, "read-only drive") != NULL, "read-only write: \"%s\"",
          g_last_log);
    read_back(img, 0, back, sizeof(back));
    for (i = 0; i < 16; i++)
        CHECK(back[i] == pattern(i), "byte %d unchanged", i);
    CHECK_U64(blk_flush(bd), VMM_OK, "flush on read-only");
    CHECK_U64(bd->stats.flush_ops, 0, "no fdatasync issued");
    blockdev_close(bd);
}

static void case_partial(void)
{
    char *img = make_image("part.img", 4096);
    struct blockdev *bd = open_ok(img, false);
    uint8_t a[10], b[7], c[13], back[30];
    struct iovec iov[3] = { { a, 10 }, { b, 7 }, { c, 13 } };
    int i;

    T_CASE("部分传输和 EINTR：继续传剩下的部分，跨段推进不错位");
    g_chunk = 3;
    g_eintr = 2;
    g_calls = 0;
    CHECK_U64(blk_preadv(bd, iov, 3, 100), VMM_OK, "read in 3-byte pieces");
    /* 每段按 3 字节切：10 → 4 次，7 → 3 次，13 → 5 次，再加 2 次 EINTR */
    CHECK_U64(g_calls, 14, "partial transfers really happened");
    for (i = 0; i < 10; i++)
        CHECK(a[i] == pattern(100 + i), "a[%d]", i);
    for (i = 0; i < 7; i++)
        CHECK(b[i] == pattern(110 + i), "b[%d]", i);
    for (i = 0; i < 13; i++)
        CHECK(c[i] == pattern(117 + i), "c[%d]", i);

    for (i = 0; i < 10; i++) a[i] = (uint8_t)(0x10 + i);
    for (i = 0; i < 7; i++)  b[i] = (uint8_t)(0x40 + i);
    for (i = 0; i < 13; i++) c[i] = (uint8_t)(0x80 + i);
    g_eintr = 1;
    CHECK_U64(blk_pwritev(bd, iov, 3, 2000), VMM_OK, "write in 3-byte pieces");
    read_back(img, 2000, back, sizeof(back));
    for (i = 0; i < 30; i++) {
        uint8_t want = i < 10 ? (uint8_t)(0x10 + i) : i < 17 ? (uint8_t)(0x40 + i - 10)
                                                             : (uint8_t)(0x80 + i - 17);
        CHECK(back[i] == want, "back[%d] = %#x, want %#x", i, back[i], want);
    }
    g_chunk = 0;
    CHECK_U64(bd->stats.errors, 0, "EINTR is not an error");
    blockdev_close(bd);

    /* 镜像在运行中被截短：范围检查按打开时的容量放行，preadv 读到文件尾 */
    T_CASE("镜像被截短：读到文件尾当作 I/O 错误，而不是死循环");
    bd = open_ok(img, false);
    if (truncate(img, 1024) != 0)
        perror("truncate");
    iov[0] = (struct iovec){ a, 10 };
    g_last_log[0] = '\0';
    CHECK_U64(blk_preadv(bd, iov, 1, 2000), VMM_ERR_SYS, "short image reported");
    CHECK(strstr(g_last_log, "Input/output error") != NULL, "EOF is EIO: \"%s\"",
          g_last_log);
    CHECK_U64(bd->stats.errors, 1, "counted");
    blockdev_close(bd);
}

int main(int argc, char **argv)
{
    int i;

    T_MAIN_BEGIN();
    /* 推进 iovec 或处理文件尾的代码写错时，最常见的结局是 xfer 原地打转；
     * 全部用例正常不到一秒，30 秒的闹钟只兜这种死循环 */
    alarm(30);
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-v"))
            vmm_log_level = 3;
    snprintf(g_dir, sizeof(g_dir), "%s/blockdev_test.XXXXXX",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(g_dir)) {
        perror("mkdtemp");
        return 1;
    }

    case_parse();
    case_link();
    case_open();
    case_rw();
    case_range();
    case_readonly();
    case_partial();

    i = T_SUMMARY();
    {
        char cmd[300];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
        if (system(cmd) != 0)
            fprintf(stderr, "cannot remove %s\n", g_dir);
    }
    return i;
}
