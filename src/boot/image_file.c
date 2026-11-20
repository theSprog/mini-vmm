/* src/boot/image_file.c — 只读映射内核镜像，约定见 include/boot/image_file.h */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "vmm.h"
#include "boot/image_file.h"

int image_file_map(const char *path, size_t min_size, const char *what,
                   struct image_file *f)
{
    struct stat st;
    void *p;
    int fd;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        vmm_err("open %s: %s", path, strerror(errno));
        return VMM_ERR_SYS;
    }
    if (fstat(fd, &st) < 0) {
        vmm_err("fstat %s: %s", path, strerror(errno));
        close(fd);
        return VMM_ERR_SYS;
    }
    if ((uint64_t)st.st_size < min_size) {
        vmm_err("%s: too small for %s", path, what);
        close(fd);
        return VMM_ERR_INVAL;
    }
    /* 映射建立后就不再需要 fd */
    p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        vmm_err("mmap %s: %s", path, strerror(errno));
        return VMM_ERR_SYS;
    }
    f->data = p;
    f->size = (size_t)st.st_size;
    return VMM_OK;
}

void image_file_unmap(struct image_file *f)
{
    munmap((void *)f->data, f->size);
}
