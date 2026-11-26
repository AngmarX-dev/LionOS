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

**SMP limitation:** AP startup and per-CPU infrastructure are present. The scheduler currently supports a restricted migration path for explicitly opted-in, independent READY user processes, with at most one migration per process, plus bounded busy-queue balancing. This is not unrestricted process or thread migration. Production QEMU busy-to-busy acceptance, including three consecutive fresh-candidate workload rounds while busy workers remain active, passed Build, Phase 26, and xHCI workflows on exact commit `e96c1e170f5a3972c9493fe45f5cbcf26c85d2c7` across the configured 2/4/8/16-vCPU test matrix. The four-wave production acceptance, with ordered fresh-candidate waves while AP workers remain active through each non-final wave, passed Build LionOS, Phase 26 CI, and xHCI USB mouse workflows on exact commit `f8beac283087e04d19fb50005467be2cd80a9b09` across the configured 2/4/8/16-vCPU test matrix. The host pthread regression separately covers two competing busy destinations and deferred queue-lock contention. On exact commit `d8d481e51f6381d7bea2bb84c0a2e36b0106cd2f`, it also runs a sustained concurrent-arrival scenario with 6,144 new queue entries, three active service workers, and a concurrent balancer; this validates host-side queue mechanics and exact-once completion only, not production scheduler convergence or simultaneous execution inside `process_lock`. Production scheduler decisions remain serialized by `process_lock`, so simultaneous execution inside that critical section is not tested. Timer interrupts that enter the scheduler are accumulated in a saturating per-CPU pending counter and charged to the running task after the scheduler next acquires `process_lock`, preventing repeated lock contention from silently extending a task's priority quantum. BSP frame-timer ticks that do not invoke scheduling are excluded; legacy non-timer scheduler entries retain their previous single-charge behavior. The sustained-arrival host regression now uses cooperative `sched_yield()` waits for its service-start gate and no-progress balancer path so busy-waiting host workers do not monopolize runner CPU time; this is host-test scheduling only and does not alter kernel synchronization. The synchronized eight-worker pending-tick regression passed the Build LionOS, Phase 26 CI, and xHCI USB mouse workflows on exact commit `c5c2b41fb5493c3f6c6829bb92e23c12aabffabc`; this confirms the host regression and configured CI workflows, not production scheduler convergence. The follow-up documentation revision `f1ff9beeafbdae190a298bc8bdb1c2ed37bbff43` also passed all required workflows: Build LionOS (run `45145032769`), Phase 26 CI (run `45145032571`), and xHCI USB mouse (run `45145032655`). This documentation-only revision does not change scheduler behavior. The subsequent documentation revision `5ebe0666f2e5f7841acacb9653e3d8da840df7f1` also passed all required workflows: Build LionOS (run `45148113739`), Phase 26 CI (run `45148116539`), and xHCI USB mouse (run `45148113710`). The Build workflow completed the stability/GUI suite, live USB image, QEMU 1080p image, BIOS/UEFI and boot-UI checks, and USB disk-image validation; Phase 26 completed its build and QEMU smoke tests; xHCI completed the USB mouse boot and USB keyboard-discovery regression. This documentation-only revision does not change scheduler behavior. The subsequent documentation-only verification commit `d491fd14eac89cd0e96c59b405d549e7b504c94b` also passed all three required workflows on that exact SHA: Build LionOS (run `45151803964`), Phase 26 CI (run `45151803700`), and xHCI USB mouse (run `45151803711`). The Build workflow completed the stability/GUI suite, live USB image, QEMU 1080p image, BIOS/UEFI and boot-UI checks, USB disk-image validation, and artifact upload; Phase 26 completed its build and QEMU smoke tests; xHCI completed the QEMU mouse boot and USB keyboard-discovery regression. This documentation-only commit did not change scheduler behavior. The follow-up documentation commit `cae8548fac47901b25099b81d1eb6051949869b3` also passed Build LionOS (run `45155307115`), Phase 26 CI (run `45155307128`), and xHCI USB mouse (run `45155307118`) on that exact SHA; all build, QEMU, USB regression, and artifact-upload steps succeeded. Sustained-load convergence, broader thread migration, and physical hardware verification remain pending.

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
| Bounded busy-queue balancing (production QEMU acceptance passed; sustained-load stress partial) | 🟡 |

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
- 🟡 Integrate per-CPU priority FIFO run queues with process lifecycle and scheduler: host-side concurrent queue stress (20,000 enqueue/dequeue rounds per worker across four independently locked queues, followed by 10,000 deterministic bidirectional transfer rounds per worker with exact-one membership validation), AP task lifecycle/wakeup/timeout/stop-continue/priority-requeue/reclamation probes, AP-owned fork/waitpid, transactional migration-batch rejection for null storage, zero/oversized counts, zero/duplicate/reserved PIDs, unresolved PIDs, and already-published eligibility batches and repeated single-process eligibility publication, and process/run-queue migration invariants are implemented; the three required workflows passed on exact commit `b8829f4a7ea2d919e24b4d9d5f6c6bdb1f646452` across the configured 1/2/4/8/16-vCPU suite. A three-round SMP migration/lifecycle stress regression now repeats the topology-aware batch with fresh processes, validates successful reaping and execution ownership in each round, and passed Build, Phase 26, and xHCI workflows on exact commit `831e6580b964041df1070fb475178a1043a30399`; the production QEMU busy-to-busy acceptance now passes in Build and Phase 26 on exact commit `7001e677034272cfb93f684d2010e101ce75f0fc` across the configured multi-vCPU matrix, with two or three moved processes and one to three busy destination CPUs per run; the host pthread queue regression covers two competing busy destinations and lock-contention deferral. The sustained-service and convergence queue models call the same `scheduler_select_victim()` helper used by the production scheduler, take coherent queue-total snapshots in ascending lock order, revalidate the selected source/destination pair before transfer, and advance per-destination cyclic cursors only after successful transfers; the sustained-arrival regression holds simulated dispatch workers at a shared start condition until both busy destinations have accepted their first transfer, then verifies exact-once completion; it also requires each busy destination's service worker to complete at least one transferred node, distinguishing a successful queue transfer from actual destination service. This removes host-thread startup timing from the destination-coverage assertion. These host scenarios validate queue mechanics and policy selection, not production scheduling timing or process eligibility. The production QEMU busy-destination regression accumulates destination coverage across its five fresh-arrival waves and requires every AP that received a busy worker to receive at least one eligible process. Each individual wave now also requires successful transfers to at least two distinct busy destinations when two or more AP workers were launched (one destination on a two-vCPU run); the QEMU script verifies all five ordered spread markers. This strengthens per-wave distribution coverage but does not prove queue-depth convergence under arbitrary arrival rates. Per-CPU timer ticks are now retained across scheduler-lock contention and charged on the next successful scheduling decision. The saturating atomic pending-tick increment is factored into a shared scheduler-policy helper; its pthread host regression checks concurrent increments for lost updates and races at the UINT32_MAX saturation boundary. The host regression now releases all eight successfully created workers through a shared start gate after each reaches the barrier; its concurrent increment and saturation checks passed Build LionOS, Phase 26 CI, and xHCI USB mouse workflows on exact commit `c5c2b41fb5493c3f6c6829bb92e23c12aabffabc`. This validates the host-side atomic counter test, not production scheduler convergence. Sustained-load convergence, simultaneous production scheduler execution (process state remains serialized by `process_lock`), broader thread migration, and physical hardware verification remain pending. The cyclic busy-victim tie-breaker now uses a per-destination cursor and advances after a successful transfer; deterministic helper tests cover 16/32-CPU wraparound, visited-mask bit 31, exhausted/empty candidates, and invalid CPU bounds. Build LionOS, Phase 26 CI, and xHCI USB mouse workflows all passed on exact commit `d61b9d8646fbf10b12d79763cbfac3855a612b3e`. These policy-helper tests do not establish production fairness or sustained-load convergence. The Build LionOS workflow (run `45142084657`), Phase 26 CI (run `45142084245`), and xHCI USB mouse workflow (run `45142084296`) subsequently all passed on exact commit `7cb462d51abaf81931515460e65e5947f9a44c52`; this confirms the tested software revision but does not close the remaining scheduler acceptance gaps. A bounded busy-queue policy is now integrated: after selecting a local task, a CPU may transfer at most one eligible READY process when a victim queue exceeds its remaining local backlog by at least two; the local task remains running and the transfer is revalidated under both queue locks. Host run-queue tests cover the exact two-task threshold, a single transfer, unchanged selected-task exclusion, and post-transfer queue accounting. A pthread regression additionally races two busy destinations against one source queue using non-blocking ordered lock acquisition, verifies that held-lock contention defers, and checks exact-one node membership after both transfers; this exercises queue mechanics only, not the production process-eligibility path or end-to-end SMP scheduling. Runtime process-level busy-to-busy acceptance passed in Build and Phase 26 CI on exact commit `7001e677034272cfb93f684d2010e101ce75f0fc`. The kernel counts successful busy-destination transfers separately from idle steals. A separate QEMU integration phase now runs bounded CPU-bound workloads that periodically yield on every AP, publishes three eligible READY processes plus a pinned control process on the BSP, and checks the production transfer counter, per-process migration metadata, ownership, exit/reaping, and execution guard. The acceptance marker is required by the SMP test script across its 2/4/8/16-vCPU configurations; Build and Phase 26 CI passed on exact commit `7001e677034272cfb93f684d2010e101ce75f0fc`, with successful production busy-balance acceptance markers in the QEMU logs. The latest three-wave busy-balance regression additionally required exactly three ordered round markers (`00000000` through `00000002`) and a final sustained-round marker; all three required workflows passed on exact commit `e96c1e170f5a3972c9493fe45f5cbcf26c85d2c7`. The five-wave production acceptance now requires ordered markers (`00000000` through `00000004`); AP workers run a 1,500,000,000-iteration workload so they remain active through each non-final candidate wave. Busy-balance candidates use a dedicated 64-round workload (2,000,000 iterations per round with a cooperative yield) rather than the shorter idle-steal probe, giving each busy destination CPU time to enter the production scheduler on high-vCPU QEMU runs. The candidate ELF must also be present in the embedded RAMFS program catalog; the follow-up runtime regression now checks its catalog presence and nonzero image size before launching the five-wave test, and the QEMU acceptance parser requires that readiness marker. The RAMFS catalog registration fix is awaiting the exact-commit CI matrix. The five-wave test increases repeated production-path coverage but does not establish sustained-load convergence. Build LionOS (run `45159926164`), Phase 26 CI (run `45159926076`), and xHCI USB regression (run `45159925975`) all passed on exact commit `fb6a9bd1750390b3d0c1a73ff3313e66b69134bc`; the QEMU matrix validates the five ordered markers but does not establish sustained-load convergence, simultaneous scheduler execution inside `process_lock`, or physical-hardware behavior. The QEMU regression script also requires every AP in each multi-vCPU boot to emit the production `process_schedule()` entry marker in addition to its first local timer interrupt marker; this verifies AP scheduler entry, not simultaneous execution inside the globally serialized process-state path. The script matches the complete AP marker within the debug byte stream instead of requiring an otherwise empty line, because separate multi-call diagnostics can share a line even though each `debug_write()` record remains contiguous. For every multi-vCPU boot, it also requires at least one `LIONOS:SMP-AP-SCHEDULE-LOCK-CONTENDED` marker, proving that an AP exercised the production `process_lock` try-lock contention path; this is coverage of the contention branch, not proof of starvation freedom or simultaneous scheduler execution. A deterministic `runqueue_test` regression now also verifies the exact dequeue order across priority lanes after a cross-CPU FIFO transfer and a priority requeue, then validates queue accounting and detached-node metadata; Build LionOS, Phase 26 CI, and the xHCI USB mouse workflow all passed on exact commit `c00e654921c083b2b1442eb453c34669592856b5`. This queue-level regression does not change the remaining production scheduler limitations. The AP-owned stop/continue lifecycle probe also verifies that out-of-range priority requests are rejected atomically for both STOPPED (detached) and READY (queued) processes, preserving process state, priority metadata, time-slice accounting, and queue membership; Build LionOS, Phase 26 CI, and xHCI USB mouse test all passed on exact commit `dd068db3860bf2b38032706e875dd98d17ee05b6`. This adds lifecycle regression coverage but does not resolve sustained-load convergence, production scheduler serialization, broader thread migration, or physical hardware verification. The subsequent sustained-arrival dispatch-gate regression passed Build LionOS (run `45179803584`), Phase 26 CI (run `45179803539`), and xHCI USB mouse test (run `45179803523`) on exact commit `586c41aa55838c871f02debacbe537ab206435ce`. This verifies the host test's synchronized start condition and the configured CI suite for that revision; it does not establish production scheduler convergence or remove the documented process-lock serialization and hardware-verification limitations.
- 🟡 Implement safe task migration and work-stealing: the current path steals only explicitly opted-in READY, independent user processes from the most-loaded online queue when the local queue is empty; a topology-aware batch tests up to four migratable processes plus one pinned control process, fixed two-queue lock ordering, current-task and live-thread-group exclusion, transactional single-process and batch eligibility publication after locked, repeated single-process opt-in rejection, all-CPU READY-queue and process/node ownership validation; both single-process and batch publication verify that READY tasks are absent from current-CPU slots; single-process publication also verifies that RUNNING tasks have exactly one matching online owner, deferred zombie reaping, and execution-owner uniqueness. Each process remains limited to one migration; bounded busy-queue balancing is implemented with a two-task backlog-skew threshold and one transfer per scheduler invocation; runtime busy-to-busy acceptance passed on exact commit `7001e677034272cfb93f684d2010e101ce75f0fc`; broader thread migration, sustained contention, and production-level simultaneous scheduler execution remain pending. The exact-commit test passed on `b8829f4a7ea2d919e24b4d9d5f6c6bdb1f646452`; the three-round repeated migration/lifecycle stress extension passed Build, Phase 26, and xHCI CI on exact commit `831e6580b964041df1070fb475178a1043a30399`.
- 🟡 Per-CPU interrupt statistics: atomic per-CPU/per-vector counters, concurrent host tests, last-valid CPU/vector boundary and out-of-range checks, isolation checks, and QEMU reporting checks for every online CPU are implemented; each AP must also report a nonzero locally-accounted LAPIC timer-vector count, and per-CPU debug records are emitted atomically so concurrent diagnostics cannot splice counter fields; ACPI MADT CPU, I/O APIC, ISA override, and Local APIC address parsing has a standalone bounded parser with host tests, including online-capable-but-disabled CPU exclusion, exact 16-CPU acceptance/17th-CPU rejection, full-width x2APIC identifier preservation, and duplicate processor-UID and APIC-ID rejection across legacy/x2APIC entries, plus reserved-field and processor-flag validation for processor records, and rejection of reserved MADT header flag bits; the BSP Local APIC validates the MADT physical base (or architectural fallback), selects an uncached mapping through PAT (or PCD when PAT is unavailable), verifies the IA32_APIC_BASE readback, and checks the MMIO APIC ID against CPUID; each AP independently applies and verifies its own xAPIC base before being published online; x2APIC mode is rejected until an x2APIC driver exists; IOAPIC MMIO discovery maps MADT-listed controllers with an uncached memory type and validates hardware ID, version, redirection count, and non-overlapping GSI ranges; ISA IRQ0/1/12 route plans consume MADT overrides, validate GSI ownership and polarity/trigger encodings, then program and read back BSP-targeted redirection entries while keeping each configured entry masked; after AP startup, an IF-disabled BSP handoff unmasks IRQ0/1/12, verifies each redirection, masks and verifies both legacy PICs, and switches those vectors to LAPIC EOI; AP Local APIC timers remain masked until PIT-channel-2 calibration yields a validated nonzero count; calibration failure prevents the AP from being published online, and the QEMU test suite requires a first timer IRQ marker from every AP; Build LionOS, Phase 26 CI, and xHCI USB mouse test all passed on exact commit `51d44f2bf6ebd3cf163289ea4c6f82d6690ed011`; broader IRQ routing and physical hardware verification remain pending
- 🟡 Evaluate Lazy FPU alongside the current FPU/SSE path: reviewed the eager FXSAVE/FXRSTOR implementation and documented why lazy switching is deferred until dedicated #NM handling, per-CPU ownership, migration synchronization, kernel-FPU policy, and measurable workload tests exist ([evaluation](docs/LAZY-FPU-EVALUATION.md)); eager switching remains active
- ✅ Run long-duration context-switch, timer, and process-lifecycle tests: expanded the userspace lifecycle regression to 64 fork/exit/reap cycles, added a dedicated scheduler-stress ELF with CPU-bound peer threads that must complete under timer preemption before the parent yields, and repeated eight four-tick futex timeouts; the QEMU `LIONOS:LONG-DURATION-SCHEDULER-TEST-OK` marker was observed repeatedly in Phase 26 and xHCI regression logs, and Build, Phase 26, and xHCI workflows all passed on commit `76cb3424`

### Phase B — Memory and Reliability

- 🟡 Harden Multiboot2 boot-information parsing and physical-range ingestion: validate the complete tag stream, memory-map entry layout and address arithmetic, module records, and end-tag boundaries; process available ranges before reserved ranges so reserved memory wins independently of firmware map ordering, with host regression coverage for both reserved-before-available and available-before-reserved overlapping descriptors; clip low/high allocator operations to their managed windows with overflow-safe page alignment and deduplicated high-memory accounting; host tests exercise low/high allocation and release, the complete 0–256 MiB low-memory bitmap with full-window contiguous allocation and release, the complete 256 MiB–64 GiB high-memory bitmap, a single contiguous allocation and full release spanning the entire high-memory window, contiguous high-page runs, fragmentation, reservations applied to already allocated pages, unchanged output storage on failed high contiguous allocations, and eight-thread concurrent low/high allocation with duplicate-page detection and free-count restoration; Multiboot2 validation also checks module command-line termination and the required 8-byte memory-map entry stride; physical-memory initialization returns an explicit status, revokes all allocator state on incomplete iteration, and the kernel halts instead of reporting a false success; the public Multiboot2 iterator now revalidates caller-visible base alignment, total-size bounds, cursor alignment/range, and terminal-state invariants on every step, with malformed-cursor host regressions; physical-range conversion rejects aliased output pointers and preserves both caller outputs on empty or invalid conversions, with sentinel-preservation checks across randomized and explicit boundary cases; end-to-end allocator initialization regressions verify that malformed mmap strides, overflowing physical ranges, reversed module intervals, and unterminated module command lines fail closed without exposing low or high pages; the memory-map entry reserved field is deliberately ignored as required by Multiboot2, with a regression using a nonzero reserved value; a combined 0–64 GiB firmware-map regression initializes the low and high allocators together, allocates contiguous runs from both windows, and verifies independent accounting and complete release; host regressions also verify available-before-reserved overlap ordering and reserved-hole contiguous-run splitting independently in both low and high allocator windows; high-memory range-query tests cover the complete 256 MiB–64 GiB window before and after reservation, including a fully reserved interior bounded by still-available edge pages, unaligned bounds, and addresses outside the managed high-memory window; additional allocator hardening and broader end-to-end coverage remain pending; structurally valid maps with no allocatable pages fail closed and are covered by the host allocator test; the host Multiboot2 fixture is kept in initialized data and runtime-checked to remain wholly inside the permanently reserved first 8 MiB, so the fixture itself cannot consume a page from the full low-memory allocator window
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
