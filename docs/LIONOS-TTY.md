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

The Process Manager validates PGID membership under its process-table lock. TTY state is updated only after validation. The Process Manager holds its process-table lock while validating the live PGID and updating the TTY foreground group; the lock order is process-table then TTY. TTY code never calls back into the Process Manager.

When the final non-thread process in a session exits or is terminated, the Process Manager releases that session's TTY and clears its input/foreground state. The TTY remains exclusive while any live process in the owning session remains.

The syscall ABI is now version 11 and UAPI version 12.

Still not implemented: keyboard backend feed, terminal read/write syscalls, echo/termios, foreground-group signal delivery, and job-control stop/continue semantics.


## H21.3 — Translated keyboard input bridge

The PS/2 scancode translator and USB HID boot-keyboard report translator now share the existing character insertion path. That path forwards each translated byte to `keyboard_tty_forward_byte()`, which feeds the software TTY, and then continues to update the legacy keyboard buffer. This keeps existing `keyboard_getchar()` and blocking keyboard readers intact while making the same translated input available to the claimed TTY session.

The TTY is initialized before `keyboard_init()` so its lock and state are ready before either PS/2 interrupts or later USB HID reports can reach the bridge. The bridge does not acquire the keyboard buffer lock while holding the TTY lock; the two input queues remain independently synchronized.

The TTY only accepts input while a session owns the console. Input sent while no session is claimed is ignored by the TTY bridge and remains available through the legacy keyboard queue. Ctrl-C is still surfaced as `TTY_INPUT_INTERRUPT` and flushes the current edit line; it does not yet deliver SIGINT. Terminal read/write file descriptors, echo, termios, and foreground process-group signal delivery remain future work.

Validation adds `make keyboard-tty-check`, covering character forwarding, backspace, line commit, Ctrl-C event propagation, and rejection when no session owns the TTY.


## H21.4 — Session-scoped TTY read/write syscalls

The native syscall ABI now exposes `LIONOS_SYS_TTY_READ` (58) and `LIONOS_SYS_TTY_WRITE` (59), with user wrappers `lion_tty_read()` and `lion_tty_write()`. Both require console capability and require the caller's live session to own the single console TTY. Requests are bounded by the 4096-byte user-copy limit and validate user-buffer access before copying.

TTY reads use the existing canonical queue and are currently nonblocking: they return a positive byte count, `LIONOS_TTY_EMPTY` when no completed line is queued, or the generic error sentinel. The caller can retry or yield; blocking TTY reads and wakeup integration are deferred so this stage does not introduce a new scheduler wait channel. TTY writes copy user data into kernel memory before sending it to the console and return the byte count on success. The legacy `SYS_READ` keyboard path and `SYS_WRITE` console path remain unchanged for compatibility.

The UAPI version is now 13 and the syscall ABI version is 12. The session test exercises an empty TTY read and a successful TTY write after claiming the terminal. Foreground process-group job control, echo, termios, standard descriptors 0/1/2, and VFS-backed terminal file descriptors remain future work.
