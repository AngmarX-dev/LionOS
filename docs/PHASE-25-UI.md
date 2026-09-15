# Phase 25 — LionOS UI

Phase 25 is the first dedicated user-facing UI phase for LionOS.

## Text UI milestone

LionOS keeps the 80x25 VGA text console for the initial UI rather than jumping directly to a graphical framebuffer desktop.

### Implemented

- Colored VGA console output
- Branded LionOS shell banner
- Color-coded prompt and command output
- Clear visual distinction between normal output, status, and errors
- Polled PS/2 mouse initialization
- Text-mode mouse cursor with yellow-on-blue highlight
- Mouse position tracking across the 80x25 console grid
- Mouse button state tracking
- `mouse` shell command for position/button diagnostics
- Keyboard input remains on the existing PS/2 IRQ1 path

## Mouse architecture

The first mouse implementation uses **polling** rather than IRQ12. The kernel initializes the PS/2 auxiliary device before enabling interrupts, then the shell polls the controller while the LAPIC/PIT timer continues to wake the idle shell loop.

The mouse driver exposes:

```c
mouse_init();
mouse_poll();
mouse_show();
mouse_hide();
mouse_x();
mouse_y();
mouse_buttons();
```

This deliberately keeps the new UI input path isolated while SMP and scheduler subsystems remain experimentally SMP-safe rather than fully concurrent.

## Future UI work

- Mouse click actions for shell/UI widgets
- Scroll-wheel support
- Interactive text menus
- Selection/highlighting
- Better cursor rendering
- Framebuffer graphics mode
- Window manager / desktop experiments

## Validation

Every UI change must continue to pass:

```bash
make test
```

The Phase 23 stability suite remains the regression gate for Phase 25.
