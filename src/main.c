/* src/main.c — 命令行解析与 Step 1 的两条启动路径 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "vmm.h"
#include "memory.h"
#include "kvm_wrappers.h"
#include "boot/gdt_pgtable.h"
#include "devices/serial8250.h"
#include "devices/irqchip.h"
#include "devices/i8042.h"
#include "boot/kernel_loader.h"
#include "boot/zeropage.h"
#include "boot/mptable.h"
#include "boot/acpi.h"
#include "devices/rtc.h"
#include "console.h"
#include "smp.h"
#include <unistd.h>

/* 长模式 payload 的默认载入地址。
 * 必须避开 [0x1000, 0x205000)（GDT + 四级页表最坏情况占用），
 * 也要避开 Step 1.2 的写入目标 0x500000。3MiB 处两边都不挨着。 */
#define PAYLOAD_LONG_GPA 0x300000ULL

/* Step 2 默认内核参数。
 *   earlyprintk  在 8250 驱动加载之前就能看到输出，内核早期崩溃时救命
 *   reboot=k     reboot 走 i8042 reset，VMM 据此退出
 *   panic=-1     panic 后立刻 reboot，从而让 VMM 退出而不是挂住
 *   noapictimer? 不加：内核态 LAPIC 的 timer 完全可用 */
#define DEFAULT_LINUX_CMDLINE \
    "console=ttyS0 earlyprintk=serial,ttyS0,115200 reboot=k panic=-1"

/* ------------------------------------------------------------------ */
/* Step 1 的两个调试设备                                                */
/* ------------------------------------------------------------------ */

/* 端口 0x10：guest 往这里写一个字节，VMM 打印出来。
 * 这是最原始的 guest->host 通信手段，在串口跑起来之前唯一能用的。 */
static int dbgport_write(void *opaque, uint64_t off, uint32_t size,
                         const void *data)
{
    const uint8_t *b = data;

    (void)opaque; (void)off;
    printf(">>> guest wrote port 0x10: 0x%02x (%u bytes)", b[0], size);
    if (b[0] >= 0x20 && b[0] < 0x7F)
        printf(" = '%c'", b[0]);
    printf("\n");
    fflush(stdout);
    return VMM_OK;
}

static int dbgport_read(void *opaque, uint64_t off, uint32_t size, void *data)
{
    (void)opaque; (void)off;
    memset(data, 0x5A, size);   /* 固定图案，方便 guest 侧验证读通路 */
    return VMM_OK;
}

/* 端口 0xF4：沿用 qemu isa-debug-exit 的约定，写入值作为退出码。
 * 后面写自动化测试脚本时会用到。 */
static int exitport_write(void *opaque, uint64_t off, uint32_t size,
                          const void *data)
{
    struct vmm_vm *vm = opaque;
    const uint8_t *b = data;

    (void)off; (void)size;
    vm->exit_code = b[0];
    vmm_info("guest requested exit, code=%d", b[0]);
    return VMM_ERR_EXIT;
}

/* ------------------------------------------------------------------ */
/* 命令行                                                              */
/* ------------------------------------------------------------------ */

void vmm_config_default(struct vmm_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode         = VMM_BOOT_REAL16;
    cfg->ram_size     = VMM_DEFAULT_RAM_SIZE;
    cfg->nr_vcpus     = 1;
    cfg->payload_gpa  = UINT64_MAX;   /* UINT64_MAX = 用模式默认值 */
    cfg->entry_gpa    = UINT64_MAX;
    cfg->log_level    = 2;
    cfg->dump_on_exit = false;
    cfg->dump_gpa     = 0x500000;
    cfg->dump_len     = 32;
}

void vmm_usage(const char *prog)
{
    fprintf(stderr,
"Usage: %s [options] <payload.bin | vmlinux | bzImage>\n"
"\n"
"  --mode real16|long64|linux  Boot mode (default: real16)\n"
"                           real16 = Step 1.1, 16-bit real mode, entry GPA 0x0\n"
"                           long64 = Step 1.2, 64-bit long mode, entry GPA 0x%llx\n"
"                           linux  = Step 2, boot Linux: vmlinux (ELF) or bzImage,\n"
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
"  --walk GVA             Print the 4-level page walk for GVA before starting\n"
"  -v                     Raise log level, repeatable (-vv enables DEBUG)\n"
"  -q                     Errors only\n"
"  -h                     Show this help\n"
"\n"
"Examples:\n"
"  # Step 1.1\n"
"  %s --mem 2M build/payload16.bin\n"
"  # Step 1.2\n"
"  %s --mode long64 --dump 500000:16 -v build/payload64.bin\n"
"  # Step 2.1\n"
"  %s --mode long64 build/serial64.bin\n"
"  # Step 2.2 / 2.3\n"
"  %s --mode linux --mem 512M --initrd initramfs.cpio.gz vmlinux\n"
"  %s --mode linux --mem 512M --initrd initramfs.cpio.gz bzImage\n"
"  # SMP\n"
"  %s --mode linux --smp 4 --mem 512M --initrd initramfs.cpio.gz vmlinux\n",
            prog, (unsigned long long)PAYLOAD_LONG_GPA, VMM_MAX_VCPUS,
            prog, prog, prog, prog, prog, prog);
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

/* 这几个只在 main 里用，不进头文件 */
static enum boot_page_gran g_gran = BOOT_PG_2M;
static uint64_t g_map_size = BOOT_IDENTITY_MAP_SIZE;
static uint64_t g_walk_gva = UINT64_MAX;
static const char *g_append;

int vmm_config_parse_args(struct vmm_config *cfg, int argc, char **argv)
{
    int i;

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
        } else if (!strcmp(a, "-v")) {
            cfg->log_level++;
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
            g_append = argv[i];
        } else if (!strcmp(a, "--map")) {
            NEED_ARG();
            if (parse_size(argv[i], &g_map_size) != VMM_OK)
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
                g_gran = BOOT_PG_2M;
            else if (!strcmp(argv[i], "4k"))
                g_gran = BOOT_PG_4K;
            else
                return VMM_ERR_INVAL;
        } else if (!strcmp(a, "--walk")) {
            NEED_ARG();
            if (parse_hex(argv[i], &g_walk_gva) != VMM_OK)
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
        (cfg->initrd_path || cfg->cmdline || g_append)) {
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
    /* Step 1 的裸 payload 不会去唤醒 AP，而且没有内核态 irqchip 时
     * 所有 vCPU 一创建就是 RUNNABLE，会从同一个入口同时跑起来 */
    if (cfg->mode != VMM_BOOT_LINUX && cfg->nr_vcpus > 1) {
        vmm_err("--smp > 1 requires --mode linux");
        return VMM_ERR_INVAL;
    }
    if (cfg->mode == VMM_BOOT_LINUX) {
        static char cmdline[ZP_CMDLINE_MAX];
        int n;

        n = snprintf(cmdline, sizeof(cmdline), "%s%s%s",
                     cfg->cmdline ? cfg->cmdline : DEFAULT_LINUX_CMDLINE,
                     g_append ? " " : "", g_append ? g_append : "");
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

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

/* 检查 payload 是否和 GDT/页表区域打架 */
static int check_payload_overlap(const struct boot_mem_layout *l,
                                 uint64_t gpa, size_t len)
{
    uint64_t boot_lo = l->gdt_gpa;
    uint64_t boot_hi = (l->gran == BOOT_PG_4K)
                     ? l->pt_gpa + (l->map_size / (2ULL << 20)) * 0x1000
                     : l->pd_gpa + 0x1000;

    if (len == 0)
        return VMM_OK;
    if (gpa < boot_hi && boot_lo < gpa + len) {
        vmm_err("payload [0x%llx, 0x%llx) overlaps GDT/page table area "
                "[0x%llx, 0x%llx)",
                (unsigned long long)gpa,
                (unsigned long long)(gpa + len),
                (unsigned long long)boot_lo,
                (unsigned long long)boot_hi);
        vmm_err("  use --load to move the payload above 0x%llx",
                (unsigned long long)boot_hi);
        return VMM_ERR_INVAL;
    }
    return VMM_OK;
}

/* Step 2.2 / 2.3：vmlinux + boot_params + initrd，然后按 64 位启动协议
 * 摆好 vCPU。串口中断接到内核态 PIC/IOAPIC 的 GSI 4。 */
static int boot_linux(struct vmm_vm *vm, struct vmm_vcpu *vcpu,
                      const struct vmm_config *cfg, struct serial8250 *com1)
{
    struct boot_mem_layout layout;
    struct kernel_image kimg;
    struct zp_config zc;
    uint64_t rsdp;
    int r;

    com1->irq_set    = irqchip_irq_cb;
    com1->irq_opaque = vm;

    r = i8042_attach(vm);
    if (r != VMM_OK)
        return r;
    r = rtc_attach(vm);
    if (r != VMM_OK)
        return r;
    r = acpi_pm_attach(vm);
    if (r != VMM_OK)
        return r;

    /* vmlinux 还是 bzImage 由文件头决定，见 boot/kernel_loader.h */
    r = kernel_load(vm, cfg->payload_path, &kimg);
    if (r != VMM_OK)
        return r;
    vmm_info("kernel format: %s", kernel_format_name(kimg.format));

    /* 内核必须完全落在 1MiB 以上，不能碰到低端的 GDT/页表/zero page */
    if (kimg.lo_gpa < ZP_HIGH_MEM_START) {
        vmm_err("kernel starts at 0x%llx, below 1MiB",
                (unsigned long long)kimg.lo_gpa);
        return VMM_ERR_INVAL;
    }

    /* 中断拓扑同时用两种格式描述，guest 内核开了哪个就用哪个 */
    r = mptable_setup(vm, vm->nr_vcpus);
    if (r != VMM_OK)
        return r;
    r = acpi_setup(vm, vm->nr_vcpus, &rsdp);
    if (r != VMM_OK)
        return r;

    memset(&zc, 0, sizeof(zc));
    zc.acpi_rsdp   = rsdp;
    zc.cmdline     = cfg->cmdline;
    zc.initrd_path = cfg->initrd_path;
    zc.kernel_end  = kimg.hi_gpa;
    if (kimg.hdr_len) {
        zc.kernel_hdr     = &kimg.hdr;
        zc.kernel_hdr_len = kimg.hdr_len;
        zc.initrd_addr_max = kimg.hdr.initrd_addr_max;
    }
    /* 协议不要求 initrd 恒等映射，但把它留在 VMM 页表覆盖的范围内不花
     * 任何代价，出了问题也好用 --walk 查 */
    if (zc.initrd_addr_max > BOOT_IDENTITY_MAP_SIZE - 1)
        zc.initrd_addr_max = BOOT_IDENTITY_MAP_SIZE - 1;
    r = zeropage_setup(vm, &zc);
    if (r != VMM_OK)
        return r;

    /* 恒等映射 1GiB 就够：内核在 16MiB 附近（bzImage 的 init_size 区间
     * 也在 16MiB 起的几十 MiB 内），zero page/cmdline 在低端，initrd 被
     * 限制在 1GiB 以下。vmlinux 的 startup_64、bzImage 的解压器都会很快
     * 换上自己的页表，这张表只用于最初那一小段。 */
    boot_layout_default(&layout);
    r = boot_layout_validate(vm, &layout);
    if (r != VMM_OK)
        return r;
    r = boot_build_page_table(vm, &layout);
    if (r != VMM_OK)
        return r;

    r = boot_setup_linux64(vcpu, &layout, kimg.entry_gpa, ZP_BOOT_PARAMS_GPA);
    if (r != VMM_OK)
        return r;

    return console_start(vm, com1);
}

int main(int argc, char **argv)
{
    struct vmm_config cfg;
    struct vmm_vm vm;
    struct boot_mem_layout layout;
    struct vmm_vcpu *vcpu;
    struct vmm_io_dev dev;
    static struct serial8250 com1;
    size_t payload_len = 0;
    int r;

    vmm_config_default(&cfg);
    if (argc < 2) {
        vmm_usage(argv[0]);
        return 1;
    }
    if (vmm_config_parse_args(&cfg, argc, argv) != VMM_OK)
        return 1;
    vmm_log_level = cfg.log_level;

    r = vm_create(&vm, &cfg);
    if (r != VMM_OK)
        return 1;

    vcpu = &vm.vcpus[0];

    /* 调试设备 */
    memset(&dev, 0, sizeof(dev));
    dev.name  = "dbg-port";
    dev.base  = 0x10;
    dev.len   = 1;
    dev.read  = dbgport_read;
    dev.write = dbgport_write;
    r = vmm_register_pio(&vm, &dev);
    if (r != VMM_OK)
        goto out;

    memset(&dev, 0, sizeof(dev));
    dev.name   = "exit-port";
    dev.base   = 0xF4;
    dev.len    = 1;
    dev.opaque = &vm;
    dev.write  = exitport_write;
    r = vmm_register_pio(&vm, &dev);
    if (r != VMM_OK)
        goto out;

    /* COM1：所有模式都挂上，Step 2.1 起 guest 的主输出通道 */
    serial8250_init(&com1, STDOUT_FILENO);
    r = serial8250_attach(&vm, &com1, SERIAL_COM1_BASE, SERIAL_COM1_IRQ);
    if (r != VMM_OK)
        goto out;

    if (cfg.mode == VMM_BOOT_LINUX) {
        r = boot_linux(&vm, vcpu, &cfg, &com1);
        if (r != VMM_OK)
            goto out;
        goto run;
    }

    /* 载入 payload */
    r = mem_load_file(&vm, cfg.payload_gpa, cfg.payload_path, &payload_len);
    if (r != VMM_OK)
        goto out;

    /* 按模式初始化 vCPU */
    if (cfg.mode == VMM_BOOT_REAL16) {
        r = boot_setup_real_mode(vcpu, cfg.entry_gpa);
        if (r != VMM_OK)
            goto out;
    } else {
        boot_layout_default(&layout);
        layout.gran     = g_gran;
        layout.map_size = g_map_size;

        r = boot_layout_validate(&vm, &layout);
        if (r != VMM_OK)
            goto out;

        /* payload 已经载入内存了，页表马上要往同一片 RAM 里写。
         * 重叠的话页表会把 payload 覆盖掉，症状是 guest 一启动就
         * 执行到页表数据当指令，报 INTERNAL_ERROR/EMULATION，
         * 非常难猜。这里提前拦下来。 */
        r = check_payload_overlap(&layout, cfg.payload_gpa, payload_len);
        if (r != VMM_OK)
            goto out;

        r = boot_build_page_table(&vm, &layout);
        if (r != VMM_OK)
            goto out;
        r = boot_build_gdt(&vm, &layout);
        if (r != VMM_OK)
            goto out;

        if (g_walk_gva != UINT64_MAX)
            boot_dump_page_walk(&vm, &layout, g_walk_gva);

        r = boot_setup_long_mode(vcpu, &layout, cfg.entry_gpa);
        if (r != VMM_OK)
            goto out;
    }

run:
    vmm_info("--- entering guest ---");
    r = vm_run(&vm);
    if (cfg.mode == VMM_BOOT_LINUX)
        console_stop();
    vmm_info("--- left guest (%s) ---", r == VMM_OK ? "ok" : "error");

    /* Ctrl-A x 打断的现场一定要看：guest 卡住时这是唯一的线索 */
    if (r == VMM_OK && (vmm_log_level >= 3 || vm.user_stop)) {
        int i;

        for (i = 0; i < vm.nr_vcpus; i++)
            vcpu_dump_state(&vm.vcpus[i]);
    }

    if (cfg.dump_on_exit) {
        fprintf(stderr, "guest memory at GPA 0x%llx, %zu bytes:\n",
                (unsigned long long)cfg.dump_gpa, cfg.dump_len);
        mem_hexdump(&vm, cfg.dump_gpa, cfg.dump_len);
    }

out:
    vm_destroy(&vm);
    return (r == VMM_OK) ? vm.exit_code : 1;
}
