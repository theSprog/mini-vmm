# mini-vmm

<!-- README.md -->

一个用 C 写在 Linux/KVM 上的学习型 VMM。目标是亲手实现现代虚拟机涉及的各类机制，从 KVM 执行、Linux 直启、传统设备，到 PCI、全模拟设备、VirtIO、vhost、VFIO、SR-IOV、热插拔、迁移和固件，最终得到一个覆盖方方面面的完整 VMM。路线图和进度见 [`MILESTONES.md`](MILESTONES.md)。

## 当前能力

- 实模式和 64 位长模式的裸 payload 执行
- Linux 以 vmlinux（ELF）或 bzImage 直启，经 initramfs 进入 BusyBox shell
- 中断驱动的 16550A 串口控制台，CMOS RTC、i8042、ACPI PM，驱动探测零超时
- `--smp N` 多核（上限 32），CPUID 拓扑按 vCPU 改写
- 后端 fd 由一个 epoll I/O 线程负责，vCPU 线程只跑 `KVM_RUN`；串口输入经它进入 guest
- `--exit-stats FILE` 在 VM 停下后按 exit 类型、按设备报告 exit 次数，并列出 KVM 自己的计数器
- 三种退出方式：`poweroff -f`（ACPI S5）、panic 后重启（i8042 reset）、`Ctrl-A x`
- guest RAM 可以超过 3 GiB：低段 `[0, 3 GiB)` 加 4 GiB 以上的高段，中间 1 GiB 留给 MMIO

限制：还没有 PCI 和块设备、网络设备。

## 构建与运行

```sh
make                                   # 产物 build/vmm 和测试 payload
tools/build_initramfs.sh               # 生成 build/initramfs.cpio.gz，需要静态 busybox
tools/build_guest_kernel.sh --src ~/linux-6.6   # guest 内核，产物在 guest-kernel/

./build/vmm --mode linux --mem 512M --smp 4 \
    --initrd build/initramfs.cpio.gz guest-kernel/vmlinux
```

guest 内核的配置是 `x86_64_defconfig` 加 `tools/guest-kernel.config`，片段里每一项的理由写在它的注释里。源码树需要是 Linux 6.6.x，且不能有树内构建产物。

默认内核命令行是 `console=ttyS0 earlyprintk=serial,ttyS0,115200 reboot=k panic=-1`，每一项的作用见 `src/main.c` 的 `DEFAULT_LINUX_CMDLINE`。完整选项见 `./build/vmm -h`。

用 `timeout` 包着运行时要加 `--foreground`，否则 VMM 处于后台进程组，guest 输入会被禁用。

## 测试

```sh
make test                              # 默认集合
make test linux-boot                   # 只跑一个 suite
make test-list                         # 列出所有 suite
```

e2e suite 以里程碑 ID 命名，需要 `/dev/kvm`；需要 guest 内核的用例读 `VMLINUX=`（bzImage 用例另读 `BZIMAGE=`），没设时取 `guest-kernel/` 里的产物，那里也没有时 SKIP。测试的组织方式和判据见 [`tests/README.md`](tests/README.md)。

## 观测：`--trace-pio`

guest 每一次落到用户态的 PIO 都可以完整录下来，设备问题不需要靠猜：

```sh
./build/vmm --mode linux --mem 512M --trace-pio /tmp/pio.txt \
    --initrd build/initramfs.cpio.gz $VMLINUX

python3 tools/pio_decode.py /tmp/pio.txt --summary                  # 按端口/设备统计
python3 tools/pio_decode.py /tmp/pio.txt --console | less           # 还原控制台文本，每行带 trace 行号区间
python3 tools/pio_decode.py /tmp/pio.txt --from 25700 --to 25800    # 看某一段
python3 tools/pio_decode.py /tmp/pio.txt --dev unhandled --syms $VMLINUX   # 谁在访问没实现的端口
```

每行格式是 `<秒>.<微秒> <设备名> <端口> R|W<宽度> <值> [rip=...]`，RIP 只对没有设备的端口采集。

## 文档

| 文档 | 内容 |
|---|---|
| [`MILESTONES.md`](MILESTONES.md) | 完整路线图、依赖关系、每个里程碑的验收标准 |
| [`AGENTS.md`](AGENTS.md) | 工作契约：权威资料、推进方式、长期 invariant、完成标准 |
| [`HANDOFF.md`](HANDOFF.md) | 开发环境、guest 内核配置兼容、测试环境的坑 |
| [`blog/`](blog/) | 《从零入门虚拟化》系列，原理篇加实现篇 |
| [`learning-note/`](learning-note/) | 按卷组织的深入笔记，追到内核源码和实测 trace |
| [`tests/README.md`](tests/README.md) | 测试怎么跑、各 suite 的判据 |
