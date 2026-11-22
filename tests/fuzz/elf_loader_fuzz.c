/* tests/fuzz/elf_loader_fuzz.c — elf_load_vmlinux() 的 libFuzzer target
 *
 * 重点关注的是 src/boot/elf_loader.c 里两处“两个 64 位文件字段相加再和
 * 文件大小比较”的检查：
 *   check_ehdr():  e_phoff + e_phnum * sizeof(Elf64_Phdr) > file_size
 *   PT_LOAD 循环:  p_offset + p_filesz > st.st_size
 * 相比之下 src/mem.c 的 gpa_to_hva() 特意写了 `if (gpa + len < gpa)`
 * 并注释了原因。同一个仓库里两种写法，这个 target 就是来判定的。
 *
 * 用法（由 tests/offline/fuzz.sh 驱动）：
 *   build/fuzz/elf_loader_fuzz -max_total_time=300 build/fuzz/corpus/elf
 */
#include "fuzz_common.h"
#include "boot/elf_loader.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct elf_load_info info;
    struct fuzz_input in;

    /* mmap 长度为 0 会失败，加载器自己会报错返回；这里直接跳过，
     * 免得把 fuzz 预算花在一个已知的平凡分支上。 */
    if (size == 0)
        return 0;

    fuzz_ram_init();
    if (fuzz_input_open(&in, data, size) != 0)
        return 0;

    memset(&info, 0xCC, sizeof(info));
    (void)elf_load_vmlinux(NULL, in.path, &info);

    fuzz_input_close(&in);
    return 0;
}
