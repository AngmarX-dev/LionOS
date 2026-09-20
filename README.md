# 🦁 LionOS

> An experimental 32-bit x86 operating system built from scratch with AI-assisted development.

LionOS is a small educational kernel focused on operating-system internals and low-level programming.

## 🚀 Current Progress

### Boot & CPU
- ✅ Multiboot2 boot
- ✅ GDT with kernel and ring-3 segments
- ✅ TSS with per-process kernel stacks
- ✅ IDT and interrupt dispatch
- ✅ PIC/PIT
- ✅ CPU exceptions and safe user page-fault handling
- ✅ SMP CPU bring-up (Phase 22)
- ✅ CPUID CPU topology detection on the BSP
- ✅ Local APIC discovery and BSP enablement
- ✅ AP startup trampoline and INIT-SIPI-SIPI delivery
- ✅ Per-CPU TSS bootstrap
- ✅ Local APIC timer interrupt and BSP preemption clock
- 🚧 Per-CPU scheduler state and concurrent scheduling
- ✅ ACPI MADT CPU enumeration with QEMU-safe fallback

### Memory
- ✅ Physical page allocator
- ✅ Paging with supervisor-only kernel mappings
- ✅ Per-process address spaces and CR3 switching
- ✅ User mappings across the lower 3 GiB
- ✅ Kernel heap with `kmalloc` / `kfree`
- 🚧 Copy-on-write memory

### Processes & syscalls
- ✅ PID-based process table
- ✅ Ring-3 user-mode entry
- ✅ Round-robin preemptive scheduling
- ✅ Saved interrupt-frame context switching
- ✅ Per-process user-page ownership
- ✅ `fork()` with private userspace address-space copies
- ✅ Parent/child process relationships
- ✅ Blocking `waitpid()` with zombie retention and deferred reaping
- ✅ `waitpid()` exit-status delivery to userspace
- ✅ Process exit codes
- ✅ Deferred process resource reclamation
- ✅ 5-argument system-call ABI
- ✅ Explicit user-pointer/range validation at syscall boundaries
- ✅ Bounded userspace string and I/O copies
- ✅ Process ownership checks for signal delivery
- ✅ Restricted signal control to child/descendant processes
- ✅ VFS syscalls: `open`, `close`, `read`, `write`, `remove`, `stat`
- ✅ Userspace file enumeration syscall
- ✅ Per-process file-descriptor tables
- ✅ Per-process capability sets with non-escalating syscall gates

### Executables & storage
- ✅ Scrolling VGA console
- ✅ Colored VGA console API
- ✅ PS/2 keyboard input
- ✅ LionOS Shell
- ✅ Phase 25 color-coded shell UI
- ✅ RAM filesystem
- ✅ `ls`, `cat`, `write`, `touch`, `rm`
- ✅ Process diagnostics with `ps`
- ✅ ELF32/i386 validation
- ✅ Multi-page ELF `PT_LOAD` loading
- ✅ BSS zero-fill
- ✅ ELF-backed ring-3 process creation
- ✅ Initial `argc` / `argv` stack
- ✅ `run <program.elf>` launcher
- ✅ True `exec()` replacement semantics with PID preservation
- ✅ Minimal userspace C runtime and libc
- ✅ Expanded userspace libc: memory/string helpers, `atoi`, and minimal `printf`
- ✅ Multi-program userspace build pipeline
- ✅ Standalone userland utilities: `echo`, `cat`, `ls`, `pwd`, `uname`, `rm`, `stat`
- ✅ Userspace integration test embedded in RAMFS
- ✅ ATA PIO sector read/write driver
- ✅ Persistent LionFS metadata and fixed-size file allocation
- ✅ Persistent files survive a kernel reboot
- ✅ VFS abstraction over RAMFS and persistent LionFS
- ✅ Normalized hierarchical directory/path names
- ✅ POSIX-style per-process file descriptors

### Networking
- ✅ Loopback IPv4 transport (`127.0.0.1`)
- ✅ Packet queues and userspace send/receive syscalls
- ✅ RTL8139 physical NIC driver\n- ✅ ARP + IPv4 + ICMP echo (ping)\n- ✅ Terminal `ping` command (QEMU user networking)

### SMP synchronization
- ✅ Atomic test-and-set spinlock
- ✅ IRQ-save / IRQ-restore locking primitive
- ✅ BSP spinlock self-test during boot
- ✅ Atomic AP online handshake
- ✅ Process-table locking
- ✅ Memory/VFS/IPC/network locking

## 🧪 Testing
- ✅ Multiboot2 kernel validation in CI
- ✅ 32-bit userspace ELF validation in CI
- ✅ Userspace process-test ELF validation in CI
- ✅ ISO generation in CI
- ✅ Automated 2-CPU QEMU SMP boot test
- ✅ Automated persistent-storage reboot test
- 🚧 Automated VFS/userspace integration test
- 📦 Bootable `lionos-iso` CI artifact

Run the complete local stability suite with:

```bash
make test
```

A successful run verifies both first and second SMP boots, persistent-storage recovery, and the final `LIONOS:READY` state.

## 🧩 Phase 22 — SMP ✅

The first AP bring-up path is implemented and validated under the two-CPU QEMU test configuration. The BSP initializes the local APIC, prepares a low-memory real-mode trampoline at `0x8000`, allocates an AP kernel stack, sends `INIT` followed by `SIPI` startup messages, and waits for the AP to report online.

The AP enters protected mode, loads the kernel page directory, jumps to `smp_ap_main()`, records its APIC ID, initializes a per-CPU TSS and IDT, enables its local APIC timer, and enables interrupts. The BSP uses an atomic online handshake before reporting the CPU online.

The BSP also uses a local APIC periodic timer on vector `48`, which drives the existing scheduler clock after process initialization. The legacy PIT timer IRQ is masked when the LAPIC timer is active.

An atomic spinlock primitive with IRQ-save/restore support is available as the synchronization foundation. A boot-time self-test verifies the primitive without pretending that the entire kernel is already SMP-safe.

CPU enumeration now consumes ACPI MADT processor entries when Multiboot provides a valid RSDP/RSDT/XSDT path, with CPUID topology as a safe fallback. AP startup uses the enumerated APIC IDs instead of assuming contiguous IDs. Shared kernel structures have IRQ-safe locking foundations for process, memory, VFS, IPC, and loopback networking, but APs do not run the normal userspace scheduler concurrently yet.

## 🧩 Phase 23 — Stability & Persistence ✅

Phase 23 adds an automated stability suite covering the release baseline:

- clean kernel and userspace rebuild
- ELF32/i386 userspace validation
- ISO generation
- first SMP boot and persistent filesystem initialization
- second SMP boot and persistence verification
- required SMP and `READY` boot markers

The full suite is implemented in [`scripts/test.sh`](scripts/test.sh) and is also executed by CI.

## 📦 Phase 24 — Documentation & Release Preparation 🚧

Phase 24 packages the validated SMP and stability work for an experimental release milestone. The release checklist and known scope limitations are documented in [`docs/PHASE-24-RELEASE.md`](docs/PHASE-24-RELEASE.md).

## 🎨 Phase 25/26 — UI ✅

The user-facing UI layer now includes both the Phase 25 text-mode improvements and the Phase 26 graphical desktop.

The UI includes:

- VGA foreground color support
- a clearer LionOS shell banner
- a color-coded `lion:/ >` prompt
- categorized `help` output
- colored success, status, and error messages
- a cleaner `about`, `ls`, `run`, and file-command presentation
- framebuffer desktop rendering at the native mode reported by Multiboot2
- mouse cursor and pixel-coordinate input
- desktop backbuffer/present path driven by the LAPIC wake-up clock
- graphical desktop windows, taskbar, launcher, and shell handoff

## 🛡️ Security Model

LionOS treats ring-3 userspace as untrusted code. Syscall entry points validate user virtual-address ranges before copying data, strings are bounded by fixed maximum lengths, and user I/O is capped to prevent oversized kernel copies.

Process-control operations are ownership-aware: a userspace process may only signal its own child/descendant processes through the current `kill` interface. Kernel PID 1 is never exposed as a signal target through this interface.

The ELF loader validates the executable structure and load ranges before creating a userspace address space. Kernel mappings are supervisor-only in cloned process page directories. Syscalls additionally check the current process capability mask before entering console, filesystem, process-control, IPC, or networking operations.

This is an educational hardening layer, not a production security boundary. The next major isolation work includes per-process file descriptors, stronger privilege separation, and more complete memory-copy primitives.

## 🛠️ Build

LionOS uses a freestanding 32-bit toolchain on Linux.

```bash
git clone https://github.com/AngmarX-dev/LionOS.git
cd LionOS
make
make userland
make iso
make disk
make run
```

`make run` starts QEMU with two virtual CPUs for the current SMP bring-up configuration.

`make run` now uses the framebuffer mode selected natively by GRUB (`gfxmode=auto`). LionOS reads the exact width and height supplied by the Multiboot2 framebuffer tag, so the GUI is laid out from the real guest framebuffer size rather than forcing 1920x1080.

QEMU fullscreen controls the display window and scaling; it does not force LionOS to pretend its framebuffer is 1920x1080.

To open QEMU in a normal window:

```bash
LIONOS_QEMU_FULLSCREEN=0 make run
```

The normal ISO and live-USB image use the same native framebuffer path. There is no 1920x1080 boot-mode constant.

`make userland` builds every userspace ELF. `make` embeds the userland programs into RAMFS as part of the kernel image.

`make disk` creates `build/lionos-disk.img` only when it does not already exist, so repeated `make run` sessions preserve filesystem contents.

For the complete release-baseline validation, run:

```bash
make test
```

## 🧪 Userland

User programs share `user/crt0.S` and `user/libc.c`, link against `user/user.ld`, and are embedded into RAMFS. The current command launcher is:

```text
lion:/ > run echo.elf
lion:/ > run ls.elf
lion:/ > run cat.elf
lion:/ > run pwd.elf
lion:/ > run uname.elf
lion:/ > run stat.elf\nlion:/ > ping 10.0.2.2
```

The kernel exposes a small UAPI through `include/uapi.h` and `include/user_api.h`. User processes receive an explicit capability mask covering console, filesystem, process-control, IPC, and networking operations; the admin capability is reserved for the kernel and cannot be granted through the userspace process API. `lion_getfile()` provides indexed VFS enumeration to userspace, allowing `ls.elf` to operate without kernel shell code.

## 🧪 Storage

The kernel currently has two filesystem backends:

```text
                 ┌──────────────┐
userspace ──────►│     VFS      │
                 └──────┬───────┘
                    ┌───┴───┐
                    ▼       ▼
                 RAMFS    LionFS
                           │
                         ATA PIO
                           │
                       disk image
```

Userspace can use the file API through `user_api.h`:

```c
int fd = lion_open("notes.txt", LIONOS_O_READ | LIONOS_O_WRITE);
lion_fwrite(fd, "hello", 5);
char buffer[16];
lion_fread(fd, buffer, sizeof(buffer));
lion_close(fd);
```

The VFS currently provides a deliberately small interface suitable for the early kernel. It unifies RAMFS and LionFS while keeping the underlying storage implementations independent.

## 🧠 Architecture

LionOS currently provides a small 32-bit x86 monolithic kernel with protected mode, GDT/IDT/TSS, interrupt handling, physical memory management, paging, a kernel heap, isolated ring-3 processes, scheduling, parent/child process lifecycle management, `fork()`/`waitpid()` primitives, in-place `exec()` replacement, a userspace C runtime/libc, system calls, syscall input validation, keyboard/console drivers, RAMFS, ATA PIO storage, persistent LionFS, a VFS abstraction, an ELF32 executable loader, a loopback and RTL8139 Ethernet networking layer with ARP/IPv4/ICMP support, Local APIC support, AP startup, per-CPU TSS/IDT bootstrap, a LAPIC scheduler timer, initial SMP synchronization primitives, and the new colored text-mode shell UI.

## 🤖 AI-Assisted Development

LionOS is developed with extensive AI assistance to design, implement, debug, test, and document low-level operating-system components.

## 📜 License

MIT License. See [LICENSE](LICENSE).

## ⚠️ Status

LionOS is an early-stage experimental operating system. Phase 22 SMP bring-up and Phase 23 stability testing are complete. Phase 24 release preparation is underway, and the Phase 25/26 UI milestones are implemented. LionOS is not intended for production use.

## Display target

LionOS boots the graphical desktop at the framebuffer mode selected by GRUB/firmware/QEMU. The kernel reports the actual width and height received from Multiboot2; the desktop timer remains 60 Hz.


## 💾 Live USB Boot

The CI build produces `build/lionos-usb.img`, a raw copy of the bootable LionOS ISO intended for live USB use. It boots LionOS without installing it to the USB drive; the current live system runs from the boot media and uses the existing RAMFS for the session.

Create the image locally with:

```bash
make clean
make usb
```

Write it to a USB stick **only after confirming the correct device**:

```bash
lsblk
sudo umount /dev/sdX* 2>/dev/null || true
sudo dd if=build/lionos-usb.img of=/dev/sdX bs=4M status=progress conv=fsync
sync
```

Replace `/dev/sdX` with the whole USB device, not a partition such as `/dev/sdX1`. The USB contents will be erased. Reboot, select the USB device in the firmware boot menu, and choose `LionOS`.



## 🌐 LionOS Browser

The graphical desktop now includes a built-in experimental browser application. It provides a native framebuffer URL bar, HTTP/1.x page loading, basic HTML-tag stripping, keyboard input, and mouse controls.

Current networking scope is intentionally small: the browser supports **HTTP over IPv4 using numeric addresses** (for example `http://10.0.2.2/`). DNS, HTTPS/TLS, JavaScript, CSS layout, images, cookies, and persistent browser storage are not implemented yet.

Open it from the desktop **Browser** icon, the taskbar, or the LionOS launcher. Press **Esc** to close it.
