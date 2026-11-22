/* include/boot/image_file.h — 把内核镜像文件整个只读映射进来
 *
 * vmlinux 和 bzImage 的装载器都按文件里记录的偏移去取数据（程序头、
 * setup header、各个段），所以先把整个文件映射成一段连续的只读内存。
 * 打开、映射失败和文件过短都在这里报错，日志带上路径。
 */
#ifndef BOOT_IMAGE_FILE_H
#define BOOT_IMAGE_FILE_H

#include <stddef.h>
#include <stdint.h>

struct image_file {
    const uint8_t *data;
    size_t size;
};

/* min_size 是格式要求的最短长度，文件更短时报 "<path>: too small for
 * <what>"；为 0 时不检查。成功后必须用 image_file_unmap 释放。 */
int  image_file_map(const char *path, size_t min_size, const char *what,
                    struct image_file *f);
void image_file_unmap(struct image_file *f);

#endif /* BOOT_IMAGE_FILE_H */
