/* src/monitor.c — monitor 的协议核心和 socket 层
 *
 * 协议、线程模型和生命周期见 include/monitor.h。本文件分三层：
 *   请求：一条命令从切分参数、查表、检查参数个数到生成回复；
 *   连接：把字节流拼成行、超长行的丢弃、异步请求期间缓存后续输入；
 *   socket：接受连接、非阻塞收发、按连接状态登记 EPOLLIN / EPOLLOUT。
 * 前两层不碰 fd，单元测试直接驱动它们。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include "vmm.h"
#include "monitor.h"
#include "io/event_loop.h"

/* 错误信息里的上限直接取自宏，改宏时信息跟着变 */
#define XSTR_(x) #x
#define XSTR(x)  XSTR_(x)
#define MSG_LINE_TOO_LONG "line too long (max " XSTR(MON_LINE_MAX) " bytes)"

struct mon_req {
    struct monitor       *mon;
    struct mon_conn      *conn;     /* NULL：连接已断开，回复丢弃 */
    const struct mon_cmd *cmd;
    FILE                 *out;      /* open_memstream，正文 */
    char                 *body;
    size_t                body_len;
    char                  err[256];
    mon_finish_fn         finish;
    void                 *finish_opaque;
    struct mon_req       *next;     /* done 链表 */
};

/* ------------------------------------------------------------------ */
/* 请求                                                                */
/* ------------------------------------------------------------------ */

static struct mon_req *req_new(struct monitor *m, struct mon_conn *c)
{
    struct mon_req *r = calloc(1, sizeof(*r));

    if (!r)
        return NULL;
    r->mon = m;
    r->conn = c;
    r->out = open_memstream(&r->body, &r->body_len);
    if (!r->out) {
        free(r);
        return NULL;
    }
    return r;
}

static void req_free(struct mon_req *r)
{
    if (r->out)
        fclose(r->out);
    free(r->body);
    free(r);
}

FILE *mon_out(struct mon_req *req)        { return req->out; }
void *mon_req_opaque(struct mon_req *req) { return req->cmd ? req->cmd->opaque : NULL; }
int   mon_req_cpu(struct mon_req *req)    { return req->conn ? req->conn->cur_cpu : 0; }
bool  mon_req_orphaned(struct mon_req *req) { return req->conn == NULL; }
void *mon_req_defer_opaque(struct mon_req *req) { return req->finish_opaque; }

void mon_req_set_cpu(struct mon_req *req, int cpu)
{
    if (req->conn)
        req->conn->cur_cpu = cpu;
}

void mon_req_defer(struct mon_req *req, mon_finish_fn finish, void *opaque)
{
    req->finish = finish;
    req->finish_opaque = opaque;
}

int mon_err(struct mon_req *req, const char *fmt, ...)
{
    va_list ap;
    char *p;

    va_start(ap, fmt);
    vsnprintf(req->err, sizeof(req->err), fmt, ap);
    va_end(ap);
    /* 结束标记必须是一行，原因里的换行压成空格 */
    for (p = req->err; *p; p++)
        if (*p == '\n' || *p == '\r')
            *p = ' ';
    return MON_ERR;
}

/* ------------------------------------------------------------------ */
/* 回复缓冲                                                            */
/* ------------------------------------------------------------------ */

static void out_append(struct mon_conn *c, const char *s, size_t n)
{
    if (c->overflow)
        return;
    if (c->out_len - c->out_off + n > MON_OUT_MAX) {
        /* 不再追加：socket 层看到 overflow 就断开这个连接 */
        c->overflow = true;
        return;
    }
    if (c->out_len + n + 1 > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 4096;
        char *p;

        while (cap < c->out_len + n + 1)
            cap *= 2;
        p = realloc(c->out, cap);
        if (!p) {
            c->overflow = true;
            return;
        }
        c->out = p;
        c->out_cap = cap;
    }
    memcpy(c->out + c->out_len, s, n);
    c->out_len += n;
    c->out[c->out_len] = '\0';
}

static void out_str(struct mon_conn *c, const char *s)
{
    out_append(c, s, strlen(s));
}

/* 正文每行加两个空格，再写结束标记 */
static void reply(struct mon_conn *c, struct mon_req *r, int status)
{
    const char *p, *end, *nl;

    if (r->out) {
        fclose(r->out);
        r->out = NULL;
    }
    p = r->body ? r->body : "";
    end = p + (r->body ? r->body_len : 0);
    while (p < end) {
        nl = memchr(p, '\n', (size_t)(end - p));
        out_str(c, "  ");
        out_append(c, p, nl ? (size_t)(nl - p) : (size_t)(end - p));
        out_str(c, "\n");
        p = nl ? nl + 1 : end;
    }
    if (status == MON_OK) {
        out_str(c, "OK\n");
    } else {
        out_str(c, "ERR ");
        out_str(c, r->err[0] ? r->err : "command failed without a reason");
        out_str(c, "\n");
    }
}

/* 不经过命令表的错误（切分阶段就失败了） */
static void reply_err(struct monitor *m, struct mon_conn *c, const char *msg)
{
    struct mon_req *r = req_new(m, c);

    if (!r) {
        out_str(c, "ERR out of memory\n");
        return;
    }
    mon_err(r, "%s", msg);
    reply(c, r, MON_ERR);
    req_free(r);
}

/* ------------------------------------------------------------------ */
/* 命令表                                                              */
/* ------------------------------------------------------------------ */

static const struct mon_cmd *find_cmd(struct monitor *m, enum mon_table t,
                                      const char *name)
{
    int i;

    for (i = 0; i < m->nr_cmds[t]; i++)
        if (!strcmp(m->cmds[t][i].name, name))
            return &m->cmds[t][i];
    return NULL;
}

int monitor_register(struct monitor *m, enum mon_table t,
                     const struct mon_cmd *c)
{
    if (!c->name || !c->name[0] || strpbrk(c->name, " \t") || !c->fn ||
        c->min_args < 0 || c->max_args < c->min_args) {
        vmm_err("monitor: malformed command '%s'", c->name ? c->name : "");
        return VMM_ERR_INVAL;
    }
    if (find_cmd(m, t, c->name)) {
        vmm_err("monitor: command '%s%s' registered twice",
                t == MON_INFO ? "info " : "", c->name);
        return VMM_ERR_INVAL;
    }
    if (m->nr_cmds[t] >= MON_MAX_CMDS) {
        vmm_err("monitor: command table full (%d)", MON_MAX_CMDS);
        return VMM_ERR_INVAL;
    }
    m->cmds[t][m->nr_cmds[t]++] = *c;
    return VMM_OK;
}

static void usage(char *buf, size_t n, enum mon_table t, const struct mon_cmd *c)
{
    snprintf(buf, n, "%s%s%s%s", t == MON_INFO ? "info " : "", c->name,
             c->args[0] ? " " : "", c->args);
}

static int call_cmd(struct mon_req *r, enum mon_table t,
                    const struct mon_cmd *c, int argc, char **argv)
{
    char u[128];
    int nargs = argc - 1;

    if (nargs < c->min_args || nargs > c->max_args) {
        usage(u, sizeof(u), t, c);
        return mon_err(r, "%s%s: %s (usage: %s)", t == MON_INFO ? "info " : "",
                       c->name, nargs < c->min_args ? "missing argument"
                                                    : "too many arguments", u);
    }
    r->cmd = c;
    return c->fn(r, argc, argv);
}

/* 列出一张表；help 列两张，info 只列子命令 */
static void list_cmds(struct monitor *m, FILE *out, bool top, bool info)
{
    char u[128];
    int t, i, w = 0;

    for (t = 0; t < 2; t++) {
        if ((t == MON_TOP && !top) || (t == MON_INFO && !info))
            continue;
        for (i = 0; i < m->nr_cmds[t]; i++) {
            usage(u, sizeof(u), (enum mon_table)t, &m->cmds[t][i]);
            if ((int)strlen(u) > w)
                w = (int)strlen(u);
        }
    }
    for (t = 0; t < 2; t++) {
        if ((t == MON_TOP && !top) || (t == MON_INFO && !info))
            continue;
        for (i = 0; i < m->nr_cmds[t]; i++) {
            usage(u, sizeof(u), (enum mon_table)t, &m->cmds[t][i]);
            fprintf(out, "%-*s  %s\n", w, u, m->cmds[t][i].help);
        }
    }
}

static int cmd_help(struct mon_req *r, int argc, char **argv)
{
    struct monitor *m = r->mon;
    const struct mon_cmd *c;
    enum mon_table t = MON_TOP;
    char u[128];

    if (argc == 1) {
        list_cmds(m, mon_out(r), true, true);
        return MON_OK;
    }
    if (argc == 3 && !strcmp(argv[1], "info")) {
        t = MON_INFO;
        c = find_cmd(m, t, argv[2]);
        if (!c)
            return mon_err(r, "unknown info command '%s' (try 'info')", argv[2]);
    } else {
        if (argc == 3)
            return mon_err(r, "help: too many arguments (usage: help [command])");
        c = find_cmd(m, t, argv[1]);
        if (!c)
            return mon_err(r, "unknown command '%s' (try 'help')", argv[1]);
    }
    usage(u, sizeof(u), t, c);
    fprintf(mon_out(r), "%s  %s\n", u, c->help);
    return MON_OK;
}

static int cmd_info(struct mon_req *r, int argc, char **argv)
{
    const struct mon_cmd *c;

    if (argc == 1) {
        list_cmds(r->mon, mon_out(r), false, true);
        return MON_OK;
    }
    c = find_cmd(r->mon, MON_INFO, argv[1]);
    if (!c)
        return mon_err(r, "unknown info command '%s' (try 'info')", argv[1]);
    return call_cmd(r, MON_INFO, c, argc - 1, argv + 1);
}

/* ------------------------------------------------------------------ */
/* 连接：拼行与执行                                                    */
/* ------------------------------------------------------------------ */

static void socket_update(struct monitor *m, struct mon_conn *c);

static void run_line(struct monitor *m, struct mon_conn *c, char *line, size_t len)
{
    char *argv[MON_MAX_ARGS + 1];
    const struct mon_cmd *cmd;
    struct mon_req *r;
    char *p, *save = NULL;
    int argc = 0, st;

    if (len > MON_LINE_MAX) {
        reply_err(m, c, MSG_LINE_TOO_LONG);
        return;
    }
    if (memchr(line, '\0', len)) {
        reply_err(m, c, "line contains a NUL byte");
        return;
    }
    line[len] = '\0';
    for (p = strtok_r(line, " \t", &save); p; p = strtok_r(NULL, " \t", &save)) {
        if (argc == MON_MAX_ARGS) {
            reply_err(m, c, "too many arguments (max " XSTR(MON_MAX_ARGS) " including the command)");
            return;
        }
        argv[argc++] = p;
    }
    if (argc == 0)
        return;         /* 空行不回复 */
    argv[argc] = NULL;

    r = req_new(m, c);
    if (!r) {
        out_str(c, "ERR out of memory\n");
        return;
    }
    cmd = find_cmd(m, MON_TOP, argv[0]);
    if (!cmd)
        st = mon_err(r, "unknown command '%s' (try 'help')", argv[0]);
    else
        st = call_cmd(r, MON_TOP, cmd, argc, argv);

    if (st == MON_PENDING) {
        if (r->finish) {
            c->pending = r;
            return;
        }
        vmm_err("monitor: '%s' returned MON_PENDING without mon_req_defer",
                argv[0]);
        st = mon_err(r, "internal error: '%s' did not complete", argv[0]);
    }
    reply(c, r, st);
    req_free(r);
}

/* 处理 c->in 里所有完整的行，遇到异步请求就停下 */
static void process_lines(struct monitor *m, struct mon_conn *c)
{
    size_t off = 0;

    while (!c->pending && off < c->in_len) {
        char *start = c->in + off;
        size_t avail = c->in_len - off;
        char *nl = memchr(start, '\n', avail);
        size_t len;

        if (c->discarding) {
            if (!nl) {
                off = c->in_len;
                break;
            }
            c->discarding = false;
            off += (size_t)(nl - start) + 1;
            reply_err(m, c, MSG_LINE_TOO_LONG);
            continue;
        }
        if (!nl) {
            /* 半行：留着等换行。超过上限（给行尾的 \r 留一个字节）就进入
             * 丢弃状态，不再缓存 */
            if (avail > MON_LINE_MAX + 1) {
                c->discarding = true;
                off = c->in_len;
            }
            break;
        }
        len = (size_t)(nl - start);
        off += len + 1;
        if (len && start[len - 1] == '\r')
            len--;
        run_line(m, c, start, len);
    }
    if (off) {
        memmove(c->in, c->in + off, c->in_len - off);
        c->in_len -= off;
    }
}

void mon_conn_init(struct mon_conn *c, int fd)
{
    memset(c, 0, sizeof(*c));
    c->fd = fd;
}

void mon_conn_input(struct monitor *m, struct mon_conn *c,
                    const char *buf, size_t n)
{
    if (c->in_len + n + 1 > c->in_cap) {
        size_t cap = c->in_cap ? c->in_cap : 2 * MON_LINE_MAX;
        char *p;

        while (cap < c->in_len + n + 1)
            cap *= 2;
        p = realloc(c->in, cap);
        if (!p) {
            vmm_err("monitor: out of memory buffering input");
            c->overflow = true;
            return;
        }
        c->in = p;
        c->in_cap = cap;
    }
    memcpy(c->in + c->in_len, buf, n);
    c->in_len += n;
    process_lines(m, c);
}

void mon_conn_release(struct monitor *m, struct mon_conn *c)
{
    (void)m;
    if (c->pending)
        c->pending->conn = NULL;
    free(c->in);
    free(c->out);
    mon_conn_init(c, -1);
}

/* ------------------------------------------------------------------ */
/* 异步完成                                                            */
/* ------------------------------------------------------------------ */

void mon_req_post(struct mon_req *req)
{
    struct monitor *m = req->mon;
    uint64_t one = 1;

    pthread_mutex_lock(&m->done_lock);
    req->next = m->done_head;
    m->done_head = req;
    pthread_mutex_unlock(&m->done_lock);
    /* eventfd 计数只会在溢出 2^64-2 时写失败，不可能发生 */
    if (write(m->wake_fd, &one, sizeof(one)) != sizeof(one))
        vmm_err("monitor: eventfd write failed: %s", strerror(errno));
}

void monitor_run_completions(struct monitor *m)
{
    struct mon_req *list, *r;
    uint64_t cnt;

    if (read(m->wake_fd, &cnt, sizeof(cnt)) < 0 && errno != EAGAIN)
        vmm_warn("monitor: eventfd read failed: %s", strerror(errno));
    pthread_mutex_lock(&m->done_lock);
    list = m->done_head;
    m->done_head = NULL;
    pthread_mutex_unlock(&m->done_lock);

    while ((r = list)) {
        struct mon_conn *c = r->conn;
        int st;

        list = r->next;
        st = r->finish(r);
        if (c) {
            c->pending = NULL;
            reply(c, r, st);
        }
        req_free(r);
        if (c) {
            process_lines(m, c);
            socket_update(m, c);
        }
    }
}

/* ------------------------------------------------------------------ */
/* socket 层                                                           */
/* ------------------------------------------------------------------ */

static struct mon_conn *conn_by_fd(struct monitor *m, int fd)
{
    int i;

    for (i = 0; i < MON_MAX_CLIENTS; i++)
        if (m->conns[i].fd == fd)
            return &m->conns[i];
    return NULL;
}

static void conn_close(struct monitor *m, struct mon_conn *c, const char *why)
{
    vmm_dbg("monitor: client fd %d closed (%s)", c->fd, why);
    evl_del(m->loop, c->fd);
    close(c->fd);
    mon_conn_release(m, c);
}

/* 尽量写出积压的回复；对端已关闭时返回 false */
static bool conn_flush(struct mon_conn *c)
{
    while (c->out_off < c->out_len) {
        ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off,
                         MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return errno == EAGAIN || errno == EWOULDBLOCK;
        }
        c->out_off += (size_t)n;
    }
    c->out_off = c->out_len = 0;
    return true;
}

/* 每次连接状态变化后调用：写回复、关掉该关的连接、重新登记事件。
 * 有异步请求未完成时不登记 EPOLLIN，新的请求留在内核的 socket 缓冲里，
 * VMM 这边的缓存因此有上限（一次 read 的量加一行） */
static void socket_update(struct monitor *m, struct mon_conn *c)
{
    uint32_t want = 0;

    if (c->fd < 0)
        return;
    if (c->overflow) {
        vmm_warn("monitor: client fd %d is not reading its replies "
                 "(over %u bytes queued), disconnecting", c->fd, MON_OUT_MAX);
        conn_close(m, c, "output overflow");
        return;
    }
    if (!conn_flush(c)) {
        conn_close(m, c, "peer gone");
        return;
    }
    if (c->eof && !c->pending && c->out_len == 0) {
        conn_close(m, c, "EOF");
        return;
    }
    if (!c->pending && !c->eof)
        want |= EPOLLIN;
    if (c->out_len > c->out_off)
        want |= EPOLLOUT;
    if (want != c->events && evl_mod(m->loop, c->fd, want) == VMM_OK)
        c->events = want;
}

static void on_conn(void *opaque, int fd, uint32_t events)
{
    struct monitor *m = opaque;
    struct mon_conn *c = conn_by_fd(m, fd);
    char buf[4096];

    if (!c)
        return;
    if ((events & EPOLLIN) && !c->pending && !c->eof) {
        ssize_t n = read(fd, buf, sizeof(buf));

        if (n > 0) {
            mon_conn_input(m, c, buf, (size_t)n);
        } else if (n == 0) {
            c->eof = true;
        } else if (errno != EAGAIN && errno != EINTR) {
            conn_close(m, c, strerror(errno));
            return;
        }
    } else if (events & (EPOLLHUP | EPOLLERR)) {
        /* 没有可读数据还报 HUP/ERR：对端已经完全关闭，回复也送不出去了 */
        conn_close(m, c, "hangup");
        return;
    }
    socket_update(m, c);
}

static void on_accept(void *opaque, int fd, uint32_t events)
{
    static const char full[] = "ERR too many monitor clients (max " XSTR(MON_MAX_CLIENTS) ")\n";
    struct monitor *m = opaque;
    struct mon_conn *c;
    int cfd;

    (void)events;
    cfd = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (cfd < 0) {
        if (errno != EAGAIN && errno != EINTR)
            vmm_warn("monitor: accept failed: %s", strerror(errno));
        return;
    }
    c = conn_by_fd(m, -1);
    if (!c) {
        /* 尽力告诉对方原因；写不进去也无所谓，关闭本身就是答复 */
        if (send(cfd, full, sizeof(full) - 1, MSG_NOSIGNAL | MSG_DONTWAIT) < 0)
            vmm_dbg("monitor: could not tell the rejected client why: %s",
                    strerror(errno));
        close(cfd);
        vmm_warn("monitor: rejected a client, already %d connected",
                 MON_MAX_CLIENTS);
        return;
    }
    if (evl_add(m->loop, cfd, EPOLLIN, on_conn, m) != VMM_OK) {
        close(cfd);
        return;
    }
    mon_conn_init(c, cfd);
    c->events = EPOLLIN;
    vmm_dbg("monitor: client fd %d connected", cfd);
}

static void on_wake(void *opaque, int fd, uint32_t events)
{
    (void)fd; (void)events;
    monitor_run_completions(opaque);
}

int monitor_init(struct monitor *m)
{
    static const struct mon_cmd help = {
        "help", "[command]", "list commands, or show one command's usage",
        0, 2, cmd_help, NULL
    };
    static const struct mon_cmd info = {
        "info", "[subcommand]", "show information about the VM (bare 'info' lists them)",
        0, MON_MAX_ARGS - 1, cmd_info, NULL
    };
    int i;

    memset(m, 0, sizeof(*m));
    m->listen_fd = -1;
    for (i = 0; i < MON_MAX_CLIENTS; i++)
        mon_conn_init(&m->conns[i], -1);
    pthread_mutex_init(&m->done_lock, NULL);
    m->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m->wake_fd < 0) {
        vmm_err("monitor: eventfd: %s", strerror(errno));
        return VMM_ERR_SYS;
    }
    monitor_register(m, MON_TOP, &help);
    monitor_register(m, MON_TOP, &info);
    return VMM_OK;
}

int monitor_listen(struct monitor *m, const char *path, struct event_loop *l)
{
    struct sockaddr_un addr;
    struct stat st;
    mode_t old;
    int fd, r;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        vmm_err("--monitor %s: path too long for a unix socket (max %zu bytes)",
                path, sizeof(addr.sun_path) - 1);
        return VMM_ERR_INVAL;
    }
    memcpy(addr.sun_path, path, strlen(path) + 1);

    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            vmm_err("--monitor %s: exists and is not a socket, refusing to "
                    "replace it", path);
            return VMM_ERR_INVAL;
        }
        /* 上一次没有正常退出留下的 socket 文件，bind 前必须删掉 */
        if (unlink(path) < 0) {
            vmm_err("--monitor %s: cannot remove stale socket: %s", path,
                    strerror(errno));
            return VMM_ERR_SYS;
        }
    }

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        vmm_err("monitor: socket: %s", strerror(errno));
        return VMM_ERR_SYS;
    }
    /* monitor 能让 VMM 退出、读 guest 寄存器，只给当前用户。umask 是进程
     * 级的，这里还在单线程的启动阶段，临时改动不会影响别的线程 */
    old = umask(0077);
    r = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    umask(old);
    if (r < 0 || listen(fd, MON_MAX_CLIENTS) < 0) {
        vmm_err("--monitor %s: %s: %s", path, r < 0 ? "bind" : "listen",
                strerror(errno));
        close(fd);
        if (r == 0)
            unlink(path);
        return VMM_ERR_SYS;
    }
    m->listen_fd = fd;
    m->loop = l;
    m->path = strdup(path);
    if (!m->path ||
        evl_add(l, fd, EPOLLIN, on_accept, m) != VMM_OK ||
        evl_add(l, m->wake_fd, EPOLLIN, on_wake, m) != VMM_OK) {
        vmm_err("monitor: cannot register with the event loop");
        return VMM_ERR_SYS;     /* monitor_close 负责清理 */
    }
    vmm_info("monitor listening on %s", path);
    return VMM_OK;
}

void monitor_close(struct monitor *m)
{
    struct mon_req *r;
    int i;

    for (i = 0; i < MON_MAX_CLIENTS; i++) {
        struct mon_conn *c = &m->conns[i];

        if (c->fd >= 0)
            close(c->fd);
        mon_conn_release(m, c);
    }
    if (m->listen_fd >= 0)
        close(m->listen_fd);
    m->listen_fd = -1;
    if (m->path) {
        unlink(m->path);
        free(m->path);
        m->path = NULL;
    }
    /* I/O 线程已停，已 post 但没处理的请求在这里释放；连接都已释放，
     * finish 看到的都是孤儿 */
    pthread_mutex_lock(&m->done_lock);
    while ((r = m->done_head)) {
        m->done_head = r->next;
        r->finish(r);
        req_free(r);
    }
    pthread_mutex_unlock(&m->done_lock);
    if (m->wake_fd >= 0)
        close(m->wake_fd);
    m->wake_fd = -1;
    pthread_mutex_destroy(&m->done_lock);
}
