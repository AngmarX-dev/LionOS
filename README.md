# 🦁 LionOS

<p align="center">
  <strong>A 32-bit x86 operating system built from scratch.</strong><br>
  Low-level engineering • Experimental kernel • AI-assisted development
</p>

<p align="center">
  <a href="https://github.com/AngmarX-dev/LionOS/actions/workflows/build.yml"><img src="https://github.com/AngmarX-dev/LionOS/actions/workflows/build.yml/badge.svg" alt="LionOS Build"></a>
  <a href="https://github.com/AngmarX-dev/LionOS/actions/workflows/xhci-mouse.yml"><img src="https://github.com/AngmarX-dev/LionOS/actions/workflows/xhci-mouse.yml/badge.svg" alt="xHCI Mouse Test"></a>
  <a href="https://github.com/AngmarX-dev/LionOS/blob/main/LICENSE"><img src="https://img.shields.io/github/license/AngmarX-dev/LionOS" alt="MIT License"></a>
  <a href="https://github.com/AngmarX-dev/LionOS"><img src="https://img.shields.io/github/stars/AngmarX-dev/LionOS" alt="GitHub Stars"></a>
</p>

<p align="center">
  <a href="#-overview">Overview</a> • <a href="#-features">Features</a> • <a href="#-architecture">Architecture</a> • <a href="#-build--run">Build & Run</a> • <a href="#-testing">Testing</a> • <a href="#-live-usb">Live USB</a>
</p>

---

## 🦁 Overview

**LionOS** is an experimental educational operating system for **32-bit x86** machines.

The project explores operating-system internals through real low-level code: bootstrapping, protected mode, interrupts, virtual memory, processes, system calls, filesystems, networking, SMP bring-up, graphics, and hardware input.

### 🎯 Project philosophy

> **Build from the bottom up. Understand every layer. Test every subsystem.**

---

## ✨ Features

### 🧠 Kernel & CPU

| Subsystem | Status |
|---|:---:|
| Multiboot2 / GRUB boot | ✅ |
| GDT / IDT / TSS | ✅ |
| CPU exceptions | ✅ |
| PIC / PIT | ✅ |
| CPUID topology detection | ✅ |
| ACPI MADT CPU enumeration | ✅ |
| Local APIC + AP startup | ✅ |
| Per-CPU TSS / IDT bootstrap | ✅ |
| Local APIC timer | ✅ |

### 🧮 Memory

| Subsystem | Status |
|---|:---:|
| Physical page allocator | ✅ |
| Paging | ✅ |
| Kernel heap | ✅ |
| Per-process address spaces | ✅ |
| User mappings below 3 GiB | ✅ |
| Physical-page reference counting | ✅ |
| Copy-on-write fork support | ✅ |
| COW write-fault resolution | ✅ |

### ⚙️ Processes & Scheduling

| Subsystem | Status |
|---|:---:|
| Ring-3 user mode | ✅ |
| Preemptive scheduling | ✅ |
| Context switching | ✅ |
| fork / waitpid / exit status | ✅ |
| True exec replacement | ✅ |
| Per-CPU scheduler state | ✅ |
| Per-CPU process ownership | ✅ |
| Scheduler work-stealing path | ✅ |

> **SMP note:** AP bring-up and scheduler foundations are implemented. The normal userspace scheduler is not currently claimed as fully concurrent on physical multicore hardware.

### 📁 Filesystems & Userland

| Subsystem | Status |
|---|:---:|
| RAMFS | ✅ |
| Persistent LionFS | ✅ |
| VFS abstraction | ✅ |
| ATA PIO storage | ✅ |
| POSIX-style file descriptors | ✅ |
| ELF32/i386 loader | ✅ |
| Ring-3 ELF processes | ✅ |
| Userspace libc | ✅ |
| Multi-program userland build | ✅ |
| VFS integration test | ✅ |
| COW integration test | ✅ |

### 🌐 Networking

| Subsystem | Status |
|---|:---:|
| Loopback IPv4 | ✅ |
| RTL8139 Ethernet | ✅ |
| ARP / IPv4 / ICMP | ✅ |
| userspace networking syscalls | ✅ |
| terminal ping | ✅ |
| experimental HTTP browser | ✅ |

### 🎨 Desktop & Input

| Subsystem | Status |
|---|:---:|
| VGA console | ✅ |
| Colorized shell UI | ✅ |
| Native framebuffer desktop | ✅ |
| Windows / taskbar / launcher | ✅ |
| Native framebuffer sizing | ✅ |
| PS/2 keyboard | ✅ |
| PS/2 mouse | ✅ |
| xHCI HID USB mouse path | ✅ |
| Cursor smoothing | ✅ |


### 🖥️ Intel Graphics

LionOS now has an **Intel graphics bootstrap layer** inspired by the architecture used by Linux's Intel graphics stack. It performs PCI graphics-function discovery, validates the Intel display function, recognizes current Alder Lake display IDs used by the Linux i915 device table, and preserves the firmware framebuffer handoff provided through Multiboot2.

This is intentionally **not a copy of Linux's i915 driver**. The LionOS implementation is a small original layer that prepares the platform for a future native Intel display engine.


---

## 🏗️ Architecture

```text
                         ┌──────────────────────────┐
                         │       GRUB / BIOS        │
                         │        Multiboot2        │
                         └────────────┬─────────────┘
                                      │
                                      ▼
                    ┌─────────────────────────────────┐
                    │          LIONOS KERNEL          │
                    │                                 │
                    │ GDT • IDT • TSS • SMP • APIC   │
                    │ Memory • Paging • COW          │
                    │ Scheduler • Syscalls • VFS    │
                    │ Drivers • Networking • GUI     │
                    └───────────────┬─────────────────┘
                                    │
                     ┌──────────────┴──────────────┐
                     ▼                             ▼
             ┌───────────────┐             ┌───────────────┐
             │  RING-3 APPS  │             │    HARDWARE   │
             │ ELF • libc    │             │ ATA • NIC     │
             │ VFS syscalls  │             │ PS/2 • xHCI   │
             └───────────────┘             └───────────────┘
```

### 🧱 Kernel layers

```text
Boot
 ↓
CPU + Interrupts
 ↓
Physical Memory + Paging
 ↓
Processes + Scheduler
 ↓
Syscalls + UAPI
 ↓
VFS + Filesystems
 ↓
Drivers + Networking
 ↓
Framebuffer GUI + Userland
```

---

## 📦 Project Structure

```text
LionOS/
├── boot/                 # Boot code and Multiboot2 entry
├── include/              # Kernel headers and UAPI
├── src/                  # Kernel, drivers, VFS, GUI, SMP
├── user/                 # Ring-3 programs, libc, tests
├── scripts/              # Automated test scripts
├── docs/                 # Design and feature documentation
├── .github/workflows/    # GitHub Actions CI
├── Makefile              # Build / ISO / disk / USB / test
├── LICENSE
└── README.md
```

---

## 🚀 Build & Run

LionOS uses a **freestanding 32-bit Linux toolchain** with NASM and GCC.

### Clone

```bash
git clone https://github.com/AngmarX-dev/LionOS.git
cd LionOS
```

### Build

```bash
make
make userland
make iso
make disk
```

### Run in QEMU

```bash
make run
```

For a normal QEMU window:

```bash
LIONOS_QEMU_FULLSCREEN=0 make run
```

The graphical desktop uses the **real framebuffer dimensions supplied through Multiboot2** instead of forcing a fixed 1920×1080 mode.

---

## 🧪 Testing

LionOS has automated validation through **GitHub Actions** plus the local stability suite.

```bash
make test
```

The test pipeline covers:

- Multiboot2 and ELF32 validation
- kernel and userspace rebuilds
- ISO generation
- QEMU boot and SMP configuration
- persistent-storage reboot checks
- VFS/userspace integration
- COW fork/write integration
- GUI smoke testing
- xHCI USB mouse testing in QEMU

> 🖱️ **Hardware note:** the CI xHCI test uses a QEMU virtual USB mouse. Continuous movement with a real physical USB mouse still requires testing on the target machine.

---

## 💻 Userland

Example programs include:

```text
echo.elf
cat.elf
ls.elf
pwd.elf
uname.elf
rm.elf
stat.elf
vfs_test.elf
cow_test.elf
```

The kernel exposes a small userspace API through **include/uapi.h** and **include/user_api.h**.

---

## 🗂️ Storage Stack

```text
                    USERSPACE
                        │
                        ▼
                     ┌─────┐
                     │ VFS │
                     └──┬──┘
                        │
                 ┌──────┴──────┐
                 ▼             ▼
               RAMFS         LionFS
                                │
                                ▼
                             ATA PIO
                                │
                                ▼
                           Disk Image
```

---

## 🌐 LionOS Browser

The graphical desktop includes an experimental native browser.

Current scope:

```text
✅ HTTP over IPv4
✅ Numeric-address URLs
✅ Native framebuffer URL bar
✅ Keyboard and mouse controls
✅ Basic HTML-tag stripping

❌ DNS
❌ HTTPS / TLS
❌ JavaScript
❌ CSS layout
❌ Image rendering
❌ Cookies
❌ Persistent browser storage
```

Example URL:

```text
http://10.0.2.2/
```

---

## 💾 Live USB

Create the raw USB image with:

```bash
make clean
make usb
```

Check the correct device first:

```bash
lsblk
```

Then write the image to the **whole USB device**, not a partition:

```bash
sudo umount /dev/sdX* 2>/dev/null || true
sudo dd if=build/lionos-usb.img of=/dev/sdX bs=4M status=progress conv=fsync
sync
```

> ⚠️ **Warning:** dd erases the selected device. Verify the device name with lsblk before writing.

---

## 🔐 Security Model

LionOS treats Ring-3 userspace as **untrusted code**.

Current protection layers include:

- user address/range validation at syscall boundaries
- bounded userspace strings and I/O copies
- ownership-aware process-control operations
- supervisor-only kernel mappings
- capability checks for major syscall groups
- ELF structure and load-range validation

> This is an educational isolation layer, not a production security boundary.

---

## 🧩 Development Milestones

| Milestone | Status |
|---|:---:|
| Phase 22 — SMP Bring-Up | ✅ |
| Phase 23 — Stability & Persistence | ✅ |
| Phase 25/26 — Graphical UI | ✅ |

### Phase 22 — SMP

Includes ACPI MADT enumeration, local APIC setup, AP startup, INIT-SIPI-SIPI delivery, AP online handshaking, per-CPU TSS/IDT initialization, LAPIC timer infrastructure, and initial synchronization primitives.

### Phase 23 — Stability

The release-baseline suite validates clean builds, ELF32 userspace, ISO generation, first boot, persistence initialization, second boot recovery, and required boot markers.

### Phase 25/26 — UI

Adds a framebuffer desktop, native framebuffer sizing, graphical windows, taskbar, launcher, shell handoff, and mouse-driven interaction.

---

## 🤖 AI-Assisted Development

LionOS uses extensive AI assistance during:

```text
Architecture
    ↓
Implementation
    ↓
Debugging
    ↓
Testing
    ↓
Documentation
    ↓
Iteration
```

The repository keeps the low-level source, tests, and engineering history visible so the project can be studied and reproduced.

---

## 🗺️ Future Roadmap

LionOS is intentionally being developed in layers. The roadmap below tracks remaining work after the scheduler, diagnostics, SIMD, SMP, thread, and IPC foundations now implemented.

### 🧠 Kernel & CPU

| Planned feature | Status |
|---|:---:|
| Kernel panic / crash dump framework | ✅ |
| Structured kernel logging levels | ✅ |
| Runtime kernel diagnostics report | ✅ |
| Better APIC / interrupt routing | ⬜ |
| Per-CPU interrupt statistics | ✅ |
| CPU feature management (SSE/AVX detection) | ✅ |
| FPU/SSE context switching | ✅ |
| Lazy FPU state management | ⬜ |
| Kernel preemption improvements | ✅ |
| Priority-aware scheduling | ✅ |
| MLFQ-style interactive scheduling | ✅ |
| Scheduler load balancing | ✅ |
| Per-CPU run queues | ✅ |
| More robust SMP synchronization | ✅ |

### ⚙️ Processes, Threads & IPC

| Planned feature | Status |
|---|:---:|
| Kernel threads | ✅ |
| User threads | ✅ |
| Thread IDs / thread lifecycle | ✅ |
| Generic sleep(channel) / wakeup(channel) API expansion | ✅ |
| Blocking keyboard reads | ⬜ |
| Blocking pipe reads/writes | ✅ |
| Blocking filesystem operations | ⬜ |
| Process groups | ⬜ |
| Sessions / controlling terminal | ⬜ |
| Foreground/background jobs | ⬜ |
| Full signal delivery and handling | ⬜ |
| Signal-safe process termination | ⬜ |
| Shared memory IPC | ⬜ |
| Pipes | ✅ |
| Named pipes | ⬜ |
| Unix-style sockets | ⬜ |

### 🧮 Memory Management

| Planned feature | Status |
|---|:---:|
| mmap-style virtual memory API | ⬜ |
| Demand paging | ⬜ |
| Page-fault driven lazy allocation | ⬜ |
| Anonymous memory mappings | ⬜ |
| Shared memory mappings | ⬜ |
| Guard pages | ⬜ |
| User/kernel memory accounting | ⬜ |
| Slab / object allocator | ⬜ |
| Per-CPU allocation caches | ⬜ |
| Memory pressure diagnostics | ⬜ |
| Swap / paging-to-disk architecture | ⬜ |

### 🖱️ Input & Device Drivers

| Planned feature | Status |
|---|:---:|
| Generic input-event subsystem | ⬜ |
| Blocking mouse event reads | ✅ |
| Blocking keyboard event reads | ⬜ |
| Keyboard key-event API beyond ASCII | ⬜ |
| Extended PS/2 keys (arrows/Home/End/Delete) | ⬜ |
| Mouse wheel support | ⬜ |
| Mouse extra buttons | ⬜ |
| Multi-device input routing | ⬜ |
| Device manager | ⬜ |
| PCI device enumeration framework | ⬜ |
| Generic driver registration API | ⬜ |
| /dev device filesystem | ⬜ |
| USB HID keyboard | ⬜ |
| USB HID gamepad | ⬜ |
| USB mass-storage support | ⬜ |
| UHCI/OHCI/EHCI support | ⬜ |
| More complete xHCI framework | ⬜ |
| Hot-plug device detection | ⬜ |

### 💽 Storage & Filesystems

| Planned feature | Status |
|---|:---:|
| LionFS directories | ⬜ |
| Directory creation/removal syscalls | ⬜ |
| File permissions and ownership | ⬜ |
| File timestamps | ⬜ |
| Symbolic links | ⬜ |
| Hard links | ⬜ |
| File locking | ⬜ |
| Mount/unmount framework | ⬜ |
| Filesystem driver interface | ⬜ |
| Read-only filesystem support | ⬜ |
| ext2 read support | ⬜ |
| FAT32 support | ⬜ |
| Improved ATA driver | ⬜ |
| AHCI/SATA support | ⬜ |
| NVMe driver | ⬜ |
| Disk cache / buffer cache | ⬜ |
| Journaling for persistent storage | ⬜ |
| Filesystem consistency checker | ⬜ |

### 🌐 Networking

| Planned feature | Status |
|---|:---:|
| UDP | ⬜ |
| TCP | ⬜ |
| Socket API | ⬜ |
| DNS resolver | ⬜ |
| DHCP client | ⬜ |
| Routing table | ⬜ |
| Network interface abstraction | ⬜ |
| Ethernet driver framework | ⬜ |
| VirtIO-net driver | ⬜ |
| e1000 driver | ⬜ |
| IPv6 | ⬜ |
| TCP retransmission/congestion control | ⬜ |
| Loopback socket support | ⬜ |
| Network configuration commands | ⬜ |
| Packet tracing / diagnostics | ⬜ |

### 🌍 Browser

| Planned feature | Status |
|---|:---:|
| DNS URL resolution | ⬜ |
| HTTPS / TLS | ⬜ |
| HTTP headers | ⬜ |
| HTTP redirects | ⬜ |
| Chunked transfer decoding | ⬜ |
| HTML parser | ⬜ |
| Proper document tree | ⬜ |
| CSS parser | ⬜ |
| CSS layout engine | ⬜ |
| Image decoding | ⬜ |
| Font rendering improvements | ⬜ |
| Tabs | ⬜ |
| Bookmarks | ⬜ |
| Downloads | ⬜ |
| Browser cache | ⬜ |
| Cookies/storage | ⬜ |
| JavaScript engine integration | ⬜ |

### 🎨 Desktop & Window System

| Planned feature | Status |
|---|:---:|
| Unified window-manager subsystem | ⬜ |
| Compositor architecture | ⬜ |
| Double/triple buffering | ⬜ |
| Damage tracking improvements | ⬜ |
| Window resize handles | ⬜ |
| Window snapping | ⬜ |
| Window z-order management | ⬜ |
| Desktop notifications | ⬜ |
| Context menus | ⬜ |
| Modal dialogs | ⬜ |
| Menus / menu bars | ⬜ |
| Scrollable widgets | ⬜ |
| Text input widget | ⬜ |
| Checkbox/radio controls | ⬜ |
| Clipboard | ⬜ |
| Drag-and-drop framework | ⬜ |
| Multiple workspaces | ⬜ |
| Lock screen | ⬜ |
| Login/session manager | ⬜ |
| Theme engine | ⬜ |
| Font scaling / HiDPI support | ⬜ |
| Accessibility APIs | ⬜ |

### 🖼️ Graphics

| Planned feature | Status |
|---|:---:|
| Native Intel display engine | ⬜ |
| Display mode setting | ⬜ |
| Hardware cursor | ⬜ |
| Multiple display support | ⬜ |
| EDID parsing | ⬜ |
| VBlank synchronization | ⬜ |
| Graphics memory management | ⬜ |
| 2D acceleration | ⬜ |
| GPU command submission foundation | ⬜ |
| Basic software rasterizer | ⬜ |
| Sprite/image compositing | ⬜ |
| PNG/BMP image decoding | ⬜ |
| Better font rasterization | ⬜ |

### 🧰 Userland & POSIX-like Environment

| Planned feature | Status |
|---|:---:|
| More complete libc | ⬜ |
| printf family | ⬜ |
| Dynamic memory allocation | ⬜ |
| Environment variables | ⬜ |
| argv / argc conventions | ⬜ |
| Standard streams | ⬜ |
| File descriptor inheritance | ✅ |
| Pipes / redirection | ⬜ |
| Shell job control | ⬜ |
| env command | ⬜ |
| grep | ⬜ |
| find | ⬜ |
| cp / mv | ⬜ |
| mkdir / rmdir | ⬜ |
| chmod / chown | ⬜ |
| ps improvements | ⬜ |
| top-style system monitor | ⬜ |
| Text editor improvements | ⬜ |
| Package/application format | ⬜ |
| Init/system service manager | ⬜ |

### 🔐 Security & Reliability

| Planned feature | Status |
|---|:---:|
| User/group identity model | ⬜ |
| Permission enforcement in VFS | ⬜ |
| Capability refinement | ⬜ |
| Kernel stack protection | ⬜ |
| User stack guard pages | ⬜ |
| ASLR research implementation | ⬜ |
| NX / executable-page policy | ⬜ |
| Syscall audit logging | ⬜ |
| Secure process-exec checks | ⬜ |
| Random number generator | ⬜ |
| Entropy collection | ⬜ |
| Secure boot research path | ⬜ |
| Watchdog / hang detection | ⬜ |
| Crash recovery diagnostics | ⬜ |
| Fuzz testing for parsers | ⬜ |

### 🧪 Testing & Developer Tools

| Planned feature | Status |
|---|:---:|
| More QEMU hardware profiles | ⬜ |
| Automated keyboard CI tests | ⬜ |
| Automated mouse CI tests | ✅ |
| Automated filesystem corruption tests | ⬜ |
| Automated process/scheduler tests | ⬜ |
| Automated syscall ABI tests | ⬜ |
| Memory allocator stress tests | ⬜ |
| SMP stress tests | ⬜ |
| Network integration tests | ⬜ |
| Browser protocol tests | ⬜ |
| Kernel unit-test harness | ⬜ |
| Kernel tracing framework | ⬜ |
| Serial console diagnostics | ⬜ |
| GDB remote debugging | ⬜ |
| QEMU monitor integration | ⬜ |
| Deterministic boot test mode | ⬜ |
| Performance benchmarks | ⬜ |
| Boot-time subsystem timing | ⬜ |

### 🏗️ Build, Release & Project Infrastructure

| Planned feature | Status |
|---|:---:|
| Reproducible builds | ⬜ |
| Versioned kernel ABI | ⬜ |
| Release artifacts on GitHub | ⬜ |
| Automated ISO releases | ⬜ |
| Automated USB image releases | ⬜ |
| Nightly CI | ⬜ |
| Hardware compatibility matrix | ⬜ |
| Documentation site | ⬜ |
| Developer setup script | ⬜ |
| Cross-compiler bootstrap script | ⬜ |
| Coding/style checks in CI | ⬜ |
| Static analysis in CI | ⬜ |
| Sanitizer-assisted host tests | ⬜ |
| Architecture decision records | ⬜ |

### 🦁 Long-Term LionOS Goals

The long-term direction is to evolve LionOS from a small experimental kernel into a coherent, self-hosted operating-system environment:

```text
Bootloader
    ↓
Kernel + Drivers
    ↓
Virtual Memory + Scheduler
    ↓
Syscalls + IPC
    ↓
VFS + Storage
    ↓
Networking
    ↓
Graphics + Window System
    ↓
libc + Shell + Applications
    ↓
Self-hosted LionOS development environment
```

Possible long-term milestones:

- ⬜ Boot LionOS on a wider range of real x86 hardware
- ⬜ Native USB keyboard and storage
- ⬜ Native Intel graphics modesetting
- ⬜ Full TCP/IP socket layer
- ⬜ Persistent multi-user filesystem
- ⬜ Complete desktop session manager
- ⬜ Native development tools
- ⬜ Self-hosted compiler/toolchain research
- ⬜ More complete POSIX-like userspace
- ⬜ Stronger real-hardware test coverage
---

## ⚠️ Status

LionOS is an **experimental operating system** intended for learning, kernel development, experimentation, and architecture exploration.

Subsystems are intentionally small and incomplete in places. Automated QEMU validation and real-hardware validation are treated as separate stages.

---

## 📜 License

Released under the **MIT License**.

See [LICENSE](LICENSE).

---

<p align="center">
  <strong>🦁 LionOS</strong><br>
  <sub>Build low-level. Learn deeply. Ship the kernel.</sub>
</p>

> CI verification: Kernel/CPU, Processes/Threads/IPC, and post-audit memory/USB/build fixes are validated through the full build/test pipeline.
