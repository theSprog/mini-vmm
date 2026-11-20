/* src/config.c — 命令行解析：把 argv 变成一份检查过的 struct vmm_config
 *
 * 这里只做两件事：认出每个选项、检查选项之间的组合。打开文件、创建 VM
 * 都不在这里，所以解析失败时还没有任何资源需要释放。唯一的例外是
 * Linux 模式下检查内核和 initramfs 可读：路径写错要在创建 VM 之前报出来。 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "vmm.h"
#include "config.h"
#include "boot/gdt_pgtable.h"
#include "boot/zeropage.h"
#include "devices/device.h"
#include "net/netdev.h"
#include "block/blockdev.h"

/* 长模式 payload 的默认载入地址。
 * 必须避开 [0x1000, 0x205000)（GDT + 四级页表最坏情况占用），
 * 也要避开长模式 payload 的写入目标 0x500000。3MiB 处两边都不挨着。 */
#define PAYLOAD_LONG_GPA 0x300000ULL

/* Linux 模式默认内核参数。
 *   earlyprintk  在 8250 驱动加载之前就能看到输出，内核早期崩溃时救命
 *   reboot=k     reboot 走 i8042 reset，VMM 据此整机复位（--no-reboot 时退出）
 *   panic=-1     panic 后立刻 reboot，而不是挂住；配合 --no-reboot，panic 让
 *                VMM 退出，测试据此拿到失败现场，而不是无限重启
 *   noapictimer? 不加：内核态 LAPIC 的 timer 完全可用 */
#define DEFAULT_LINUX_CMDLINE \
    "console=ttyS0 earlyprintk=serial,ttyS0,115200 reboot=k panic=-1"

void vmm_config_default(struct vmm_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode         = VMM_BOOT_LINUX;
    cfg->ram_size     = VMM_DEFAULT_RAM_SIZE;
    cfg->nr_vcpus     = 1;
    cfg->payload_gpa  = UINT64_MAX;   /* UINT64_MAX = 用模式默认值 */
    cfg->entry_gpa    = UINT64_MAX;
    cfg->log_level    = 2;
    cfg->dump_on_exit = false;
    cfg->dump_gpa     = 0x500000;
    cfg->dump_len     = 32;
    cfg->gdb_port     = -1;
    cfg->pit_reinject = true;
    cfg->pt_gran      = BOOT_PG_2M;
    cfg->map_size     = BOOT_IDENTITY_MAP_SIZE;
    cfg->walk_gva     = UINT64_MAX;
}

void vmm_usage(const char *prog)
{
    fprintf(stderr,
"Usage: %s [options] <payload.bin | vmlinux | bzImage>\n"
"\n"
"  --mode real16|long64|linux  Boot mode (default: linux)\n"
"                           real16 = 16-bit real mode payload, entry GPA 0x0\n"
"                           long64 = 64-bit long mode payload, entry GPA 0x%llx\n"
"                           linux  = boot Linux: vmlinux (ELF) or bzImage,\n"
"                                    detected from the file header\n"
"  --initrd FILE          linux: initramfs (cpio or cpio.gz), required\n"
"  --cmdline STR          linux: replace the default kernel command line\n"
"  --append STR           linux: append to the kernel command line\n"
"  --mem SIZE             Guest RAM size, K/M/G suffix allowed (default: 128M)\n"
"  --smp N                Number of vCPUs, 1..%d (default: 1, N > 1 needs linux)\n"
"  --load GPA             Payload load address in hex (overrides mode default)\n"
"  --entry GPA            First instruction address in hex (default: --load)\n"
"  --gran 2m|4k           long64 page table granularity (default: 2m)\n"
"                           2m = PML4/PDPT/PD only, PS bit set in the PD\n"
"                           4k = full 4 levels, 1GiB needs 512 page tables\n"
"  --map SIZE             Identity mapped range (default: 1G)\n"
"  --dump GPA:LEN         Hexdump guest memory on exit (hex GPA : decimal len)\n"
"  --trace-pio FILE       Log every PIO access that exits to user space,\n"
"                           one line per access; rip= is appended for ports\n"
"                           that have no device behind them\n"
"  --trace-mmio FILE      Same for MMIO accesses that exit to user space\n"
"                           (PCI BARs, unclaimed addresses); decode both\n"
"                           with tools/trace_decode.py\n"
"  --exit-stats FILE      When the guest stops, write VM exit counts to FILE\n"
"                           ('-' = stderr): user-space exits per reason and\n"
"                           per device, plus KVM's own counters\n"
"  --gdb PORT             Serve the GDB remote protocol on 127.0.0.1:PORT\n"
"                           (0 = any free port, printed in the log); the guest\n"
"                           stays paused until gdb connects\n"
"  --monitor PATH         Accept HMP-style text commands on a unix socket at\n"
"                           PATH (info status/cpus/registers/exit-stats/\n"
"                           irqchip, cpu, quit; 'help' lists them). Replies\n"
"                           end with a line\n"
"                           'OK' or 'ERR <reason>'; body lines start with two\n"
"                           spaces. Try: socat READLINE UNIX-CONNECT:PATH\n"
"  --kernel-irqchip on|split\n"
"                         linux: where the interrupt controllers live\n"
"                           (default: on). on = PIC, IOAPIC, PIT and LAPIC\n"
"                           all in KVM; split = only the LAPIC in KVM, the\n"
"                           8259A pair, IOAPIC and 8254 PIT in this process\n"
"  --pit-reinject on|off  linux: PIT ticks the guest has not acknowledged yet\n"
"                           are queued and delivered one per acknowledgement\n"
"                           (on, default) or merged into one interrupt (off);\n"
"                           same policy in both irqchip modes\n"
"  --no-reboot            linux: exit when the guest requests a reset\n"
"                           (reboot, panic with panic=-1) instead of\n"
"                           resetting the machine and booting again\n"
"  --device DRIVER[,PROP=VALUE...]\n"
"                         linux: add a device, repeatable; PCI devices take\n"
"                           slots 1, 2, ... in command line order.\n"
"                           Syntax follows QEMU -device: ',,' is a literal\n"
"                           comma, a property may appear only once.\n"
"                           --device help lists the drivers\n"
"  --netdev TYPE,id=ID[,PROP=VALUE...]\n"
"                         linux: add a network backend for the NIC whose\n"
"                           netdev= names ID; each backend serves exactly\n"
"                           one NIC. --netdev help lists the types\n"
"  --drive id=ID,file=PATH[,format=raw][,readonly=on]\n"
"                         linux: add a raw disk image for the disk controller\n"
"                           whose drive= names ID; each drive serves exactly\n"
"                           one controller. The image is locked while the VM\n"
"                           runs\n"
"  --walk GVA             Print the 4-level page walk for GVA before starting\n"
"  -v                     Raise log level by one: the default is INFO, one -v\n"
"                           reaches DEBUG (the highest). -vv equals -v -v\n"
"  -q                     Errors only\n"
"  -h                     Show this help\n"
"\n"
"Exit status:\n"
"  0  the guest powered off or exited normally, or requested a reset under\n"
"     --no-reboot; also monitor quit and Ctrl-A x\n"
"  1  the VMM failed (bad option, KVM error, guest fault it cannot handle)\n"
"  3  the guest reported a panic through --device pvpanic\n"
"  payloads may write any other code to the debug exit port 0xF4\n"
"\n"
"Examples:\n"
"  # real mode payload\n"
"  %s --mem 2M build/payload16.bin\n"
"  # long mode payload, dump the bytes it wrote\n"
"  %s --mode long64 --dump 500000:16 -v build/payload64.bin\n"
"  # serial payload\n"
"  %s --mode long64 build/serial64.bin\n"
"  # Linux direct boot\n"
"  %s --mode linux --mem 512M --initrd initramfs.cpio.gz vmlinux\n"
"  %s --mode linux --mem 512M --initrd initramfs.cpio.gz bzImage\n"
"  # SMP\n"
"  %s --mode linux --smp 4 --mem 512M --initrd initramfs.cpio.gz vmlinux\n"
"  # which device caused the VM exits\n"
"  %s --mode long64 --exit-stats - build/serial64.bin\n"
"  # a PCI device\n"
"  %s --mode linux --mem 512M --device pci-test --initrd initramfs.cpio.gz vmlinux\n",
            prog, (unsigned long long)PAYLOAD_LONG_GPA, VMM_MAX_VCPUS,
            prog, prog, prog, prog, prog, prog, prog, prog);
}

static int parse_size(const char *s, uint64_t *out)
{
    char *end;
    unsigned long long v = strtoull(s, &end, 0);

    if (end == s)
        return VMM_ERR_INVAL;
    switch (*end) {
    case 'g': case 'G': v <<= 30; end++; break;
    case 'm': case 'M': v <<= 20; end++; break;
    case 'k': case 'K': v <<= 10; end++; break;
    case '\0': break;
    default: return VMM_ERR_INVAL;
    }
    if (*end)
        return VMM_ERR_INVAL;
    *out = v;
    return VMM_OK;
}

static int parse_hex(const char *s, uint64_t *out)
{
    char *end;
    unsigned long long v = strtoull(s, &end, 16);

    if (end == s || *end)
        return VMM_ERR_INVAL;
    *out = v;
    return VMM_OK;
}

/* 解析过程中要记住、但不属于 struct vmm_config 的东西 */
struct parse_state {
    struct vmm_config *cfg;
    const char *prog;            /* argv[0]，打印用法时用 */
    /* --append 只在拼内核命令行时用一次，拼好的结果写回 cfg->cmdline */
    const char *append;
    /* 这两个选项的默认值也是合法的取值，只看 cfg 分不出用户写没写 */
    bool irqchip_given;
    bool reinject_given;
};

/* ---- 每个选项一个处理函数：arg 是选项的参数（不带参数的选项为 NULL），
 * 返回 VMM_OK 或 VMM_ERR_INVAL ---- */

static int opt_help(struct parse_state *st, char *arg)
{
    vmm_usage(st->prog);
    exit(0);
}

static int opt_quiet(struct parse_state *st, char *arg)
{
    st->cfg->log_level = 0;
    return VMM_OK;
}

static int opt_mode(struct parse_state *st, char *arg)
{
    if (!strcmp(arg, "real16"))
        st->cfg->mode = VMM_BOOT_REAL16;
    else if (!strcmp(arg, "long64"))
        st->cfg->mode = VMM_BOOT_LONG64;
    else if (!strcmp(arg, "linux"))
        st->cfg->mode = VMM_BOOT_LINUX;
    else {
        vmm_err("unknown mode: %s", arg);
        return VMM_ERR_INVAL;
    }
    return VMM_OK;
}

static int opt_mem(struct parse_state *st, char *arg)
{
    if (parse_size(arg, &st->cfg->ram_size) != VMM_OK) {
        vmm_err("cannot parse --mem: %s", arg);
        return VMM_ERR_INVAL;
    }
    return VMM_OK;
}

static int opt_smp(struct parse_state *st, char *arg)
{
    char *end;
    long n = strtol(arg, &end, 10);

    if (end == arg || *end || n < 1 || n > VMM_MAX_VCPUS) {
        vmm_err("--smp must be an integer in 1..%d: %s", VMM_MAX_VCPUS, arg);
        return VMM_ERR_INVAL;
    }
    st->cfg->nr_vcpus = (int)n;
    return VMM_OK;
}

static int opt_initrd(struct parse_state *st, char *arg)
{
    st->cfg->initrd_path = arg;
    return VMM_OK;
}

static int opt_cmdline(struct parse_state *st, char *arg)
{
    st->cfg->cmdline = arg;
    return VMM_OK;
}

static int opt_append(struct parse_state *st, char *arg)
{
    st->append = arg;
    return VMM_OK;
}

/* 十六进制地址类选项的共同写法：解析失败时报出选项名和原值 */
static int parse_hex_opt(const char *opt, const char *arg, uint64_t *out)
{
    if (parse_hex(arg, out) != VMM_OK) {
        vmm_err("cannot parse %s: %s", opt, arg);
        return VMM_ERR_INVAL;
    }
    return VMM_OK;
}

static int opt_map(struct parse_state *st, char *arg)
{
    if (parse_size(arg, &st->cfg->map_size) != VMM_OK) {
        vmm_err("cannot parse --map: %s", arg);
        return VMM_ERR_INVAL;
    }
    return VMM_OK;
}

static int opt_load(struct parse_state *st, char *arg)
{
    return parse_hex_opt("--load", arg, &st->cfg->payload_gpa);
}

static int opt_entry(struct parse_state *st, char *arg)
{
    return parse_hex_opt("--entry", arg, &st->cfg->entry_gpa);
}

static int opt_gran(struct parse_state *st, char *arg)
{
    if (!strcmp(arg, "2m"))
        st->cfg->pt_gran = BOOT_PG_2M;
    else if (!strcmp(arg, "4k"))
        st->cfg->pt_gran = BOOT_PG_4K;
    else {
        vmm_err("--gran must be 2m or 4k: %s", arg);
        return VMM_ERR_INVAL;
    }
    return VMM_OK;
}

static int opt_trace_pio(struct parse_state *st, char *arg)
{
    st->cfg->trace_pio_path = arg;
    return VMM_OK;
}

static int opt_trace_mmio(struct parse_state *st, char *arg)
{
    st->cfg->trace_mmio_path = arg;
    return VMM_OK;
}

static int opt_gdb(struct parse_state *st, char *arg)
{
    char *end;
    long n = strtol(arg, &end, 10);

    if (end == arg || *end || n < 0 || n > 65535) {
        vmm_err("--gdb must be a TCP port in 0..65535: %s", arg);
        return VMM_ERR_INVAL;
    }
    st->cfg->gdb_port = (int)n;
    return VMM_OK;
}

static int opt_monitor(struct parse_state *st, char *arg)
{
    st->cfg->monitor_path = arg;
    return VMM_OK;
}

static int opt_no_reboot(struct parse_state *st, char *arg)
{
    st->cfg->no_reboot = true;
    return VMM_OK;
}

static int opt_kernel_irqchip(struct parse_state *st, char *arg)
{
    if (!strcmp(arg, "on")) {
        st->cfg->irqchip = VMM_IRQCHIP_ON;
    } else if (!strcmp(arg, "split")) {
        st->cfg->irqchip = VMM_IRQCHIP_SPLIT;
    } else if (!strcmp(arg, "off")) {
        vmm_err("--kernel-irqchip off (LAPIC in user space) is not "
                "implemented yet; it belongs to the user-lapic "
                "milestone");
        return VMM_ERR_INVAL;
    } else {
        vmm_err("--kernel-irqchip must be on or split: %s", arg);
        return VMM_ERR_INVAL;
    }
    st->irqchip_given = true;
    return VMM_OK;
}

static int opt_pit_reinject(struct parse_state *st, char *arg)
{
    if (!strcmp(arg, "on")) {
        st->cfg->pit_reinject = true;
    } else if (!strcmp(arg, "off")) {
        st->cfg->pit_reinject = false;
    } else {
        vmm_err("--pit-reinject must be on or off: %s", arg);
        return VMM_ERR_INVAL;
    }
    st->reinject_given = true;
    return VMM_OK;
}

static int opt_exit_stats(struct parse_state *st, char *arg)
{
    st->cfg->exit_stats_path = arg;
    return VMM_OK;
}

/* --device、--netdev、--drive 都可以重复，各自最多 VMM_MAX_DEVICES 个 */
static int check_room(int count, const char *opt)
{
    if (count == VMM_MAX_DEVICES) {
        vmm_err("too many %s (max %d)", opt, VMM_MAX_DEVICES);
        return VMM_ERR_INVAL;
    }
    return VMM_OK;
}

static int opt_device(struct parse_state *st, char *arg)
{
    struct vmm_config *cfg = st->cfg;

    if (!strcmp(arg, "help")) {
        vmm_device_list_types(stdout);
        exit(0);
    }
    if (check_room(cfg->nr_devices, "--device") != VMM_OK ||
        vmm_device_parse(arg, &cfg->devices[cfg->nr_devices]) != VMM_OK)
        return VMM_ERR_INVAL;
    cfg->nr_devices++;
    return VMM_OK;
}

static int opt_netdev(struct parse_state *st, char *arg)
{
    struct vmm_config *cfg = st->cfg;

    if (!strcmp(arg, "help")) {
        netdev_list_types(stdout);
        exit(0);
    }
    if (check_room(cfg->nr_netdevs, "--netdev") != VMM_OK ||
        netdev_parse(arg, &cfg->netdevs[cfg->nr_netdevs]) != VMM_OK)
        return VMM_ERR_INVAL;
    cfg->nr_netdevs++;
    return VMM_OK;
}

static int opt_drive(struct parse_state *st, char *arg)
{
    struct vmm_config *cfg = st->cfg;

    if (check_room(cfg->nr_drives, "--drive") != VMM_OK ||
        blockdev_parse(arg, &cfg->drives[cfg->nr_drives]) != VMM_OK)
        return VMM_ERR_INVAL;
    cfg->nr_drives++;
    return VMM_OK;
}

static int opt_walk(struct parse_state *st, char *arg)
{
    return parse_hex_opt("--walk", arg, &st->cfg->walk_gva);
}

/* GPA:LEN，GPA 是十六进制，LEN 按 C 的写法（十进制、0x 十六进制都行）。
 * 两段都必须整段是数字：后面跟着别的字符多半是写错了，按前缀截断会
 * 悄悄 dump 错的地址或长度 */
static int opt_dump(struct parse_state *st, char *arg)
{
    char *colon = strchr(arg, ':');
    char *end;
    unsigned long long gpa, len;

    if (colon) {
        gpa = strtoull(arg, &end, 16);
        if (end == arg || end != colon)
            colon = NULL;
    }
    if (colon) {
        len = strtoull(colon + 1, &end, 0);
        if (end == colon + 1 || *end)
            colon = NULL;
    }
    if (!colon) {
        vmm_err("--dump must be GPA:LEN (hex GPA, decimal LEN): %s", arg);
        return VMM_ERR_INVAL;
    }
    st->cfg->dump_gpa = gpa;
    st->cfg->dump_len = (size_t)len;
    st->cfg->dump_on_exit = true;
    return VMM_OK;
}

struct cli_option {
    const char *name;
    bool has_arg;
    int (*apply)(struct parse_state *st, char *arg);
};

/* -v 不在表里：它可以合写成 -vv，见 is_verbose_flag */
static const struct cli_option options[] = {
    { "-h",               false, opt_help },
    { "--help",           false, opt_help },
    { "-q",               false, opt_quiet },
    { "--mode",           true,  opt_mode },
    { "--mem",            true,  opt_mem },
    { "--smp",            true,  opt_smp },
    { "--initrd",         true,  opt_initrd },
    { "--cmdline",        true,  opt_cmdline },
    { "--append",         true,  opt_append },
    { "--map",            true,  opt_map },
    { "--load",           true,  opt_load },
    { "--entry",          true,  opt_entry },
    { "--gran",           true,  opt_gran },
    { "--trace-pio",      true,  opt_trace_pio },
    { "--trace-mmio",     true,  opt_trace_mmio },
    { "--gdb",            true,  opt_gdb },
    { "--monitor",        true,  opt_monitor },
    { "--no-reboot",      false, opt_no_reboot },
    { "--kernel-irqchip", true,  opt_kernel_irqchip },
    { "--pit-reinject",   true,  opt_pit_reinject },
    { "--exit-stats",     true,  opt_exit_stats },
    { "--device",         true,  opt_device },
    { "--netdev",         true,  opt_netdev },
    { "--drive",          true,  opt_drive },
    { "--walk",           true,  opt_walk },
    { "--dump",           true,  opt_dump },
};

static const struct cli_option *find_option(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof(options) / sizeof(options[0]); i++)
        if (!strcmp(options[i].name, name))
            return &options[i];
    return NULL;
}

/* -v 可以重复，也可以合写成 -vv：每个 v 提高一级 */
static bool is_verbose_flag(const char *a)
{
    return a[0] == '-' && a[1] == 'v' && strspn(a + 1, "v") == strlen(a + 1);
}

static int parse_argv(struct parse_state *st, int argc, char **argv)
{
    int i, r;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const struct cli_option *o = find_option(a);
        char *arg = NULL;

        if (o) {
            if (o->has_arg) {
                if (++i >= argc) {
                    vmm_err("%s requires an argument", a);
                    return VMM_ERR_INVAL;
                }
                arg = argv[i];
            }
            r = o->apply(st, arg);
            if (r != VMM_OK)
                return r;
        } else if (is_verbose_flag(a)) {
            st->cfg->log_level += (int)strlen(a + 1);
        } else if (a[0] == '-' && a[1]) {
            vmm_err("unknown option: %s", a);
            return VMM_ERR_INVAL;
        } else {
            st->cfg->payload_path = a;
        }
    }
    return VMM_OK;
}

/* 裸 payload 模式（real16/long64）下不能用的选项。按表的顺序检查，报出
 * 第一个违反的 */
static int check_payload_mode(const struct parse_state *st)
{
    const struct vmm_config *cfg = st->cfg;
    const struct {
        bool given;
        const char *msg;
    } linux_only[] = {
        { cfg->initrd_path || cfg->cmdline || st->append,
          "--initrd/--cmdline/--append require --mode linux" },
        /* 裸 payload 不会去唤醒 AP，而且没有内核态 irqchip 时
         * 所有 vCPU 一创建就是 RUNNABLE，会从同一个入口同时跑起来 */
        { cfg->nr_vcpus > 1, "--smp > 1 requires --mode linux" },
        /* PCI 总线只在 Linux 模式下创建：描述它的 DSDT 和 MP table 只有
         * 这个模式才生成，裸 payload 拿不到总线的地址窗口和中断路由 */
        { cfg->nr_devices,   "--device requires --mode linux" },
        { cfg->nr_netdevs,   "--netdev requires --mode linux" },
        { cfg->nr_drives,    "--drive requires --mode linux" },
        { cfg->no_reboot,    "--no-reboot requires --mode linux" },
        /* 裸 payload 模式不创建任何中断控制器 */
        { st->irqchip_given,  "--kernel-irqchip requires --mode linux" },
        { st->reinject_given, "--pit-reinject requires --mode linux" },
    };
    size_t i;

    for (i = 0; i < sizeof(linux_only) / sizeof(linux_only[0]); i++) {
        if (linux_only[i].given) {
            vmm_err("%s", linux_only[i].msg);
            return VMM_ERR_INVAL;
        }
    }
    return VMM_OK;
}

/* Linux 模式内核与 initramfs 缺一不可。没有 initramfs 时内核找不到
 * rootfs，只会在启动末尾以 "VFS: Unable to mount root fs" panic，
 * 现象离真正的原因（忘了传参、路径写错）太远，所以在创建 VM 之前
 * 就拦下来。需要专门验证 panic -> reboot -> VMM 退出这条路径时，
 * 照常提供 initramfs，再加 --append rdinit=/nonexistent：内核在
 * initramfs 里找不到 init，转去挂载 root=，没有 root= 就同样 VFS panic。 */
static int check_linux_files(const struct vmm_config *cfg)
{
    if (!cfg->initrd_path) {
        vmm_err("--mode linux requires --initrd FILE "
                "(build one with tools/build_initramfs.sh)");
        return VMM_ERR_INVAL;
    }
    if (access(cfg->initrd_path, R_OK) != 0) {
        vmm_err("initrd %s: %s", cfg->initrd_path, strerror(errno));
        return VMM_ERR_INVAL;
    }
    if (access(cfg->payload_path, R_OK) != 0) {
        vmm_err("kernel %s: %s", cfg->payload_path, strerror(errno));
        return VMM_ERR_INVAL;
    }
    return VMM_OK;
}

/* 内核命令行 = --cmdline（缺省用 DEFAULT_LINUX_CMDLINE），再接上 --append */
static int build_linux_cmdline(struct vmm_config *cfg, const char *append)
{
    static char cmdline[ZP_CMDLINE_MAX];
    int n;

    n = snprintf(cmdline, sizeof(cmdline), "%s%s%s",
                 cfg->cmdline ? cfg->cmdline : DEFAULT_LINUX_CMDLINE,
                 append ? " " : "", append ? append : "");
    if (n < 0 || (size_t)n >= sizeof(cmdline)) {
        vmm_err("kernel command line is too long (max %d bytes)",
                ZP_CMDLINE_MAX - 1);
        return VMM_ERR_INVAL;
    }
    cfg->cmdline = cmdline;
    return VMM_OK;
}

int vmm_config_parse_args(struct vmm_config *cfg, int argc, char **argv)
{
    struct parse_state st = { .cfg = cfg, .prog = argv[0] };
    bool is_linux;
    int r;

    r = parse_argv(&st, argc, argv);
    if (r != VMM_OK)
        return r;

    if (!cfg->payload_path) {
        vmm_err("missing payload file");
        return VMM_ERR_INVAL;
    }
    is_linux = cfg->mode == VMM_BOOT_LINUX;
    r = is_linux ? check_linux_files(cfg) : check_payload_mode(&st);
    if (r != VMM_OK)
        return r;

    /* --netdev、--drive 可以写在引用它们的 --device 之后，所以引用要等
     * 全部解析完。两边都检查完再返回，一次报出所有引用错误 */
    r = netdev_link(cfg->devices, cfg->nr_devices,
                    cfg->netdevs, cfg->nr_netdevs);
    if (blockdev_link(cfg->devices, cfg->nr_devices,
                      cfg->drives, cfg->nr_drives) != VMM_OK || r != VMM_OK)
        return VMM_ERR_INVAL;

    if (is_linux) {
        r = build_linux_cmdline(cfg, st.append);
        if (r != VMM_OK)
            return r;
    }

    /* 按模式补默认地址 */
    if (cfg->payload_gpa == UINT64_MAX)
        cfg->payload_gpa = (cfg->mode == VMM_BOOT_LONG64)
                           ? PAYLOAD_LONG_GPA : 0x0;
    if (cfg->entry_gpa == UINT64_MAX)
        cfg->entry_gpa = cfg->payload_gpa;

    return VMM_OK;
}
