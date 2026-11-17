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

The owning session can query or update a bounded attribute bitmask through `lion_tty_getattr()` and `lion_tty_setattr()`. At this milestone, supported flags are `LIONOS_TTY_ATTR_ECHO` and `LIONOS_TTY_ATTR_ISIG`; unknown bits are rejected. Both flags default on. Echo remains deferred to process-context reads. When ISIG is disabled, byte 0x03 is retained as canonical input instead of being routed as SIGINT. Canonical line buffering remains enabled in H21.9. Attribute operations are protected by the TTY input lock and restricted to the owning session.

The new syscalls are 60 (`TTY_GETATTR`) and 61 (`TTY_SETATTR`). This is a small termios foundation, not a complete POSIX termios implementation.


## H21.10 — Canonical mode control

`LIONOS_TTY_ATTR_ICANON` selects the input discipline. In canonical mode, the existing edit buffer, backspace handling, CR-to-LF normalization, and newline commit behavior remain active. In noncanonical mode, each incoming byte is placed directly into the bounded input ring, echo is queued unchanged when enabled, and blocked readers are woken after each accepted byte. Backspace and carriage return are data in this mode. `TTY_INPUT_DATA_READY` distinguishes immediate byte acceptance from a committed canonical line. The input queue remains bounded; overflow is reported without overwriting unread bytes. A change to ICANON is rejected while an uncommitted canonical edit is pending, preventing silent loss or reinterpretation of that partial line. VMIN/VTIME timing controls and full POSIX termios semantics are not included.


## H21.11 — Noncanonical VMIN threshold

The TTY attribute structure now includes `vmin` (0–255), defaulting to 1. In noncanonical mode, standard-descriptor blocking reads wait until at least `min(vmin, requested_capacity)` bytes are queued. Each byte arrival wakes sleepers so they can re-check the threshold under the input lock; bytes remain queued until the threshold is met. A VMIN value of zero makes an empty noncanonical read return immediately. Canonical reads retain line-commit behavior and ignore VMIN. VTIME and POSIX inter-byte timers remain out of scope.


## H21.12 — Noncanonical VTIME

The TTY attribute structure now includes `vtime` (0–255 deciseconds), defaulting to zero. In noncanonical mode the four VMIN/VTIME combinations follow the POSIX timing model:

- VMIN=0, VTIME=0: return immediately with available bytes, including zero when the queue is empty.
- VMIN=0, VTIME>0: start a total read timeout immediately; return as soon as at least one byte arrives, or return zero on expiry.
- VMIN>0, VTIME=0: wait until `min(VMIN, requested_capacity)` bytes are available.
- VMIN>0, VTIME>0: wait indefinitely for the first byte, then restart an inter-byte timeout after each wake/input byte; return available partial data when the inter-byte timer expires.

The timer uses the process scheduler's nominal 120 Hz tick source; each decisecond is converted to 12 scheduler ticks, rounded up. Timed waits use per-process timeout state so concurrent readers do not share or consume one another's timeout result. The process manager arms the deadline only after the scheduler has saved the blocked context. Input wakeups cancel the pending timeout before the read is retried; session release and process termination also clear the wait state.

Canonical reads ignore VTIME and retain newline-commit semantics. This implements the VMIN/VTIME read timing subset, not the full POSIX termios API.


## H21.13 — Foreground/background terminal I/O policy

The TTY I/O entry points now receive the calling process identity and enforce the session's foreground process group. A background process group attempting to read from the controlling TTY is denied and receives a pending SIGTTIN (21). Background writes remain permitted by default; when the TOSTOP attribute (0x08) is enabled, background writes are denied and the caller's process group receives pending SIGTTOU (22). Foreground reads and writes remain unaffected.

The Process Manager validates the caller, session, process-group membership, and foreground group while holding the process-table lock. It queues the terminal signal for every live process in the caller's background group and its threads. The VFS drops its descriptor-table lock before entering TTY policy, preserving the existing lock hierarchy. Explicit TTY syscalls and standard descriptors share the same policy.

This milestone queues SIGTTIN/SIGTTOU and rejects the disallowed I/O operation; it does not yet implement signal dispositions, default stop actions, or shell wait/continue job-control orchestration. Those remain separate work.


## H21.14 — waitpid stop notifications and nonblocking queries

`LIONOS_SYS_WAITPID_OPTIONS` (62) adds `LIONOS_WAIT_NOHANG` and `LIONOS_WAIT_UNTRACED` without changing legacy `WAITPID`. WNOHANG returns zero when no requested event is ready. WUNTRACED reports a stopped child once using `(signal << 8) | 0x7F`; `LIONOS_WIFSTOPPED` and `LIONOS_WSTOPSIG` decode the status. A blocked options-aware parent is awakened on a child stop. Wait state is preserved if the waiting parent itself is stopped and continued; pending child exit/stop events are rechecked on resume. SIGCONT clears an unreported stale stop event. H21.15 adds WCONTINUED (status `0xFFFF`) and default SIGTTIN/SIGTTOU job-control stops. A background TTY read stops its complete process group with SIGTTIN; a background write does so with SIGTTOU only when TOSTOP is enabled. SIGCONT resumes the group and publishes a one-shot continued event to the parent. The test verifies stop/continue reports for both operations. ## H21.16 — Orphaned process groups

When a process exits, its children are reparented to PID 1. If that transition makes a process group orphaned while it contains stopped members, LionOS queues SIGHUP for every process/thread in the group and resumes the group with SIGCONT. Background TTY reads and TOSTOP writes from an already-orphaned group fail without generating SIGTTIN/SIGTTOU. Adopted zombies are deferred for reaping until no CPU owns their execution context.

SIGHUP is currently recorded as pending; default termination and user signal dispositions are not implemented. Orphan transitions caused by SETPGID and SETSID are handled in H21.17. Shell orchestration remains future work.


## H21.17 — Orphan transitions caused by session and process-group changes

The orphaned-process-group transition check is now shared by all topology-changing paths. When `setsid()` moves a process and its threads to a new session, the previous process group is re-evaluated. When `setpgid()` moves a process/thread group, both the old and destination process groups are re-evaluated. Only a non-orphaned-to-orphaned transition that leaves at least one stopped process queues SIGHUP and resumes the group with SIGCONT; already-orphaned groups are not notified repeatedly. TTY readers are woken after a successful session/process-group change so blocked I/O revalidates foreground ownership. QEMU runtime coverage exercises both `setsid()`- and `setpgid()`-induced transitions, including the one-shot WCONTINUED event and pending SIGHUP observation.


## H21.18 — Default SIGHUP delivery

A pending SIGHUP now follows its default action: the target process terminates at its next syscall-return boundary with wait status `128 + SIGHUP`. Orphaned process-group transitions continue to queue SIGHUP and SIGCONT; this milestone does not add asynchronous preemption or user-installed signal handlers.

## H21.19 — Default SIGINT delivery

A pending SIGINT now follows its default action and terminates the target process at its next syscall-return boundary with wait status `128 + SIGINT`. The dedicated `sigint_test.elf` keeps this runtime assertion separate from the broader signal-control executable so each ELF remains within the existing 16 KiB validation limit. When both SIGHUP and SIGINT are pending, SIGHUP is delivered first. User-installed signal handlers, masks, and asynchronous delivery remain out of scope.

## H21.20 — User signal dispositions and safe handler return

Processes can register a userspace handler or SIG_IGN disposition for SIGINT and SIGHUP through LIONOS_SYS_SIGACTION. Handler and restorer addresses are accepted only when both resolve to executable user pages in the caller's address space. Dispositions are stored per process, inherited by fork(), and caught dispositions are reset by exec(); ignored dispositions remain ignored across exec().

At a syscall-return boundary, a pending handled signal is delivered by saving the complete 19-word interrupt context in kernel-owned process state and constructing a bounded two-word call frame on the validated user stack. The handler receives the signal number using the i386 cdecl convention. The userspace CRT provides a return trampoline that invokes LIONOS_SYS_SIGRETURN; the kernel restores the saved interrupt context and only then resumes the interrupted instruction stream. A process cannot invoke sigreturn unless it currently has an active signal frame. Nested handlers are deferred until the active handler returns.

The default actions for unhandled SIGINT and SIGHUP remain termination with status 128 + signal; ignored dispositions consume the pending signal without terminating. Only SIGINT and SIGHUP are accepted by this API; other signal numbers, including SIGKILL and SIGSTOP, cannot be registered. Signal actions are process-leader scoped. Signal masks and alternate signal stacks are not implemented. Delivery occurs at syscall-return boundaries and does not yet wake a process blocked in a wait. Nested delivery is deferred until the active handler returns.

The QEMU integration test registers a SIGINT handler before fork, verifies disposition inheritance, synchronizes with the child through IPC, delivers SIGINT, and checks that the handler returns through sigreturn and the child exits normally. Host CI does not replace physical keyboard or SMP stress validation.


## H21.21 — Interruptible waitpid

Blocking `waitpid()` and `waitpid_options()` now return `LIONOS_EINTR` (-4) when a pending, non-ignored SIGINT or SIGHUP arrives before a child event becomes available. Child exit/stop/continue events take precedence when already ready, WNOHANG retains its zero result, and ignored dispositions leave the wait blocked. A caught signal remains pending after the EINTR return and its handler is entered at the next syscall-return boundary. The scheduler publishes the blocked syscall frame before another CPU may wake it, preventing writes to a stale frame. Pipe, IPC, TTY and futex waits remain outside this milestone.
