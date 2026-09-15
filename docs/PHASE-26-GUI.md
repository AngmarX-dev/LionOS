# Phase 26 — LionOS Graphical UI

Phase 26 begins the transition from the VGA text console to a real graphical desktop environment.

## Design goals

- Keep the existing text shell available as a recovery/debug fallback.
- Add a framebuffer abstraction instead of drawing directly from shell code.
- Introduce a simple 2D graphics primitive layer: pixels, rectangles, lines, and text.
- Build a compositor/window manager with movable windows, title bars, buttons, and focus.
- Integrate keyboard and mouse input into GUI events.
- Add a desktop background, taskbar, launcher, and clock.
- Build a small set of native GUI applications.

## Planned architecture

```text
                 LionOS Kernel
                       |
             +---------+---------+
             |                   |
        Input devices       Framebuffer
        keyboard/mouse          |
             |             2D graphics
             |                   |
             +---------+---------+
                       |
                GUI event queue
                       |
                 Window manager
                       |
                 Compositor
                 /     |      \
                /      |       \
            Desktop  Taskbar  Windows
```

## UI direction

The first desktop should feel familiar like a lightweight combination of classic Windows and Linux desktop conventions while keeping LionOS's own visual identity:

- dark blue/black desktop background
- LionOS gold/yellow accents
- blue title bars and panels
- clean rectangular windows
- visible focus state
- simple Start/launcher menu
- taskbar with running applications
- mouse cursor

The project should not copy proprietary artwork, icons, or branding from Windows/macOS/Linux. The goal is familiar interaction patterns with an original LionOS visual design.

## Milestones

### 26.1 Framebuffer foundation
- framebuffer information structure
- pixel format handling
- back buffer
- clear/fill operations
- rectangle primitives
- bitmap/font rendering

### 26.2 GUI input
- mouse cursor rendering
- mouse movement events
- left/right/middle button events
- keyboard events
- GUI focus handling

### 26.3 Window manager
- window creation/destruction
- z-order
- focus
- dragging
- minimize/close controls
- clipping
- repainting

### 26.4 Desktop shell
- desktop background
- taskbar
- launcher/menu
- clock
- system status area

### 26.5 Native applications
- terminal
- file manager
- text editor
- system information
- settings

## Regression rule

Every GUI milestone must keep the existing kernel regression suite passing:

```bash
make test
```

SMP, persistence, userspace, and storage behavior must not regress because of UI work.
