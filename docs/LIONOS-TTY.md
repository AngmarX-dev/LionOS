# LionOS TTY Core — H21

## Scope

H21.1 introduces a hardware-independent software TTY core for the firmware console. It is deliberately separate from PS/2 and xHCI drivers.

The current implementation provides:

- Exclusive console ownership by one session, claimed by its session leader.
- Foreground process-group storage scoped to the owning session.
- Canonical line editing with backspace and carriage-return normalization.
- A bounded completed-line queue and nonblocking reads.
- Ctrl-C recognition as an input event that flushes the current edit line.

## Important limitations

This is not yet a complete terminal device or POSIX job-control implementation.

- No keyboard backend calls `tty_input_byte()` yet; PS/2 and USB input behavior is unchanged.
- Ctrl-C is reported as `TTY_INPUT_INTERRUPT`; it does not deliver SIGINT.
- No syscall, VFS device node, standard file descriptors, terminal output processing, echo, termios, or blocking read is exposed yet.
- `tty_set_foreground_pgid()` enforces TTY session ownership, but the caller must separately verify that the PGID exists and belongs to that SID. This validation belongs in the Process Manager integration stage.
- The current core has one console TTY instance, not a dynamic TTY allocator.

## Kernel API contract

`tty_claim_session(sid, leader_pid)` accepts only a nonzero session whose leader PID equals its SID. A different session cannot steal an already claimed console.

`tty_release_session(sid)` releases only the matching owner and clears queued and edited input.

`tty_set_foreground_pgid(sid, pgid)` accepts only the current owner SID and a nonzero PGID. It does not independently inspect process tables.

`tty_input_byte(byte)` is intended for a future translated-input bridge. It buffers printable bytes until newline, handles backspace, and reports Ctrl-C without performing signal delivery.

`tty_read(sid, destination, capacity)` copies only completed canonical input into a kernel buffer. It is nonblocking and returns `TTY_READ_EMPTY` when no completed bytes are queued. Syscall integration must validate user pointers and perform usercopy separately.

## Validation

`make tty-check` runs a host-side test with a test-only spinlock stub. It covers session ownership, foreground PGID storage, canonical editing, partial reads, Ctrl-C flush behavior, line-size bounds, and release cleanup. This host test does not validate hardware input or kernel interrupt concurrency.

## H21.2 — Session and Process Group integration

The syscall interface now provides:

- `lion_tty_claim()`: only a session leader with console capability can claim the single console TTY.
- `lion_tty_getpgrp()`: returns the foreground process group for the caller's session.
- `lion_tty_setpgrp(pgid)`: changes the foreground group only when a live process group exists in the caller's session.

The Process Manager validates PGID membership under its process-table lock. TTY state is updated only after validation. The process-table lock is released before the TTY lock is acquired.

When the final non-thread process in a session exits or is terminated, the Process Manager releases that session's TTY and clears its input/foreground state. The TTY remains exclusive while any live process in the owning session remains.

The syscall ABI is now version 11 and UAPI version 12.

Still not implemented: keyboard backend feed, terminal read/write syscalls, echo/termios, foreground-group signal delivery, and job-control stop/continue semantics.
