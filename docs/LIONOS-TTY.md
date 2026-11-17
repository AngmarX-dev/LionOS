# LionOS TTY Core — H21

## Scope

H21.1 introduces a hardware-independent software TTY core for the firmware console. It is deliberately separate from PS/2 and xHCI drivers.

The current implementation provides:

- Exclusive console ownership by one session, claimed by its session leader.
- Foreground process-group storage scoped to the owning session.
- Canonical line editing with backspace and carriage-return normalization.
- A bounded completed-line queue and nonblocking reads.
- Ctrl-C recognition, foreground-process-group SIGINT queuing, and bounded deferred visual echo.

## Important limitations

This is not yet a complete terminal device or POSIX job-control implementation.

- PS/2 and USB HID translated input reaches the TTY through the common keyboard character path.
- Ctrl-C queues SIGINT for the owning session's validated foreground process group. It does not yet implement userspace signal handlers or default SIGINT termination.
- Canonical echo is deferred until a process-context TTY read drains the bounded echo queue; it is not immediate per-keystroke echo. The initial runtime attribute API toggles echo and Ctrl-C signal processing; canonical mode remains fixed. Full POSIX `termios`, dynamic TTY devices, and complete job control remain unimplemented.
- `tty_set_foreground_pgid()` enforces TTY session ownership. The Process Manager validates PGID existence and session membership while holding the process-table lock before updating foreground state.
- The current core has one console TTY instance, not a dynamic TTY allocator.

## Kernel API contract

`tty_claim_session(sid, leader_pid)` accepts only a nonzero session whose leader PID equals its SID. A different session cannot steal an already claimed console.

`tty_release_session(sid)` releases only the matching owner and clears queued and edited input.

`tty_set_foreground_pgid(sid, pgid)` accepts only the current owner SID and a nonzero PGID. It does not independently inspect process tables.

`tty_input_byte(byte)` is called by the translated keyboard bridge. It buffers printable bytes until newline, handles backspace, queues deferred echo, and routes Ctrl-C to the validated foreground process group.

`tty_read(sid, destination, capacity)` copies only completed canonical input into a kernel buffer. It is nonblocking and returns `TTY_READ_EMPTY` when no completed bytes are queued; before reading, it drains queued echo from process context. Syscall integration must validate user pointers and perform usercopy separately.

## Validation

`make tty-check` runs a host-side test with a test-only spinlock stub. It covers session ownership, foreground PGID storage, canonical editing and echo, backspace visual output, partial reads, Ctrl-C signal routing and marker, line-size bounds, and release cleanup. This host test does not validate hardware input or kernel interrupt concurrency.

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

The TTY only accepts input while a session owns the console. Input sent while no session is claimed is ignored by the TTY bridge and remains available through the legacy keyboard queue. Ctrl-C is still surfaced as `TTY_INPUT_INTERRUPT` and flushes the current edit line; it does not yet deliver SIGINT. Terminal echo, termios, and foreground process-group signal delivery remain future work. Standard TTY descriptors and blocking stdin were integrated in H21.5 and H21.6.

Validation adds `make keyboard-tty-check`, covering character forwarding, backspace, line commit, Ctrl-C event propagation, and rejection when no session owns the TTY.


## H21.4 — Session-scoped TTY read/write syscalls

The native syscall ABI now exposes `LIONOS_SYS_TTY_READ` (58) and `LIONOS_SYS_TTY_WRITE` (59), with user wrappers `lion_tty_read()` and `lion_tty_write()`. Both require console capability and require the caller's live session to own the single console TTY. Requests are bounded by the 4096-byte user-copy limit and validate user-buffer access before copying.

The explicit `lion_tty_read()` syscall remains nonblocking and returns a positive byte count, `LIONOS_TTY_EMPTY` when no completed line is queued, or the generic error sentinel. Standard descriptor 0 uses the VFS `FREAD` path and blocks atomically on a dedicated TTY wait channel when its canonical queue is empty. The dedicated TTY input lock remains held until `process_sleep_on()` has published the waiting state, preventing a lost wakeup. It is separate from the TTY ownership/foreground lock so the sleep path does not invert the Process Manager's process-table-to-TTY lock order. Committing a newline wakes waiting readers after releasing the input lock. Releasing the owning session also wakes readers so they re-check ownership and fail rather than sleeping indefinitely. The syscall dispatcher restarts `FREAD` after wakeup, causing the VFS descriptor and session permissions to be revalidated. TTY writes copy user data into kernel memory before sending it to the console and return the byte count on success. The legacy `SYS_READ` keyboard path and `SYS_WRITE` console path remain unchanged for compatibility.

The UAPI version is now 13 and the syscall ABI version is 12. The session test exercises an empty TTY read and a successful TTY write after claiming the terminal. Standard descriptors 0/1/2 are reserved as session-scoped TTY file descriptions for each newly loaded userspace process. Descriptor 0 is read-only; descriptors 1 and 2 are write-only. `lion_fread(0, ...)` and `lion_fwrite(1|2, ...)` route through the VFS descriptor layer to the claimed session TTY. The descriptors are inherited by `fork`, retained across `exec`, and released by existing process descriptor cleanup. Closing a standard descriptor is permitted and does not silently recreate it. The explicit `lion_tty_read()` syscall remains nonblocking and returns `LIONOS_TTY_EMPTY` when no canonical line is ready; standard descriptor 0 blocks until a canonical line is available. Foreground process-group job control, echo, termios, and signal delivery for Ctrl-C remain future work.


## H21.5 — Standard TTY descriptors

New userspace processes receive VFS-backed descriptors 0, 1, and 2 for standard input, output, and error. Descriptor 0 is read-only; descriptors 1 and 2 are write-only. The open-file descriptions participate in the existing reference-counted descriptor lifecycle, including fork inheritance, exec preservation, and process teardown. TTY output is serialized against session release, and the VFS drops its lock before entering TTY operations.

## H21.6 — Blocking canonical stdin

`lion_fread(0, ...)` now sleeps when no completed canonical line is available. The TTY input lock and generic `process_sleep_on()` protocol close the check-to-sleep race. Newline commit wakes all readers on the TTY channel; the syscall restarts and revalidates the descriptor and session before consuming queued bytes. Session release also wakes readers. The direct `lion_tty_read()` syscall deliberately remains nonblocking for compatibility. Host tests cover the empty-queue sleep path, newline wakeup, line consumption, and session-release wakeup. Hardware-level keyboard/QEMU injection has not been separately validated by this host test.

## H21.7 — Ctrl-C foreground signal routing

The canonical TTY now treats byte 0x03 as a foreground interrupt request. It discards the current uncommitted edit line, snapshots the owning session and foreground PGID, releases the input/state locks, and asks the Process Manager to queue SIGINT for every live process in that process group and its live threads.

The Process Manager revalidates that the TTY still belongs to the same session, that the foreground PGID has not changed, and that every target belongs to that session before changing any pending-signal bit. The process-table-to-TTY lock order matches the existing foreground-PGID update path. No process-table lock is acquired while the keyboard/TTY input lock is held.

This milestone queues SIGINT (signal number 2) and makes it visible through the existing pending-signal query. It does **not** implement userspace signal handlers, default SIGINT termination, terminal echo, or complete POSIX job-control semantics. A Ctrl-C request is rejected if the foreground group is stale or no longer belongs to the owning session.

The host TTY test verifies that Ctrl-C clears the edit buffer and dispatches the expected session/foreground-PGID request through a test stub; it does not execute the Process Manager's group/session validation. The kernel CI suite verifies compilation and existing regression paths, but a dedicated runtime test for this new Process Manager helper and physical keyboard testing are still outstanding.
 
## H21.8 — Canonical input echo

Canonical input now queues visual echo bytes for ordinary characters, accepted tabs, newline, backspace editing, and the Ctrl-C marker. The keyboard input path only appends to a bounded echo queue while holding the input lock; it never writes to the console from keyboard interrupt context.

Echo is drained by a process-context TTY read. The flush takes locks in output → input → TTY-state order, releases the input lock before console output, and serializes the copied bytes under the output lock so concurrent readers cannot reorder echo output. If the bounded echo queue is full, additional visual echo is dropped without rejecting or corrupting canonical input. Session release resets the echo queue with the other TTY buffers.

Host tests verify ordinary echo, backspace visual editing, Ctrl-C display marker, newline, and maximum-length input. Echo is deferred until a reader drains it; immediate per-keystroke display and hardware keyboard testing remain future work.


## H21.9 — Runtime TTY attributes

The owning session can query or update a bounded attribute bitmask through `lion_tty_getattr()` and `lion_tty_setattr()`. Supported flags are `LIONOS_TTY_ATTR_ECHO`, `LIONOS_TTY_ATTR_ISIG`, and `LIONOS_TTY_ATTR_ICANON`; unknown bits are rejected. All three flags default on. Echo remains deferred to process-context reads. When ISIG is disabled, byte 0x03 is retained as input instead of being routed as SIGINT. With ICANON enabled, input is edited and committed at newline. With ICANON disabled, every byte is queued immediately, including backspace and carriage return, and readers are woken for each accepted byte. The byte-oriented mode does not implement VMIN/VTIME yet. Attribute operations are protected by the TTY input lock and restricted to the owning session.

The new syscalls are 60 (`TTY_GETATTR`) and 61 (`TTY_SETATTR`). This is a small termios foundation, not a complete POSIX termios implementation.


## H21.10 — Canonical mode control

`LIONOS_TTY_ATTR_ICANON` selects the input discipline. In canonical mode, the existing edit buffer, backspace handling, CR-to-LF normalization, and newline commit behavior remain active. In noncanonical mode, each incoming byte is placed directly into the bounded input ring, echo is queued unchanged when enabled, and blocked readers are woken after each accepted byte. Backspace and carriage return are data in this mode. `TTY_INPUT_DATA_READY` distinguishes immediate byte acceptance from a committed canonical line. The input queue remains bounded; overflow is reported without overwriting unread bytes. A change to ICANON is rejected while an uncommitted canonical edit is pending, preventing silent loss or reinterpretation of that partial line. VMIN/VTIME timing controls and full POSIX termios semantics are not included.
