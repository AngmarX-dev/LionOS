CARGO ?= cargo
GRUB_FILE ?= grub-file
TARGET := i686-unknown-linux-gnu
KERNEL := target/$(TARGET)/release/lionos-rust

.PHONY: all check clean
all:
	$(CARGO) build --release --target $(TARGET)
	$(GRUB_FILE) --is-x86-multiboot2 $(KERNEL)

check: all
	file $(KERNEL)
	readelf -h $(KERNEL)

clean:
	$(CARGO) clean
