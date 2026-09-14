BUILD := build
ISO := $(BUILD)/lionos.iso
KERNEL := $(BUILD)/lionos.bin
USER_ELF := $(BUILD)/hello.elf
USER_OBJS := $(BUILD)/crt0.o $(BUILD)/hello.o $(BUILD)/libc.o
USER_EMBED := $(BUILD)/hello_elf.o

CC := gcc
LD := ld
NASM := nasm
CFLAGS := -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra -Werror -O2 -Iinclude
USER_CFLAGS := -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-builtin -Wall -Wextra -Werror -O2 -Iinclude
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib
USER_LDFLAGS := -m elf_i386 -T user/user.ld -nostdlib

C_SOURCES := $(wildcard src/*.c)
C_OBJECTS := $(patsubst src/%.c,$(BUILD)/%.o,$(C_SOURCES))
ASM_OBJECTS := $(BUILD)/boot.o

.PHONY: all clean iso run check userspace

all: $(KERNEL)

userspace: $(USER_ELF)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: boot/boot.asm | $(BUILD)
	$(NASM) -f elf32 $< -o $@

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/crt0.o: user/crt0.S | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/hello.o: user/hello.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/libc.o: user/libc.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_ELF): $(USER_OBJS) user/user.ld | $(BUILD)
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_OBJS)

$(USER_EMBED): $(USER_ELF) | $(BUILD)
	cd $(BUILD) && $(LD) -r -m elf_i386 -b binary hello.elf -o hello_elf.o

$(KERNEL): $(ASM_OBJECTS) $(C_OBJECTS) $(USER_EMBED) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(ASM_OBJECTS) $(C_OBJECTS) $(USER_EMBED)
	grub-file --is-x86-multiboot2 $@

iso: $(KERNEL)
	mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/lionos.bin
	cp boot/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(BUILD)/iso

check: $(KERNEL)
	grub-file --is-x86-multiboot2 $(KERNEL)

run: iso
	qemu-system-i386 -cdrom $(ISO) -m 128M

clean:
	rm -rf $(BUILD)
