#!/usr/bin/env bash
# tests/step1.sh — Step 1 验收
#
#   Step 1.1  实模式 16 位汇编执行，KVM_EXIT_IO / KVM_EXIT_HLT
#   Step 1.2  4 级恒等映射页表 + GDT，64 位长模式写内存
#
# 用法：
#   ./tests/step1.sh          跑全部自动用例，再打印人工验收指引
#   ./tests/step1.sh -a       只跑自动用例
#   ./tests/step1.sh -m       只打印人工验收指引

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

P16="$ROOT/build/payload16.bin"
P64="$ROOT/build/payload64.bin"

# Step 1.2 payload 写入的两个 8 字节魔数，小端排布后的样子。
# 直接比对整行 hexdump，比只查一个 0x11 之类的片段可靠得多。
EXPECT_BYTES="88 77 66 55 44 33 22 11  be ba fe ca ef be ad de"

# ------------------------------------------------------------------
auto_tests() {

head1 "Step 1.1 — 16 位实模式"

# payload: mov al,0x42; out 0x10,al; in al,0x10; out 0x10,al; hlt
#
# 两次 out 分别验证两个方向：
#   0x42 = guest 主动写出的立即数        -> OUT 通路
#   0x5a = VMM 在 IN 时填的固定图案回显  -> IN  通路
# 只验第一个的话，IN 方向写错（比如没按 data_offset 定位缓冲区）
# 也一样能过，所以这里两个都查。
run_case "real16: OUT/IN + HLT" \
    "$VMM" --mem 2M "$P16"
expect_rc 0
expect_has "16-bit real mode, CS:IP = 0000:0000"
expect_has "guest wrote port 0x10: 0x42"
expect_has "guest wrote port 0x10: 0x5a"
expect_has "HLT, stopping"
expect_has "left guest (ok)"
# 实模式跑通说明 SVM 不需要 unrestricted guest 那套兜底
expect_not "SHUTDOWN"
expect_not "INTERNAL_ERROR"
end_case

# 2MiB 是 Step 1.1 spec 里写的最小内存。能跑说明我们没有偷偷
# 依赖低端内存里的其它东西（BIOS/IVT/页表都不需要）。
run_case "real16: 入口地址显式指定" \
    "$VMM" --mem 2M --load 0 --entry 0 "$P16"
expect_rc 0
expect_has "guest wrote port 0x10: 0x42"
end_case

head1 "Step 1.2 — 64 位长模式（2MiB 大页）"

# 0x64 = 'd'，payload 里读回校验通过才会输出这个值；
# 校验失败会输出 0x21 = '!'。
run_case "long64/2m: 恒等映射 + 写 GPA 0x500000" \
    "$VMM" --mode long64 --dump 500000:32 "$P64"
expect_rc 0
expect_has "64-bit long mode, CR3=0x2000"
expect_has "identity map 1024 MiB via 512 2MiB pages"
expect_has "GDT @ GPA 0x1000: 4 entries (CS=0x08 DS=0x10)"
expect_has "guest wrote port 0x10: 0x64"
expect_not "guest wrote port 0x10: 0x21"   # payload 内部读回校验失败
expect_has "$EXPECT_BYTES"
expect_not "SHUTDOWN"
expect_not "INTERNAL_ERROR"
end_case

head1 "Step 1.2 — 64 位长模式（完整四级，4KiB 粒度）"

# 同一个 payload 换一套页表跑通，才能说明"页表建对了"而不是
# "碰巧 KVM 帮我们兜住了"。
run_case "long64/4k: 512 张 PT + 页表遍历" \
    "$VMM" --mode long64 --gran 4k --mem 16M --walk 500000 \
           --dump 500000:32 "$P64"
expect_rc 0
expect_has "PT[512]=0x5000..0x205000"
expect_has "identity map 1024 MiB, 4KiB granularity"
# 四级遍历每一级都要落在算得出来的位置上：
#   PDE  索引 = 0x500000 >> 21       = 2   -> PT @ 0x5000 + 2*0x1000 = 0x7000
#   PTE  索引 = (0x500000 >> 12) & 0x1ff = 256
expect_has "PML4E [  0] @ 0x2000"
expect_has "PDPE  [  0] @ 0x3000"
expect_has "PDE   [  2] @ 0x4000"
expect_has "PTE   [256] @ 0x7000"
expect_has "=> GPA 0x500000"
expect_has "guest wrote port 0x10: 0x64"
expect_has "$EXPECT_BYTES"
end_case

head1 "错误路径"

# 这几条验的是"报错要有用"。Step 1 阶段最费时间的不是写对，
# 是写错之后看不懂为什么错，所以诊断信息本身也要测。

# payload 载入地址 0x300000，1MiB 内存放不下
run_case "内存不足时给出可操作的提示" \
    "$VMM" --mode long64 --mem 1M "$P64"
expect_rc 1
expect_has "exceeds guest RAM"
expect_has "--mem"
end_case

# 0x2000 正是 PML4 的位置，页表会把 payload 覆盖掉
run_case "payload 与页表区重叠时提前拦截" \
    "$VMM" --mode long64 --load 2000 "$P64"
expect_rc 1
expect_has "overlaps GDT/page table area"
expect_has "use --load to move the payload above"
# 关键：必须在进 guest 之前就拦住，而不是等 guest 崩了才报
expect_not "entering guest"
end_case

run_case "未知选项" \
    "$VMM" --nonsense "$P16"
expect_rc 1
expect_has "unknown option"
end_case

}

# ------------------------------------------------------------------
manual_guide() {

head1 "人工验收（需要额外权限或人眼判断，不进自动断言）"

guide "确认 host 侧真的走了 SVM 而不是软件模拟" \
"lsmod | grep kvm
sudo dmesg | grep -i 'SVM\|kvm'" \
"应看到 kvm_amd 已加载；dmesg 里有 SVM 相关初始化信息。
如果看到的是 kvm_intel，说明机器认错了，本项目的 arch 后端会拒绝启动。"

guide "用 KVM 的 tracepoint 核对 exit 次数" \
"sudo perf stat -e 'kvm:kvm_exit,kvm:kvm_entry' -- ./build/vmm --mem 2M build/payload16.bin" \
"kvm:kvm_entry 与 kvm:kvm_exit 计数应相等。
exit 总数会明显大于 VMM 打印的 exits=N —— 因为绝大多数 exit
（比如 NPT violation 造成的缺页）被 kvm_amd 在内核态就地处理掉了，
根本没返回用户态。这个差值本身就是 Step 4 做 ioeventfd 优化的动机。"

guide "看清楚 VMM 到底发了哪些 ioctl" \
"strace -f -e trace=ioctl,openat,mmap ./build/vmm --mem 2M build/payload16.bin 2>&1 | grep -E 'kvm|KVM'" \
"应能看到完整的三层 fd 序列：
    openat(\"/dev/kvm\")            -> 系统级 fd
    ioctl(3, KVM_CREATE_VM)        -> VM 级 fd
    ioctl(4, KVM_CREATE_VCPU)      -> vCPU 级 fd
    mmap(..., 5, 0)                -> kvm_run 共享页
    ioctl(5, KVM_RUN) 若干次
注意 KVM_RUN 之间没有任何"取数据"的 ioctl —— exit 信息全部通过
mmap 出来的那一页共享内存传递，这是 KVM 用户态接口的核心设计。"

guide "确认 SVM 特性位与 /proc/cpuinfo 一致" \
"# 不要手写 grep 模式去对——那等于维护第二份命名表，早晚写错。
# 直接把我们输出的那一行剪下来，拿它当模式去 /proc/cpuinfo 里查。
./build/vmm --mem 2M build/payload16.bin 2>&1 | sed -n 's/.*SVM features: //p' | tr ' ' '\\n' | sort -u > /tmp/vmm-svm.txt

grep -m1 '^flags' /proc/cpuinfo | tr ' ' '\\n' | grep -x -F -f /tmp/vmm-svm.txt | sort -u > /tmp/kernel-svm.txt

diff /tmp/vmm-svm.txt /tmp/kernel-svm.txt && echo '一致'" \
"应输出'一致'（diff 无差异）。

我们是自己跑 CPUID Fn8000_000A 读 EDX 的位，内核是另一条独立路径读的
同一份硬件寄存器，所以两边必须完全吻合。

diff 里出现 '<' 开头的行 = 我们报了但内核没报，说明位定义抄错了，
或者名字没跟 arch/x86/include/asm/cpufeatures.h 的 word 15 对齐
（典型例子：bit 5 内核叫 vmcb_clean 而不是 vmcbclean）。

反过来不会出现 '>' 行 —— 这条命令是用我们的列表去筛内核的，
内核多报的特性（我们表里没列的位）不会进入比对。"

guide "故意制造 triple fault，确认诊断路径可用" \
"# 把入口指到一片全 0 的内存：0x00 0x00 解码成 add %al,(%rax)，
# rax=0 时会往 GVA 0 写；0 有恒等映射所以不会立刻崩，
# 会一直执行 add 直到撞上没映射的地址。
./build/vmm --mode long64 --entry 900000 build/payload64.bin" \
"预期看到 SHUTDOWN 或 INTERNAL_ERROR，并且后面跟着完整的
vCPU 状态转储（rip/cr0/cr3/efer/段寄存器）。
这条不做自动断言是因为具体停在哪条指令依赖内存内容，
但"出错时能拿到足够的现场"这一点必须人工确认过一次。"

}

# ------------------------------------------------------------------
main() {
    local mode="${1:-all}"

    case "$mode" in
    -m|--manual) manual_guide; return 0 ;;
    -a|--auto)   ;;
    all|"")      ;;
    *) echo "用法: $0 [-a 只跑自动 | -m 只看人工指引]"; return 2 ;;
    esac

    need_kvm
    need_build
    auto_tests

    local rc=0
    summary || rc=1

    if [ "$mode" != "-a" ] && [ "$mode" != "--auto" ]; then
        manual_guide
    fi
    return $rc
}

main "${1:-all}"