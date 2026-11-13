CARGO ?= cargo
GRUB_FILE ?= grub-file
TARGET := i686-unknown-linux-gnu
KERNEL := target/$(TARGET)/release/lionos-rust

.PHONY: all check clean
all: $(KERNEL)

$(KERNEL): Cargo.toml Cargo.lock build.rs linker.ld boot/boot.asm src/main.rs
	$(CARGO) build --release --target $(TARGET)
	$(GRUB_FILE) --is-x86-multiboot2 $@

check: $(KERNEL)
	file $(KERNEL)
	readelf -h $(KERNEL)

clean:
	$(CARGO) clean
