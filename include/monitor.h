/* include/monitor.h — 运行时控制接口：unix socket 上按行的文本命令
 *
 * 命令名和输出沿用 QEMU HMP 的习惯（info status、info cpus、info registers、
 * cpu、quit、help），用过 QEMU 的人不用重新记。HMP 只给人用：回复没有结束
 * 标记，靠下一个提示符判断输出结束，错误也是普通文本；QEMU 让程序走 JSON 的
 * QMP，这个项目不做 JSON 管理 API（见 AGENTS.md 的范围之外），所以同一套文本
 * 协议要同时让人和测试脚本可靠地使用。为此加两条规则：
 *
 *   请求：一行一条命令，按空格和制表符切分参数，行尾的 \r 被忽略，空行
 *         不产生回复。超过 MON_LINE_MAX 字节的行整行丢弃，到换行为止回复
 *         一次 ERR。
 *   回复：若干正文行，每行以两个空格开头；最后一行是 "OK" 或
 *         "ERR <原因>"，行首没有空格。读到第一行不以空格开头的就是回复
 *         结束，正文里写什么都不会和结束标记混淆。
 *
 * 不做行编辑和提示符：HMP 在 socket 上也做行编辑，每输入一个字符都回显
 * 一串终端控制码，脚本难以解析。人用时由客户端负责行编辑，例如
 * socat READLINE UNIX-CONNECT:<path>。
 *
 * 线程模型
 *   监听 socket、客户端连接、命令处理函数全部运行在 event loop 的 I/O 线程
 *   上，处理器表和连接表只有这一个线程读写，不需要锁。命令要读 vCPU 自己
 *   线程上才能拿到的状态（寄存器）时不能在 I/O 线程上同步等待：vCPU 可能
 *   正卡在某个 exit 的处理里，或者被 gdbstub 暂停着，同步等的话串口、网卡
 *   这些后端也跟着停住。所以命令可以返回 MON_PENDING，把工作交给别的线程，
 *   完成后在任意线程调 mon_req_post()；monitor 经 eventfd 回到 I/O 线程，
 *   再调 finish 回调生成回复。同一个连接在命令完成前不再读新的请求，回复
 *   顺序因此总是和请求顺序一致。
 *
 * 生命周期
 *   monitor_init → monitor_register（命令）→ monitor_listen（evl_start 之前）
 *   → 运行 → evl_stop → monitor_close。monitor_close 在 I/O 线程停下之后
 *   调用，把还没送回 I/O 线程的异步请求逐个调 finish 释放掉，回复丢弃。
 *   异步请求的发起方必须保证 evl_stop 之前它会调 mon_req_post()：vCPU 工作
 *   队列在 vCPU 线程退出时把剩余工作执行完（见 smp.h），vm_run 返回时所有
 *   工作都已 post。
 *
 * 加锁顺序：命令处理函数在 I/O 线程上，需要设备状态时先拿 vm->io_lock，
 * 和 io/event_loop.h 写明的顺序一致。
 */
#ifndef MONITOR_H
#define MONITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>

struct event_loop;

#define MON_MAX_CLIENTS   4
/* 一行命令的上限。命令都很短，1024 足够；限制它是为了让一个不发换行的
 * 客户端不能让 VMM 无限制地缓存 */
#define MON_LINE_MAX      1024
#define MON_MAX_ARGS      16
/* 一个连接积压的待发送回复上限。客户端不读回复时 I/O 线程不会阻塞在它
 * 身上（socket 是非阻塞的），积压超过这个值就断开它。32 个 vCPU 的
 * info registers -a 约 40 KiB，1 MiB 留足余量 */
#define MON_OUT_MAX       (1u << 20)
#define MON_MAX_CMDS      32

/* 命令处理函数的返回值 */
enum {
    MON_OK      = 0,    /* 回复已完成，结束标记为 OK */
    MON_ERR     = 1,    /* 已用 mon_err() 写好原因，结束标记为 ERR */
    MON_PENDING = 2,    /* 已调 mon_req_defer()，稍后由 mon_req_post() 完成 */
};

/* 两张命令表：顶层命令，以及 "info" 下的子命令（HMP 的做法） */
enum mon_table { MON_TOP, MON_INFO };

struct mon_req;
typedef int (*mon_cmd_fn)(struct mon_req *req, int argc, char **argv);
/* 异步请求完成后在 I/O 线程上调用，写正文并返回 MON_OK 或 MON_ERR。
 * 连接已经断开时也会调用（mon_req_orphaned() 为真），用来释放资源，
 * 写出的内容被丢弃 */
typedef int (*mon_finish_fn)(struct mon_req *req);

struct mon_cmd {
    const char *name;       /* 不含空格；info 子命令只写子命令名 */
    const char *args;       /* 用法里的参数部分，例如 "[-a]"；没有参数为 "" */
    const char *help;       /* 一行说明 */
    int         min_args;   /* 不含命令名本身（info 子命令也不含 "info"） */
    int         max_args;
    mon_cmd_fn  fn;
    void       *opaque;     /* 处理函数用 mon_req_opaque() 取回 */
};

/* 一个连接。socket 层和单元测试共用：单元测试不建 socket，直接用
 * mon_conn_input() 喂字节、读 out。字段只在 I/O 线程上访问。 */
struct mon_conn {
    int     fd;             /* socket；单元测试里为 -1 */
    char   *in;             /* 已收到、还没处理的字节 */
    size_t  in_len, in_cap;
    bool    discarding;     /* 当前行超长，丢弃到下一个换行 */
    char   *out;            /* 待发送的回复 */
    size_t  out_len, out_cap, out_off;
    bool    overflow;       /* out 超过 MON_OUT_MAX，socket 层据此断开 */
    struct mon_req *pending;    /* 正在等待完成的异步请求 */
    int     cur_cpu;        /* HMP 的“当前 CPU”，cpu 命令设置，info registers 使用 */
    /* 客户端关闭了写端（read 返回 0）。不马上关连接：已收到的命令照常
     * 执行，回复发完再关，这样 printf 'info status\n' | nc -U 这类一次性
     * 用法也能拿到回复。最后一段没有换行的半行被丢弃 */
    bool    eof;
    uint32_t events;        /* 当前向 event loop 登记的事件 */
};

struct monitor {
    struct mon_cmd   cmds[2][MON_MAX_CMDS];
    int              nr_cmds[2];

    /* socket 层 */
    struct event_loop *loop;
    int              listen_fd;
    char            *path;          /* monitor_close 时删除 */
    struct mon_conn  conns[MON_MAX_CLIENTS];

    /* 异步完成：任意线程 mon_req_post() 把请求挂进 done 链表并写 wake_fd，
     * I/O 线程读 wake_fd 后取走整条链表 */
    int              wake_fd;
    pthread_mutex_t  done_lock;
    struct mon_req  *done_head;
};

int  monitor_init(struct monitor *m);
/* 只能在 monitor_listen 之前调用（单线程阶段）。名字重复或表满返回
 * VMM_ERR_INVAL */
int  monitor_register(struct monitor *m, enum mon_table t,
                      const struct mon_cmd *c);

/* 在 path 上建 unix socket 并注册到 l。path 已存在且是 socket 时删掉重建
 * （上一次没有正常退出留下的）；是别的文件时报错，不删用户的文件。
 * socket 文件只对当前用户可读写。evl_start 之前调用。 */
int  monitor_listen(struct monitor *m, const char *path, struct event_loop *l);
/* evl_stop 之后调用：关闭所有连接和监听 socket，删除 socket 文件，释放
 * 未完成的异步请求 */
void monitor_close(struct monitor *m);

/* ---- 命令处理函数使用 ---- */

FILE *mon_out(struct mon_req *req);                 /* 正文输出 */
int   mon_err(struct mon_req *req, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));          /* 返回 MON_ERR */
void *mon_req_opaque(struct mon_req *req);
int   mon_req_cpu(struct mon_req *req);
void  mon_req_set_cpu(struct mon_req *req, int cpu);
/* 声明这个请求要异步完成：处理函数调用后返回 MON_PENDING。opaque 用
 * mon_req_defer_opaque() 取回 */
void  mon_req_defer(struct mon_req *req, mon_finish_fn finish, void *opaque);
void *mon_req_defer_opaque(struct mon_req *req);
bool  mon_req_orphaned(struct mon_req *req);
/* 任意线程调用，每个异步请求恰好一次 */
void  mon_req_post(struct mon_req *req);

/* ---- 连接层，socket 层和单元测试使用 ---- */

void mon_conn_init(struct mon_conn *c, int fd);
/* 处理收到的字节：拼行、执行命令、把回复追加到 c->out。连接上有异步
 * 请求未完成时只缓存，不执行 */
void mon_conn_input(struct monitor *m, struct mon_conn *c,
                    const char *buf, size_t n);
/* 释放连接的缓冲区；有未完成的异步请求时把它标记为孤儿 */
void mon_conn_release(struct monitor *m, struct mon_conn *c);
/* I/O 线程上处理所有已 post 的异步请求（socket 层由 wake_fd 触发） */
void monitor_run_completions(struct monitor *m);

#endif /* MONITOR_H */
