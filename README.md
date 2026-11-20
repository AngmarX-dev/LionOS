# 🦁 LionOS

<p align="center">
  <strong>An experimental 32-bit x86 operating system built from the ground up</strong><br>
  Kernel Development · Memory Management · SMP · Drivers · Networking · Desktop
</p>

<p align="center">
  <a href="https://github.com/AngmarX-dev/LionOS/actions/workflows/build.yml"><img src="https://github.com/AngmarX-dev/LionOS/actions/workflows/build.yml/badge.svg" alt="LionOS Build"></a>
  <a href="https://github.com/AngmarX-dev/LionOS/actions/workflows/xhci-mouse.yml"><img src="https://github.com/AngmarX-dev/LionOS/actions/workflows/xhci-mouse.yml/badge.svg" alt="xHCI Mouse Test"></a>
  <a href="https://img.shields.io/github/license/AngmarX-dev/LionOS"><img src="https://img.shields.io/github/license/AngmarX-dev/LionOS" alt="License"></a>
</p>

<p align="center">
  <a href="#introduction">Introduction</a> · <a href="#current-capabilities">Capabilities</a> ·
  <a href="#architecture">Architecture</a> · <a href="#build-and-run">Build & Run</a> ·
  <a href="#roadmap">Roadmap</a>
</p>

---

## Introduction

**LionOS** is an educational and experimental operating system focused on real kernel development and low-level subsystems. The goal is to incrementally build a modular operating system with memory management, processes, filesystems, I/O, networking, and a graphical environment.

LionOS is still under active development and is not a ready-to-use replacement for Windows or Linux. This document distinguishes implemented capabilities from experimental features and future plans.

### Development Principles

- Primary language: freestanding C, with limited NASM use where required.
- Correctness, reliability, code quality, and hardware compatibility take priority.
- Preserve stable code paths and prevent regressions in the kernel, memory, input, USB, and graphics subsystems.
- Test in QEMU and CI before declaring a development milestone complete.
- Distinguish hardware detection, initialization, driver loading, and functionality that has actually been tested.

### Status Legend

| Mark | Meaning |
|---|---|
| ✅ | Implemented, with related software/CI tests passing |
| 🟡 | Partially implemented, experimental, or subject to known limitations |
| ⬜ | Not implemented or currently planned |

A passing CI run alone does not guarantee operation on every physical hardware configuration.

---

## Current Capabilities

### Kernel, CPU, and Interrupts

| Capability | Status |
|---|:---:|
| GRUB and Multiboot2 boot | ✅ |
| GDT / IDT / TSS and Protected Mode | ✅ |
| CPU exceptions and kernel panic path | ✅ |
| PIC / PIT and basic interrupt handling | ✅ |
| CPUID and CPU feature detection | ✅ |
| ACPI MADT and Local APIC discovery | ✅ |
| AP startup using INIT / SIPI and handshake | ✅ |
| Per-CPU TSS / IDT and Local APIC timer | ✅ |
| SMP bring-up and tests with up to 16 vCPUs in QEMU | ✅ |
| Basic MTRR / PAT snapshot and validation | 🟡 |
| Complete interrupt routing across diverse hardware | ⬜ |
| Lazy FPU | ⬜ |

**SMP limitation:** AP startup and per-CPU infrastructure are present. However, the scheduler should not yet be considered a fully concurrent scheduler with unrestricted process migration across all CPUs. Work-stealing and safe task migration are not complete.

### Memory Management

| Capability | Status |
|---|:---:|
| Physical page allocator | ✅ |
| Paging and kernel virtual address space | ✅ |
| Kernel heap | ✅ |
| Separate address spaces for processes | ✅ |
| User/kernel separation | ✅ |
| Physical page reference counting | ✅ |
| Copy-on-Write for fork and write-fault handling | ✅ |
| Support for higher physical addresses | 🟡 |
| NX and page permission enforcement | 🟡 |
| mmap and anonymous mappings | ⬜ |
| Demand paging and lazy allocation | ⬜ |
| Slab / object allocator | ⬜ |
| Memory accounting and guard pages | ⬜ |
| Swap and memory-pressure management | ⬜ |

### Processes, Threads, Scheduler, and IPC

| Capability | Status |
|---|:---:|
| Ring 3 user program execution | ✅ |
| Context switching and preemptive scheduling | ✅ |
| fork / exec / exit / waitpid | ✅ |
| Kernel threads and user threads | ✅ |
| PID / TID and thread lifecycle | ✅ |
| Priority-based and interactive scheduling | ✅ |
| Per-CPU scheduler state and CPU ownership | ✅ |
| FPU/SSE context switching | ✅ |
| Pipes and blocking pipe I/O | ✅ |
| Basic process groups and sessions | ✅ |
| waitpid with WNOHANG / WUNTRACED / WCONTINUED | ✅ |
| Complete foreground/background and controlling TTY support | 🟡 |
| Complete user-space signal delivery and handling | ⬜ |
| Shared memory and Unix domain sockets | ⬜ |
| Limited READY user-process migration (one migration per process) | 🟡 |
| General task migration and real load balancing | ⬜ |

### Storage, VFS, and Filesystems

| Capability | Status |
|---|:---:|
| VFS and file descriptors | ✅ |
| RAMFS | ✅ |
| Persistent LionFS | ✅ |
| ATA PIO | ✅ |
| ELF32 loader | ✅ |
| Read-only Ext2 | 🟡 |
| Complete directory and file-management operations in LionFS | ⬜ |
| Permissions / ownership / timestamps | ⬜ |
| Complete mount / unmount and filesystem driver API | ⬜ |
| FAT32 | ⬜ |
| AHCI / SATA | ⬜ |
| NVMe | ⬜ |
| Buffer cache, journaling, and recovery | ⬜ |

### Input, USB, and Device Drivers

| Capability | Status |
|---|:---:|
| PS/2 keyboard and mouse | ✅ |
| Basic PCI device enumeration | 🟡 |
| xHCI USB controller path | ✅ |
| USB HID mouse | ✅ |
| USB HID keyboard and HID report parsing | 🟡 |
| USB composite devices and multiple interfaces | 🟡 |
| HID boot protocol and some non-boot reports | 🟡 |
| Cursor smoothing | ✅ |
| Generic input event API | ⬜ |
| I²C-HID for built-in laptop keyboards and touchpads | ⬜ |
| USB mass storage and USB gamepads | ⬜ |
| General hot-plug support and device manager | ⬜ |
| UHCI / OHCI / EHCI | ⬜ |

**Hardware note:** Many modern laptops expose their built-in keyboard and touchpad through I²C-HID. PS/2 or USB HID support does not imply support for those devices. The xHCI CI test uses a virtual QEMU USB device and is not a substitute for testing on physical hardware.

### Networking

| Capability | Status |
|---|:---:|
| IPv4 loopback | ✅ |
| RTL8139 Ethernet | 🟡 |
| Ethernet TX/RX | 🟡 |
| ARP / IPv4 / ICMP | 🟡 |
| User-space ping | 🟡 |
| Basic network system calls | 🟡 |
| TCP / UDP and socket API | ⬜ |
| DHCP and DNS resolver | ⬜ |
| IPv6 and routing | ⬜ |
| VirtIO-net and Intel e1000 | ⬜ |
| Network configuration and diagnostic tools | ⬜ |

### Desktop, GUI, and Browser

| Capability | Status |
|---|:---:|
| VGA console and shell | ✅ |
| Firmware framebuffer from Multiboot2 | ✅ |
| Framebuffer-based graphical desktop | ✅ |
| Basic windows, taskbar, and launcher | ✅ |
| Wallpaper and graphical assets | ✅ |
| Mouse cursor and basic interaction | ✅ |
| Software compositor | 🟡 |
| Independent, complete window manager | ⬜ |
| Double / triple buffering and damage tracking | ⬜ |
| Clipboard, notifications, and multiple workspaces | ⬜ |
| Font engine and HiDPI | ⬜ |
| Experimental browser with HTTP over IPv4 using numeric addresses | 🟡 |
| Basic HTML text display | 🟡 |
| DNS, HTTPS/TLS, JavaScript, and CSS layout | ⬜ |
| Image rendering and browser storage | ⬜ |

The current browser is an experimental networking and basic text-rendering demonstration. It is not comparable to modern web browsers.

### Graphics and GPU

The current LionOS display path uses the **firmware framebuffer** and software image composition. Intel graphics discovery is being developed as groundwork for a native driver; detecting a GPU or reading MMIO does not mean that graphics acceleration is operational.

| Capability | Status |
|---|:---:|
| Preserve and use the Multiboot2 framebuffer | ✅ |
| Basic software compositor | 🟡 |
| PCI graphics device discovery | ✅ |
| Detection of Intel display functions and supported device IDs | ✅ |
| Intel UHD detection on tested Alder Lake hardware | ✅ |
| Initial BAR and MMIO range validation | 🟡 |
| Full GGTT / stolen-memory ownership validation | ⬜ |
| Native Intel display driver and mode setting | ⬜ |
| Graphics memory manager | ⬜ |
| Hardware cursor and VBlank | ⬜ |
| 2D BLT / fill / copy acceleration | ⬜ |
| GPU command submission and ring management | ⬜ |
| GPU acceleration integrated with the compositor | ⬜ |
| Native NVIDIA and AMD drivers | ⬜ |
| EDID and multi-display support | ⬜ |

**Important limitation:** Intel/NVIDIA/AMD hardware acceleration is not currently active, and GPU command submission is not performed. The software framebuffer path must remain available as a fallback.

### Userland and Utilities

| Capability | Status |
|---|:---:|
| ELF32 user programs and user API | ✅ |
| Basic libc and user-space memory allocation | 🟡 |
| echo / cat / ls / pwd / uname | ✅ |
| rm / stat / grep / find / cp / mv | ✅ |
| ping and VFS / COW test programs | 🟡 |
| TTY with blocking input and canonical / noncanonical modes | 🟡 |
| Complete shell job control | ⬜ |
| mkdir / rmdir / chmod / chown | ⬜ |
| Complete process and system monitors | ⬜ |
| Package manager and application format | ⬜ |
| Init and service manager | ⬜ |
| Native debugging tools | ⬜ |

---

## Architecture

```text
                    GRUB / Multiboot2
                           |
                           v
                  +-------------------+
                  |    LIONOS KERNEL  |
                  |-------------------|
                  | CPU / Interrupts  |
                  | SMP / Scheduler   |
                  | Memory / Paging   |
                  | Syscalls / UAPI   |
                  | VFS / Filesystems |
                  | Drivers / Network |
                  | Framebuffer GUI   |
                  +---------+---------+
                            |
                 +----------+----------+
                 |                     |
                 v                     v
          +-------------+       +--------------+
          | RING-3 APPS |       |   HARDWARE   |
          | ELF32 / libc|       | ATA / RTL8139|
          | VFS / Syscall|      | PS/2 / xHCI  |
          +-------------+       +--------------+
```

### Repository Structure

```text
LionOS/
├── boot/                 # Multiboot2 and boot code
├── include/              # Kernel and UAPI headers
├── src/                  # Kernel, drivers, VFS, GUI, and SMP
├── user/                 # User-space programs, libc, and tests
├── assets/               # Logo, icons, and wallpapers
├── docs/                 # Technical and design documentation
├── scripts/              # Build and test scripts
├── tests/                # Standalone tests
├── .github/workflows/    # GitHub Actions
├── Makefile
├── linker.ld
└── README.md
```

## Requirements

- A Linux host system
- GCC with 32-bit output support
- NASM, GNU Make, and Binutils
- GRUB tools and xorriso for ISO generation
- QEMU for execution and testing

LionOS is a freestanding kernel and does not depend on the host operating system runtime.

## Build and Run

```bash
git clone https://github.com/AngmarX-dev/LionOS.git
cd LionOS

make
make userland
make iso
make disk
```

Run LionOS in QEMU:

```bash
make run
```

To run QEMU in a regular window:

```bash
LIONOS_QEMU_FULLSCREEN=0 make run
```

Run the test suite:

```bash
make test
```

If build targets differ, the Makefile is the authoritative reference for available commands.

## Testing and Validation

Project validation is performed at several levels:

- Kernel and userland builds
- Multiboot2 and ELF32 validation
- ISO and disk image generation
- QEMU boot and smoke tests
- Memory, VFS, and Copy-on-Write tests
- Persistence checks across reboot
- SMP tests with multiple CPU configurations
- xHCI USB mouse tests in QEMU

Passing QEMU/CI tests does not guarantee operation on every BIOS, chipset, USB controller, GPU, or laptop. Physical testing is still required for each hardware family.

## Creating a Live USB

Build the image first:

```bash
make clean
make usb
```

Identify the USB device:

```bash
lsblk
```

Write the image to the whole USB device, not to a partition:

```bash
sudo umount /dev/sdX* 2>/dev/null || true
sudo dd if=build/lionos-usb.img of=/dev/sdX bs=4M status=progress conv=fsync
sync
```

> **Warning:** The dd command erases all data on the destination device. Verify the target device with lsblk several times before running it.

---

## Security Model and Limitations

LionOS uses Ring 3 for user-space execution and validates system-call inputs. User-memory range checks, input validation, and restrictions on access to selected operations are implemented or under development.

However, the project has not undergone an independent security audit. It does not yet provide a complete user/group and permission model, ASLR, comprehensive protection against kernel vulnerabilities, or guarantees of safety against malicious code. It is therefore not suitable for sensitive data or production environments.

---

## Roadmap

The following sequence describes engineering priorities for the remaining work. It is not a delivery schedule or a release-date commitment.

### Phase A — Kernel and SMP Stabilization

- ✅ Complete SMP and synchronization stress tests
- ✅ Design per-CPU run queues with explicit ownership and locking ([design document](docs/SCHEDULER-RUNQUEUE-DESIGN.md))
- 🟡 Integrate per-CPU priority FIFO run queues with process lifecycle and scheduler; concurrent host stress and QEMU AP task create/dispatch/exit/reap, BSP-to-AP blocked-task wakeup, AP-local timer-timeout/one-shot result verification, and AP-local stop/continue with remote queue validation and AP-owned READY-task priority requeue, host verification that multiple priority changes preserve destination-lane FIFO order, AP probe reclamation checks for detached queue metadata, non-current ownership, page-aligned kernel stacks, and cleared process slots, AP-local user-process execution with fork/waitpid lifecycle coverage, a host-tested two-queue READY-node transfer primitive, and the initial opt-in READY-user-process stealing path plus QEMU coverage for two independently migrated user processes, a pinned-by-default control process, migration ownership, and per-CPU steal accounting are implemented; ordinary processes remain pinned by default; Build, Phase 26, and xHCI CI passed on commit 567d0b2e for the existing 1/4/8/16-vCPU coverage; a dedicated 2-vCPU scheduler and migration regression has now been added and awaits CI verification; each opted-in process is limited to one migration and kernel threads, user threads, and leaders with unreaped sibling threads remain pinned; longer-duration SMP stress, general load balancing, and physical hardware verification remain pending
- 🟡 Implement safe task migration and work-stealing: the initial scheduler path steals only explicitly opted-in READY user processes from the most-loaded online queue when local work is empty; QEMU validation now covers two independently migrated user processes and one pinned-by-default control process; ordinary processes remain pinned by default, with fixed two-queue lock ordering, current-task exclusion, unreaped-thread-group exclusion, and a one-migration-per-process limit; broader balancing and thread migration remain pending
- 🟡 Per-CPU interrupt statistics: atomic per-CPU/per-vector counters, concurrent host tests, bounds/isolation checks, and QEMU reporting checks for every online CPU are implemented; IOAPIC routing, legacy IRQ remapping through MADT overrides, and end-to-end routing validation remain pending
- ⬜ Evaluate Lazy FPU alongside the current FPU/SSE path
- ⬜ Run long-duration context-switch, timer, and process-lifecycle tests

### Phase B — Memory and Reliability

- ⬜ Complete page management across all supported address ranges
- ⬜ mmap and anonymous mappings
- ⬜ Demand paging and lazy allocation
- ⬜ Slab / object allocator
- ⬜ Guard pages and memory accounting
- ⬜ Memory-pressure tests and error recovery
- ⬜ Design swap if required by the architecture

### Phase C — Drivers and Input

- ⬜ Generic device model and driver registration API
- ⬜ Complete PCI capability and BAR resource management
- ⬜ Generic input event subsystem
- ⬜ I²C and I²C-HID for modern hardware
- ⬜ Complete USB HID and test with multiple physical controllers
- ⬜ USB mass storage
- ⬜ Hot-plug and device lifecycle management

### Phase D — Storage and Filesystems

- ⬜ Complete LionFS directories and file operations
- ⬜ Permissions, ownership, and timestamps
- ⬜ Mount / unmount and filesystem interface
- ⬜ FAT32 and Ext2 improvements
- ⬜ AHCI followed by NVMe
- ⬜ Buffer cache and disk cache
- ⬜ Journaling or an appropriate recovery mechanism
- ⬜ Filesystem check tools and sudden-power-loss tests

### Phase E — Networking

- ⬜ Complete and harden RTL8139
- ⬜ UDP and TCP
- ⬜ Socket API
- ⬜ DHCP and DNS
- ⬜ Network interface abstraction
- ⬜ VirtIO-net and e1000
- ⬜ IPv6 and routing
- ⬜ Packet diagnostics and network stability tests

### Phase F — Graphics Engine

- ⬜ Complete CPU/GPU detection and feature discovery without dependence on a single model
- ⬜ Validate BAR, MMIO, MTRR, and PAT
- ⬜ Identify GGTT and stolen-memory ownership and ranges
- ⬜ Implement safe graphics memory management
- ⬜ Implement limited, testable 2D operations (fill / copy / BLT)
- ⬜ Design command submission only after memory and engine state are proven correct
- ⬜ Integrate acceleration with the compositor while retaining software fallback
- ⬜ Expand native support for Intel GPUs
- ⬜ Evaluate driver architecture for AMD and NVIDIA
- ⬜ EDID, mode setting, VBlank, and multi-display

**Graphics safety principle:** Do not modify the GGTT or ring buffers, or submit GPU commands, until graphics-memory ownership, cache type, address translation, and engine state have been verified. The software framebuffer must remain a fallback path.

### Phase G — Desktop and User Experience

- ⬜ Independent window manager
- ⬜ Compositor with double / triple buffering
- ⬜ Damage tracking and Z-order management
- ⬜ Widget toolkit and text input
- ⬜ Clipboard, notifications, and dialogs
- ⬜ Workspaces and session manager
- ⬜ Font rendering and HiDPI
- ⬜ Accessibility and multi-device input management

### Phase H — Userland and Development Tools

- ⬜ Complete libc and standard file interfaces
- ⬜ Shell job control and redirection
- ⬜ Complete signal API
- ⬜ File and permission management tools
- ⬜ Process and system monitors
- ⬜ Editor and debugging tools
- ⬜ Package manager and application format
- ⬜ Init and service manager

### Phase I — Browser and Network Applications

- ⬜ More complete HTTP support, including headers, redirects, and chunked transfer
- ⬜ DNS and HTTPS/TLS
- ⬜ HTML parser and document tree
- ⬜ CSS parser and layout engine
- ⬜ Image decoder and font integration
- ⬜ Browser storage, cookies, and downloads
- ⬜ Evaluate a JavaScript engine as a separate milestone

### Phase J — Security and Release Engineering

- ⬜ User/group support and permission enforcement
- ⬜ More precise capability model
- ⬜ Kernel-stack and user-stack protection
- ⬜ ASLR and hardening
- ⬜ Fuzzing for parsers and system calls
- ⬜ Long-running tests and regression tests across multiple QEMU configurations
- ⬜ Physical hardware test matrix
- ⬜ Reproducible and versioned release artifacts
- ⬜ Kernel and driver development documentation
- ⬜ Evaluate future self-hosting

---

## Long-Term Goals

1. A reliable kernel with independent memory and process management.
2. A multiprocessor scheduler with safe synchronization and task migration.
3. An extensible driver model for diverse hardware.
4. A persistent filesystem and modern storage support.
5. Complete networking with TCP/IP and user-space tools.
6. An independent desktop with a compositor and acceleration on supported hardware.
7. A native programming environment and development tools.
8. A documented, testable, and auditable security model.

These are long-term goals and must not be confused with currently available capabilities.

## Current Project Status

- The repository contains the foundations of the kernel, user space, VFS, input, experimental networking, and desktop.
- The current display path uses a framebuffer and software compositor.
- Intel GPUs are discovered at the PCI/bootstrap level; native acceleration has not been implemented.
- SMP bring-up has been tested in QEMU; unrestricted task migration between CPUs is not complete.
- Physical execution across diverse devices still requires hardware testing.
- LionOS is a development and experimentation project, not yet a stable daily-use operating system.

## Contributing

Use GitHub Issues and Pull Requests to propose features, report bugs, or contribute code. Kernel changes should include an explanation of limitations, relevant tests, and an assessment of their impact on existing subsystems.

## License

This project is released under the MIT License. See the [LICENSE](LICENSE) file for details.
