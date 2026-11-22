/* tools/guest/devmem.c — guest 里按物理地址读写一次 MMIO
 *
 * 用法和 busybox 的 devmem applet 相同：devmem ADDRESS [WIDTH [VALUE]]。
 * tools/build_initramfs.sh 把它静态编译后放进 initramfs 的 /sbin。
 *
 * 为什么需要它：guest 里没有驱动的设备，验收时要从用户态直接访问它的
 * MMIO BAR。x86 的 /dev/mem 只允许 read()/write() RAM 范围内的地址
 * （valid_phys_addr_range 以 high_memory 为界），BAR 在 RAM 之上，只能
 * mmap 之后用一条 load/store 访问；busybox 的 devmem 正是这么做的，但
 * 发行版的静态 busybox 常常没有编进这个 applet。
 *
 * 每次访问是一条指定宽度的 volatile load/store，VMM 一侧就看到一次同宽度的
 * KVM_EXIT_MMIO，不会像 memcpy 那样被拆成逐字节访问。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum { EXIT_OK = 0, EXIT_FAIL = 1, EXIT_USAGE = 2 };

static void usage(FILE *out)
{
    fprintf(out,
"Usage: devmem ADDRESS [WIDTH [VALUE]]\n"
"\n"
"Read or write one value at a physical address through an mmap of /dev/mem.\n"
"\n"
"  ADDRESS  physical address (decimal, or hex with 0x prefix)\n"
"  WIDTH    access width in bits: 8, 16, 32 or 64 (default: 32)\n"
"  VALUE    value to write; without it the value is read and printed\n"
"\n"
"EXAMPLE:\n"
"  devmem 0xc0000000            read 32 bits, prints e.g. 0x74736574\n"
"  devmem 0xc0000004 32 0x1234  write 32 bits\n"
"\n"
"NOTE:\n"
"  ADDRESS must be aligned to WIDTH. Each access is a single load or store\n"
"  of exactly WIDTH bits. Needs root and a kernel that allows /dev/mem\n"
"  mappings of the target range (STRICT_DEVMEM allows unclaimed MMIO).\n");
}

static int parse_u64(const char *s, uint64_t *out)
{
    char *end;

    errno = 0;
    *out = strtoull(s, &end, 0);
    return errno || end == s || *end ? -1 : 0;
}

int main(int argc, char **argv)
{
    uint64_t addr, value = 0, page, off;
    unsigned long width = 32;
    long psz;
    int fd, writing;
    volatile void *p;
    void *map;

    if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        usage(stdout);
        return EXIT_OK;
    }
    if (argc < 2 || argc > 4 || parse_u64(argv[1], &addr)) {
        usage(stderr);
        return EXIT_USAGE;
    }
    if (argc >= 3) {
        char *end;

        width = strtoul(argv[2], &end, 10);
        if (*end || (width != 8 && width != 16 && width != 32 && width != 64)) {
            fprintf(stderr, "devmem: WIDTH must be 8, 16, 32 or 64: %s\n", argv[2]);
            return EXIT_USAGE;
        }
    }
    writing = argc == 4;
    if (writing && parse_u64(argv[3], &value)) {
        fprintf(stderr, "devmem: cannot parse VALUE: %s\n", argv[3]);
        return EXIT_USAGE;
    }
    if (addr % (width / 8)) {
        fprintf(stderr, "devmem: ADDRESS 0x%" PRIx64 " is not aligned to %lu bits\n",
                addr, width);
        return EXIT_USAGE;
    }

    psz  = sysconf(_SC_PAGESIZE);
    page = addr & ~(uint64_t)(psz - 1);
    off  = addr - page;

    fd = open("/dev/mem", (writing ? O_RDWR : O_RDONLY) | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "devmem: open /dev/mem: %s\n", strerror(errno));
        return EXIT_FAIL;
    }
    map = mmap(NULL, (size_t)psz, writing ? PROT_READ | PROT_WRITE : PROT_READ,
               MAP_SHARED, fd, (off_t)page);
    if (map == MAP_FAILED) {
        fprintf(stderr, "devmem: mmap 0x%" PRIx64 ": %s\n", page, strerror(errno));
        close(fd);
        return EXIT_FAIL;
    }
    p = (volatile uint8_t *)map + off;

    if (writing) {
        switch (width) {
        case 8:  *(volatile uint8_t *)p  = (uint8_t)value;  break;
        case 16: *(volatile uint16_t *)p = (uint16_t)value; break;
        case 32: *(volatile uint32_t *)p = (uint32_t)value; break;
        default: *(volatile uint64_t *)p = value;           break;
        }
    } else {
        switch (width) {
        case 8:  value = *(volatile uint8_t *)p;  break;
        case 16: value = *(volatile uint16_t *)p; break;
        case 32: value = *(volatile uint32_t *)p; break;
        default: value = *(volatile uint64_t *)p; break;
        }
        printf("0x%0*" PRIX64 "\n", (int)(width / 4), value);
    }

    munmap(map, (size_t)psz);
    close(fd);
    return EXIT_OK;
}
