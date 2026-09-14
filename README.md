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
- ✅ Deferred process resource reclamation
- ✅ Per-process user-page ownership
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
- 🚧 True `exec()` replacement semantics
- 🚧 Userspace libc / program API
- 🚧 Persistent disk filesystem

### Testing
- ✅ Multiboot2 kernel validation in CI
- ✅ ISO generation in CI
- ✅ Automated QEMU boot smoke test
- 📦 Bootable `lionos-iso` CI artifact

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
make run
```

## 🧪 Shell

```text
lion> help
lion> ls
lion> run hello.elf
lion> ps
lion> mem
lion> cat readme.txt
lion> write hello.txt Hello from LionOS
lion> cat hello.txt
lion> rm hello.txt
```

RAMFS is memory-backed and recreated on every boot.

## 🧠 Architecture

LionOS currently provides a small 32-bit x86 monolithic kernel with protected mode, GDT/IDT/TSS, interrupt handling, physical memory management, paging, a kernel heap, isolated ring-3 processes, scheduling, system calls, keyboard/console drivers, RAMFS, and an ELF32 executable loader.

## 🤖 AI-Assisted Development

LionOS is developed with extensive AI assistance to design, implement, debug, test, and document low-level operating-system components.

## 📜 License

MIT License. See [LICENSE](LICENSE).

## ⚠️ Status

LionOS is an early-stage experimental operating system. It is not intended for production use.

---

**Built by AngmarX 🦁**
