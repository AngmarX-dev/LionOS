# LionOS — Rust Rewrite Track

This branch is an isolated Rust rewrite experiment based on the existing
`feature/graphics-engine-v2` boot contract. It does not modify the C branch
or the repository's default branch.

## Implemented foundations in this branch

- 32-bit x86 Multiboot2 entry point, dedicated bootstrap stack and linker script.
- Freestanding `no_std` Rust kernel with VGA text and COM1 serial diagnostics.
- Flat ring-0 GDT setup and a 256-entry fail-stop IDT.
- Legacy PIC remapping with all IRQ lines kept masked.
- Initial supervisor-only 4 GiB identity mapping using 4 MiB PSE pages.
- Fixed physical-page allocator with contiguous page allocation; kernel image,
  boot information, low memory and Multiboot2 modules are reserved.
- Page-backed Rust global allocator (allocations own contiguous 4 KiB pages).
- Legacy PIC remapping and masking (interrupt sources remain disabled).
- Validated Multiboot2 tag traversal.
- Fixed-capacity 4 KiB physical frame allocator for the 32-bit physical address
  space, initialized from usable memory-map entries.
- PCI configuration-space enumeration across buses, devices and functions.
- RGB 24/32-bpp Multiboot2 framebuffer metadata validation and CPU pixel/rectangle
  drawing when the framebuffer is addressable below 4 GiB.
- Bounded in-memory file store (RAMFS core).
- Static task registry and priority-based ready-task selection metadata.
- PS/2 keyboard data polling primitive.
- Ethernet frame-header parser.
- Initial syscall-number ABI draft.
- Basic framebuffer window rectangle compositor primitive.
- GitHub Actions build/Multiboot2 validation workflow.

## Important scope boundary

This is **not yet a complete or boot-verified operating system**. The modules
above provide real foundations, but several subsystems are intentionally
limited and must not be represented as production-ready:

| Subsystem | Current state |
|---|---|
| Boot / Multiboot2 | Entry and metadata parsing implemented |
| GDT / IDT | Flat GDT and fail-stop IDT; no per-exception recovery |
| PIC / IRQ | PIC remapped and masked; interrupt handlers not enabled |
| Physical memory | Fixed bitmap allocator and 4 GiB identity map; no isolation or demand paging |
| Heap | Page-backed global allocator; page-granular allocations only |
| PCI | Configuration-space enumeration; no general driver manager/BAR mapping |
| Framebuffer | CPU software drawing; no accelerated GPU command submission |
| Intel UHD | PCI discovery only; native display/GPU driver not implemented |
| NVIDIA RTX | PCI discovery only; native driver not implemented |
| Tasks | Registry only; no context switch, scheduler tick or user mode |
| Syscalls | ABI draft; dispatch intentionally returns not-implemented |
| Filesystem | RAM-only bounded file store; no VFS/block storage/persistent disk FS |
| Input | PS/2 scancode polling; no IRQ-driven input, USB HID or xHCI |
| Networking | Ethernet header parser only; no NIC driver, ARP, IP, TCP or sockets |
| Desktop | Basic rectangle drawing; no font system, windows/events or Liquid Glass |
| SMP / APIC | Not implemented |
| Power management | Not implemented |

Native GPU acceleration is a substantial independent driver project. Intel
and NVIDIA require different hardware initialization and memory-management
paths; PCI detection or a framebuffer does not imply GPU acceleration. The
NVIDIA laptop GPU path also depends on platform firmware, display routing,
and documented device-specific interfaces.

## Build prerequisites

On Debian/Kali/Ubuntu:

```sh
sudo apt-get update
sudo apt-get install -y build-essential gcc-multilib nasm grub-common binutils
rustup target add i686-unknown-linux-gnu
```

Build from this directory:

```sh
make
make check
```

Expected kernel image:

```
target/i686-unknown-linux-gnu/release/lionos-rust
```

The build and boot path have not yet been run as part of this rewrite session.
Do not write this experimental image to a physical disk or USB. Once the code
is ready, first boot it in QEMU and review serial diagnostics.

## Remaining engineering work before meaningful end-to-end testing

1. Verify and correct the current freestanding build/link contract.
2. Replace the bring-up identity map with a deliberate kernel virtual-memory layout,
   page-level permissions, guard pages and memory protection.
3. Add per-vector exception stubs, IRQ dispatch, PIT/APIC timer and safe IRQ handling.
4. Add a real scheduler and architecture context-switch implementation.
5. Add user-mode transition, process address spaces and syscall entry/return.
6. Add a VFS abstraction, initramfs and persistent block-device support.
7. Add storage drivers appropriate to supported hardware (NVMe/AHCI/ATA).
8. Add USB host-controller and HID support.
9. Add NIC drivers and a tested network stack.
10. Expand the graphics stack: framebuffer backend, fonts, compositor, window/event
    system, then device-specific Intel graphics support.
11. Define a realistic NVIDIA support boundary and implement only documented,
    testable device paths.
12. Add power management, ACPI, SMP and regression tests.
13. Build a bootable image and validate each milestone in QEMU before hardware tests.

The existing C implementation remains the reference branch until the Rust
rewrite has demonstrated feature parity through repeatable tests.
