# Ubuntu / Linux Open-Source Adoption Plan for LionOS

**Document status:** architecture and source-adoption audit; no third-party source has been imported by this document.  
**Audit date:** 2026-11-15  
**Audited branch:** `feature/graphics-engine-v2`  
**Audited baseline commit:** `012c146e962a85b0817c8d7d54389166f760dbed`  
**Pull request:** [#44](https://github.com/AngmarX-dev/LionOS/pull/44)

## 1. Executive decision

LionOS should remain an independently developed kernel and operating system. Ubuntu is a distribution, not a kernel implementation that can be copied as one coherent source tree. The useful reuse unit is an individual upstream project or source package, selected only after checking:

1. its exact upstream source revision;
2. per-file and package licensing;
3. target architecture and ABI;
4. dependencies on Linux kernel interfaces or Linux-specific services;
5. build-system and toolchain requirements;
6. runtime behavior under LionOS;
7. maintenance and security-update ownership.

**Recommended strategy:** reuse compatible user-space programs and portable libraries first; use Linux kernel and driver code as technical reference unless a separately reviewed, license-compatible component can be isolated. Do not replace the LionOS kernel with Linux as part of this effort.

No code change, binary import, driver activation, or hardware behavior is authorized by this plan.

## 2. Repository baseline inspected

The audit inspected the branch README, MIT license, Makefile, UAPI, user API, user libc, ELF loader, VFS, RAMFS/DiskFS, ATA PIO driver, networking implementation, GRUB configuration, and the existing GPU validation matrix.

Observed architecture:

- Kernel target is **32-bit x86 / ELF32 / i386**, built with GCC `-m32`, `-ffreestanding`, and a custom linker script.
- Boot already uses **GRUB + Multiboot2**. Replacing GRUB with another bootloader is not an adoption priority.
- User programs are separate Ring-3 ELF32 images, linked with `-nostdlib` and a LionOS-specific linker script.
- Userland currently has a small custom libc and a custom `int 0x80` system-call ABI.
- The current UAPI declares `LIONOS_UAPI_VERSION 5` and `LIONOS_SYSCALL_ABI 4`; it is not the Linux i386 syscall ABI.
- Existing process support includes fork/exec/wait, threads, IPC, pipes and signals, but the public API is LionOS-specific.
- VFS currently fronts RAMFS and persistent LionFS/DiskFS. The ATA PIO path is not a general Linux block-device layer.
- Networking includes an RTL8139 path and limited IPv4/ARP/ICMP functionality. The README explicitly lists TCP, socket API and DNS as incomplete.
- The current user allocator is a 32 KiB monotonic arena; `free()` does not reclaim memory.
- The existing graphics path keeps the firmware framebuffer and CPU compositor operational. GPU mapping and submission remain gated off.

These facts rule out treating ordinary Ubuntu binaries as directly executable on LionOS. A Linux-targeted binary expects Linux ABI behavior, not merely an ELF header.

## 3. Candidate reuse matrix

| Component / source | License / source notes | LionOS compatibility | Decision |
|---|---|---|---|
| GRUB | GPLv3-or-later project; already used by the build/boot flow | Already integrated as the bootloader; not a kernel dependency | **Keep current integration.** Maintain notices/source obligations for distributed GRUB components. |
| Linux kernel source | Kernel is GPL-2.0-only overall, with per-file licensing rules and a syscall exception for user programs | Kernel internals are tightly coupled to Linux data structures, memory, scheduler, device model and VFS | **Reference only by default.** No wholesale code copy into the MIT-licensed LionOS kernel. Review every file and license before any exceptional reuse. |
| Ubuntu Linux kernel tree | Ubuntu maintains release-specific kernel trees in Launchpad | Same Linux-kernel coupling; Ubuntu patches do not make the kernel portable to LionOS | **Research/reference source**, not a drop-in kernel. Pin release/commit when studying a behavior. |
| Intel i915 graphics driver | Part of Linux graphics/kernel infrastructure; depends on DRM, GEM, DMA, MMU, interrupt and scheduler services | Hardware sequences can inform a native driver, but Linux driver source cannot be compiled as a LionOS driver | **Use as a design reference.** Extract hardware contracts and state machines into independently reviewed LionOS code; keep current GGTT safety gates. |
| BusyBox | GPL-2-only for the project; individual files can have different notices | Useful multicall userland, but needs a substantial libc/POSIX surface and compatible process, terminal and filesystem semantics | **Candidate after ABI/libc work.** Initially evaluate a small applet subset; do not import or link before a file-level license review and build proof. |
| musl libc | Supports i386, but its documented OS interface is the Linux syscall layer | Architecture support does not imply OS ABI compatibility; current LionOS syscalls are different | **Not a drop-in.** Consider only after a syscall compatibility layer is designed and tested, or keep the existing libc while expanding it. |
| e2fsprogs | Source project provides ext2/ext3/ext4 utilities; licensing is mixed and must be checked file-by-file | Host utilities can create/check images; they do not provide a kernel filesystem driver. On-target tools need libc, block I/O and filesystem syscalls | **Use host-side tools first** for image inspection/tests. On-target utilities are later work, after an ext-family filesystem and block-device layer exist. |
| GNU Coreutils | Individual programs are generally GPL-licensed; dependencies and exact notices vary | Many applets need standard libc, environment, directory traversal, permissions, pipes and process semantics | **Later userland candidate.** Prefer a small subset after POSIX-like APIs stabilize. |
| APT / dpkg | Debian package-management stack with dependency, archive, signature, maintainer-script and filesystem conventions | Far beyond the current package/runtime interfaces | **Do not port now.** Define a small signed LionOS package format and installer before considering compatibility. |
| systemd | Linux-specific service manager with extensive Linux kernel and userspace dependencies | Requires Linux facilities and a much broader runtime model | **Not a current target.** Design a small native init/service supervisor instead. |

### Important distinctions

- Ubuntu's `main` and `universe` components contain open-source packages; `restricted` contains closed-source software; `multiverse` can contain software that does not meet open-source requirements. Repository membership is not a substitute for inspecting the package's actual license.
- A source package may contain files under different licenses. The package name or Ubuntu component alone is not enough to approve code reuse.
- A program being written in C, supporting i386, or producing ELF does not make it compatible with LionOS.
- A Linux driver is not a hardware specification. Register sequences must be cross-checked against vendor documentation, generation-specific behavior and a maintained implementation.

## 4. Why direct imports are blocked today

### 4.1 ABI mismatch

LionOS uses a custom software interrupt syscall entry and LionOS-specific syscall numbers/structures. Linux i386 programs expect Linux syscall numbers, return-value conventions, errno behavior, signal semantics, process semantics and filesystem behavior. The current ABI must not be silently reinterpreted as Linux ABI.

### 4.2 libc gap

The existing libc is intentionally small. It has basic string/memory/stdio helpers and a simple allocator, but it is not a complete POSIX libc. In particular, the current monotonic allocator's `free()` is a no-op. A mature libc or BusyBox must not be linked until allocation, process, file, time, signal, thread and error semantics are complete enough for the selected program set.

### 4.3 Filesystem and block-device gap

LionFS is a custom persistent format. e2fsprogs utilities manage ext-family filesystems; they do not make the kernel understand ext2/3/4. A safe adoption path requires a generic block-device interface, a read-only filesystem implementation and corruption/bounds tests before any write support.

### 4.4 Networking gap

The present networking API is packet-oriented and custom. A POSIX socket API requires socket descriptors, bind/connect/listen/accept, send/receive, poll/select semantics, TCP state handling, timers, buffers and error propagation. Linux network utilities cannot be assumed to work against the existing API.

### 4.5 Kernel-driver gap

Linux drivers expect Linux's driver model, DMA mapping, interrupt, locking, power-management, memory-management and device-lifetime APIs. Porting a driver is a subsystem project, not a file-copy operation.

## 5. Proposed implementation sequence

The sequence below is intentionally dependency-ordered. Each stage must have an explicit acceptance test and a separate review.

### Stage A — Third-party provenance and license controls

- Add a third-party component register before the first source import.
- Record upstream URL, exact tag/commit, source-package version, license expression, copyright notices, local patches and redistribution obligations.
- Preserve original license and copyright headers.
- Add source archives or reproducible retrieval instructions for shipped copyleft binaries.
- Keep imported code in clearly separated directories; do not mix third-party code into kernel files.
- Reject components whose license or source provenance cannot be established.

**Exit gate:** every proposed imported file has an approved license record and reproducible upstream provenance.

### Stage B — Freeze and document the LionOS user ABI

- Publish stable UAPI structure sizes, alignment, integer widths, syscall numbers and return conventions.
- Define a consistent error model and user-visible `errno` mapping.
- Add ABI version negotiation and compatibility tests.
- Add user-pointer validation tests for all new interfaces.
- Do not change existing syscall meanings; add versioned interfaces where necessary.

**Exit gate:** ABI conformance tests run in CI and old user programs continue to pass.

### Stage C — Build a POSIX-inspired compatibility layer

**Implementation progress (2026-11-15):** the first native user-space file-operation slice is present in `include/posix_compat.h` and `user/posix_compat.c`: open/read/write/close plus path stat/unlink wrappers. The wrappers preserve LionOS's native descriptor and error conventions; they do not claim POSIX conformance. The existing QEMU VFS user test now exercises this layer, including append and negative cases. Descriptor duplication, standard streams, seek, directory APIs, process/environment semantics and time/terminal interfaces remain future work. This stage is therefore **in progress**, not complete.


Implement only interfaces needed by a selected program set, in dependency order:

1. file descriptors and `open/read/write/close`;
2. `stat/fstat/lseek`, directory enumeration and path operations;
3. `dup/dup2`, standard input/output/error and pipe integration;
4. `execve/waitpid/fork` behavior, environment and process exit status;
5. `getcwd/chdir/uname`, clock/time and basic terminal control;
6. signal and thread behavior required by the selected programs;
7. sockets only after TCP/IP and descriptor integration are ready.

Use compatibility wrappers over native LionOS services. Do not emulate Linux-only semantics with fake success returns.

**Exit gate:** a documented POSIX subset passes syscall boundary, process, filesystem, concurrency and negative/error-path tests.

### Stage D — libc strategy

**Implementation progress (2026-11-15):** Stage D has started by hardening the existing native libc without changing the kernel ABI or importing third-party code. The first slice:
- replaces the monotonic allocator with an 8-byte-aligned, bounded first-fit arena allocator;
- implements reuse, splitting and adjacent free-block coalescing; invalid, out-of-arena and duplicate frees are ignored;
- fixes `memmove` overlap direction without relational comparison of unrelated C pointers;
- prevents signed-overflow undefined behavior in the native `atoi` implementation by saturating at the `int` limits;
- corrects `printf`'s returned character count for `%u`, `%d` and `%x`;
- extends the existing Ring-3 userland integration test to cover overlap, allocator alignment/reuse/coalescing, overflow rejection and formatted-output counts.

This allocator is a per-process 32 KiB arena and is **not thread-safe**; it is not a general-purpose replacement for a mature libc allocator. The saturating `atoi` overflow behavior is a documented LionOS extension, not a claim of strict ISO C behavior. The current implementation remains a deliberately small libc.

Next work must address allocator concurrency/ownership contracts and expand tests for the remaining string, formatting, process, file, signal, time and threading interfaces. Evaluate musl only as a porting project: its i386 support is useful, but its documented system interface is Linux syscalls. It needs a deliberate LionOS syscall backend and conformance work. Do not replace the current libc until a side-by-side test suite demonstrates equivalent or better behavior. Keep the kernel freestanding and independent of user-space libc.

**Exit gate:** libc tests cover strings, memory, formatting, allocation/free, process, file, signal, time and threading functions selected for the first imported program.

### Stage E — First imported userland utility

- Start with one small, non-privileged command-line utility.
- BusyBox may be evaluated as a later multicall option, but GPL-2-only obligations and per-file exceptions must be respected.
- Build a small applet subset with unsupported features disabled.
- Keep the binary in user space; never link it into the kernel.
- Add a test that runs the binary as an ordinary Ring-3 process and exercises its error paths.

**Exit gate:** reproducible ELF32 build, runtime test, license/source package and no regression in existing native utilities.

### Stage F — Filesystem interoperability

- Define a generic block-device interface independently of ATA PIO.
- Add a read-only ext2 implementation first, with strict superblock, inode, direct/indirect block mapping, directory and bounds validation.
- Use e2fsprogs on the host to create controlled test images and verify image consistency.
- Add fuzz/corruption tests before implementing writes.
- Keep LionFS as the existing native persistent format until migration and recovery are proven.

**Exit gate:** read-only ext2 image tests pass in QEMU and malformed images fail safely without modifying storage.

### Stage G — Driver research and native adaptation

- Use Linux driver source to identify relevant hardware state machines and documented register requirements.
- Prefer vendor programming manuals and architecture specifications as the primary hardware contract.
- Write native LionOS abstractions for PCI, MMIO, DMA, interrupts, power and resource lifetime.
- Port only small, isolated algorithms after checking their source-file license.
- For Intel graphics, keep the current policy: PCI detection is not operational acceleration; no GGTT mapping or command submission until ownership, coherency, invalidation and recovery are proven.
- Do not modify xHCI source as part of the current Graphics Engine v2 branch.

**Exit gate:** each driver reports DETECTED, INITIALIZED, LOADED, OPERATIONAL and TESTED as distinct states, backed by evidence.

### Stage H — Package and service model

- Create a LionOS-native package manifest with file hashes, architecture, dependencies, license metadata and install/remove scripts with strict sandboxing.
- Define signature verification and rollback before remote package installation.
- Start with offline local packages.
- Do not claim APT/dpkg compatibility until dependency resolution, maintainer scripts, shared-library loading and package database recovery are implemented.
- Implement a small native init/service supervisor instead of importing systemd.

**Exit gate:** package install, verification, rollback and removal tests preserve system integrity after injected failures.

## 6. Explicit non-goals for the first adoption cycle

- Replacing the LionOS kernel with Linux.
- Copying the Ubuntu root filesystem into LionOS.
- Claiming Linux binary compatibility based only on ELF32.
- Importing all Ubuntu packages or the full GNU userland at once.
- Importing proprietary drivers or packages from `restricted` / `multiverse`.
- Enabling GPU memory mappings or command submission merely because Linux i915 performs them.
- Changing xHCI source in the current graphics branch.
- Treating a successful compile as proof of runtime compatibility.

## 7. CI and review requirements

For every implementation stage:

1. Confirm branch and head commit before editing.
2. Keep changes limited to that stage.
3. Perform two independent code reviews.
4. Run formatting/static checks where available, full build, unit/self-tests and QEMU regression tests.
5. Inspect the complete diff and verify no unrelated subsystem changed.
6. Confirm all required GitHub Actions workflows are green on the exact final commit SHA.
7. Keep physical-hardware-only claims marked as pending until tested on real hardware.

Documentation-only adoption planning does not prove runtime support.

## 8. Sources and provenance references

### Ubuntu source and archive workflow

- [Ubuntu: Package archive and component definitions](https://ubuntu.com/project/docs/how-ubuntu-is-made/concepts/package-archive/)
- [Ubuntu: Obtain the source of a package](https://ubuntu.com/project/docs/contributors/setup/get-the-source-of-a-package/)
- [Ubuntu Kernel: Obtain kernel source using Git](https://ubuntu.com/kernel/docs/how-to/source-code/obtain-kernel-source-git/)
- [Ubuntu Kernel: Enable source repositories](https://ubuntu.com/kernel/docs/how-to/source-code/enable-source-repositories/)

### Upstream projects and licensing

- [Linux kernel licensing rules](https://www.kernel.org/doc/html/latest/process/license-rules.html)
- [BusyBox license statement](https://busybox.net/license.html)
- [musl supported platforms](https://wiki.musl-libc.org/supported-platforms)
- [GNU GRUB license and project information](https://www.gnu.org/software/grub/)
- [e2fsprogs project and supported filesystem tools](https://e2fsprogs.sourceforge.net/)

The links above are research entry points, not a substitute for checking the exact revision and license files of any source file selected for import.

## 9. Audit result

**Architecture decision:** APPROVED — keep the LionOS kernel native and adopt selected open-source user-space components incrementally.

**Implementation status:** Stages A and B have passed their recorded CI gates; Stage C's first native file-wrapper slice passed all required workflows on its recorded commit. Stage D is now in progress with a bounded native libc hardening slice; runtime verification is pending. Do not begin with a wholesale musl, BusyBox, Linux driver or Ubuntu root-filesystem import.

**Third-party source imported by this audit:** none.  
**Kernel source changed by this audit:** none.  
**xHCI source changed by this audit:** none.  
**Physical hardware validation:** still required for hardware-specific driver claims.

This document is a completed adoption plan, not a claim that Ubuntu applications, Linux drivers, musl, BusyBox, ext4 or Linux binary compatibility are already operational on LionOS.
