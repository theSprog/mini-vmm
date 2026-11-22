/* tools/guest/evkey.c — 在 guest 里接住某个输入设备的按键事件
 *
 * 用法：evkey NAME COUNT FIFO
 *   找到名字（/sys/class/input/eventN/device/name）等于 NAME 的输入设备，
 *   打开它之后 fork：父进程打印 "evkey: listening on /dev/input/eventN" 并
 *   退出，子进程在后台读事件，收满 COUNT 个 EV_KEY 后把每个事件写成一行
 *   "key CODE VALUE" 写进 FIFO（mkfifo 建好的命名管道），然后退出。
 *   60 秒内没收满时写 "evkey: timeout, got N of COUNT key events"。
 *
 * 为什么是这个形状：e2e 用例要先在 guest 里开始监听，再由 host 经 monitor
 * 按电源按钮，最后在 guest 里读结果，三步之间靠 shell 提示符衔接。
 *   - 父进程等设备打开之后才返回，提示符出现时监听一定已经就位，host 那边
 *     按下按钮时不会因为还没打开设备而丢事件；
 *   - 结果走 FIFO：读端 `cat FIFO` 会阻塞到子进程写完、关掉写端为止，等的
 *     正是“事件收齐了”这件事本身，不用猜事件什么时候送到。
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { EXIT_OK = 0, EXIT_FAIL = 1, EXIT_USAGE = 2 };

#define MAX_KEYS   64
/* 兜底：host 那一步失败时子进程不要一直挂在后台 */
#define TIMEOUT_MS 60000

static int open_by_name(const char *name, char *path, size_t cap)
{
    DIR *d = opendir("/sys/class/input");
    struct dirent *e;
    int fd = -1;

    if (!d) {
        perror("evkey: /sys/class/input");
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        char p[320], buf[256];
        FILE *f;

        if (strncmp(e->d_name, "event", 5) != 0)
            continue;
        snprintf(p, sizeof(p), "/sys/class/input/%s/device/name", e->d_name);
        f = fopen(p, "r");
        if (!f)
            continue;
        if (fgets(buf, sizeof(buf), f)) {
            buf[strcspn(buf, "\n")] = '\0';
            if (strcmp(buf, name) == 0) {
                snprintf(path, cap, "/dev/input/%s", e->d_name);
                fd = open(path, O_RDONLY);
                if (fd < 0)
                    fprintf(stderr, "evkey: %s: %s\n", path, strerror(errno));
                fclose(f);
                break;
            }
        }
        fclose(f);
    }
    closedir(d);
    if (fd < 0 && !path[0])
        fprintf(stderr, "evkey: no input device named '%s'\n", name);
    return fd;
}

static void report(const char *fifo, const char *text)
{
    int fd = open(fifo, O_WRONLY);     /* 阻塞到读端打开 */
    size_t len = strlen(text);

    if (fd < 0) {
        fprintf(stderr, "evkey: %s: %s\n", fifo, strerror(errno));
        return;
    }
    if (write(fd, text, len) != (ssize_t)len)
        fprintf(stderr, "evkey: short write to %s\n", fifo);
    close(fd);
}

int main(int argc, char **argv)
{
    char path[300] = "", out[MAX_KEYS * 32 + 128];
    size_t used = 0;
    int fd, count, got = 0;
    pid_t pid;

    if (argc != 4 || (count = atoi(argv[2])) <= 0 || count > MAX_KEYS) {
        fprintf(stderr, "usage: evkey NAME COUNT FIFO   (1 <= COUNT <= %d)\n", MAX_KEYS);
        return EXIT_USAGE;
    }
    fd = open_by_name(argv[1], path, sizeof(path));
    if (fd < 0)
        return EXIT_FAIL;

    pid = fork();
    if (pid < 0) {
        perror("evkey: fork");
        return EXIT_FAIL;
    }
    if (pid > 0) {
        printf("evkey: listening on %s (%s)\n", path, argv[1]);
        return EXIT_OK;
    }

    while (got < count) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        struct input_event ev;
        int r = poll(&p, 1, TIMEOUT_MS);

        if (r == 0) {
            used += (size_t)snprintf(out + used, sizeof(out) - used,
                                     "evkey: timeout, got %d of %d key events\n",
                                     got, count);
            break;
        }
        if (r < 0) {
            if (errno == EINTR)
                continue;
            used += (size_t)snprintf(out + used, sizeof(out) - used,
                                     "evkey: poll: %s\n", strerror(errno));
            break;
        }
        if (read(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
            used += (size_t)snprintf(out + used, sizeof(out) - used,
                                     "evkey: short read from %s\n", path);
            break;
        }
        if (ev.type != EV_KEY)
            continue;
        used += (size_t)snprintf(out + used, sizeof(out) - used,
                                 "key %u %d\n", ev.code, ev.value);
        got++;
    }
    report(argv[3], out);
    return EXIT_OK;
}
