# LionOS User ABI and System Call Contract

**Status:** specification and conformance guards for the native syscall ABI. Stage F6 adds a read-only ext2 directory-stream syscall and user-space wrappers.
**Target:** 32-bit x86, ELF32 / EM_386, Ring 3.
**UAPI version:** 16 (LIONOS_UAPI_VERSION).
**System-call ABI:** 14 (LIONOS_SYSCALL_ABI).
**Version query:** syscall number 0 returns 14.
**Canonical headers:** include/uapi.h, include/vfs_uapi.h, include/syscall.h, include/user_api.h.

## 1. Scope and compatibility

This is LionOS's native user/kernel ABI. It is not the Linux i386 syscall ABI and does not promise binary compatibility with Linux applications, glibc, musl's Linux backend, or Ubuntu packages.

`struct lion_tty_attributes` now contains `flags` and `vmin` (0–255); `vmin` applies only to noncanonical blocking reads and defaults to 1. The UAPI version describes published user-facing constants and structures. The syscall ABI version describes the low-level syscall calling contract. These are separate version values.

Existing syscall numbers, meanings, argument order, structure layouts, and sentinel values are compatibility-sensitive. Never reuse a number or silently change an existing meaning. Add calls only at unused numbers and update headers, dispatcher, wrappers, documentation, and tests together. An incompatible change requires an explicit ABI version decision and migration plan.

## 2. Execution and syscall entry

- User applications are ELF32 executables for EM_386, entered at _start in user/crt0.S.
- The syscall entry is software interrupt int $0x80.
- IDT vector 128 is installed as a 32-bit interrupt gate with DPL 3 (flags 0xEE).
- EAX carries the syscall number; up to five 32-bit arguments are passed in EBX, ECX, EDX, ESI, and EDI, in that order.
- The 32-bit result is returned in EAX.
- The interrupt stub saves general and segment registers. The dispatcher replaces the saved EAX result. Do not rely on undocumented register side effects.
- The syscall is synchronous unless the selected call documents a blocking/reschedule result.

| Register | Meaning |
|---|---|
| EAX | syscall number on entry; result on return |
| EBX | argument 0 |
| ECX | argument 1 |
| EDX | argument 2 |
| ESI | argument 3 |
| EDI | argument 4 |

The inline wrappers in include/user_api.h are the preferred interface for native user programs.

## 3. Integer widths, pointers, and structures

- The ABI is 32-bit. User pointers and syscall arguments are represented as 32-bit values.
- Public scalar fields use fixed-width integer types.
- Boundary structures must document field order, width, alignment, and size. Do not make native pointers, long, bit-fields, or compiler-dependent enums part of a persistent ABI layout.
- Current user memory validation accepts addresses in [0x10000000, 0xC0000000). Non-zero ranges must not overflow the upper bound and must be mapped in the current address space. Writable output ranges additionally require writable mappings or a resolvable copy-on-write page.
- A zero-length range still requires the pointer value to lie inside the user virtual-address interval; it does not prove that a page is mapped.
- Several syscall paths cap a single user buffer at 4096 bytes. Individual APIs may impose smaller limits.
- Strings are copied into bounded kernel buffers and must contain a NUL terminator within the destination capacity.

### 3.1 Published boundary structures

| Structure | Size | Layout |
|---|---:|---|
| struct lion_stat | 12 bytes | size at 0, backend at 4, flags at 8; uint32_t fields |
| struct lion_mouse_event | 12 bytes | dx at 0, dy at 4, buttons at 8, source at 9, reserved bytes at 10 |
| struct lion_dirent | 264 bytes | inode at 0, type at 4, name at 5; 256-byte name array and 4-byte structure alignment |

The two reserved bytes in lion_mouse_event are currently written as zero and must be ignored by callers. tests/uapi_layout.c protects these sizes and offsets at compile time.

Internal kernel structures such as process, ipc_message, and net_packet are not stable user ABI structures merely because their declarations are reachable through current headers. User programs must rely on syscall contracts, not kernel-private layouts.

## 4. Return and error model

The current ABI returns a raw unsigned 32-bit value in EAX. It does not implement POSIX errno or Linux's -errno range.

| Value | Current meaning |
|---|---|
| 0 | LIONOS_SYSCALL_OK for calls using zero-success convention |
| 0xFFFFFFFF | LIONOS_SYSCALL_ERR for generic failure |
| 0xFFFFFFFE | Context-specific empty or blocked sentinel; not a universal error |
| Other values | Call-specific result: byte count, PID/TID, state, IP value, handle, etc. |

Interpret each result according to its syscall. 0xFFFFFFFE means empty for some nonblocking receive calls and blocked/reschedule for selected blocking calls. It must not be globally translated into one errno value.

Some kernel functions return signed int32_t values which are cast to the raw uint32_t result. Wrappers that require signed interpretation cast the result back to int32_t. Non-zero does not universally mean failure.

## 5. Syscall registry

The numeric registry is defined in include/uapi.h and include/vfs_uapi.h. The dispatcher is src/syscall.c.

| Number | Symbol | Arguments (in order) | Result / notes |
|---:|---|---|---|
| 0 | SYS_ABI_VERSION | — | Returns LIONOS_SYSCALL_ABI (14) |
| 1 | LIONOS_SYS_PUTC | character | 0 on success |
| 2 | LIONOS_SYS_GETPID | — | Current process ID |
| 3 | LIONOS_SYS_YIELD | — | Requests scheduler yield |
| 4 | LIONOS_SYS_EXIT | exit code | Terminates current process |
| 5 | LIONOS_SYS_GETCHAR | — | Character value or generic error |
| 6 | LIONOS_SYS_KBD_AVAIL | — | Available keyboard input count |
| 7 | LIONOS_SYS_WRITE | buffer, length | Console bytes written; max 4096 |
| 8 | LIONOS_SYS_READ | buffer, capacity | Bytes read; max 4096 |
| 9 | LIONOS_SYS_CLEAR | — | 0 on success |
| 10 | LIONOS_SYS_MEMINFO | — | Free physical page count; read-only metadata query used by the native `meminfo.elf` utility |
| 11 | LIONOS_SYS_EXEC | NUL-terminated program name | Replaces current image; returns PID on success |
| 12 | LIONOS_SYS_FORK | — | Child PID in parent; child receives 0 |
| 13 | LIONOS_SYS_WAITPID | PID (0 = any child), writable status pointer | Child PID on completion; may block |
| 14 | LIONOS_SYS_OPEN | path, flags | File descriptor or generic error |
| 15 | LIONOS_SYS_CLOSE | file descriptor | VFS close result |
| 16 | LIONOS_SYS_FREAD | fd, writable buffer, capacity | Bytes read / signed VFS result |
| 17 | LIONOS_SYS_FWRITE | fd, readable buffer, length | Bytes written / signed VFS result |
| 18 | LIONOS_SYS_REMOVE | path | VFS remove result |
| 19 | LIONOS_SYS_STAT | path, writable lion_stat pointer | 0 on success |
| 44 | LIONOS_SYS_READDIR | path, writable uint32 offset pointer, writable lion_dirent pointer | 1 entry, 0 EOF, generic error; ext2 directories only |
| 20 | LIONOS_SYS_IPC_SEND | receiver PID, data, length | IPC result; max message 128 bytes |
| 21 | LIONOS_SYS_IPC_RECV | writable data, capacity, optional sender-PID pointer | Bytes, empty sentinel, or error |
| 22 | LIONOS_SYS_IPC_PENDING | — | Pending message count |
| 23 | LIONOS_SYS_GETPPID | — | Parent PID |
| 24 | LIONOS_SYS_KILL | target PID, signal | Target must be a descendant/child |
| 25 | LIONOS_SYS_GETSTATE | PID | Process state or signed failure |
| 26 | LIONOS_SYS_SIGPENDING | PID | Pending signal bitmask |
| 27 | LIONOS_SYS_NET_SEND | destination IP, packed source/destination ports, data, length | Max payload 256 bytes |
| 28 | LIONOS_SYS_NET_RECV | port, writable data, capacity, optional source-IP and source-port pointers | Bytes, empty sentinel, or error |
| 29 | LIONOS_SYS_NET_PENDING | port | Pending packet count |
| 30 | LIONOS_SYS_NET_GETIP | — | Local IPv4 value |
| 31 | LIONOS_SYS_GETFILE | index, writable name buffer, capacity | Name length; capacity at least 2 |
| 32 | LIONOS_SYS_NET_PING | target IP | Ping result |
| 33 | LIONOS_SYS_MOUSE_READ | writable lion_mouse_event pointer | 0, blocked sentinel, or error |
| 34 | LIONOS_SYS_MOUSE_PENDING | — | Pending mouse event count |
| 35 | LIONOS_SYS_GETTID | — | Current thread ID |
| 36 | LIONOS_SYS_THREAD_CREATE | user entry address | TID or error |
| 37 | LIONOS_SYS_THREAD_JOIN | TID, writable status pointer | TID on completion; may block |
| 38 | LIONOS_SYS_THREAD_EXIT | exit code | Terminates current thread |
| 39 | LIONOS_SYS_PIPE_CREATE | writable read-handle pointer, writable write-handle pointer | Pipe creation result |
| 40 | LIONOS_SYS_PIPE_READ | handle, writable data buffer, capacity | Bytes read or blocked/error; capacity max 128 |
| 41 | LIONOS_SYS_PIPE_WRITE | handle, readable data buffer, length | Bytes written or blocked/error; length max 128 |
| 42 | LIONOS_SYS_PIPE_CLOSE | handle | Pipe close result |
| 43 | LIONOS_SYS_IPC_RECV_BLOCKING | writable data, capacity, optional sender-PID pointer | Bytes or blocked/error |
| 47 | LIONOS_SYS_FUTEX_WAIT | aligned word, expected value | Blocks while word matches expected |
| 48 | LIONOS_SYS_FUTEX_WAKE | aligned word, maximum waiters | Number of woken waiters |
| 49 | LIONOS_SYS_FUTEX_WAIT_TIMEOUT | aligned word, expected value, timeout ticks | 0 wake, 2 timeout, 3 interrupted, generic error |
| 50 | LIONOS_SYS_GETPGID | PID (0 = caller) | Process group ID or error |
| 51 | LIONOS_SYS_SETPGID | PID (0 = caller), PGID (0 = target PID) | Set process group; may join caller's group or a direct child's group |
| 52 | LIONOS_SYS_KILLPG | PGID, signal | Deliver signal to all live descendant process leaders in the group; caller's own group is rejected |
| 53 | LIONOS_SYS_SETSID | — | Create a new session and process group; fails if caller is already a process-group leader |
| 54 | LIONOS_SYS_GETSID | PID (0 = caller) | Session ID or error
| 55 | LIONOS_SYS_TTY_CLAIM | — | Claim the single console TTY; session leader only |
| 56 | LIONOS_SYS_TTY_GETPGRP | — | Foreground process group for caller's TTY session |
| 57 | LIONOS_SYS_TTY_SETPGRP | PGID | Set foreground process group within caller's session |
| 58 | LIONOS_SYS_TTY_READ | writable buffer, capacity | Canonical bytes, empty sentinel, or error; nonblocking; max 4096 |
| 59 | LIONOS_SYS_TTY_WRITE | readable buffer, length | Console bytes for owning TTY session; max 4096 |
| 60 | LIONOS_SYS_TTY_GETATTR | writable `struct lion_tty_attributes *` | Read supported TTY flags and VMIN threshold |
| 61 | LIONOS_SYS_TTY_SETATTR | readable `struct lion_tty_attributes *` | Set ECHO/ISIG/ICANON flags and VMIN; unknown values rejected

### Capability and validation notes

- Console calls require PROCESS_CAP_CONSOLE.
- File/VFS calls require PROCESS_CAP_FS.
- Mutating process/thread operations such as EXEC, FORK, WAITPID, KILL, KILLPG, SETPGID, SETSID, THREAD_CREATE, and THREAD_JOIN require PROCESS_CAP_PROCESS. GETSID is also capability-gated; EXIT and other read-only metadata queries are not all gated by that capability.
- IPC and pipe calls require PROCESS_CAP_IPC.
- Network calls require PROCESS_CAP_NET.
- The dispatcher validates user pointers before dereferencing user buffers. Each call also applies capacity, ownership, and state checks.
- NET_RECV treats source metadata pointers as optional. If a non-null pointer is not writable, current code skips writing that metadata rather than failing the receive. This is legacy behavior, not a recommended pattern for new calls.
- WAITPID and THREAD_JOIN require writable status pointers; PIPE_READ requires a writable destination buffer, while PIPE_WRITE requires a readable source buffer. READDIR requires separately writable, non-overlapping offset and directory-entry buffers. The conformance checker guards these access directions against accidental regression.
- THREAD_CREATE checks that its entry address lies in the user address interval, but the current syscall path does not prove that the address is mapped executable before scheduling the thread; an invalid entry can fault when the thread runs.
- MEMINFO and the GETPPID, GETSTATE, SIGPENDING, and GETTID metadata queries are not all capability-gated in the current dispatcher. Their access policy should be reviewed before exposing additional untrusted applications.

## 6. User process entry

The loader creates an initial stack containing argc, argv[0], and a null argv[1] terminator. user/crt0.S passes argc and argv to main, then routes main's return status through lion_exit_code_bridge.

This is not a complete System V i386 process-start ABI. Environment vectors, auxiliary vectors, TLS setup, dynamic linking, and ELF shared objects are not provided.

## 7. POSIX-inspired compatibility subset

The native process-group layer assigns each new process a group named by its PID, inherits PGID across fork, and shares PGID across threads. SETPGID can create a group led by the caller or a direct child, join the caller's group, or join a group led by another direct child. KILLPG snapshots live non-thread process leaders in the requested group and signals them only when every member is a descendant of the caller; signaling the caller's own group is rejected. SIGSTOP and SIGCONT are propagated to the leader's user threads. Direct SIGTERM/SIGKILL and KILLPG termination fail closed when a target process has any live user thread, because remote thread-stack reclamation is not yet safe. Group membership is evaluated at syscall dispatch, so later membership changes do not alter the captured target set. Each newly created process starts a session whose SID and initial PGID equal its PID; fork inherits SID and PGID, and user threads share both. SETSID creates a new session and process group atomically for the caller and its threads, and fails for process-group leaders. SETPGID rejects cross-session moves and attempts to change a session leader's process group. GETSID reports the SID of a live process. Controlling terminals and foreground/background terminal job control are not implemented.

The current first compatibility slice is a user-space wrapper layer in include/posix_compat.h and user/posix_compat.c. It maps these native LionOS services:

| Wrapper | Native service | Scope |
|---|---|---|
| lion_posix_open | OPEN | Existing LionOS read/write/append flags only |
| lion_posix_read | FREAD | One buffer up to 4096 bytes |
| lion_posix_write | FWRITE | One buffer up to 4096 bytes |
| lion_posix_close | CLOSE | Native process descriptor |
| lion_posix_stat | STAT | Path-based metadata |
| lion_posix_unlink | REMOVE | Native VFS removal |
| opendir / readdir / closedir | READDIR + STAT | Heap-owned directory stream; currently ext2 read-only directories only |

The wrapper rejects unknown open flags, requires READ or WRITE, and requires WRITE when APPEND is selected. It returns -1 for wrapper-detected errors and otherwise preserves the native syscall result. It does not expose errno, O_CREAT/O_TRUNC, fstat, lseek, dup/dup2, or POSIX standard descriptors (0/1/2). Directory streams are a LionOS extension over the native READDIR syscall: `opendir` validates that the path is a directory, `readdir` returns an entry pointer or null, and `closedir` releases the stream. Because errno is not implemented, null from `readdir` does not distinguish EOF from an I/O or validation error. The current kernel READDIR backend supports ext2 only. Zero-length read/write calls are treated as no-ops and do not validate the descriptor, matching the wrapper's documented limited scope rather than full POSIX behavior.

The QEMU VFS userspace test exercises invalid flags, invalid descriptors, zero-length calls, create-via-native-write-open, append, readback, stat and unlink through these wrappers. This validates the selected LionOS path only; it is not a POSIX conformance certification.

## 8. Unsupported compatibility claims

This specification does not imply Linux i386 syscall compatibility, POSIX conformance, glibc/musl drop-in compatibility, dynamic ELF linking, Linux device-node/ioctl conventions, Linux signal ABI, or a stable ABI for internal kernel headers.

The current user libc is small and native. Its allocator is a bounded 32 KiB first-fit arena with splitting, reuse and adjacent-block coalescing; it is per-process and not thread-safe.

## 9. Conformance and change procedure

Run make abi-check and make test.

The ABI check:
1. parses both syscall-number headers and rejects duplicate numeric assignments;
2. checks every registered syscall has a dispatcher case;
3. verifies syscall 0 returns the declared ABI version;
4. compiles tests/uapi_layout.c to enforce published structure sizes and offsets.

These checks detect registry drift and layout changes. They do not prove runtime correctness, security, scheduler behavior, or complete user-pointer safety. New or changed syscall behavior still requires focused runtime tests for invalid pointers, boundary lengths, permission failures, blocked paths, and userland regressions.

Before changing the ABI, update canonical headers, dispatcher, wrapper, this document, layout guards, and runtime tests in one reviewed change. Preserve old behavior unless an explicitly versioned transition is approved.
