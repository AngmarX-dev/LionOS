# Phase 25 — LionOS UI

Phase 25 begins the user-facing UI work for LionOS.

## v0.1 UI direction

LionOS currently uses the classic 80x25 VGA text console. The first UI milestone keeps that architecture and improves the shell experience without changing kernel/process behavior.

### First goals

- Colored VGA console output
- Cleaner LionOS shell banner
- Color-coded prompt
- Improved command/help presentation
- Clear visual distinction between normal output and errors
- Preserve the existing keyboard, VFS, ELF, SMP, and persistence behavior

## Current scope

This phase is a **text-mode UI/TUI** milestone, not a graphical framebuffer desktop.

A future graphical UI can be added after the text-console layer is stable.

## Validation

Every UI change must continue to pass:

```bash
make test
```

The Phase 23 stability suite remains the regression gate for Phase 25.
