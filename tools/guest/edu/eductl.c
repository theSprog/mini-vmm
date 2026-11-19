/* tools/guest/edu/eductl.c — 在 guest 里驱动 /dev/eduN，检查 DMA 和中断闭环
 *
 * selftest 的判据不依赖 DMA 自己的结果：DMA 写进设备缓冲区的数据经 MMIO
 * 窗口（read）读回比对，MMIO 窗口写进去的数据（write）经 DMA 搬回比对。
 * 每一步都等驱动拿到完成中断才返回，所以中断和设备完成计数的增量也是
 * 精确可算的：每轮两次成功的 DMA、一次越界的 DMA（设备报 BAD_RANGE，同样
 * 完成并发中断）、一次软件中断，共 4 个中断、3 次 DMA 完成。
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "edu_regs.h"
#include "edu_ioctl.h"

enum { EXIT_OK = 0, EXIT_FAIL = 1, EXIT_USAGE = 2 };

static void usage(FILE *f)
{
	fprintf(f,
"usage: eductl [-d DEV] info\n"
"       eductl [-d DEV] selftest [ROUNDS]\n"
"\n"
"  -d DEV     character device (default /dev/edu0)\n"
"  info       print interrupt mode and counters\n"
"  selftest   ROUNDS rounds (default 1) of: DMA to device checked via MMIO,\n"
"             MMIO write checked via DMA from device, an out-of-range DMA that\n"
"             must fail with EINVAL, and a software interrupt; then checks the\n"
"             interrupt and DMA counters grew by exactly 4 and 3 per round\n"
"\n"
"EXAMPLE:\n"
"  eductl info\n"
"  eductl selftest 10\n"
"\n"
"NOTE: needs the edu module loaded; exit 0 on success, 1 on any mismatch,\n"
"2 on a usage error.\n");
}

static uint32_t rng_state;

static uint8_t rng_byte(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return (uint8_t)rng_state;
}

static int get_info(int fd, struct edu_info *info)
{
	if (ioctl(fd, EDU_IOC_INFO, info) < 0) {
		fprintf(stderr, "EDU_IOC_INFO: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static int do_dma(int fd, uint32_t dir, uint32_t off, uint32_t len, void *buf)
{
	struct edu_dma req = {
		.dir = dir, .off = off, .len = len,
		.user_addr = (uint64_t)(uintptr_t)buf,
	};

	return ioctl(fd, EDU_IOC_DMA, &req);
}

static int compare(const char *what, uint32_t off, const uint8_t *want,
		   const uint8_t *got, uint32_t len)
{
	uint32_t i;

	for (i = 0; i < len; i++)
		if (want[i] != got[i]) {
			fprintf(stderr, "%s: off 0x%x len 0x%x: byte %u is 0x%02x, want 0x%02x\n",
				what, off, len, i, got[i], want[i]);
			return -1;
		}
	return 0;
}

/* 一轮里用到的 (偏移, 长度)：整块、单字节、不对齐的短块、贴着末尾 */
static const struct { uint32_t off, len; } shapes[] = {
	{ 0, EDU_BUF_SIZE }, { 0, 1 }, { 3, 7 }, { 512, 512 }, { EDU_BUF_SIZE - 5, 5 },
};
#define NR_SHAPES (sizeof(shapes) / sizeof(shapes[0]))

static int one_round(int fd, unsigned round)
{
	static uint8_t want[EDU_BUF_SIZE], got[EDU_BUF_SIZE];
	uint32_t off = shapes[round % NR_SHAPES].off;
	uint32_t len = shapes[round % NR_SHAPES].len;
	uint32_t bit = 1u << (8 + round % 24), i;

	rng_state = 0x9E3779B9u ^ round;

	/* DMA 进设备，经 MMIO 读回 */
	for (i = 0; i < len; i++)
		want[i] = rng_byte();
	if (do_dma(fd, EDU_DMA_DIR_TO_DEVICE, off, len, want) < 0) {
		fprintf(stderr, "DMA to device off 0x%x len 0x%x: %s\n", off, len, strerror(errno));
		return -1;
	}
	if (pread(fd, got, len, off) != (ssize_t)len) {
		fprintf(stderr, "pread off 0x%x len 0x%x: %s\n", off, len, strerror(errno));
		return -1;
	}
	if (compare("DMA to device, read back via MMIO", off, want, got, len))
		return -1;

	/* MMIO 写进设备，经 DMA 读回 */
	for (i = 0; i < len; i++)
		want[i] = rng_byte();
	if (pwrite(fd, want, len, off) != (ssize_t)len) {
		fprintf(stderr, "pwrite off 0x%x len 0x%x: %s\n", off, len, strerror(errno));
		return -1;
	}
	memset(got, 0, len);
	if (do_dma(fd, EDU_DMA_DIR_FROM_DEVICE, off, len, got) < 0) {
		fprintf(stderr, "DMA from device off 0x%x len 0x%x: %s\n", off, len, strerror(errno));
		return -1;
	}
	if (compare("MMIO write, read back via DMA", off, want, got, len))
		return -1;

	/* 越界：设备判 BAD_RANGE，驱动返回 EINVAL */
	errno = 0;
	if (do_dma(fd, EDU_DMA_DIR_FROM_DEVICE, EDU_BUF_SIZE - 8, 16, got) == 0 || errno != EINVAL) {
		fprintf(stderr, "out-of-range DMA: want EINVAL, got %s\n",
			errno ? strerror(errno) : "success");
		return -1;
	}

	if (ioctl(fd, EDU_IOC_RAISE, &bit) < 0) {
		fprintf(stderr, "EDU_IOC_RAISE 0x%x: %s\n", bit, strerror(errno));
		return -1;
	}
	return 0;
}

static int selftest(int fd, unsigned rounds)
{
	struct edu_info before, after;
	unsigned r;

	if (get_info(fd, &before))
		return EXIT_FAIL;
	for (r = 0; r < rounds; r++)
		if (one_round(fd, r)) {
			fprintf(stderr, "selftest: round %u failed\n", r);
			return EXIT_FAIL;
		}
	if (get_info(fd, &after))
		return EXIT_FAIL;
	if (after.irq_count - before.irq_count != 4ull * rounds ||
	    after.dma_done - before.dma_done != 3u * rounds) {
		fprintf(stderr, "selftest: irqs +%llu dma +%u, want +%u and +%u\n",
			(unsigned long long)(after.irq_count - before.irq_count),
			after.dma_done - before.dma_done, 4 * rounds, 3 * rounds);
		return EXIT_FAIL;
	}
	if (after.int_status) {
		fprintf(stderr, "selftest: INT_STATUS 0x%x left set\n", after.int_status);
		return EXIT_FAIL;
	}
	printf("selftest: %u rounds ok (%s), irqs +%u, dma +%u\n", rounds,
	       after.irq_mode ? "MSI" : "INTx", 4 * rounds, 3 * rounds);
	return EXIT_OK;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/edu0";
	struct edu_info info;
	unsigned rounds = 1;
	int opt, fd, rc;
	char *end;

	while ((opt = getopt(argc, argv, "d:h")) != -1) {
		switch (opt) {
		case 'd': dev = optarg; break;
		case 'h': usage(stdout); return EXIT_OK;
		default:  usage(stderr); return EXIT_USAGE;
		}
	}
	if (optind >= argc) {
		fprintf(stderr, "missing command (see -h)\n");
		return EXIT_USAGE;
	}
	if (strcmp(argv[optind], "info") && strcmp(argv[optind], "selftest")) {
		fprintf(stderr, "unknown command '%s' (see -h)\n", argv[optind]);
		return EXIT_USAGE;
	}
	if (!strcmp(argv[optind], "selftest") && optind + 1 < argc) {
		rounds = (unsigned)strtoul(argv[optind + 1], &end, 10);
		if (*end || rounds == 0 || rounds > 1000000) {
			fprintf(stderr, "ROUNDS must be 1..1000000, got '%s' (see -h)\n", argv[optind + 1]);
			return EXIT_USAGE;
		}
	}

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		return EXIT_FAIL;
	}
	if (!strcmp(argv[optind], "info")) {
		rc = get_info(fd, &info) ? EXIT_FAIL : EXIT_OK;
		if (rc == EXIT_OK)
			printf("mode %s irqs %llu dma_done %u int_status 0x%x\n",
			       info.irq_mode ? "MSI" : "INTx",
			       (unsigned long long)info.irq_count, info.dma_done, info.int_status);
	} else {
		rc = selftest(fd, rounds);
	}
	close(fd);
	return rc;
}
