# tools/guest/edu/Makefile — 编译 edu 驱动模块和 eductl
#
# 在 guest 里对着正在运行的内核编译：KDIR 默认是发行版 linux-headers 装好的
# /lib/modules/$(uname -r)/build。edu_regs.h 放在同一目录（e2e 把它和这些
# 源文件一起拷进 guest），在仓库里则从 include/devices/ 找。
#
#   make            模块 edu.ko 和用户态工具 eductl
#   make KDIR=DIR   对着另一份内核构建树编译模块

obj-m := edu.o
ccflags-y := -I$(src) -I$(src)/../../../include/devices

KDIR ?= /lib/modules/$(shell uname -r)/build

all: modules eductl

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

eductl: eductl.c edu_ioctl.h
	$(CC) -O2 -Wall -Wextra -I. -I../../../include/devices -o $@ eductl.c

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f eductl

.PHONY: all modules clean
