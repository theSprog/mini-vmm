/* tests/fuzz/bzimage_fuzz.c — bzimage_load() 的 libFuzzer target
 *
 * 关注点：setup_sects / syssize / init_size / pref_address /
 * kernel_version 这几个字段都直接来自文件，且参与地址与长度运算。
 * 其中 file[0x201]（短跳转偏移）决定 hdr_len，kernel_version 决定一次
 * 相对文件起点的字符串读取——两处都是典型的“文件说了算”的越界候选。
 *
 * 用法（由 tests/offline/fuzz.sh 驱动）：
 *   build/fuzz/bzimage_fuzz -max_total_time=300 build/fuzz/corpus/bzimage
 */
#include "fuzz_common.h"
#include "boot/kernel_loader.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct kernel_image img;
    struct fuzz_input in;

    if (size == 0)
        return 0;

    fuzz_ram_init();
    if (fuzz_input_open(&in, data, size) != 0)
        return 0;

    memset(&img, 0xCC, sizeof(img));
    (void)bzimage_load(NULL, in.path, &img);

    fuzz_input_close(&in);
    return 0;
}
