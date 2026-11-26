BUILD := build
ISO := $(BUILD)/lionos.iso
DISK := $(BUILD)/lionos-disk.img
KERNEL := $(BUILD)/lionos.bin
USER_COMMON_OBJS := $(BUILD)/crt0.o $(BUILD)/libc.o $(BUILD)/posix_compat.o

USER_PROGRAMS := hello meminfo process_test scheduler_stress_test futex_test migration_test busy_balance_candidate_test busy_balance_test ipc_test pipe_test signal_test sigint_test signal_handler_test tty_job_control_test net_test vfs_test cow_test nx_test echo cat ls pwd uname rm stat ping env grep find cp mv userland_test
USER_ELFS := $(addprefix $(BUILD)/,$(addsuffix .elf,$(USER_PROGRAMS)))
USER_EMBEDS := $(addprefix $(BUILD)/,$(addsuffix _elf.o,$(USER_PROGRAMS)))

CC := gcc
HOST_CC ?= gcc
LD := ld
NASM := nasm
CFLAGS := -m32 -MMD -MP -fno-strict-aliasing -ffreestanding -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra -Wno-unused-function -Werror -O2 -Iinclude
ICON_HEADER := $(BUILD)/lion_icons.h
WALLPAPER_HEADER := $(BUILD)/lion_wallpaper.h
WALLPAPER_MINIMAL_HEADER := $(BUILD)/lion_wallpaper_minimal.h
WALLPAPER_SUNRISE_HEADER := $(BUILD)/lion_wallpaper_sunrise.h
BOOT_SPLASH_HEADER := $(BUILD)/lion_boot_splash.h
ICON_SOURCES := assets/branding/lionos-emblem.png assets/branding/lionos-logo.png assets/branding/lionos-wordmark.png \
    assets/icons/applications/file-manager.png assets/icons/applications/image-viewer.png \
    assets/icons/applications/settings.png assets/icons/applications/terminal.png assets/icons/applications/text-editor.png \
    assets/icons/desktop/computer.png assets/icons/desktop/file.png assets/icons/desktop/folder.png assets/icons/desktop/trash.png \
    assets/icons/filetypes/file-audio.png assets/icons/filetypes/file-code.png assets/icons/filetypes/file-image.png assets/icons/filetypes/file-pdf.png \
    assets/icons/system/battery.png assets/icons/system/help.png assets/icons/system/network.png assets/icons/system/power.png \
    assets/icons/system/security.png assets/icons/system/update.png assets/icons/system/volume.png assets/icons/system/wifi.png
GUI_CFLAGS := $(CFLAGS) -Wno-error=missing-field-initializers -Wno-error=misleading-indentation -I$(BUILD)
GUI_DESKTOP_CFLAGS := $(CFLAGS)
USER_CFLAGS := -m32 -fno-strict-aliasing -ffreestanding -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-builtin -Wall -Wextra -Werror -O2 -Iinclude
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib
USER_LDFLAGS := -m elf_i386 -T user/user.ld -nostdlib

C_SOURCES := $(filter-out src/ramfs.c src/kernel.c src/kernel_runtime.c src/gui.c src/gui_runtime.c src/gui_desktop.c src/gui_lion.c,$(wildcard src/*.c))
C_OBJECTS := $(patsubst src/%.c,$(BUILD)/%.o,$(C_SOURCES))
ASM_OBJECTS := $(BUILD)/boot.o

.PHONY: all clean iso usb disk ext2-disk run check test third-party-check abi-check block-device-check ext2-ro-check ata-identify-check ata-block-device-check elf-check usercopy-check tty-check keyboard-tty-check memory-type-check runqueue-check scheduler-policy-check interrupt-stats-check acpi-madt-check lapic-base-check ioapic-check multiboot2-check phys-range-check memory-allocator-check userspace userland process-test ipc-test signal-test net-test
all: $(KERNEL)
userspace userland: $(USER_ELFS)
process-test: $(BUILD)/process_test.elf
ipc-test: $(BUILD)/ipc_test.elf
signal-test: $(BUILD)/signal_test.elf
net-test: $(BUILD)/net_test.elf

test: third-party-check abi-check block-device-check ext2-ro-check ata-identify-check ata-block-device-check elf-check usercopy-check tty-check keyboard-tty-check memory-type-check runqueue-check scheduler-policy-check interrupt-stats-check acpi-madt-check lapic-base-check ioapic-check multiboot2-check phys-range-check memory-allocator-check
	bash scripts/test.sh

block-device-check: | $(BUILD)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude src/block_device.c tests/block_device_test.c -o $(BUILD)/block_device_test
	$(BUILD)/block_device_test

ext2-ro-check: | $(BUILD)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude src/block_device.c src/ext2_ro.c tests/ext2_ro_test.c -o $(BUILD)/ext2_ro_test
	$(BUILD)/ext2_ro_test

ata-identify-check: | $(BUILD)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude src/ata_identify.c tests/ata_identify_test.c -o $(BUILD)/ata_identify_test
	$(BUILD)/ata_identify_test

ata-block-device-check: | $(BUILD)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude src/block_device.c src/ata_block.c tests/ata_block_device_test.c -o $(BUILD)/ata_block_device_test
	$(BUILD)/ata_block_device_test

elf-check: | $(BUILD)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude src/elf.c tests/elf_test.c -o $(BUILD)/elf_test
	$(BUILD)/elf_test

usercopy-check: | $(BUILD)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude src/usercopy.c tests/usercopy_test.c -o $(BUILD)/usercopy_test
	$(BUILD)/usercopy_test

tty-check: | $(BUILD)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude src/tty.c tests/tty_test.c tests/tty_lock_stub.c -o $(BUILD)/tty_test
	$(BUILD)/tty_test

keyboard-tty-check: | $(BUILD)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude src/tty.c src/keyboard_tty.c tests/keyboard_tty_test.c tests/tty_lock_stub.c -o $(BUILD)/keyboard_tty_test
	$(BUILD)/keyboard_tty_test

memory-type-check: | $(BUILD)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude src/memory_type.c tests/memory_type_test.c -o $(BUILD)/memory_type_test
	$(BUILD)/memory_type_test

scheduler-policy-check: | $(BUILD)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pedantic -pthread -Iinclude src/scheduler_policy.c tests/scheduler_policy_test.c -o $(BUILD)/scheduler_policy_test
	$(BUILD)/scheduler_policy_test

runqueue-check: | $(BUILD)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude src/runqueue.c src/scheduler_policy.c tests/runqueue_test.c -o $(BUILD)/runqueue_test
	$(BUILD)/runqueue_test
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pedantic -pthread -Iinclude src/runqueue.c src/scheduler_policy.c tests/runqueue_concurrency_test.c -o $(BUILD)/runqueue_concurrency_test
	timeout -k 5s 180s $(BUILD)/runqueue_concurrency_test

interrupt-stats-check: | $(BUILD)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pedantic -pthread -Iinclude src/interrupt_stats.c tests/interrupt_stats_test.c -o $(BUILD)/interrupt_stats_test
	$(BUILD)/interrupt_stats_test

acpi-madt-check: | $(BUILD)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude src/acpi_madt.c tests/acpi_madt_test.c -o $(BUILD)/acpi_madt_test
	$(BUILD)/acpi_madt_test

lapic-base-check: | $(BUILD)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude src/apic_base.c tests/apic_base_test.c -o $(BUILD)/apic_base_test
	$(BUILD)/apic_base_test

ioapic-check: | $(BUILD)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude src/ioapic_probe.c tests/ioapic_test.c -o $(BUILD)/ioapic_test
	$(BUILD)/ioapic_test

multiboot2-check: | $(BUILD)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude src/multiboot2.c tests/multiboot2_test.c -o $(BUILD)/multiboot2_test
	$(BUILD)/multiboot2_test

phys-range-check: | $(BUILD)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pedantic -Iinclude src/phys_range.c tests/phys_range_test.c -o $(BUILD)/phys_range_test
	$(BUILD)/phys_range_test

memory-allocator-check: | $(BUILD)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pedantic -pthread -no-pie -Wl,--defsym,_kernel_start=0x01000000 -Wl,--defsym,_kernel_end=0x01000000 -Iinclude src/memory.c src/phys_range.c src/multiboot2.c tests/memory_test.c -o $(BUILD)/memory_test
	$(BUILD)/memory_test

third-party-check:
	python3 scripts/check_third_party_manifest.py

abi-check:
	python3 scripts/check_uapi.py
	$(CC) $(USER_CFLAGS) -fsyntax-only tests/uapi_layout.c

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: boot/boot.asm | $(BUILD)
	$(NASM) -f elf32 $< -o $@

$(ICON_HEADER): scripts/png_to_icons.py $(ICON_SOURCES) | $(BUILD)
	python3 scripts/png_to_icons.py $(ICON_HEADER)

$(WALLPAPER_HEADER): scripts/png_to_wallpaper.py assets/wallpapers/lionos-wallpaper-default.png | $(BUILD)
	python3 scripts/png_to_wallpaper.py assets/wallpapers/lionos-wallpaper-default.png $@ LION_WALLPAPER lion_wallpaper_rgb565

$(WALLPAPER_MINIMAL_HEADER): scripts/png_to_wallpaper.py assets/wallpapers/lionos-wallpaper-minimal.png | $(BUILD)
	python3 scripts/png_to_wallpaper.py assets/wallpapers/lionos-wallpaper-minimal.png $@ LION_WALLPAPER_MINIMAL lion_wallpaper_minimal_rgb565

$(WALLPAPER_SUNRISE_HEADER): scripts/png_to_wallpaper.py assets/wallpapers/lionos-wallpaper-sunrise.png | $(BUILD)
	python3 scripts/png_to_wallpaper.py assets/wallpapers/lionos-wallpaper-sunrise.png $@ LION_WALLPAPER_SUNRISE lion_wallpaper_sunrise_rgb565

$(BOOT_SPLASH_HEADER): scripts/png_to_wallpaper.py assets/boot/boot-splash.png | $(BUILD)
	python3 scripts/png_to_wallpaper.py assets/boot/boot-splash.png $@ LION_BOOT_SPLASH lion_boot_splash_rgb565

$(BUILD)/gui_lion.o: src/gui_lion.c $(ICON_HEADER) $(WALLPAPER_HEADER) $(WALLPAPER_MINIMAL_HEADER) $(WALLPAPER_SUNRISE_HEADER) | $(BUILD)
	$(CC) $(GUI_CFLAGS) -I$(BUILD) -c $< -o $@

$(BUILD)/framebuffer.o: src/framebuffer.c $(BOOT_SPLASH_HEADER) | $(BUILD)
	$(CC) $(CFLAGS) -I$(BUILD) -c $< -o $@

$(BUILD)/kernel_runtime.o: src/kernel_runtime.c src/kernel.c $(wildcard include/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/crt0.o: user/crt0.S | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/libc.o: user/libc.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/posix_compat.o: user/posix_compat.c include/posix_compat.h include/user_api.h | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/hello.o: user/hello.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/meminfo.o: user/bin/meminfo.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/process_test.o: user/process_test.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -Os -c $< -o $@
$(BUILD)/scheduler_stress_test.o: user/scheduler_stress_test.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -Os -c $< -o $@
$(BUILD)/futex_test.o: user/futex_test.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -Os -c $< -o $@
$(BUILD)/migration_test.o: user/migration_test.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -Os -c $< -o $@
$(BUILD)/busy_balance_candidate_test.o: user/busy_balance_candidate_test.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -Os -c $< -o $@
$(BUILD)/busy_balance_test.o: user/busy_balance_test.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -Os -c $< -o $@
$(BUILD)/ipc_test.o: user/ipc_test.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/pipe_test.o: user/pipe_test.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/signal_test.o: user/signal_test.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -Os -c $< -o $@
$(BUILD)/sigint_test.o: user/sigint_test.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -Os -c $< -o $@
$(BUILD)/signal_handler_test.o: user/signal_handler_test.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -Os -c $< -o $@
$(BUILD)/tty_job_control_test.o: user/tty_job_control_test.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -Os -c $< -o $@
$(BUILD)/net_test.o: user/net_test.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/vfs_test.o: user/vfs_test.c include/posix_compat.h include/user_libc.h | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/cow_test.o: user/cow_test.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/nx_test.o: user/nx_test.c | $(BUILD)
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
$(BUILD)/env.o: user/bin/env.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/grep.o: user/bin/grep.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/find.o: user/bin/find.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/cp.o: user/bin/cp.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(BUILD)/mv.o: user/bin/mv.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/userland_test.o: user/userland_test.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/hello.elf: $(USER_COMMON_OBJS) $(BUILD)/hello.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/hello.o
$(BUILD)/meminfo.elf: $(USER_COMMON_OBJS) $(BUILD)/meminfo.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/meminfo.o
$(BUILD)/process_test.elf: $(USER_COMMON_OBJS) $(BUILD)/process_test.o user/user.ld
	$(LD) $(USER_LDFLAGS) -s -o $@ $(USER_COMMON_OBJS) $(BUILD)/process_test.o
$(BUILD)/scheduler_stress_test.elf: $(USER_COMMON_OBJS) $(BUILD)/scheduler_stress_test.o user/user.ld
	$(LD) $(USER_LDFLAGS) -s -o $@ $(USER_COMMON_OBJS) $(BUILD)/scheduler_stress_test.o
$(BUILD)/futex_test.elf: $(USER_COMMON_OBJS) $(BUILD)/futex_test.o user/user.ld
	$(LD) $(USER_LDFLAGS) -s -o $@ $(USER_COMMON_OBJS) $(BUILD)/futex_test.o
$(BUILD)/migration_test.elf: $(USER_COMMON_OBJS) $(BUILD)/migration_test.o user/user.ld
	$(LD) $(USER_LDFLAGS) -s -o $@ $(USER_COMMON_OBJS) $(BUILD)/migration_test.o
$(BUILD)/busy_balance_candidate_test.elf: $(USER_COMMON_OBJS) $(BUILD)/busy_balance_candidate_test.o user/user.ld
	$(LD) $(USER_LDFLAGS) -s -o $@ $(USER_COMMON_OBJS) $(BUILD)/busy_balance_candidate_test.o
$(BUILD)/busy_balance_test.elf: $(USER_COMMON_OBJS) $(BUILD)/busy_balance_test.o user/user.ld
	$(LD) $(USER_LDFLAGS) -s -o $@ $(USER_COMMON_OBJS) $(BUILD)/busy_balance_test.o
$(BUILD)/ipc_test.elf: $(USER_COMMON_OBJS) $(BUILD)/ipc_test.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/ipc_test.o
$(BUILD)/pipe_test.elf: $(USER_COMMON_OBJS) $(BUILD)/pipe_test.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/pipe_test.o
$(BUILD)/signal_test.elf: $(USER_COMMON_OBJS) $(BUILD)/signal_test.o user/user.ld
	$(LD) $(USER_LDFLAGS) -s -o $@ $(USER_COMMON_OBJS) $(BUILD)/signal_test.o
$(BUILD)/sigint_test.elf: $(USER_COMMON_OBJS) $(BUILD)/sigint_test.o user/user.ld
	$(LD) $(USER_LDFLAGS) -s -o $@ $(USER_COMMON_OBJS) $(BUILD)/sigint_test.o
$(BUILD)/signal_handler_test.elf: $(USER_COMMON_OBJS) $(BUILD)/signal_handler_test.o user/user.ld
	$(LD) $(USER_LDFLAGS) -s -o $@ $(USER_COMMON_OBJS) $(BUILD)/signal_handler_test.o
$(BUILD)/tty_job_control_test.elf: $(USER_COMMON_OBJS) $(BUILD)/tty_job_control_test.o user/user.ld
	$(LD) $(USER_LDFLAGS) -s -o $@ $(USER_COMMON_OBJS) $(BUILD)/tty_job_control_test.o
$(BUILD)/net_test.elf: $(USER_COMMON_OBJS) $(BUILD)/net_test.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/net_test.o
$(BUILD)/vfs_test.elf: $(USER_COMMON_OBJS) $(BUILD)/vfs_test.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/vfs_test.o
$(BUILD)/cow_test.elf: $(USER_COMMON_OBJS) $(BUILD)/cow_test.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/cow_test.o
$(BUILD)/nx_test.elf: $(USER_COMMON_OBJS) $(BUILD)/nx_test.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/nx_test.o
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
$(BUILD)/env.elf: $(USER_COMMON_OBJS) $(BUILD)/env.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/env.o
$(BUILD)/grep.elf: $(USER_COMMON_OBJS) $(BUILD)/grep.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/grep.o
$(BUILD)/find.elf: $(USER_COMMON_OBJS) $(BUILD)/find.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/find.o
$(BUILD)/cp.elf: $(USER_COMMON_OBJS) $(BUILD)/cp.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/cp.o
$(BUILD)/mv.elf: $(USER_COMMON_OBJS) $(BUILD)/mv.o user/user.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_COMMON_OBJS) $(BUILD)/mv.o
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
	python3 scripts/init_disk_image.py $(DISK)

EXT2_TEST_DISK := $(BUILD)/ext2-slave-test.img

ext2-disk: | $(BUILD)
	python3 scripts/make_ext2_fixture.py $(EXT2_TEST_DISK)

check: iso disk
	grub-file --is-x86-multiboot2 $(KERNEL)

run: $(KERNEL) disk
	sh scripts/qemu_fullscreen.sh

clean:
	rm -rf $(BUILD)

-include $(C_OBJECTS:.o=.d) $(BUILD)/kernel_runtime.d $(BUILD)/gui_lion.d
