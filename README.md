# 🦁 LionOS

> An experimental operating system built from scratch with AI-assisted development.

LionOS is a small x86 kernel project focused on learning operating-system internals and low-level programming.

## 🚀 Current Progress

- ✅ Multiboot2 boot
- ✅ GDT initialization
- ✅ IDT and interrupt handling
- ✅ PIC/PIT setup
- ✅ Keyboard input
- ✅ Physical memory manager
- ✅ Paging
- ✅ Kernel heap and `kmalloc`
- ✅ CPU exception handling
- ✅ System calls
- 🚧 More kernel features in development

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
git clone https://github.com/Ehsan1389A-gif/LionOS.git
cd LionOS
make
```

## 🧠 Project Focus

LionOS is primarily an educational and experimental project covering:

- Kernel development
- x86 architecture
- Memory management
- Interrupts and exceptions
- Paging
- Heap allocation
- System calls
- Low-level C and Assembly

## 🤖 AI-Assisted Development

LionOS is developed with extensive AI assistance. The project explores how AI can help design, implement, debug, and document low-level operating-system components while keeping the development process understandable and testable.

## 📜 License

MIT License. See [LICENSE](LICENSE).

## ⚠️ Status

LionOS is an early-stage experimental operating system. It is not intended for production use.

---

**Built by AngmarX 🦁**