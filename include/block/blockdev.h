/* include/block/blockdev.h — 磁盘后端（--drive）与磁盘控制器前端之间的接口
 *
 * 分层：磁盘控制器（AHCI，以后的 NVMe、virtio-blk）是前端，只和 guest 的
 * 寄存器、命令结构、DMA 打交道；后端负责把字节读写到 host 上的镜像文件。
 * 前端不碰后端 fd，后端不认识扇区、命令和寄存器，两者之间只有三个动作：
 *
 *   读   blk_preadv  从字节偏移 offset 读满整个 iovec
 *   写   blk_pwritev 把整个 iovec 写到 offset
 *   刷盘 blk_flush   之前写入的数据落到稳定存储
 *
 * 接口按字节而不是按扇区，因为扇区大小是前端的语义：AHCI 盘是 512 字节
 * 逻辑扇区，NVMe 的 LBA 格式可以是 4096。容量也只给字节数，前端自己换算，
 * 并决定镜像尾部不满一个扇区的字节怎么处理。
 *
 * I/O 是同步的，在调用者的线程上完成（目前是 vCPU 线程，持着设备锁）。
 * 这是先慢路径后快路径里的“慢路径”：一次 guest 命令对应一次阻塞的
 * preadv/pwritev，执行流一眼能看完；异步提交留给 blk-io-uring 里程碑，
 * 到时这里的同步实现就是对照组。
 *
 * 生命周期分两个阶段，和 --netdev 一致（见 net/netdev.h）：
 *   解析阶段  blockdev_parse 检查参数，只分配对象，不打开文件；全部参数
 *            解析完之后，blockdev_link 把每个前端的 drive= 引用解析成对象，
 *            并检查一一对应
 *   启动阶段  blockdev_open 打开镜像、取得容量、加锁，必须在前端 attach
 *            之前：前端要按容量生成给 guest 看的设备信息（比如 IDENTIFY）
 * 对象不释放，理由同 --device：前端注册进总线后一直引用到进程退出。
 *
 * 镜像格式只有 raw：镜像里的第 N 个字节就是磁盘的第 N 个字节，没有元数据，
 * host 侧可以直接用 mke2fs、debugfs 这类工具读写，验收测试正是这样核对
 * guest 写入的内容。
 */
#ifndef BLOCK_BLOCKDEV_H
#define BLOCK_BLOCKDEV_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/uio.h>

struct vmm_device;

/* id 的长度上限，理由同 NETDEV_ID_MAX */
#define BLOCKDEV_ID_MAX   32

/* 一次 blk_preadv/blk_pwritev 最多的 iovec 段数，等于 Linux 的 IOV_MAX
 * （preadv 本身的上限）。guest 的 scatter-gather 表可以更长（AHCI 的 PRDT
 * 最多 65535 项），前端按这个上限分批提交。 */
#define BLK_MAX_IOV       1024

/* 计数在 blk_* 里更新。调用者在设备锁下调用，同一个后端只接一个前端，
 * 所以不会被并发写；读者只在 VM 停下后读，不需要原子 */
struct blockdev_stats {
    uint64_t rd_ops, rd_bytes;
    uint64_t wr_ops, wr_bytes;
    uint64_t flush_ops;
    uint64_t errors;              /* 越界、只读盘上的写、系统调用失败 */
};

struct blockdev {
    char        id[BLOCKDEV_ID_MAX];
    char       *filename;
    bool        read_only;
    const char *arg;              /* 原始参数，报错用；指向 argv，进程内有效 */
    const char *user;             /* 引用它的 --device 参数，blockdev_link 填 */

    int         fd;               /* 未打开时为 -1 */
    uint64_t    size;             /* 镜像字节数，blockdev_open 填 */

    struct blockdev_stats stats;
};

/* --drive 的参数值，如 "id=d0,file=disk.img,format=raw"。失败时已打印原因 */
int  blockdev_parse(const char *arg, struct blockdev **out);

/* 所有参数解析完之后调用一次：把每个设备的 drive= 解析成对象，要求引用的
 * id 存在、每个 --drive 恰好被一个设备引用。失败时已打印原因。 */
int  blockdev_link(struct vmm_device *devs, int nr_devs,
                   struct blockdev *const *drives, int nr_drives);

/* 打开镜像并加锁：可写盘加排他锁，只读盘加共享锁。两个 VMM 同时写同一个
 * 镜像时，各自的文件系统缓存互不知道对方，磁盘内容会被悄悄写坏；加锁
 * 让第二个 VMM 在启动时就报错。失败时已打印原因。 */
int  blockdev_open(struct blockdev *bd);

/* 读写都要求 [offset, offset + iov 总长) 完全落在镜像内，并且传完全部字节。
 * 成功返回 VMM_OK。越界或在只读盘上写返回 VMM_ERR_INVAL（前端应当先按
 * 容量和只读属性检查，这里是兜底）；系统调用失败返回 VMM_ERR_SYS，errno
 * 有效。失败都计入 stats.errors 并打印日志，前端负责把错误报告给 guest。 */
int  blk_preadv(struct blockdev *bd, const struct iovec *iov, int iovcnt,
                uint64_t offset);
int  blk_pwritev(struct blockdev *bd, const struct iovec *iov, int iovcnt,
                 uint64_t offset);

/* fdatasync。只读盘上什么也不做，返回 VMM_OK */
int  blk_flush(struct blockdev *bd);

/* 打印统计并关闭 fd，在所有 vCPU 停下之后调用 */
void blockdev_close(struct blockdev *bd);

#endif /* BLOCK_BLOCKDEV_H */
