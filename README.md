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
- ✅ VFS syscalls: `open`, `close`, `read`, `write`, `remove`, `stat`

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
- ✅ Expanded userspace libc
- ✅ Compiler-built 32-bit `hello.elf` embedded in RAMFS
- ✅ Compiler-built `process_test.elf` embedded in RAMFS
- ✅ ATA PIO sector read/write driver
- ✅ Persistent LionFS metadata and fixed-size file allocation
- ✅ Persistent files survive a kernel reboot
- ✅ VFS abstraction over RAMFS and persistent LionFS
- 🚧 Rich directory/path support
- 🚧 POSIX-style file descriptors per process

## Testing
- ✅ Multiboot2 kernel validation in CI
- ✅ 32-bit userspace ELF validation in CI
- ✅ Userspace process-test ELF validation in CI
- ✅ ISO generation in CI
- ✅ Automated QEMU boot smoke test
- ✅ Automated persistent-storage reboot test
- 🚧 Automated VFS userspace integration test
- 📦 Bootable `lionos-iso` CI artifact

The persistent-storage test boots the same QEMU disk image twice. The first boot initializes `.boot`; the second boot verifies that the marker survives reboot.

## 🛠️ Build

LionOS uses a freestanding 32-bit toolchain on Linux.

```bash
git clone https://github.com/AngmarX-dev/LionOS.git
cd LionOS
make iso
make disk
make run
```

`make disk` creates `build/lionos-disk.img` only when it does not already exist, so repeated `make run` sessions preserve filesystem contents.

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

Userspace can use the new file API through `user_api.h`:

```c
int fd = lion_open("notes.txt", LIONOS_O_READ | LIONOS_O_WRITE);
lion_fwrite(fd, "hello", 5);
char buffer[16];
lion_fread(fd, buffer, sizeof(buffer));
lion_close(fd);
```

The VFS currently provides a deliberately small interface suitable for the early kernel. It unifies RAMFS and LionFS while keeping the underlying storage implementations independent.

## 🧠 Architecture

LionOS currently provides a small 32-bit x86 monolithic kernel with protected mode, GDT/IDT/TSS, interrupt handling, physical memory management, paging, a kernel heap, isolated ring-3 processes, scheduling, parent/child process lifecycle management, `fork()`/`waitpid()` primitives, in-place `exec()` replacement, a userspace C runtime/libc, system calls, keyboard/console drivers, RAMFS, ATA PIO storage, persistent LionFS, a VFS abstraction, and an ELF32 executable loader.

## 🤖 AI-Assisted Development

LionOS is developed with extensive AI assistance to design, implement, debug, test, and document low-level operating-system components.

## 📜 License

MIT License. See [LICENSE](LICENSE).

## ⚠️ Status

LionOS is an early-stage experimental operating system. It is not intended for production use.

---

**Built by AngmarX 🦁**
