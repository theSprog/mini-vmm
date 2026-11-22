/* include/gdb/gdbstub.h — gdb 远程调试：KVM 后端、连接线程、整机暂停
 *
 * 结构：
 *   gdbstub 线程   poll 监听 socket / 连接 / 一个 eventfd，把 gdb 的字节喂给
 *                  协议层（gdb/rsp.h），实现协议层要的 target 回调
 *   vCPU 线程      在两个地方进入 gdbstub：KVM_RUN 被打断时（被要求暂停就
 *                  停在条件变量上），以及 KVM_EXIT_DEBUG（断点、单步）
 *
 * 暂停的 invariant：
 *   - gdbstub 只在所有 vCPU 都已暂停（或已在退出）时访问 vCPU 状态和 guest
 *     内存、改断点、发 vCPU ioctl。暂停中的 vCPU 线程不在 KVM_RUN 里，
 *     KVM 的 vcpu->mutex 是空闲的，所以 gdbstub 线程可以直接对它的 fd
 *     发 KVM_GET_REGS / KVM_SET_GUEST_DEBUG 等 ioctl。
 *   - vCPU 只在两种时刻暂停：KVM_RUN 因 immediate_exit 返回 EINTR 之后，
 *     或 KVM_EXIT_DEBUG 之后。不在处理完 KVM_EXIT_IO / MMIO 后直接暂停：
 *     KVM 要到下一次 KVM_RUN 才完成那条 IO 指令（写回 rax、推进 rip，
 *     见 Documentation/virt/kvm/api.rst 对 KVM_EXIT_IO 的说明），此时读出的
 *     寄存器是半截状态。带着 immediate_exit 再进一次 KVM_RUN，KVM 会先完成
 *     挂起的操作再返回 EINTR。
 *   - 要求暂停时先在锁里清掉 run[i] 并置 immediate_exit，再发 SIGUSR1，
 *     和 vm_request_stop() 同一个道理：没进 KVM_RUN 的被 immediate_exit
 *     挡住，已经在里面的被信号踢出来。immediate_exit 只在 vCPU 被放行时
 *     由它自己清零。
 *   - 一次停止只报告一个事件。另一个 vCPU 同时撞上断点时它的事件被丢弃：
 *     #BP 退出时 rip 还指着 int3，硬件指令断点是 fault 类，恢复后都会再撞
 *     一次，事件不会丢；同时完成的单步本来就会被 gdb 的下一条命令取代。
 */
#ifndef GDB_GDBSTUB_H
#define GDB_GDBSTUB_H

#include <stdint.h>

struct vmm_vm;
struct vmm_vcpu;
struct gdbstub;

/* 在 127.0.0.1:port 上监听（port 0 由内核分配），日志里打出实际端口。
 * 只监听回环地址：gdbstub 能任意读写 guest 内存和寄存器。
 * 返回后所有 vCPU 都处于“不许运行”状态，第一次进 KVM_RUN 就会暂停，
 * 等 gdb 连上并发出恢复命令。必须在 vm_run() 之前调用。 */
int  gdbstub_init(struct vmm_vm *vm, uint16_t port);
/* vm_run() 返回后、vm_destroy() 之前调用：结束线程，关闭所有 fd */
void gdbstub_destroy(struct vmm_vm *vm);

/* vCPU 线程：KVM_RUN 返回 EINTR 后调用，被要求暂停时在这里阻塞 */
void gdbstub_vcpu_interrupted(struct vmm_vcpu *v);
/* vCPU 线程：KVM_EXIT_DEBUG 的处理，返回 VMM_OK 或错误码 */
int  gdbstub_vcpu_debug_exit(struct vmm_vcpu *v);
/* vm_request_stop() 调用：VM 要结束了，唤醒暂停中的 vCPU 和 gdbstub 线程 */
void gdbstub_vm_stopping(struct gdbstub *g);

/* 整机复位（reset.h）时 gdb 连接、断点表、每个 vCPU 的放行状态都保留，
 * vm_reset 在 vCPU 线程全部 join 之后调用这两个函数：
 *   before  在重新装载镜像之前，把软件断点的原字节写回 guest 内存；
 *   after   装载之后重新读出原字节、写入 int3，再唤醒 gdbstub 线程处理
 *           复位期间推迟的停止事件。
 * 两步不能合成一步：装载不一定覆盖断点所在的页（例如模块的代码），没被
 * 覆盖的地方还是 int3，只看装载后的内存分不清它是我们写的还是镜像本来
 * 的字节。
 * 软件断点能跨越复位命中的前提是断点字节由 VMM 装载：vmlinux（ELF）的
 * 各段直接装到物理地址上，满足；bzImage 的内核由 guest 里的解压器在运行时
 * 写出，int3 会被解压结果覆盖，这时用 hbreak（硬件断点比较线性地址，不在
 * 内存里）。 */
void gdbstub_reset_before(struct gdbstub *g);
void gdbstub_reset_after(struct gdbstub *g);

#endif /* GDB_GDBSTUB_H */
