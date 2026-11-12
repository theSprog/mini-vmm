# ref — 入库的规范与手册

里程碑 `参考` 字段里拿不到稳定在线副本、又要反复查阅的硬件手册放在这里。厂商常常改链接或给下载加访问限制，入库后实现和笔记里引用的章节号始终对应同一份文件。Linux 头文件、内核源码、VirtIO spec 这类有稳定版本号可查的资料不入库。

| 文件 | 文档 | 用于 | 来源 | sha256 |
|---|---|---|---|---|
| `8254x_GBe_SDM.pdf` | Intel 8254x Family of Gigabit Ethernet Controllers Software Developer's Manual（doc 317453），410 页，覆盖 82540EM | emu-net | Intel 原链接 `pcie-gbe-controllers-open-source-manual.pdf` 对脚本下载返回 403，取自 MIT 6.828 镜像 `https://pdos.csail.mit.edu/6.828/2019/readings/hardware/8254x_GBe_SDM.pdf` | `aa6ea7ee1484dcd431a7ca67d2ec5151b006ca3bfbe741c98e14e34edaefdb4b` |
| `93XX46_EEPROM_DS21749H.pdf` | Microchip 93AA46/93LC46/93C46 1K Microwire Serial EEPROM 数据手册（DS21749H） | emu-net：82540EM 外挂的 64 字 EEPROM，EECD 位操作的时序 | `https://ww1.microchip.com/downloads/en/DeviceDoc/21749H.pdf` | `8d358ab574f3ba93b59b60895c3b5e418499275669d3b6d643c899c87dee6780` |
| `serial-ata-ahci-spec-rev1-3-1.pdf` | Serial ATA AHCI 1.3.1 Specification，137 页 | emu-blk：HBA 与端口寄存器、命令列表、命令表、PRDT、Received FIS 区、中断 | Intel 原链接 `https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/serial-ata-ahci-spec-rev1-3-1.pdf` 对脚本下载返回 403，取自 UW CSE 451 镜像 `https://courses.cs.washington.edu/courses/cse451/21sp/readings/ahci.pdf` | `3bbd4b7fc7dbef60948f3e56575d860ffc02a4d3571a20cd757aa2b22fa8e56a` |
| `SerialATA_Revision_3_0_Gold.pdf` | Serial ATA Revision 3.0 Gold，663 页 | emu-blk：FIS 的字节布局（第 10.3 节）和设备签名。AHCI 规范只规定 FIS 放在内存的哪个偏移，FIS 的内容由这份定义 | SATA-IO `https://sata-io.org/system/files/specifications/SerialATA_Revision_3_0_Gold.pdf` | `44f97a66a8aab2df66e7193bdc85b6c9dfe120fb693026d2d65ee0c3b9c64fb0` |
| `ATA8-ACS_d1699r4a.pdf` | T13/1699-D ATA8-ACS 工作草案 Revision 4a，462 页 | emu-blk：IDENTIFY DEVICE 数据各 word、READ/WRITE DMA EXT 与 FLUSH CACHE EXT 的语义和状态位 | T13 的草案下载需要登录，取自 Georgia Tech CS3210 镜像 `https://tc.gtisc.gatech.edu/cs3210/2016/fall/r/hardware/ATA8-ACS.pdf` | `b76fcd8fbbb47c2d2ac4f197fb00c6a95ad199d702e26f8744b941d2b91333ac` |

换版本时更新 sha256，并检查代码注释里引用的章节号是否还对得上。
