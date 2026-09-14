# 🦁 LionOS

> An experimental operating system built from scratch with AI-assisted development.

LionOS is a small x86 kernel project focused on learning operating-system internals and low-level programming.

## 🚀 Current Progress

- ✅ Multiboot2 boot
- ✅ GDT initialization with ring-3 segments
- ✅ TSS with per-process kernel stacks
- ✅ IDT and interrupt handling
- ✅ PIC/PIT setup
- ✅ Keyboard input
- ✅ Physical memory manager
- ✅ Paging with controlled user mappings
- ✅ Per-process page directories and CR3 switching
- ✅ Private user page tables
- ✅ Kernel heap and `kmalloc`
- ✅ CPU exception handling
- ✅ System call ABI
- ✅ Ring-3 user-mode entry
- ✅ Process table and PIDs
- ✅ Round-robin scheduling
- ✅ Saved interrupt-frame context switching
- 🚧 Process resource reclamation
- 🚧 Filesystem and drivers

## 🛠️ Build

LionOS is developed on Linux using a freestanding 32-bit toolchain.

### Requirements

- GCC with 32-bit support
- NASM
- GNU Make
- GRUB / Multiboot2 tools
- xorriso
- QEMU

```bash
git clone https://github.com/AngmarX-dev/LionOS.git
cd LionOS
make
```

## 🧠 Project Focus

LionOS is primarily an educational and experimental project covering:

- Kernel development
- x86 architecture
- Memory management
- Interrupts and exceptions
- Paging and address-space isolation
- Heap allocation
- System calls
- User mode and processes
- Scheduling and context switching
- Low-level C and Assembly

## 🤖 AI-Assisted Development

LionOS is developed with extensive AI assistance. The project explores how AI can help design, implement, debug, and document low-level operating-system components while keeping the development process understandable and testable.

## 📜 License

MIT License. See [LICENSE](LICENSE).

## ⚠️ Status

LionOS is an early-stage experimental operating system. It is not intended for production use.

---

**Built by AngmarX 🦁**
