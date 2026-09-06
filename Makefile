CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -std=gnu11 -Iinclude
AS      := as
LD      := ld
OBJCOPY := objcopy

BUILD_DIR := build

.PHONY: all clean run

all: $(BUILD_DIR)/vmm $(BUILD_DIR)/payload.bin

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/vmm: src/vm.c include/vmm.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ src/vm.c

$(BUILD_DIR)/payload.o: tools/payload/real_mode_stub.S | $(BUILD_DIR)
	$(AS) --32 -o $@ $<

$(BUILD_DIR)/payload.elf: $(BUILD_DIR)/payload.o
	$(LD) -m elf_i386 -N -e _start -Ttext 0x1000 -o $@ $<

$(BUILD_DIR)/payload.bin: $(BUILD_DIR)/payload.elf
	$(OBJCOPY) -O binary $< $@

run: all
	./$(BUILD_DIR)/vmm $(BUILD_DIR)/payload.bin

clean:
	rm -rf $(BUILD_DIR)
