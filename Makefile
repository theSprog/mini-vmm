# Makefile — mini-vmm，基于 KVM 的极简 VMM
#
# 只编译下面 SRCS 里列出的文件。src/ 下其余 .c 目前仍是占位注释，
# 会在后续 Step 逐个填充并加进这个列表。
#
# 验收测试不放在这里，见 tests/ 目录：
#   ./tests/step1.sh

CC      := gcc
AS      := as
LD      := ld
OBJCOPY := objcopy

CFLAGS  := -Wall -Wextra -Wno-unused-parameter -O2 -g -std=gnu11 -Iinclude
LDFLAGS := -pthread

BUILD   := build

SRCS := \
	src/log.c \
	src/kvm_wrappers.c \
	src/mem.c \
	src/vm.c \
	src/main.c \
	src/arch/arch_detect.c \
	src/arch/amd/svm.c \
	src/boot/gdt_pgtable.c \
	src/devices/serial8250.c \
	src/devices/irqchip.c \
	src/devices/i8042.c \
	src/boot/elf_loader.c \
	src/boot/bzimage.c \
	src/boot/kernel_loader.c \
	src/boot/zeropage.c \
	src/boot/mptable.c \
	src/boot/acpi.c \
	src/devices/rtc.c \
	src/console.c \
	src/smp.c

OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

PAYLOADS := $(BUILD)/payload16.bin $(BUILD)/payload64.bin $(BUILD)/serial64.bin

.PHONY: all clean cc-json

all: $(BUILD)/vmm $(PAYLOADS)

$(BUILD)/vmm: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

# ---- payload ----
# 16 位载荷用 --32 汇编（.code16 需要 32 位目标），链接到 0x0。
$(BUILD)/payload16.elf: tools/payload/real_mode_stub.S
	@mkdir -p $(dir $@)
	$(AS) --32 -o $(BUILD)/payload16.o $<
	$(LD) -m elf_i386 -N -e _start -Ttext 0x0 -o $@ $(BUILD)/payload16.o

# 64 位载荷链接到 0x300000，和 VMM 里的 PAYLOAD_LONG_GPA 保持一致。
$(BUILD)/payload64.elf: tools/payload/long_mode_stub.S
	@mkdir -p $(dir $@)
	$(AS) --64 -o $(BUILD)/payload64.o $<
	$(LD) -m elf_x86_64 -N -e _start -Ttext 0x300000 -o $@ $(BUILD)/payload64.o

# Step 2.1 串口载荷，和 payload64 一样放 0x300000，用 cpp 预处理 #define。
$(BUILD)/serial64.elf: tools/payload/serial_stub.S
	@mkdir -p $(dir $@)
	$(CC) -c -o $(BUILD)/serial64.o $<
	$(LD) -m elf_x86_64 -N -e _start -Ttext 0x300000 -o $@ $(BUILD)/serial64.o

$(BUILD)/%.bin: $(BUILD)/%.elf
	$(OBJCOPY) -O binary $< $@

# ---- compile_commands.json ----
#
# 给 clangd / ccls / IDE 用的编译数据库。
#
# 这里是直接从 SRCS 列表生成，而不是用 bear/compiledb 之类的工具去
# 拦截真实编译过程。原因有两个：一是不引入额外依赖，服务器上现装
# bear 未必顺手；二是本项目的编译命令是完全均一的（同一份 CFLAGS，
# 没有 per-file 的特殊开关），拦截真实构建并不会得到更多信息。
#
# 如果哪天出现 per-file 的编译差异（比如某个文件要单独关优化），
# 就该换成：bear -- make -B
#
# 注意：clangd 会用这里的 CFLAGS 去解析头文件，所以 -Iinclude 必须在，
# 否则跳转到 "vmm.h" 会失败。
cc-json: compile_commands.json

compile_commands.json: Makefile
	@echo '[' > $@
	@first=1; for f in $(SRCS); do \
		obj="$(BUILD)/$${f#src/}"; obj="$${obj%.c}.o"; \
		if [ $$first -eq 0 ]; then echo ',' >> $@; fi; \
		first=0; \
		printf '  {\n    "directory": "%s",\n    "file": "%s",\n    "output": "%s",\n    "arguments": [' \
			"$(CURDIR)" "$(CURDIR)/$$f" "$(CURDIR)/$$obj" >> $@; \
		sep=''; \
		for a in $(CC) $(CFLAGS) -c -o "$$obj" "$$f"; do \
			printf '%s"%s"' "$$sep" "$$a" >> $@; sep=', '; \
		done; \
		printf ']\n  }' >> $@; \
	done
	@echo '' >> $@
	@echo ']' >> $@
	@echo "generated $@ ($(words $(SRCS)) entries)"

clean:
	rm -rf $(BUILD) compile_commands.json
# ---- 验收测试 ----
#
#   make test              默认集合：unit tables step1 step2 step6 step7
#   make test step1        只跑一个 suite
#   make test unit fuzz    跑多个
#   make test all          默认集合 + fuzz
#   make test-list         列出所有 suite
#
# 分工延续 tests/README.md 那条：make 负责“把要跑的东西编出来”，
# tests/ 负责“判断产物对不对”。所以这里只有构建规则和一次转发，
# 断言逻辑一行都不放在 Makefile 里。

TEST_SUITES := all offline e2e unit negctl tables fuzz step1 step2 step6 step7

# `make test step1` 里的 step1 对 make 来说也是一个 goal，不声明的话
# 会报 "No rule to make target 'step1'"。all 例外——它是真实目标。
TEST_SEL      := $(filter $(TEST_SUITES),$(MAKECMDGOALS))
TEST_SEL_NOOP := $(filter-out all,$(TEST_SEL))
ifneq ($(TEST_SEL_NOOP),)
$(TEST_SEL_NOOP):
	@:
endif

# 单元测试一律带 ASan + UBSan。-fno-sanitize-recover=all 是关键：
# 默认 UBSan 只打一行警告然后继续跑，退出码仍是 0，等于没测。
CFLAGS_TEST := -Wall -Wextra -Wno-unused-parameter -O1 -g -std=gnu11 \
               -Iinclude -Itests/unit \
               -fsanitize=address,undefined -fno-sanitize-recover=all \
               -fno-omit-frame-pointer

FUZZ_CC     := clang
FUZZ_CFLAGS := -Wall -Wextra -Wno-unused-parameter -O1 -g -std=gnu11 \
               -Iinclude -Itests/fuzz \
               -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
               -fno-omit-frame-pointer

UNIT_BINS := $(BUILD)/tests/boot_tables_test \
             $(BUILD)/tests/boot_tables_test_noalign \
             $(BUILD)/tests/mem_test \
             $(BUILD)/tests/io_bus_test

# 同一份 boot_tables_test 编两遍，理由和 fuzz 那两个 target 一样：
# 严格版负责报出未对齐访问这类未定义行为（unit suite 用它），
# 关掉 alignment 检查的版本负责让 iasl 这个外部 oracle 还能跑起来
# （tables suite 用它）。否则一处 UB 会把一整类判据全挡在门外。
CFLAGS_TEST_NOALIGN := $(CFLAGS_TEST) -fno-sanitize=alignment

FUZZ_BINS := $(BUILD)/fuzz/elf_loader_fuzz \
             $(BUILD)/fuzz/bzimage_fuzz \
             $(BUILD)/fuzz/elf_loader_fuzz_noalign \
             $(BUILD)/fuzz/bzimage_fuzz_noalign

# 为什么每个加载器有两个 target：
# 严格版一旦发现未对齐访问就立刻终止（-fno-sanitize-recover），这在
# x86 上虽然跑得通、但确实是 C 标准里的未定义行为，值得单独报出来。
# 问题是它会把 fuzz 预算全部消耗在同一个浅层发现上，越界和整数溢出这些
# 更值钱的目标反而永远探不到。所以再编一份关掉 alignment 检查的，
# 用它去跑长时间的深度探索；ASan 与其余 UBSan 检查都还在。
FUZZ_CFLAGS_NOALIGN := $(FUZZ_CFLAGS) -fno-sanitize=alignment

$(BUILD)/tests/boot_tables_test: tests/unit/boot_tables_test.c \
                                 src/boot/mptable.c src/boot/acpi.c \
                                 src/boot/zeropage.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_TEST) -o $@ $^

$(BUILD)/tests/boot_tables_test_noalign: tests/unit/boot_tables_test.c \
                                         src/boot/mptable.c src/boot/acpi.c \
                                         src/boot/zeropage.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_TEST_NOALIGN) -o $@ $^

$(BUILD)/tests/mem_test: tests/unit/mem_test.c tests/unit/mem_mutants.c \
                         src/mem.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_TEST) -o $@ $^

$(BUILD)/tests/io_bus_test: tests/unit/io_bus_test.c \
                            tests/unit/io_bus_mutants.c src/vm.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_TEST) -o $@ $^ -lpthread

$(BUILD)/fuzz/elf_loader_fuzz: tests/fuzz/elf_loader_fuzz.c \
                               src/boot/elf_loader.c
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -o $@ $^

$(BUILD)/fuzz/bzimage_fuzz: tests/fuzz/bzimage_fuzz.c src/boot/bzimage.c
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -o $@ $^

$(BUILD)/fuzz/elf_loader_fuzz_noalign: tests/fuzz/elf_loader_fuzz.c \
                                       src/boot/elf_loader.c
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_CFLAGS_NOALIGN) -o $@ $^

$(BUILD)/fuzz/bzimage_fuzz_noalign: tests/fuzz/bzimage_fuzz.c \
                                    src/boot/bzimage.c
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_CFLAGS_NOALIGN) -o $@ $^

# 没有 clang 就不构建 fuzz target，fuzz suite 会自己 SKIP 并说明原因。
HAVE_FUZZ_CC := $(shell command -v $(FUZZ_CC) 2>/dev/null)

TEST_PREREQ := $(BUILD)/vmm $(PAYLOADS) $(UNIT_BINS)
ifneq ($(HAVE_FUZZ_CC),)
TEST_PREREQ += $(FUZZ_BINS)
endif

.PHONY: test test-list tests-clean $(TEST_SEL_NOOP)

test: $(TEST_PREREQ)
	@./tests/run.sh $(TEST_SEL)

test-list:
	@./tests/run.sh --list

tests-clean:
	rm -rf $(BUILD)/tests $(BUILD)/fuzz
