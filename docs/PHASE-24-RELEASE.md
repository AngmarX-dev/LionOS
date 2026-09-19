# LionOS Phase 24 — Documentation & Release Preparation

Phase 24 packages the completed Phase 22 SMP bring-up and Phase 23 stability work into a release-ready project state.

## Verified baseline

The current Phase 23 suite verifies:

- 32-bit kernel build
- Userspace ELF32/i386 validation
- ISO generation
- Two-CPU QEMU boot
- AP INIT/SIPI startup
- Per-CPU TSS initialization
- Per-CPU IDT loading
- AP Local APIC timer initialization
- SMP online handshake
- First-boot persistent filesystem initialization
- Second-boot persistent filesystem verification

Run the complete suite with:

```bash
make test
```

A successful run ends with:

```text
LionOS Phase 23 stability test: PASS
```

## Release build

Build the kernel, userspace, ISO, and persistent disk image with:

```bash
make clean
make
make userland
make iso
make disk
```

The bootable image is generated at:

```text
build/lionos.iso
```

The persistent test disk is generated at:

```text
build/lionos-disk.img
```

## Release validation

Before cutting a public release:

1. Run `make test` successfully.
2. Confirm the ISO is produced and `grub-file --is-x86-multiboot2 build/lionos.bin` succeeds.
3. Boot the ISO with QEMU using two virtual CPUs.
4. Confirm `LIONOS:SMP-CPU-ONLINE` and `LIONOS:READY` appear.
5. Confirm the persistence test reaches `LIONOS:PERSIST-OK` on the second boot.
6. Review the Git diff and repository status for unintended files.
7. Record the release commit and generate release notes.

## Known scope limitations

The SMP implementation is still a bootstrap layer rather than a fully concurrent scheduler. Shared process state, memory, VFS, IPC, and loopback networking now have IRQ-safe locking foundations, while APs remain out of normal userspace scheduling until per-CPU scheduler state and broader hardware enumeration are complete.

Not yet release-complete production features include:

- Fully concurrent per-CPU scheduling
- ACPI MADT-based CPU enumeration
- Full privilege/capability separation
- Physical network-card drivers
- Copy-on-write memory
- Automated end-to-end userspace VFS integration coverage

These limitations are expected for an educational experimental operating system.

## Release principle

A LionOS release should be treated as a reproducible experimental milestone, not a production operating-system distribution.
