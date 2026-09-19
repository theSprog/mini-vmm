/* tests/unit/io_bus_mutants.c — io_bus_dispatch_* 的故意错误实现
 *
 * 每个 mutant 对应 include/vmm.h 与 HANDOFF_STEP2_CN.md 里写明的一条
 * 分发契约。写法照抄真实实现，只改目标那一处。
 */
#include <string.h>

#include "mutants.h"

/* M1：未注册端口读回 0x00 而不是 0xFF。
 * 判据出处 arch/x86/kernel/i8237.c 的 i8237A_init_ops()：
 *   if (dma_inb(DMA_PAGE_0) == 0xFF) return -ENODEV;
 * 读回 0x00 的话 Linux 会认为这台机器有 8237 DMA 控制器。 */
static int m_unhandled_reads_zero(struct vmm_vcpu *vcpu, uint16_t port,
                                  bool is_write, uint32_t size,
                                  uint32_t count, uint8_t *data)
{
    const struct vmm_io_dev *dev = vmm_lookup_pio(vcpu->vm, port);
    uint32_t i;

    if (!dev) {
        if (!is_write)
            memset(data, 0x00, (size_t)size * count);
        return VMM_OK;
    }
    for (i = 0; i < count; i++) {
        uint8_t *p = data + (size_t)i * size;
        uint64_t off = port - dev->base;
        int r = is_write
              ? (dev->write ? dev->write(dev->opaque, off, size, p) : VMM_OK)
              : (dev->read  ? dev->read(dev->opaque, off, size, p)  : VMM_OK);
        if (r != VMM_OK)
            return r;
    }
    return VMM_OK;
}

/* M2：offset 用绝对端口号，没有减去 dev->base。
 * 设备实现不该知道自己被挂在哪里，这是 virtio-mmio 多实例的前提。 */
static int m_absolute_offset(struct vmm_vcpu *vcpu, uint16_t port,
                             bool is_write, uint32_t size, uint32_t count,
                             uint8_t *data)
{
    const struct vmm_io_dev *dev = vmm_lookup_pio(vcpu->vm, port);
    uint32_t i;

    if (!dev) {
        if (!is_write)
            memset(data, 0xFF, (size_t)size * count);
        return VMM_OK;
    }
    for (i = 0; i < count; i++) {
        uint8_t *p = data + (size_t)i * size;
        int r = is_write
              ? (dev->write ? dev->write(dev->opaque, port, size, p) : VMM_OK)
              : (dev->read  ? dev->read(dev->opaque, port, size, p)  : VMM_OK);
        if (r != VMM_OK)
            return r;
    }
    return VMM_OK;
}

/* M3：忽略 count，只调一次回调。串口用 outsb 批量输出时会丢字节。 */
static int m_ignores_count(struct vmm_vcpu *vcpu, uint16_t port,
                           bool is_write, uint32_t size, uint32_t count,
                           uint8_t *data)
{
    const struct vmm_io_dev *dev = vmm_lookup_pio(vcpu->vm, port);
    uint64_t off;

    if (!dev) {
        if (!is_write)
            memset(data, 0xFF, (size_t)size * count);
        return VMM_OK;
    }
    off = port - dev->base;
    if (is_write)
        return dev->write ? dev->write(dev->opaque, off, size, data) : VMM_OK;
    return dev->read ? dev->read(dev->opaque, off, size, data) : VMM_OK;
}

/* M4：回调返回错误后继续跑完剩下的 count。
 * VMM_ERR_EXIT 是 guest 请求关机/复位的约定信号，i8042 的 0xFE 脉冲
 * 之后再执行剩下的访问是没有意义的。 */
static int m_continues_after_error(struct vmm_vcpu *vcpu, uint16_t port,
                                   bool is_write, uint32_t size,
                                   uint32_t count, uint8_t *data)
{
    const struct vmm_io_dev *dev = vmm_lookup_pio(vcpu->vm, port);
    uint32_t i;
    int last = VMM_OK;

    if (!dev) {
        if (!is_write)
            memset(data, 0xFF, (size_t)size * count);
        return VMM_OK;
    }
    for (i = 0; i < count; i++) {
        uint8_t *p = data + (size_t)i * size;
        uint64_t off = port - dev->base;
        int r = is_write
              ? (dev->write ? dev->write(dev->opaque, off, size, p) : VMM_OK)
              : (dev->read  ? dev->read(dev->opaque, off, size, p)  : VMM_OK);
        if (r != VMM_OK)
            last = r;
    }
    return last;
}

/* M5：未注册端口只填一个字节，剩下的留原样。
 * 表现是 guest 读到半截脏数据，而不是干净的全 1。 */
static int m_unhandled_fills_one_byte(struct vmm_vcpu *vcpu, uint16_t port,
                                      bool is_write, uint32_t size,
                                      uint32_t count, uint8_t *data)
{
    const struct vmm_io_dev *dev = vmm_lookup_pio(vcpu->vm, port);
    uint32_t i;

    if (!dev) {
        if (!is_write && size * count > 0)
            data[0] = 0xFF;
        return VMM_OK;
    }
    for (i = 0; i < count; i++) {
        uint8_t *p = data + (size_t)i * size;
        uint64_t off = port - dev->base;
        int r = is_write
              ? (dev->write ? dev->write(dev->opaque, off, size, p) : VMM_OK)
              : (dev->read  ? dev->read(dev->opaque, off, size, p)  : VMM_OK);
        if (r != VMM_OK)
            return r;
    }
    return VMM_OK;
}

/* M6：串操作的步长写成 1 而不是 size。size=2 时会重复读同一批字节。 */
static int m_wrong_stride(struct vmm_vcpu *vcpu, uint16_t port,
                          bool is_write, uint32_t size, uint32_t count,
                          uint8_t *data)
{
    const struct vmm_io_dev *dev = vmm_lookup_pio(vcpu->vm, port);
    uint32_t i;

    if (!dev) {
        if (!is_write)
            memset(data, 0xFF, (size_t)size * count);
        return VMM_OK;
    }
    for (i = 0; i < count; i++) {
        uint8_t *p = data + i;
        uint64_t off = port - dev->base;
        int r = is_write
              ? (dev->write ? dev->write(dev->opaque, off, size, p) : VMM_OK)
              : (dev->read  ? dev->read(dev->opaque, off, size, p)  : VMM_OK);
        if (r != VMM_OK)
            return r;
    }
    return VMM_OK;
}

/* M7（MMIO 侧）：未注册地址不填 0xFF，直接原样返回 */
static int m_mmio_unhandled_untouched(struct vmm_vcpu *vcpu, uint64_t addr,
                                      bool is_write, uint32_t len,
                                      uint8_t *data)
{
    const struct vmm_io_dev *dev = vmm_lookup_mmio(vcpu->vm, addr);
    uint64_t off;

    if (!dev)
        return VMM_OK;
    off = addr - dev->base;
    if (is_write)
        return dev->write ? dev->write(dev->opaque, off, len, data) : VMM_OK;
    return dev->read ? dev->read(dev->opaque, off, len, data) : VMM_OK;
}

/* M8（MMIO 侧）：offset 用绝对地址 */
static int m_mmio_absolute_offset(struct vmm_vcpu *vcpu, uint64_t addr,
                                  bool is_write, uint32_t len, uint8_t *data)
{
    const struct vmm_io_dev *dev = vmm_lookup_mmio(vcpu->vm, addr);

    if (!dev) {
        if (!is_write)
            memset(data, 0xFF, len);
        return VMM_OK;
    }
    if (is_write)
        return dev->write ? dev->write(dev->opaque, addr, len, data) : VMM_OK;
    return dev->read ? dev->read(dev->opaque, addr, len, data) : VMM_OK;
}

const struct io_mutant io_mutants[] = {
    { "unhandled-reads-zero",  "未注册端口必须读回 0xFF",        m_unhandled_reads_zero,      NULL },
    { "absolute-offset",       "offset 必须相对 dev->base",       m_absolute_offset,           NULL },
    { "ignores-count",         "count>1 必须逐次回调",            m_ignores_count,             NULL },
    { "continues-after-error", "回调返回非 OK 必须立即中止",      m_continues_after_error,     NULL },
    { "unhandled-one-byte",    "未注册读必须填满 size*count",     m_unhandled_fills_one_byte,  NULL },
    { "wrong-stride",          "串操作每次前进 size 字节",        m_wrong_stride,              NULL },
    { "mmio-unhandled-untouched", "未注册 MMIO 读必须填 0xFF",    NULL, m_mmio_unhandled_untouched },
    { "mmio-absolute-offset",  "MMIO offset 必须相对 dev->base",  NULL, m_mmio_absolute_offset     },
};

const int io_mutants_count = (int)(sizeof(io_mutants) / sizeof(io_mutants[0]));
