# Phase 26 — LionOS Graphical UI

Phase 26 begins the real graphical desktop layer while preserving the Phase 23 stability baseline and the Phase 25 text UI as the fallback path.

## Stage 26.1 — Framebuffer foundation

- Multiboot2 framebuffer request: 1024x768x32
- GRUB graphical payload configuration
- Framebuffer discovery from the Multiboot2 framebuffer tag
- High-memory framebuffer mapping through the existing kernel MMIO page table
- Basic 32-bit RGB pixel and rectangle drawing
- Dark desktop background with LionOS gold/blue UI accents
- Framebuffer-backed text console with a small built-in bitmap font

## Stage 26.2 — Input

Next:

- PS/2 mouse integration with the graphical cursor
- Keyboard event routing into the graphical shell
- Click/focus handling

## Stage 26.3 — Desktop compositor

Planned:

- Window surfaces
- Window borders and title bars
- Dragging and focus
- Minimize/maximize/close controls
- Taskbar/dock
- Desktop launcher

## Stage 26.4 — Native applications

Planned first applications:

- Terminal
- File manager
- Text editor
- Settings
- System information

## Regression gate

Every graphical change must continue to pass:

```bash
make test
```

The graphical layer is additive. A framebuffer failure must fall back to the existing VGA console rather than making the kernel unbootable.
