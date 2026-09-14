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

### Memory
- ✅ Physical page allocator
- ✅ Paging with supervisor-only kernel mappings
- ✅ Per-process address spaces and CR3 switching
- ✅ User mappings across the lower 3 GiB
- ✅ Kernel heap with `kmalloc` / `kfree`

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
- ✅ System-call ABI

### Executables & storage
- ✅ Scrolling VGA console
- ✅ PS/2 keyboard input
- ✅ LionOS Shell
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
- ✅ Expanded userspace libc: `memmove`, `memcmp`, `strncmp`, `strchr`, `strrchr`, `atoi`
- ✅ Runtime libc self-test integrated into `process_test.elf`
- ✅ Compiler-built 32-bit `hello.elf` embedded in RAMFS
- ✅ Compiler-built `process_test.elf` embedded in RAMFS
- ✅ ATA PIO sector read/write driver
- ✅ Persistent LionFS metadata and fixed-size file allocation
- ✅ Persistent files survive a kernel reboot
- 🚧 VFS / filesystem syscall layer

## Testing
- ✅ Multiboot2 kernel validation in CI
- ✅ 32-bit userspace ELF validation in CI
- ✅ Userspace process-test ELF validation in CI
- ✅ ISO generation in CI
- ✅ Automated QEMU boot smoke test
- ✅ Automated persistent-storage reboot test
- 📦 Bootable `lionos-iso` CI artifact

The process lifecycle test exercises:

- Userspace libc string and memory primitives
- `fork()` parent/child return values
- Round-robin scheduling with explicit `yield()` calls
- Blocking `waitpid()`
- Child exit status delivery (`42`)
- `exec()` from userspace
- PID preservation across `exec()`

The persistent-storage test boots the same QEMU disk image twice. The first boot initializes `.boot`; the second boot must read the same marker from disk and emit `LIONOS:PERSIST-OK`.

## 🛠️ Build

LionOS uses a freestanding 32-bit toolchain on Linux.

### Requirements

- GCC with 32-bit support
- NASM
- GNU Make
- GRUB / Multiboot2 tools
- xorriso
- mtools
- QEMU

```bash
git clone https://github.com/AngmarX-dev/LionOS.git
cd LionOS
make iso
make disk
make run
```

`make disk` creates `build/lionos-disk.img` only when it does not already exist, so repeated `make run` sessions preserve the filesystem contents. `make clean` removes the image.

To build the standalone userspace ELFs:

```bash
make userspace
make process-test
readelf -h build/hello.elf
readelf -h build/process_test.elf
```

## 🧪 Shell

```text
lion> help
lion> ls
lion> run hello.elf
lion> run process_test.elf
lion> ps
lion> mem
lion> cat readme.txt
lion> write hello.txt Hello from LionOS
lion> cat readme.txt
lion> rm hello.txt
lion> dls
disk> dwrite notes.txt This survives reboot
disk> dcat notes.txt
disk> drm notes.txt
```

RAMFS is memory-backed and recreated on every boot. The embedded ELF programs are immutable RAMFS entries.

LionFS is a small persistent filesystem stored directly on the QEMU ATA disk. It currently supports up to 32 files, with 24-byte names and 4 KiB per file. It uses fixed file slots rather than a general-purpose free-space allocator; a VFS and richer filesystem layer are planned next.

## 🧠 Architecture

LionOS currently provides a small 32-bit x86 monolithic kernel with protected mode, GDT/IDT/TSS, interrupt handling, physical memory management, paging, a kernel heap, isolated ring-3 processes, scheduling, parent/child process lifecycle management, `fork()`/`waitpid()` process primitives, in-place `exec()` replacement, a small userspace C runtime/libc, system calls, keyboard/console drivers, RAMFS, an ATA PIO storage driver, a persistent LionFS filesystem, and an ELF32 executable loader.

## 🤖 AI-Assisted Development

LionOS is developed with extensive AI assistance to design, implement, debug, test, and document low-level operating-system components.

## 📜 License

MIT License. See [LICENSE](LICENSE).

## ⚠️ Status

LionOS is an early-stage experimental operating system. It is not intended for production use.

---

**Built by AngmarX 🦁**
