# LionOS Rust Rewrite (Experimental)

This directory is an independent Rust rewrite track. It does not replace or
modify the existing C implementation. The starting point deliberately keeps
the existing 32-bit x86 + GRUB/Multiboot2 boot contract.

## Current implementation

- NASM Multiboot2 entry point and dedicated kernel stack.
- Freestanding `#![no_std]` Rust kernel entry.
- VGA text console and COM1 serial diagnostics.
- Bounded Multiboot2 tag traversal.
- Memory-map accounting (discovery only; no allocator yet).
- Firmware framebuffer metadata discovery (does not change display modes).
- Read-only PCI bus-0 display-class discovery.
- Minimal compiler support routines for memory operations.

This is a bootable Rust **bootstrap**, not feature parity with the mature C
branch yet. Paging, physical/virtual allocators, interrupts, APIC/SMP,
processes, syscalls, filesystems, networking, USB HID, desktop compositor,
and native Intel/NVIDIA GPU drivers remain unported.

## Toolchain (Ubuntu / Debian)

Install:

```sh
sudo apt-get update
sudo apt-get install -y build-essential gcc-multilib nasm grub-common
rustup target add i686-unknown-linux-gnu
```

Build from this directory:

```sh
make
make check
```

The output is `target/i686-unknown-linux-gnu/release/lionos-rust`.
The linker script places the kernel at 1 MiB and retains the Multiboot2
header near the start of the image.

## Safety and hardware notes

- Test in QEMU first; do not write this experimental kernel to a disk or USB.
- The PCI scan only reads configuration space and reports display-class
  functions. It does not map BARs, write GPU registers, or submit GPU commands.
- The framebuffer tag is inspected only. The firmware-selected mode remains
  untouched.
- The current Rust track does not yet support the existing desktop or userland.

## Porting sequence

1. Boot contract, serial/VGA diagnostics, Multiboot2 parsing.
2. CPU tables, exceptions, interrupt controller and timer.
3. Physical memory map and page allocator, then paging and heap.
4. PCI framework and safe MMIO mapping.
5. Framebuffer renderer and compositor.
6. Intel display-engine bring-up behind a conservative fallback.
7. Scheduler, processes, syscalls and user address spaces.
8. VFS, RAMFS/LionFS, block storage and userland.
9. USB HID, networking and desktop applications.
10. Hardware and QEMU regression coverage against the C branch.

Each milestone must build and pass its own tests before the next subsystem
is enabled. The original `feature/graphics-engine-v2` branch remains intact.
