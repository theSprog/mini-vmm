# ref — 入库的规范与手册

里程碑 `参考` 字段里拿不到稳定在线副本、又要反复查阅的硬件手册放在这里。厂商常常改链接或给下载加访问限制，入库后实现和笔记里引用的章节号始终对应同一份文件。Linux 头文件、内核源码、VirtIO spec 这类有稳定版本号可查的资料不入库。

| 文件 | 文档 | 用于 | 来源 | sha256 |
|---|---|---|---|---|
| `8254x_GBe_SDM.pdf` | Intel 8254x Family of Gigabit Ethernet Controllers Software Developer's Manual（doc 317453），410 页，覆盖 82540EM | emu-net | Intel 原链接 `pcie-gbe-controllers-open-source-manual.pdf` 对脚本下载返回 403，取自 MIT 6.828 镜像 `https://pdos.csail.mit.edu/6.828/2019/readings/hardware/8254x_GBe_SDM.pdf` | `aa6ea7ee1484dcd431a7ca67d2ec5151b006ca3bfbe741c98e14e34edaefdb4b` |

换版本时更新 sha256，并检查代码注释里引用的章节号是否还对得上。
