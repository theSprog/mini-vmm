/* include/devices/pci_test.h — 验证 PCI 总线用的最小设备（--pci-test）
 *
 * pci-bus 里程碑要证明“guest 能枚举到设备、BAR 能被探测和搬动、访问能
 * 路由到设备”，这需要一个总线上的设备，但还不需要任何设备语义。这个
 * 设备只有两个 BAR，背后是同一组寄存器：
 *
 *   BAR0  32 位 MMIO，4 KiB
 *   BAR1  IO，16 字节
 *
 *   偏移  宽度  访问  内容
 *   0x0   4     RO    PCI_TEST_MAGIC，按字节读是 "test"
 *   0x4   4     RW    scratch：写什么读回什么，两个 BAR 共享
 *   0x8   4     RO    这次访问经过的 BAR 编号（0 或 1）
 *   其余        RO    读 0，写丢弃
 *
 * 寄存器按字节建模，任意宽度、任意偏移的访问都按小端拼出结果，
 * 所以 guest 不论用 devmem 的哪种宽度、还是 dd sysfs 的 resource 文件
 * （内核可能拆成更小的访问），都能读到完整的值。
 * scratch 共享是为了一次验证两条路：从 IO BAR 写进去的值，经 MMIO BAR
 * 读出来，说明两次访问落到了同一个设备上。
 *
 * class 0xFF0000（“不属于任何已定义类别”，PCI Code and ID Assignment
 * Spec）、ID 1234:0002，guest 不会为它绑定驱动。INTA 用来检查 _PRT 和
 * MP table 给出的路由；设备本身从不触发中断。
 */
#ifndef DEVICES_PCI_TEST_H
#define DEVICES_PCI_TEST_H

#include <stdint.h>

#include "pci/pci.h"

#define PCI_DEVICE_ID_TEST    0x0002
#define PCI_TEST_MAGIC        0x74736574u      /* 't' 'e' 's' 't' */
#define PCI_TEST_REG_MAGIC    0x0
#define PCI_TEST_REG_SCRATCH  0x4
#define PCI_TEST_REG_BAR      0x8
#define PCI_TEST_REGS_LEN     12
#define PCI_TEST_MMIO_SIZE    0x1000
#define PCI_TEST_IO_SIZE      16

struct pci_test;

struct pci_test_bar {
    struct pci_test *dev;
    uint32_t         index;
};

struct pci_test {
    struct pci_device   pci;
    struct pci_test_bar bar_ctx[2];
    uint32_t            scratch;
};

int pci_test_attach(struct pci_test *t, struct pci_bus *bus);

#endif /* DEVICES_PCI_TEST_H */
