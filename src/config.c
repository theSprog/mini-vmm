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

int vmm_config_parse_args(struct vmm_config *cfg, int argc, char **argv)
{
    int i, r;
    bool irqchip_given = false;
    bool reinject_given = false;
    /* --append 只在拼内核命令行时用一次，拼好的结果写回 cfg->cmdline */
    const char *append = NULL;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

#define NEED_ARG() do {                                               \
        if (++i >= argc) {                                    \
            vmm_err("%s requires an argument", a);                \
            return VMM_ERR_INVAL;                         \
        }                                                     \
    } while (0)

        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            vmm_usage(argv[0]);
            exit(0);
        } else if (a[0] == '-' && a[1] == 'v' &&
                   strspn(a + 1, "v") == strlen(a + 1)) {
            /* -v 可以重复，也可以合写成 -vv：每个 v 提高一级 */
            cfg->log_level += (int)strlen(a + 1);
        } else if (!strcmp(a, "-q")) {
            cfg->log_level = 0;
        } else if (!strcmp(a, "--mode")) {
            NEED_ARG();
            if (!strcmp(argv[i], "real16"))
                cfg->mode = VMM_BOOT_REAL16;
            else if (!strcmp(argv[i], "long64"))
                cfg->mode = VMM_BOOT_LONG64;
            else if (!strcmp(argv[i], "linux"))
                cfg->mode = VMM_BOOT_LINUX;
            else {
                vmm_err("unknown mode: %s", argv[i]);
                return VMM_ERR_INVAL;
            }
        } else if (!strcmp(a, "--mem")) {
            NEED_ARG();
            if (parse_size(argv[i], &cfg->ram_size) != VMM_OK) {
                vmm_err("cannot parse --mem: %s", argv[i]);
                return VMM_ERR_INVAL;
            }
        } else if (!strcmp(a, "--smp")) {
            char *end;
            long n;

            NEED_ARG();
            n = strtol(argv[i], &end, 10);
            if (end == argv[i] || *end || n < 1 || n > VMM_MAX_VCPUS) {
                vmm_err("--smp must be an integer in 1..%d: %s",
                        VMM_MAX_VCPUS, argv[i]);
                return VMM_ERR_INVAL;
            }
            cfg->nr_vcpus = (int)n;
        } else if (!strcmp(a, "--initrd")) {
            NEED_ARG();
            cfg->initrd_path = argv[i];
        } else if (!strcmp(a, "--cmdline")) {
            NEED_ARG();
            cfg->cmdline = argv[i];
        } else if (!strcmp(a, "--append")) {
            NEED_ARG();
            append = argv[i];
        } else if (!strcmp(a, "--map")) {
            NEED_ARG();
            if (parse_size(argv[i], &cfg->map_size) != VMM_OK)
                return VMM_ERR_INVAL;
        } else if (!strcmp(a, "--load")) {
            NEED_ARG();
            if (parse_hex(argv[i], &cfg->payload_gpa) != VMM_OK)
                return VMM_ERR_INVAL;
        } else if (!strcmp(a, "--entry")) {
            NEED_ARG();
            if (parse_hex(argv[i], &cfg->entry_gpa) != VMM_OK)
                return VMM_ERR_INVAL;
        } else if (!strcmp(a, "--gran")) {
            NEED_ARG();
            if (!strcmp(argv[i], "2m"))
                cfg->pt_gran = BOOT_PG_2M;
            else if (!strcmp(argv[i], "4k"))
                cfg->pt_gran = BOOT_PG_4K;
            else
                return VMM_ERR_INVAL;
        } else if (!strcmp(a, "--trace-pio")) {
            NEED_ARG();
            cfg->trace_pio_path = argv[i];
        } else if (!strcmp(a, "--trace-mmio")) {
            NEED_ARG();
            cfg->trace_mmio_path = argv[i];
        } else if (!strcmp(a, "--gdb")) {
            char *end;
            long n;

            NEED_ARG();
            n = strtol(argv[i], &end, 10);
            if (end == argv[i] || *end || n < 0 || n > 65535) {
                vmm_err("--gdb must be a TCP port in 0..65535: %s", argv[i]);
                return VMM_ERR_INVAL;
            }
            cfg->gdb_port = (int)n;
        } else if (!strcmp(a, "--monitor")) {
            NEED_ARG();
            cfg->monitor_path = argv[i];
        } else if (!strcmp(a, "--no-reboot")) {
            cfg->no_reboot = true;
        } else if (!strcmp(a, "--kernel-irqchip")) {
            NEED_ARG();
            if (!strcmp(argv[i], "on")) {
                cfg->irqchip = VMM_IRQCHIP_ON;
            } else if (!strcmp(argv[i], "split")) {
                cfg->irqchip = VMM_IRQCHIP_SPLIT;
            } else if (!strcmp(argv[i], "off")) {
                vmm_err("--kernel-irqchip off (LAPIC in user space) is not "
                        "implemented yet; it belongs to the user-lapic "
                        "milestone");
                return VMM_ERR_INVAL;
            } else {
                vmm_err("--kernel-irqchip must be on or split: %s", argv[i]);
                return VMM_ERR_INVAL;
            }
            irqchip_given = true;
        } else if (!strcmp(a, "--pit-reinject")) {
            NEED_ARG();
            if (!strcmp(argv[i], "on")) {
                cfg->pit_reinject = true;
            } else if (!strcmp(argv[i], "off")) {
                cfg->pit_reinject = false;
            } else {
                vmm_err("--pit-reinject must be on or off: %s", argv[i]);
                return VMM_ERR_INVAL;
            }
            reinject_given = true;
        } else if (!strcmp(a, "--exit-stats")) {
            NEED_ARG();
            cfg->exit_stats_path = argv[i];
        } else if (!strcmp(a, "--device")) {
            NEED_ARG();
            if (!strcmp(argv[i], "help")) {
                vmm_device_list_types(stdout);
                exit(0);
            }
            if (cfg->nr_devices == VMM_MAX_DEVICES) {
                vmm_err("too many --device (max %d)", VMM_MAX_DEVICES);
                return VMM_ERR_INVAL;
            }
            if (vmm_device_parse(argv[i], &cfg->devices[cfg->nr_devices]) != VMM_OK)
                return VMM_ERR_INVAL;
            cfg->nr_devices++;
        } else if (!strcmp(a, "--netdev")) {
            NEED_ARG();
            if (!strcmp(argv[i], "help")) {
                netdev_list_types(stdout);
                exit(0);
            }
            if (cfg->nr_netdevs == VMM_MAX_DEVICES) {
                vmm_err("too many --netdev (max %d)", VMM_MAX_DEVICES);
                return VMM_ERR_INVAL;
            }
            if (netdev_parse(argv[i], &cfg->netdevs[cfg->nr_netdevs]) != VMM_OK)
                return VMM_ERR_INVAL;
            cfg->nr_netdevs++;
        } else if (!strcmp(a, "--drive")) {
            NEED_ARG();
            if (cfg->nr_drives == VMM_MAX_DEVICES) {
                vmm_err("too many --drive (max %d)", VMM_MAX_DEVICES);
                return VMM_ERR_INVAL;
            }
            if (blockdev_parse(argv[i], &cfg->drives[cfg->nr_drives]) != VMM_OK)
                return VMM_ERR_INVAL;
            cfg->nr_drives++;
        } else if (!strcmp(a, "--walk")) {
            NEED_ARG();
            if (parse_hex(argv[i], &cfg->walk_gva) != VMM_OK)
                return VMM_ERR_INVAL;
        } else if (!strcmp(a, "--dump")) {
            char *colon;
            NEED_ARG();
            colon = strchr(argv[i], ':');
            if (!colon)
                return VMM_ERR_INVAL;
            *colon = '\0';
            if (parse_hex(argv[i], &cfg->dump_gpa) != VMM_OK)
                return VMM_ERR_INVAL;
            cfg->dump_len = (size_t)strtoul(colon + 1, NULL, 0);
            cfg->dump_on_exit = true;
        } else if (a[0] == '-' && a[1]) {
            vmm_err("unknown option: %s", a);
            return VMM_ERR_INVAL;
        } else {
            cfg->payload_path = a;
        }
#undef NEED_ARG
    }

    if (!cfg->payload_path) {
        vmm_err("missing payload file");
        return VMM_ERR_INVAL;
    }

    if (cfg->mode != VMM_BOOT_LINUX &&
        (cfg->initrd_path || cfg->cmdline || append)) {
        vmm_err("--initrd/--cmdline/--append require --mode linux");
        return VMM_ERR_INVAL;
    }
    /* Linux 模式内核与 initramfs 缺一不可。没有 initramfs 时内核找不到
     * rootfs，只会在启动末尾以 "VFS: Unable to mount root fs" panic，
     * 现象离真正的原因（忘了传参、路径写错）太远，所以在创建 VM 之前
     * 就拦下来。需要专门验证 panic -> reboot -> VMM 退出这条路径时，
     * 照常提供 initramfs，再加 --append rdinit=/nonexistent：内核在
     * initramfs 里找不到 init，转去挂载 root=，没有 root= 就同样 VFS panic。 */
    if (cfg->mode == VMM_BOOT_LINUX) {
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
    }
    /* 裸 payload 不会去唤醒 AP，而且没有内核态 irqchip 时
     * 所有 vCPU 一创建就是 RUNNABLE，会从同一个入口同时跑起来 */
    if (cfg->mode != VMM_BOOT_LINUX && cfg->nr_vcpus > 1) {
        vmm_err("--smp > 1 requires --mode linux");
        return VMM_ERR_INVAL;
    }
    /* PCI 总线只在 Linux 模式下创建：描述它的 DSDT 和 MP table 只有
     * 这个模式才生成，裸 payload 拿不到总线的地址窗口和中断路由 */
    if (cfg->mode != VMM_BOOT_LINUX && cfg->nr_devices) {
        vmm_err("--device requires --mode linux");
        return VMM_ERR_INVAL;
    }
    if (cfg->mode != VMM_BOOT_LINUX && cfg->nr_netdevs) {
        vmm_err("--netdev requires --mode linux");
        return VMM_ERR_INVAL;
    }
    if (cfg->mode != VMM_BOOT_LINUX && cfg->nr_drives) {
        vmm_err("--drive requires --mode linux");
        return VMM_ERR_INVAL;
    }
    if (cfg->no_reboot && cfg->mode != VMM_BOOT_LINUX) {
        vmm_err("--no-reboot requires --mode linux");
        return VMM_ERR_INVAL;
    }
    /* 裸 payload 模式不创建任何中断控制器 */
    if (irqchip_given && cfg->mode != VMM_BOOT_LINUX) {
        vmm_err("--kernel-irqchip requires --mode linux");
        return VMM_ERR_INVAL;
    }
    if (reinject_given && cfg->mode != VMM_BOOT_LINUX) {
        vmm_err("--pit-reinject requires --mode linux");
        return VMM_ERR_INVAL;
    }
    /* --netdev、--drive 可以写在引用它们的 --device 之后，所以引用要等
     * 全部解析完。两边都检查完再返回，一次报出所有引用错误 */
    r = netdev_link(cfg->devices, cfg->nr_devices,
                    cfg->netdevs, cfg->nr_netdevs);
    if (blockdev_link(cfg->devices, cfg->nr_devices,
                      cfg->drives, cfg->nr_drives) != VMM_OK || r != VMM_OK)
        return VMM_ERR_INVAL;
    if (cfg->mode == VMM_BOOT_LINUX) {
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
    }

    /* 按模式补默认地址 */
    if (cfg->payload_gpa == UINT64_MAX)
        cfg->payload_gpa = (cfg->mode == VMM_BOOT_LONG64)
                           ? PAYLOAD_LONG_GPA : 0x0;
    if (cfg->entry_gpa == UINT64_MAX)
        cfg->entry_gpa = cfg->payload_gpa;

    return VMM_OK;
}
