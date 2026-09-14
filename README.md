# 🦁 LionOS

> An experimental 32-bit x86 operating system built from scratch with AI-assisted development.

LionOS is a small kernel project focused on learning operating-system internals, x86 architecture, memory management, processes, scheduling, system calls, and filesystem design.

## 🚀 Current Progress

### Boot & CPU
- ✅ Multiboot2 boot
- ✅ GDT with kernel and ring-3 segments
- ✅ TSS with per-process kernel stacks
- ✅ IDT and interrupt dispatch
- ✅ PIC remapping
- ✅ PIT at 100 Hz
- ✅ CPU exception handling
- ✅ Safe user-mode page-fault termination

### Memory
- ✅ Physical page allocator
- ✅ Paging and supervisor-only kernel mappings
- ✅ Per-process address spaces
- ✅ Private user page tables
- ✅ CR3 address-space switching
- ✅ Kernel heap with `kmalloc` / `kfree`

### Processes & syscalls
- ✅ PID-based process table
- ✅ Ring-3 user-mode entry
- ✅ Round-robin preemptive scheduling
- ✅ Saved interrupt-frame context switching
- ✅ Deferred process resource reclamation
- ✅ System-call ABI
- ✅ Keyboard, console, memory and process syscalls

### User interface & storage
- ✅ Scrolling VGA text console
- ✅ PS/2 keyboard input buffer
- ✅ LionOS Shell v0.4
- ✅ RAM filesystem (RAMFS)
- ✅ `ls`, `cat`, `write`, `touch`, `rm`
- ✅ `mem`, `ps`, `uname`, `uptime`, `version`, `about`
- 🚧 ELF executable loader
- 🚧 Userspace shell
- 🚧 Persistent disk filesystem

### Testing
- ✅ Multiboot2 kernel validation in CI
- ✅ ISO generation in CI
- ✅ Automated QEMU boot smoke test
- 📦 CI publishes a bootable `lionos-iso` artifact

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

To verify the kernel without building the ISO:

```bash
make check
```

## 🧪 Shell

After booting LionOS, the kernel shell provides commands such as:

```text
lion> help
lion> ls
lion> cat readme.txt
lion> write hello.txt Hello from LionOS
lion> cat hello.txt
lion> ps
lion> mem
lion> rm hello.txt
```

RAMFS is currently memory-backed and is recreated on every boot; it is not yet a persistent disk filesystem.

## 🧠 Project Focus

LionOS is an educational project covering:

- Kernel development
- x86 protected mode
- GDT / IDT / TSS
- Interrupts and exceptions
- Physical memory management
- Paging and address-space isolation
- Kernel heap allocation
- System calls
- Ring-3 execution
- Processes and scheduling
- Filesystem fundamentals
- Low-level C and Assembly

## 🤖 AI-Assisted Development

LionOS is developed with extensive AI assistance. The project explores how AI can help design, implement, debug, test, and document low-level operating-system components while keeping the resulting system understandable and testable.

## 📜 License

MIT License. See [LICENSE](LICENSE).

## ⚠️ Status

LionOS is an early-stage experimental operating system. It is not intended for production use.

---

**Built by AngmarX 🦁**
