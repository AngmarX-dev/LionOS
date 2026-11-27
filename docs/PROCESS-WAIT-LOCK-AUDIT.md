# Process Wait and Subsystem Lock-Call-Site Audit

## Scope and evidence

This is a bounded source audit of the wait/wakeup paths at parent commit
`ea4e2b27e3615531cb616c72e1493cde86f8247b` on `feature/graphics-engine-v2`. It follows the implementation in
`src/process.c`, `src/futex.c`, `src/pipe.c`, `src/tty.c`,
`src/ipc.c`, and the relevant declarations in `include/process.h`.

This document records observed acquisition order and handoff behavior. It is
not a complete repository-wide lock-rank graph, a proof of absence of reverse
edges in every subsystem, or authorization to reduce synchronization. Line
numbers below refer to the audited source snapshot and may move in later
commits.

## Central wait handoff

`process_sleep_on_kind()` in `src/process.c` performs the following sequence:

1. The caller enters with its subsystem/object lock already held and local
   interrupts disabled (the API contract is also stated in
   `include/process.h`).
2. It acquires `process_lock` while retaining that caller lock.
3. It publishes wait channel/kind/timeout metadata and transitions the current
   process to `PROCESS_WAITING`.
4. If publication fails, it releases `process_lock`, then releases the caller
   lock, and returns an error rather than claiming the task blocked.
5. On success, it requests a blocked reschedule, releases `process_lock`,
   releases the caller lock, and returns `PROCESS_WAIT_BLOCKED`.

The wait lock therefore nests outside `process_lock` on this path. This
ordering closes the check-to-sleep lost-wakeup window and must not be silently
reversed. The operation is failure-aware: it does not return the blocked
result until the WAITING transition has succeeded.

## Observed wait and wake call sites

| Subsystem | Blocking-side order | Wake-side behavior | Audit note |
|---|---|---|---|
| Futex | `buckets[index].lock → process_lock` through `process_futex_sleep_on[_timeout]()` | `futex_wake()` retains the bucket lock while `process_futex_wakeup()` acquires `process_lock`; bucket unlock follows | This is an explicit bucket-to-process edge on both wait and wake. |
| Pipe | `pipe_lock → process_lock` in blocking read/write | Pipe read/write/close/close-all release `pipe_lock` before calling `process_wakeup()` | Wake does not retain the pipe lock in the inspected paths. |
| TTY | `console_tty.input_lock → process_lock` in blocking read | Input paths release `input_lock` before `process_wakeup()` | Timeout and ordinary wait use the same central handoff. |
| IPC | `ipc_lock → process_lock` in blocking receive | Send/receive release `ipc_lock` before `process_wakeup()` | The blocking receive loop rechecks the message queue after a non-blocked return. |
| AP lifecycle probe | Probe-local wait lock → `process_lock` | BSP calls `process_wakeup()` after releasing `process_lock` | Test-only lock path; each AP owns its own probe lock. |

### Futex details

- `futex_wait()` acquires the selected bucket lock, atomically reads the
  user word, and retains the bucket lock until the process is committed to
  WAITING or the value mismatch path releases it.
- `futex_wake()` acquires the same bucket lock and calls
  `process_futex_wakeup()` before releasing it. That callee acquires
  `process_lock`, publishes READY only after queue insertion succeeds, and
  kicks remote owners only after releasing `process_lock`.
- `process_futex_tick()` increments the AP-local timer counter and uses a
  non-blocking `process_lock` attempt. The inspected timeout scan does not
  acquire a futex bucket lock.
- The inspected process/scheduler timeout path has no futex-bucket acquisition
  while holding `process_lock`. This is a bounded observation, not a proof
  that no such reverse edge exists elsewhere in the repository or future code.

### Pipe, TTY, and IPC details

- Pipe read/write call `process_sleep_on()` while holding `pipe_lock`.
  Normal wakeups occur after that lock is released. `pipe_close_all()`
  snapshots and clears the owner's pipe masks under `pipe_lock`, releases it,
  then wakes affected channels.
- TTY blocking reads call `process_sleep_on()` or
  `process_sleep_on_timeout()` while holding `console_tty.input_lock`.
  Input producers and attribute changes release the input lock before waking
  readers.
- Blocking IPC receive calls `process_sleep_on()` while holding `ipc_lock`.
  IPC send and successful receive release the lock before waking the receiver
  channel.

## Scheduler and timeout interactions

- The documented process/run-queue order remains
  `process_lock → one run-queue lock`; two run-queue locks are acquired in
  ascending CPU order.
- `process_wakeup()` and `process_futex_wakeup()` publish READY and queue
  membership while holding `process_lock`. Remote reschedule IPIs are sent
  after releasing it.
- `process_futex_tick()` changes timeout/wait state under `process_lock`
  and does not need to acquire the originating subsystem lock.
- These paths do not justify moving process-state publication outside
  `process_lock`: the wait metadata, process state, and run-queue membership
  are still one serialized transaction.

## Additional lock domains still requiring a full graph

The current audit does not yet establish a complete total order for:

- `vfs_lock` and the backend locks reached by diskfs, ext2, and ramfs;
- `pipe_lock` relative to process descriptor teardown and process-slot reuse;
- `memory_lock`, page allocation/free, page-table mutation, and TLB shootdown;
- paging map locks and high-memory mapping locks;
- signal delivery, process-group/session operations, and scheduler callbacks;
- every caller of APIs that accept a borrowed `struct process *`.

The lifetime audit already identifies fork, exec, exit, reaping, and VFS/pipe
teardown as boundaries where process-lock release is intentional. Those
boundaries still need stable references or a rigorously proven exclusion
condition before any caller can safely retain a process pointer across an
unlock. The process-table slot is reusable storage, not a stable object handle.

## Required next steps

1. Enumerate every lock acquisition in the above subsystem files and every
   cross-subsystem call made while a lock is held; record a directed lock graph.
2. Inspect all `process_sleep_on*` callers and their matching wake paths when
   adding or changing a wait channel. Preserve the check-to-sleep atomicity.
3. Verify that no VFS, pipe, paging, allocator, signal, or scheduler callback
   introduces a reverse edge into an already-held lock domain.
4. Define stable process references/generation handles and an exit/reap
   ownership state machine before shortening process lifetime critical sections.
5. Add debug lock-rank assertions and targeted concurrent wait/wake, timeout,
   close-vs-exit, fork/exec/exit, and deferred-reclamation regressions.
6. Keep the global `process_lock` until the complete graph, lifetime protocol,
   and production-kernel acceptance tests are implemented and reviewed.

## Conclusion

The audited wait paths consistently use the caller-held subsystem lock before
`process_lock`. Futex wake deliberately keeps its bucket lock across the
process-state wake operation; pipe, TTY, and IPC wake paths release their
subsystem locks first. The timeout scan takes only `process_lock`. These are
important lock-order facts, but they do not complete the broader VFS/paging/
allocator/lifetime audit. No runtime synchronization change is made by this
document.
