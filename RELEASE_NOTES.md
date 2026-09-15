# LionOS v1.0.0

## First Release — CLI Edition

LionOS v1.0.0 is the first public release of LionOS, an experimental operating system built from scratch with AI-assisted development.

### Included

- Multiboot2 / GRUB boot
- GDT and IDT
- Interrupt handling
- PIC and PIT support
- Keyboard input
- Physical memory management
- Paging / virtual memory
- Kernel heap
- Processes and scheduler
- System calls and Ring 3 userspace
- ELF userspace loader
- `fork()`, `waitpid()`, and `exec()`
- Userspace libc and command-line utilities
- RAMFS and persistent LionFS storage
- VFS and filesystem syscalls
- IPC
- Signals and process control
- Loopback IPv4 networking
- Security hardening and userspace pointer validation
- SMP / multicore foundations
- Automated build and stability-test infrastructure

## User Interface

v1.0.0 is intentionally a command-line release. A graphical interface is planned for LionOS v2.0.

## Status

This release is intended as the first development milestone of LionOS. Hardware support is limited and the primary target is QEMU/i386 during development.

## Build

```bash
make
make iso
make disk
make run
```

For the automated test suite:

```bash
make test
```

## Project

https://github.com/AngmarX-dev/LionOS
