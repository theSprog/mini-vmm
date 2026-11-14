/* include/gdb/rsp.h — GDB Remote Serial Protocol 的协议层
 *
 * 规范：GDB 手册附录 E（Remote Serial Protocol），寄存器描述见附录 G.5.5
 * i386 Features。
 *
 * 这一层只管字节流和命令语义：拆包、校验和、转义、ack，把每条命令翻译成
 * 对 target 的一次调用，再把结果编码成应答。它不认识 KVM，也不碰 socket：
 * 输入由调用方喂进 rsp_feed()，输出经 write 回调送出，目标机的停止事件由
 * 调用方用 rsp_report_stop() 报告。这样同一份协议代码既能接真实 VMM
 * （gdb/gdbstub.c），也能在单元测试里接一个纯内存的假 target 逐字节回放。
 *
 * 线程：一个 session 只能被一个线程驱动，rsp_* 不加锁。target 回调在调用
 * rsp_feed() 的线程里同步执行。
 *
 * 只实现 all-stop 模式：gdb 的每条恢复命令都要等到所有 CPU 停下后才得到
 * 一个应答。non-stop 模式要求每个线程独立停止和报告，而 VMM 停一个 vCPU
 * 时总是把整台 VM 停下（停下的 vCPU 可能持有 guest 内核锁，其余 vCPU
 * 接着跑只会在那把锁上自旋），non-stop 在这里没有对应的语义。
 */
#ifndef GDB_RSP_H
#define GDB_RSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* qSupported 里报给 gdb 的 PacketSize：gdb 发来的一个包的数据部分最多
 * 这么多字节。M 包每字节内存占两个十六进制字符，所以一次最多写一半。 */
#define RSP_PACKET_SIZE   4096

/* 线程号 = CPU 下标 + 1。线程号 0 表示“任意线程”，-1 表示“所有线程”，
 * 所以真实线程从 1 开始编号（附录 E 的 thread-id syntax）。 */
#define RSP_MAX_CPUS      32

/*
 * x86-64 寄存器文件，字段顺序就是 target.xml 里的寄存器编号顺序：
 * org.gnu.gdb.i386.core（0..39）、org.gnu.gdb.i386.sse（40..56）、
 * org.gnu.gdb.i386.segments（57..58）。gpr[] 的顺序是 gdb 的
 * rax rbx rcx rdx rsi rdi rbp rsp r8..r15，和 struct kvm_regs 里
 * rsp 在 rbp 前面的顺序不同，换算见 gdb/x86.h。
 *
 * ftag、fiseg、fioff、foseg、fooff 不在结构里：它们在 g 包里以 'x'
 * 报告为不可用，理由见 rsp.c 的寄存器表。
 */
struct rsp_regs {
    uint64_t gpr[16];
    uint64_t rip;
    uint32_t eflags;
    uint32_t seg[6];        /* cs ss ds es fs gs 的选择子 */
    uint8_t  st[8][10];     /* 80 位扩展精度，按栈位置 ST(0)..ST(7) */
    uint32_t fctrl, fstat, fop;
    uint8_t  xmm[16][16];
    uint32_t mxcsr;
    uint64_t fs_base, gs_base;
};

/* 可写寄存器：gpr、rip、eflags，也就是编号 0..17。其余寄存器的写入
 * 返回错误，理由见 rsp.c 的 handle_P()。 */
#define RSP_REG_RIP       16
#define RSP_REG_EFLAGS    17
#define RSP_NR_WRITABLE   18

enum rsp_bp_type {
    RSP_BP_SW = 0,          /* Z0：内存断点（int3） */
    RSP_BP_HW = 1,          /* Z1：硬件指令断点（DR0..DR3） */
};

enum rsp_action {
    RSP_ACT_NONE = 0,       /* vCont 没提到的线程保持停止 */
    RSP_ACT_CONT,
    RSP_ACT_STEP,
};

enum rsp_stop_kind {
    RSP_STOP_TRAP,          /* T05：单步完成，或连接时的初始停止 */
    RSP_STOP_SWBREAK,       /* T05 swbreak: */
    RSP_STOP_HWBREAK,       /* T05 hwbreak: */
    RSP_STOP_INTERRUPT,     /* T02：gdb 发来 Ctrl-C */
    RSP_STOP_EXITED,        /* W：VM 已经结束 */
};

struct rsp_stop {
    enum rsp_stop_kind kind;
    int  cpu;               /* 停在哪个 CPU 上；EXITED 时不用 */
    int  exit_code;         /* 只有 EXITED 用 */
};

/*
 * target 回调。返回 int 的回调 0 表示成功，负值表示失败（应答 E 包）。
 * 除 interrupt 外，所有回调都只在目标机停止时被调用。
 */
struct rsp_target_ops {
    int    (*get_regs)(void *t, int cpu, struct rsp_regs *r);
    /* 只需要写回 gpr、rip、eflags，其余字段是 get_regs 读到的原值 */
    int    (*set_regs)(void *t, int cpu, const struct rsp_regs *r);
    /* addr 是 cpu 视角的虚拟地址。返回实际读写的字节数，可以少于 len
     * （遇到翻译失败的页就停），0 表示第一个字节就不可访问。 */
    size_t (*read_mem)(void *t, int cpu, uint64_t addr, void *buf, size_t len);
    size_t (*write_mem)(void *t, int cpu, uint64_t addr, const void *buf,
                        size_t len);
    /* cpu 是 gdb 当前选中的线程，软件断点的地址经它的页表翻译 */
    int    (*insert_bp)(void *t, int cpu, enum rsp_bp_type type, uint64_t addr);
    int    (*remove_bp)(void *t, int cpu, enum rsp_bp_type type, uint64_t addr);
    /* act[i] 是 CPU i 的动作，至少有一个不是 NONE。返回后目标机在跑，
     * 停下时调用方用 rsp_report_stop() 报告。 */
    void   (*resume)(void *t, const enum rsp_action *act);
    /* 目标机在跑时 gdb 发来 Ctrl-C：请求停机，停稳后照常报告停止 */
    void   (*interrupt)(void *t);
    /* D：撤掉所有断点并让所有 CPU 继续跑；k：结束 VM */
    void   (*detach)(void *t);
    void   (*kill)(void *t);
};

/* rsp_feed() 的返回值 */
enum rsp_status {
    RSP_CONTINUE = 0,       /* 会话继续 */
    RSP_CLOSED,             /* gdb 发了 D 或 k，或者已报告 W，调用方关闭连接 */
    RSP_IO_ERROR,           /* write 回调失败 */
};

/* 解包状态机的状态 */
enum rsp_rx_state { RSP_RX_IDLE, RSP_RX_DATA, RSP_RX_ESC, RSP_RX_CS1, RSP_RX_CS2 };

struct rsp_session {
    const struct rsp_target_ops *ops;
    void   *target;
    int     nr_cpus;
    /* 返回 0 表示 len 字节全部送出 */
    int   (*write)(void *io, const void *buf, size_t len);
    void   *io;

    enum rsp_rx_state rx;
    char    pkt[RSP_PACKET_SIZE + 1];   /* 已去转义的数据部分，末尾补 '\0' */
    size_t  pkt_len;
    bool    pkt_overflow;
    uint8_t sum;                        /* 收到的数据部分的校验和 */
    uint8_t cs_hi;

    /* 上一个发出去的包，gdb 回 '-' 时原样重发 */
    char    last[2 * RSP_PACKET_SIZE + 8];
    size_t  last_len;

    bool    running;                    /* 恢复之后、报告停止之前 */
    bool    io_err;                     /* 本次调用里 write 回调失败过 */
    bool    closed;
    int     g_cpu;                      /* Hg 选中的线程，g/p/m/Z 用它 */
    struct rsp_stop last_stop;          /* '?' 的应答 */
    bool    gdb_swbreak, gdb_hwbreak;   /* gdb 在 qSupported 里声明支持 */
};

/* 新连接：目标机此时必须已经停下，'?' 报告 cpu 0 上的 T05 */
void rsp_session_init(struct rsp_session *s, const struct rsp_target_ops *ops,
                      void *target, int nr_cpus,
                      int (*write)(void *io, const void *buf, size_t len),
                      void *io);

/* 喂入从 gdb 收到的字节，处理其中所有完整的包 */
enum rsp_status rsp_feed(struct rsp_session *s, const void *buf, size_t len);

/* 把 len 字节数据组成一个完整的包 $<转义后的数据>#<校验和> 写进 out，
 * 返回写入的字节数。out 至少要有 2 * len + 4 字节（每个字节最多转义成
 * 两个，外加 '$'、'#' 和两位校验和）。独立成函数是为了让单元测试直接
 * 检查转义：现有的应答里都没有需要转义的字符，经由协议到不了这段代码。 */
size_t rsp_frame(char *out, const char *data, size_t len);

/* 目标机停稳之后调用：发出停止应答，会话回到“已停止”状态。
 * EXITED 之后会话结束，返回 RSP_CLOSED。 */
enum rsp_status rsp_report_stop(struct rsp_session *s,
                                const struct rsp_stop *st);

#endif /* GDB_RSP_H */
