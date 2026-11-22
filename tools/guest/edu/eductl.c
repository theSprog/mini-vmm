/* tools/guest/edu/eductl.c — 在 guest 里驱动 /dev/eduN，检查 DMA 和中断闭环
 *
 * selftest 的判据不依赖 DMA 自己的结果：DMA 写进设备缓冲区的数据经 MMIO
 * 窗口（read）读回比对，MMIO 窗口写进去的数据（write）经 DMA 搬回比对。
 * 每一步都等驱动拿到完成中断才返回，所以中断和设备完成计数的增量也是
 * 精确可算的：每轮两次成功的 DMA、一次越界的 DMA（设备报 BAD_RANGE，同样
 * 完成并发中断）、一次软件中断，共 4 个中断、3 次 DMA 完成。MSI-X 下还按
 * 向量对账：DMA 完成都在向量 0，第 r 轮的软件中断位落在哪个字节就算哪个
 * 向量的；每个向量的处理函数被调用的次数也必须等于它认领的次数，多出来的
 * 就是没有原因的消息。
 *
 * masktest 在 MSI-X 下检查屏蔽和 pending 位（PCI 3.0 第 6.8.3.5 节）：屏蔽
 * 期间中断原因出现时，对应的 PBA 位置 1、处理函数没被调用；解除屏蔽后补发
 * 恰好一条、PBA 清 0；屏蔽期间原因被确认掉时 PBA 跟着清 0，解除屏蔽后不再
 * 补发。驱动只交回读到的事实，判断在这里。
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
"       eductl [-d DEV] masktest vector N | function | acked N\n"
"\n"
"  -d DEV     character device (default /dev/edu0)\n"
"  info       print interrupt mode and counters\n"
"  selftest   ROUNDS rounds (default 1) of: DMA to device checked via MMIO,\n"
"             MMIO write checked via DMA from device, an out-of-range DMA that\n"
"             must fail with EINVAL, and a software interrupt; then checks the\n"
"             interrupt and DMA counters grew by exactly 4 and 3 per round,\n"
"             and under MSI-X the count of every vector\n"
"  masktest   MSI-X only. vector N: mask vector N (1-3), raise its interrupt,\n"
"             expect its PBA bit and no call, unmask, expect exactly one call\n"
"             and a clear PBA. function: the same through the Function Mask\n"
"             for vectors 1-3 at once. acked N: mask, raise, acknowledge the\n"
"             cause while masked, expect the PBA bit to clear and no replay\n"
"\n"
"EXAMPLE:\n"
"  eductl info\n"
"  eductl selftest 10\n"
"  eductl masktest vector 2\n"
"  eductl masktest function\n"
"  eductl masktest acked 3\n"
"\n"
"NOTE: needs the edu module loaded (irq_mode=msix for masktest); exit 0 on\n"
"success, 1 on any mismatch, 2 on a usage error.\n");
}

static const char *const mode_name[] = {
	[EDU_MODE_INTX] = "INTx", [EDU_MODE_MSI] = "MSI", [EDU_MODE_MSIX] = "MSI-X",
};

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

/* 软件中断位 bit 由哪个向量负责：单向量时都是 0，MSI-X 时按字节 */
static unsigned vec_of(const struct edu_info *info, uint32_t bit)
{
	unsigned n;

	for (n = 0; n < info->nvec; n++)
		if (info->nvec == 1 || (bit & EDU_INT_VEC_MASK(n)))
			return n;
	return 0;
}

static int selftest(int fd, unsigned rounds)
{
	unsigned long long want[EDU_MSIX_VECTORS] = { 0 };
	struct edu_info before, after;
	unsigned r, n;
	int bad = 0;

	if (get_info(fd, &before))
		return EXIT_FAIL;
	if (before.nvec < 1 || before.nvec > EDU_MSIX_VECTORS) {
		fprintf(stderr, "selftest: driver reports %u vectors\n", before.nvec);
		return EXIT_FAIL;
	}
	for (r = 0; r < rounds; r++) {
		if (one_round(fd, r)) {
			fprintf(stderr, "selftest: round %u failed\n", r);
			return EXIT_FAIL;
		}
		want[0] += 3;
		want[vec_of(&before, 1u << (8 + r % 24))] += 1;
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
	for (n = 0; n < after.nvec; n++) {
		unsigned long long h = after.vec_handled[n] - before.vec_handled[n];
		unsigned long long c = after.vec_calls[n] - before.vec_calls[n];

		if (h != want[n] || c != want[n]) {
			fprintf(stderr, "selftest: vector %u handled +%llu called +%llu, want +%llu\n",
				n, h, c, want[n]);
			bad = 1;
		}
	}
	if (bad)
		return EXIT_FAIL;
	if (after.int_status) {
		fprintf(stderr, "selftest: INT_STATUS 0x%x left set\n", after.int_status);
		return EXIT_FAIL;
	}
	printf("selftest: %u rounds ok (%s), irqs +%u, dma +%u", rounds,
	       mode_name[after.irq_mode % 3], 4 * rounds, 3 * rounds);
	if (after.nvec > 1) {
		printf(", per vector");
		for (n = 0; n < after.nvec; n++)
			printf(" +%llu", want[n]);
	}
	printf("\n");
	return EXIT_OK;
}

static void print_calls(const char *what, const __u64 *c)
{
	unsigned n;

	printf(" %s", what);
	for (n = 0; n < EDU_MSIX_VECTORS; n++)
		printf(" +%llu", (unsigned long long)c[n]);
}

/* 判据见文件头。want_masked/want_after 是每个向量应有的调用次数 */
static int masktest(int fd, unsigned kind, unsigned vector)
{
	static const char *const kind_name[] = { "vector", "function", "acked" };
	struct edu_masktest t = { .kind = kind, .vector = vector };
	unsigned long long want_pba = 0;
	__u64 want_after[EDU_MSIX_VECTORS] = { 0 };
	unsigned n;
	int r, bad = 0;

	if (kind == EDU_MASKTEST_FUNCTION) {
		for (n = 1; n < EDU_MSIX_VECTORS; n++) {
			want_pba |= 1ull << n;
			want_after[n] = 1;
		}
	} else {
		want_pba = 1ull << vector;
		want_after[vector] = 1;
	}

	r = ioctl(fd, EDU_IOC_MASKTEST, &t);
	if (r < 0 && errno != ETIMEDOUT) {
		fprintf(stderr, "EDU_IOC_MASKTEST: %s\n", strerror(errno));
		return EXIT_FAIL;
	}
	printf("masktest %s", kind_name[kind]);
	if (kind != EDU_MASKTEST_FUNCTION)
		printf(" %u", vector);
	printf(": masked pba 0x%llx", (unsigned long long)t.pba_masked);
	print_calls("calls", t.calls_masked);
	if (kind == EDU_MASKTEST_ACKED)
		printf("; acked pba 0x%llx", (unsigned long long)t.pba_acked);
	printf("; after pba 0x%llx", (unsigned long long)t.pba_after);
	print_calls("calls", t.calls_after);
	printf("%s\n", r < 0 ? "; TIMEOUT waiting for the interrupt" : "");

	if (r < 0)
		bad = 1;
	if (t.pba_masked != want_pba) {
		fprintf(stderr, "masktest: PBA 0x%llx while masked, want 0x%llx\n",
			(unsigned long long)t.pba_masked, want_pba);
		bad = 1;
	}
	for (n = 0; n < EDU_MSIX_VECTORS; n++)
		if (t.calls_masked[n]) {
			fprintf(stderr, "masktest: vector %u called while masked\n", n);
			bad = 1;
		}
	if (kind == EDU_MASKTEST_ACKED && t.pba_acked) {
		fprintf(stderr, "masktest: PBA 0x%llx after the cause was acknowledged, want 0\n",
			(unsigned long long)t.pba_acked);
		bad = 1;
	}
	if (t.pba_after) {
		fprintf(stderr, "masktest: PBA 0x%llx at the end, want 0\n",
			(unsigned long long)t.pba_after);
		bad = 1;
	}
	for (n = 0; n < EDU_MSIX_VECTORS; n++)
		if (t.calls_after[n] != want_after[n]) {
			fprintf(stderr, "masktest: vector %u called %llu times, want %llu\n", n,
				(unsigned long long)t.calls_after[n],
				(unsigned long long)want_after[n]);
			bad = 1;
		}
	if (!bad)
		printf("masktest %s: ok\n", kind_name[kind]);
	return bad ? EXIT_FAIL : EXIT_OK;
}

/* "vector N" / "function" / "acked N"，N 为 1..EDU_MSIX_VECTORS-1 */
static int parse_masktest(int argc, char **argv, unsigned *kind, unsigned *vector)
{
	char *end;

	if (argc < 1)
		return -1;
	if (!strcmp(argv[0], "function") && argc == 1) {
		*kind = EDU_MASKTEST_FUNCTION;
		*vector = 0;
		return 0;
	}
	if (argc != 2)
		return -1;
	if (!strcmp(argv[0], "vector"))
		*kind = EDU_MASKTEST_VECTOR;
	else if (!strcmp(argv[0], "acked"))
		*kind = EDU_MASKTEST_ACKED;
	else
		return -1;
	*vector = (unsigned)strtoul(argv[1], &end, 10);
	if (*end || *vector < 1 || *vector >= EDU_MSIX_VECTORS)
		return -1;
	return 0;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/edu0";
	struct edu_info info;
	unsigned rounds = 1, kind = 0, vector = 0, n;
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
	if (strcmp(argv[optind], "info") && strcmp(argv[optind], "selftest") &&
	    strcmp(argv[optind], "masktest")) {
		fprintf(stderr, "unknown command '%s' (see -h)\n", argv[optind]);
		return EXIT_USAGE;
	}
	if (!strcmp(argv[optind], "masktest") &&
	    parse_masktest(argc - optind - 1, argv + optind + 1, &kind, &vector)) {
		fprintf(stderr, "masktest takes 'vector N', 'function' or 'acked N' with N 1..%d (see -h)\n",
			EDU_MSIX_VECTORS - 1);
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
		if (rc == EXIT_OK) {
			printf("mode %s irqs %llu dma_done %u int_status 0x%x vectors %u",
			       mode_name[info.irq_mode % 3], (unsigned long long)info.irq_count,
			       info.dma_done, info.int_status, info.nvec);
			for (n = 0; n < info.nvec && n < EDU_MSIX_VECTORS; n++)
				printf(" v%u=%llu/%llu", n, (unsigned long long)info.vec_handled[n],
				       (unsigned long long)info.vec_calls[n]);
			printf("\n");
		}
	} else if (!strcmp(argv[optind], "masktest")) {
		rc = masktest(fd, kind, vector);
	} else {
		rc = selftest(fd, rounds);
	}
	close(fd);
	return rc;
}
