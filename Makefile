BUILD := build
ISO := $(BUILD)/lionos.iso
DISK := $(BUILD)/lionos-disk.img
KERNEL := $(BUILD)/lionos.bin
USER_COMMON_OBJS := $(BUILD)/crt0.o $(BUILD)/libc.o

USER_PROGRAMS := hello process_test ipc_test signal_test net_test echo cat ls pwd uname rm stat ping userland_test
USER_ELFS := $(addprefix $(BUILD)/,$(addsuffix .elf,$(USER_PROGRAMS)))
USER_EMBEDS := $(addprefix $(BUILD)/,$(addsuffix _elf.o,$(USER_PROGRAMS)))

CC := gcc
LD := ld
NASM := nasm
CFLAGS := -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra -Wno-unused-function -Werror -O2 -Iinclude
ICON_HEADER := $(BUILD)/lion_icons.h
WALLPAPER_HEADER := $(BUILD)/lion_wallpaper.h
ICON_SOURCES := Icons/lionos-icon.png Icons/Terminal-icon.png Icons/Browser-icon.png Icons/Desktop-icon.png Icons/DocumentsFolder-icon.png Icons/Tools-icon.png Icons/OldComputer-icon.png Icons/Home-icon.png Icons/Trash-icon.png
GUI_CFLAGS := $(CFLAGS) -Wno-error=missing-field-initializers -Wno-error=misleading-indentation -I$(BUILD)
GUI_DESKTOP_CFLAGS := $(CFLAGS)
USER_CFLAGS := -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-builtin -Wall -Wextra -Werror -O2 -Iinclude
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib
USER_LDFLAGS := -m elf_i386 -T user/user.ld -nostdlib

C_SOURCES := $(filter-out src/ramfs.c src/kernel.c src/kernel_runtime.c src/gui.c src/gui_runtime.c src/gui_desktop.c src/gui_lion.c,$(wildcard src/*.c))
C_OBJECTS := $(patsubst src/%.c,$(BUILD)/%.o,$(C_SOURCES))
ASM_OBJECTS := $(BUILD)/boot.o

.PHONY: all clean iso usb disk run check test userspace userland process-test ipc-test signal-test net-test
all: $(KERNEL)
userspace userland: $(USER_ELFS)
process-test: $(BUILD)/process_test.elf
ipc-test: $(BUILD)/ipc_test.elf
signal-test: $(BUILD)/signal_test.elf
net-test: $(BUILD)/net_test.elf

test:
	bash scripts/test.sh

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: boot/boot.asm | $(BUILD)
	$(NASM) -f elf32 $< -o $@

$(ICON_HEADER): scripts/png_to_icons.py $(ICON_SOURCES) | $(BUILD)
	python3 scripts/png_to_icons.py $(ICON_HEADER)

$(WALLPAPER_HEADER): scripts/png_to_wallpaper.py LionOS-Wallpaper.png | $(BUILD)
	python3 scripts/png_to_wallpaper.py $(WALLPAPER_HEADER)

$(BUILD)/gui_lion.o: src/gui_lion.c $(ICON_HEADER) $(WALLPAPER_HEADER) | $(BUILD)
	$(CC) $(GUI_CFLAGS) -I$(BUILD) -c $< -o $@

$(BUILD)/kernel_runtime.o: src/kernel_runtime.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

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

$(BUILD)/echo.o: user/bin/echo.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/cat.o: user/bin/cat.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/ls.o: user/bin/ls.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/pwd.o: user/bin/pwd.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/uname.o: user/bin/uname.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/rm.o: user/bin/rm.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/stat.o: user/bin/stat.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/ping.o: user/bin/ping.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/userland_test.o: user/userland_test.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/hello.elf: $(USER_COMMON_OBJS) $(BUILD)/hello.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/hello.o
$(BUILD)/process_test.elf: $(USER_COMMON_OBJS) $(BUILD)/process_test.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/process_test.o
$(BUILD)/ipc_test.elf: $(USER_COMMON_OBJS) $(BUILD)/ipc_test.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/ipc_test.o
$(BUILD)/signal_test.elf: $(USER_COMMON_OBJS) $(BUILD)/signal_test.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/signal_test.o
$(BUILD)/net_test.elf: $(USER_COMMON_OBJS) $(BUILD)/net_test.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/net_test.o
$(BUILD)/echo.elf: $(USER_COMMON_OBJS) $(BUILD)/echo.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/echo.o
$(BUILD)/cat.elf: $(USER_COMMON_OBJS) $(BUILD)/cat.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/cat.o
$(BUILD)/ls.elf: $(USER_COMMON_OBJS) $(BUILD)/ls.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/ls.o
$(BUILD)/pwd.elf: $(USER_COMMON_OBJS) $(BUILD)/pwd.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/pwd.o
$(BUILD)/uname.elf: $(USER_COMMON_OBJS) $(BUILD)/uname.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/uname.o
$(BUILD)/rm.elf: $(USER_COMMON_OBJS) $(BUILD)/rm.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/rm.o
$(BUILD)/stat.elf: $(USER_COMMON_OBJS) $(BUILD)/stat.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/stat.o
$(BUILD)/ping.elf: $(USER_COMMON_OBJS) $(BUILD)/ping.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/ping.o
$(BUILD)/userland_test.elf: $(USER_COMMON_OBJS) $(BUILD)/userland_test.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/userland_test.o

$(BUILD)/%_elf.o: $(BUILD)/%.elf | $(BUILD)
	cd $(BUILD) && $(LD) -r -m elf_i386 -b binary $*.elf -o $*_elf.o

$(KERNEL): $(ASM_OBJECTS) $(C_OBJECTS) $(USER_EMBEDS) $(BUILD)/kernel_runtime.o $(BUILD)/gui_lion.o linker.ld
	$(LD) $(LDFLAGS) -o $@ $(ASM_OBJECTS) $(C_OBJECTS) $(BUILD)/kernel_runtime.o $(BUILD)/gui_lion.o $(USER_EMBEDS)
	grub-file --is-x86-multiboot2 $@

iso: $(KERNEL)
	@command -v grub-mkrescue >/dev/null 2>&1 || (echo 'ERROR: install grub-pc-bin grub-efi-amd64-bin xorriso'; exit 1)
	@grub-mkstandalone --version >/dev/null 2>&1 || (echo 'ERROR: EFI GRUB tools missing; install grub-efi-amd64-bin'; exit 1)
	mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/lionos.bin
	cp boot/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(BUILD)/iso

usb: iso
	cp $(ISO) $(BUILD)/lionos-usb.img

disk: | $(BUILD)
	if [ ! -f $(DISK) ]; then truncate -s 8M $(DISK); fi

check: iso disk
	grub-file --is-x86-multiboot2 $(KERNEL)

run: $(KERNEL) disk
	sh scripts/qemu_fullscreen.sh

clean:
	rm -rf $(BUILD)
