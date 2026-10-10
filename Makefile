CC = clang
CFLAGS = --target=i386-elf -ffreestanding -O2 -Wall -Wextra -fno-pic -fno-stack-protector -fno-builtin -fno-omit-frame-pointer -m32 -mno-sse -mno-sse2 -mno-mmx
ASFLAGS = --target=i386-elf -ffreestanding -m32
LDFLAGS = -m elf_i386 -T src/linker.ld -nostdlib -static

BUILD_DIR = build
TARGET = $(BUILD_DIR)/kernel.elf
OBJ_BOOT = $(BUILD_DIR)/boot.o
OBJ_INT = $(BUILD_DIR)/interrupts.o
OBJ_KERNEL = $(BUILD_DIR)/kernel.o

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(OBJ_BOOT): src/boot.s | $(BUILD_DIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(OBJ_INT): src/interrupts.s | $(BUILD_DIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(OBJ_KERNEL): src/kernel.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJ_BOOT) $(OBJ_INT) $(OBJ_KERNEL)
	ld.lld $(LDFLAGS) -o $@ $(OBJ_BOOT) $(OBJ_INT) $(OBJ_KERNEL)

run: $(TARGET)
	qemu-system-i386 -kernel $(TARGET) -serial stdio -display none

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all run clean
