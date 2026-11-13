# LionOS — Rust Rewrite Track

This branch is an isolated Rust rewrite experiment based on the existing
`feature/graphics-engine-v2` boot contract. It does not modify the C branch
or the repository's default branch.

## Implemented foundations in this branch

- 32-bit x86 Multiboot2 entry point, dedicated bootstrap stack and linker script.
- Freestanding `no_std` Rust kernel with VGA text and COM1 serial diagnostics.
- Flat ring-0 GDT setup, CPUID feature discovery and a 256-entry fail-stop IDT.
- Legacy PIC remapping with all IRQ lines kept masked.
- Initial supervisor-only 4 GiB identity mapping using 4 MiB PSE pages, guarded by a CPUID PSE capability check.
- Fixed physical-page allocator with contiguous page allocation; kernel image,
  boot information, low memory and Multiboot2 modules are reserved.
- Page-backed Rust global allocator (allocations own contiguous 4 KiB pages).
- Validated Multiboot2 tag traversal.
- Fixed-capacity 4 KiB physical frame allocator for the 32-bit physical address
  space, initialized from usable memory-map entries.
- PCI configuration-space enumeration, command/status and interrupt metadata, and passive BAR decoding (no sizing or MMIO access).
- Fixed-capacity device registry (256 entries) and driver match/probe API (16 slots); no hardware drivers are registered yet.
- RGB 24/32-bpp Multiboot2 framebuffer metadata validation and CPU pixel/rectangle
  drawing when the framebuffer is addressable below 4 GiB.
- Bounded in-memory file store (RAMFS core).
- Static task registry and priority-based ready-task selection metadata.
- PS/2 keyboard data polling primitive.
- Ethernet frame-header parser.
- Initial syscall-number ABI draft.
- Basic framebuffer window rectangle compositor primitive.
- BIOS bootable Multiboot2 ISO packaging via GRUB, with a hybrid-ISO workflow intended for USB writing.
- Manually dispatched GitHub Actions workflow that builds the kernel and ISO and uploads the image artifact.

## USB boot scope and limitations

The current USB image is a **legacy BIOS / x86 32-bit boot image**. Firmware
and GRUB load the kernel from the USB device into RAM; the kernel does not
currently need a USB mass-storage driver just to be loaded. This is different
from supporting USB devices after the kernel starts.

- The image build uses `make iso` or `scripts/build-usb-image.sh`.
- Output: `dist/lionos-bios-usb.iso`.
- On Windows, write the ISO with Rufus and choose **DD mode** if Rufus asks.
  Writing the image erases the selected USB drive.
- In firmware setup, enable legacy BIOS/CSM boot if available and select the USB
  device from the one-time boot menu.
- This image does not currently provide UEFI boot support.
- Native xHCI/EHCI/OHCI/UHCI host-controller drivers are not implemented.
- USB keyboard and mouse HID drivers are not implemented. Firmware USB legacy
  emulation may work on some machines, but is not guaranteed.
- The kernel currently has no persistent block storage or USB filesystem
  access after handoff. Its RAMFS is volatile; removing the USB after boot
  does not provide a persistent storage path.
- The current interrupt subsystem masks all IRQs. USB controller interrupts
  therefore cannot be used yet.
- The ISO pipeline and image itself have not been boot-tested in QEMU or on
  physical hardware in this development session. Do not use it as a production
  or recovery operating system.

## Important scope boundary

This is **not yet a complete or boot-verified operating system**. The modules
above provide real foundations, but several subsystems are intentionally
limited and must not be represented as production-ready:

| Subsystem | Current state |
|---|---|
| Boot / Multiboot2 | 32-bit kernel entry; BIOS GRUB ISO packaging added; boot verification pending |
| UEFI | Not implemented |
| GDT / IDT | Flat GDT and fail-stop IDT; no per-exception recovery |
| PIC / IRQ | PIC remapped and masked; interrupt handlers not enabled |
| Physical memory | Fixed bitmap allocator and 4 GiB identity map; no isolation or demand paging |
| Heap | Page-backed global allocator; page-granular allocations only |
| PCI / devices | Passive enumeration and BAR metadata, bounded registry and driver-binding API; no BAR sizing, resource mapping or active drivers |
| USB boot | BIOS firmware + GRUB loads the kernel; post-handoff USB storage is not supported |
| USB controllers | xHCI/EHCI/OHCI/UHCI drivers not implemented |
| USB HID | Keyboard/mouse drivers not implemented |
| Framebuffer | CPU software drawing; no accelerated GPU command submission |
| Intel UHD | PCI discovery only; native display/GPU driver not implemented |
| NVIDIA RTX | PCI discovery only; native driver not implemented |
| Tasks | Registry only; no context switch, scheduler tick or user mode |
| Syscalls | ABI draft; dispatch intentionally returns not-implemented |
| Filesystem | RAM-only bounded file store; no VFS/block storage/persistent disk FS |
| Input | PS/2 scancode polling; no IRQ-driven input or USB HID |
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
sudo apt-get install -y build-essential gcc-multilib nasm grub-common grub-pc-bin xorriso binutils
rustup target add i686-unknown-linux-gnu
```

Build and validate the kernel:

```sh
make
make check
```

Build the BIOS USB image:

```sh
make iso
# or
./scripts/build-usb-image.sh
```

Expected outputs:

```
target/i686-unknown-linux-gnu/release/lionos-rust
dist/lionos-bios-usb.iso
```

The GitHub Actions workflow is manually dispatched from the Actions tab; it
builds the kernel and ISO and uploads `lionos-bios-usb-image` as an artifact.
It is not triggered automatically on every push.

## Remaining engineering work before meaningful end-to-end testing

1. Verify and correct the current freestanding build/link contract.
2. Replace the bring-up identity map with a deliberate kernel virtual-memory layout,
   page-level permissions, guard pages and memory protection.
3. Add per-vector exception stubs, IRQ dispatch, PIT/APIC timer and safe IRQ handling.
4. Add a real scheduler and architecture context-switch implementation.
5. Add user-mode transition, process address spaces and syscall entry/return.
6. Add a VFS abstraction, initramfs and persistent block-device support.
7. Add storage drivers appropriate to supported hardware (NVMe/AHCI/ATA).
8. Implement xHCI first, including PCI BAR resource sizing/mapping, DMA-safe
   memory, command/event rings, interrupter setup, port reset and device
   enumeration; then add USB HID keyboard/mouse and USB mass-storage drivers.
9. Add UEFI boot support with an architecture-compatible loader and handoff.
10. Add NIC drivers and a tested network stack.
11. Expand the graphics stack: framebuffer backend, fonts, compositor, window/event
    system, then device-specific Intel graphics support.
12. Define a realistic NVIDIA support boundary and implement only documented,
    testable device paths.
13. Add power management, ACPI, SMP and regression tests.
14. Boot-test the ISO in QEMU, then validate BIOS/UEFI and USB controller variants
    on physical hardware before declaring the image ready.

The existing C implementation remains the reference branch until the Rust
rewrite has demonstrated feature parity through repeatable tests.
