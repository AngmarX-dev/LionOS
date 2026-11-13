CARGO ?= cargo
GRUB_FILE ?= grub-file
GRUB_MKRESCUE ?= grub-mkrescue
TARGET := i686-unknown-linux-gnu
KERNEL := target/$(TARGET)/release/lionos-rust
ISO_ROOT := target/iso-root
ISO := dist/lionos-bios-usb.iso

.PHONY: all check iso verify-iso usb-image clean
all:
	$(CARGO) build --release --target $(TARGET)
	$(GRUB_FILE) --is-x86-multiboot2 $(KERNEL)

check: all
	file $(KERNEL)
	readelf -h $(KERNEL)
	readelf -l $(KERNEL)
	objdump -h $(KERNEL)

iso: all
	command -v $(GRUB_MKRESCUE) >/dev/null
	command -v xorriso >/dev/null
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot/grub dist
	cp $(KERNEL) $(ISO_ROOT)/boot/lionos-rust
	cp iso/boot/grub/grub.cfg $(ISO_ROOT)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(ISO) $(ISO_ROOT)
	test -s $(ISO)
	file $(ISO)

verify-iso: iso
	bash scripts/verify-iso.sh $(ISO) $(KERNEL) iso/boot/grub/grub.cfg

usb-image: verify-iso
	@echo "BIOS bootable ISO created: $(ISO)"
	@echo "Write it to a USB with Rufus in DD mode. This erases the selected USB."

clean:
	$(CARGO) clean
	rm -rf target/iso-root dist
