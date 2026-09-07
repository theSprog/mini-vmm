# Makefile — mini-vmm，基于 KVM 的极简 VMM
#
# Step 1 只编译下面 SRCS 里列出的文件。src/ 下其余 .c 目前仍是占位注释，
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
	src/boot/gdt_pgtable.c

OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

PAYLOADS := $(BUILD)/payload16.bin $(BUILD)/payload64.bin

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