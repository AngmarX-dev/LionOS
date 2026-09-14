BUILD := build
ISO := $(BUILD)/lionos.iso
DISK := $(BUILD)/lionos-disk.img
KERNEL := $(BUILD)/lionos.bin
USER_COMMON_OBJS := $(BUILD)/crt0.o $(BUILD)/libc.o
HELLO_ELF := $(BUILD)/hello.elf
HELLO_OBJS := $(USER_COMMON_OBJS) $(BUILD)/hello.o
HELLO_EMBED := $(BUILD)/hello_elf.o
PROCESS_TEST_ELF := $(BUILD)/process_test.elf
PROCESS_TEST_OBJS := $(USER_COMMON_OBJS) $(BUILD)/process_test.o
PROCESS_TEST_EMBED := $(BUILD)/process_test_elf.o
IPC_TEST_ELF := $(BUILD)/ipc_test.elf
IPC_TEST_OBJS := $(USER_COMMON_OBJS) $(BUILD)/ipc_test.o
IPC_TEST_EMBED := $(BUILD)/ipc_test_elf.o
SIGNAL_TEST_ELF := $(BUILD)/signal_test.elf
SIGNAL_TEST_OBJS := $(USER_COMMON_OBJS) $(BUILD)/signal_test.o
SIGNAL_TEST_EMBED := $(BUILD)/signal_test_elf.o
NET_TEST_ELF := $(BUILD)/net_test.elf
NET_TEST_OBJS := $(USER_COMMON_OBJS) $(BUILD)/net_test.o
NET_TEST_EMBED := $(BUILD)/net_test_elf.o
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
.PHONY: all clean iso disk run check userspace process-test ipc-test signal-test net-test
all: $(KERNEL)
userspace: $(HELLO_ELF)
process-test: $(PROCESS_TEST_ELF)
ipc-test: $(IPC_TEST_ELF)
signal-test: $(SIGNAL_TEST_ELF)
net-test: $(NET_TEST_ELF)
$(BUILD):
	mkdir -p $(BUILD)
$(BUILD)/boot.o: boot/boot.asm | $(BUILD)
	$(NASM) -f elf32 $< -o $@
$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/crt0.o: user/crt0.S | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/libc.o: user/libc.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/hello.o: user/hello.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/process_test.o: user/process_test.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/ipc_test.o: user/ipc_test.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/signal_test.o: user/signal_test.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/net_test.o: user/net_test.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(HELLO_ELF): $(HELLO_OBJS) user/user.ld | $(BUILD)
	$(LD) $(USER_LDFLAGS) -o $@ $(HELLO_OBJS)
$(PROCESS_TEST_ELF): $(PROCESS_TEST_OBJS) user/user.ld | $(BUILD)
	$(LD) $(USER_LDFLAGS) -o $@ $(PROCESS_TEST_OBJS)
$(IPC_TEST_ELF): $(IPC_TEST_OBJS) user/user.ld | $(BUILD)
	$(LD) $(USER_LDFLAGS) -o $@ $(IPC_TEST_OBJS)
$(SIGNAL_TEST_ELF): $(SIGNAL_TEST_OBJS) user/user.ld | $(BUILD)
	$(LD) $(USER_LDFLAGS) -o $@ $(SIGNAL_TEST_OBJS)
$(NET_TEST_ELF): $(NET_TEST_OBJS) user/user.ld | $(BUILD)
	$(LD) $(USER_LDFLAGS) -o $@ $(NET_TEST_OBJS)
$(HELLO_EMBED): $(HELLO_ELF) | $(BUILD)
	cd $(BUILD) && $(LD) -r -m elf_i386 -b binary hello.elf -o hello_elf.o
$(PROCESS_TEST_EMBED): $(PROCESS_TEST_ELF) | $(BUILD)
	cd $(BUILD) && $(LD) -r -m elf_i386 -b binary process_test.elf -o process_test_elf.o
$(IPC_TEST_EMBED): $(IPC_TEST_ELF) | $(BUILD)
	cd $(BUILD) && $(LD) -r -m elf_i386 -b binary ipc_test.elf -o ipc_test_elf.o
$(SIGNAL_TEST_EMBED): $(SIGNAL_TEST_ELF) | $(BUILD)
	cd $(BUILD) && $(LD) -r -m elf_i386 -b binary signal_test.elf -o signal_test_elf.o
$(NET_TEST_EMBED): $(NET_TEST_ELF) | $(BUILD)
	cd $(BUILD) && $(LD) -r -m elf_i386 -b binary net_test.elf -o net_test_elf.o
$(KERNEL): $(ASM_OBJECTS) $(C_OBJECTS) $(HELLO_EMBED) $(PROCESS_TEST_EMBED) $(IPC_TEST_EMBED) $(SIGNAL_TEST_EMBED) $(NET_TEST_EMBED) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(ASM_OBJECTS) $(C_OBJECTS) $(HELLO_EMBED) $(PROCESS_TEST_EMBED) $(IPC_TEST_EMBED) $(SIGNAL_TEST_EMBED) $(NET_TEST_EMBED)
	grub-file --is-x86-multiboot2 $@
iso: $(KERNEL)
	mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/lionos.bin
	cp boot/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(BUILD)/iso
disk: | $(BUILD)
	if [ ! -f $(DISK) ]; then truncate -s 8M $(DISK); fi
check: $(KERNEL)
	grub-file --is-x86-multiboot2 $(KERNEL)
run: iso disk
	qemu-system-i386 -cdrom $(ISO) -drive file=$(DISK),format=raw,if=ide -m 128M
clean:
	rm -rf $(BUILD)
