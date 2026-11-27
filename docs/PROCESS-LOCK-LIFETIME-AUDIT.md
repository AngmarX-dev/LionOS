# Process-Lock Ownership and Lifetime Audit

## Scope and status

This is a source-level audit snapshot of `src/process.c`, `include/process.h`,
`src/runqueue.c`, and `docs/SCHEDULER-RUNQUEUE-DESIGN.md` at commit
`5884f95bb007e60ddf8ad57d10cc51df7d7a3da6`.

It inventories the responsibilities that currently depend on the global
`process_lock` and identifies prerequisites for any future lock-scope
reduction. It is not a lock-splitting implementation, a proof that every
external subsystem has been audited, or evidence of production scheduler
parallelism. The global lock must remain in place until the prerequisites and
acceptance tests below are completed.

## Current synchronization contract

- `process_lock` protects process-table slot allocation and reuse, PID/TID
  identity, process and thread-group state, parent/child and session metadata,
  wait registrations, process ownership fields, and the consistency relation
  between process state and run-queue membership.
- The established lock order is `process_lock` before one run-queue lock.
  Operations that hold two run-queue locks acquire them in ascending CPU-index
  order and release them in reverse order. A run-queue-lock holder must not
  acquire `process_lock`.
- The scheduler interrupt path disables local interrupts and uses
  `spinlock_try_acquire(&process_lock)`; it does not spin waiting for the
  global lock. Timer ticks are accumulated separately and charged after a
  later successful scheduling decision.
- Process state and queue membership are currently committed as one serialized
  transaction. The per-CPU queues therefore do not, by themselves, make
  process lifecycle or scheduler decisions parallel.

## Audited responsibility groups

### 1. Slot identity, process creation, and publication

Relevant paths include `process_create_ex_vas_internal()`,
`process_create_ex_vas_locked()`, `process_create_kernel_thread_on_cpu()`,
and `process_create_user_thread()`.

The lock currently protects free-slot selection, PID/TID assignment, initial
process metadata, and READY publication. Creation is not a single short
critical section: kernel stacks and thread pages are allocated while the lock
is held; user-thread creation also maps and zeroes a page before publishing
the task. The user-process creation path deliberately releases the lock around
`vfs_stdio_init_process()`, then reacquires it to publish READY or roll back.

**Lifetime requirement:** a pointer to a process-table slot cannot safely be
used after dropping the lock unless the caller holds a stable reference or a
generation-checked handle. A slot may otherwise be reaped and reused for a
different PID.

### 2. Fork and exec address-space ownership

Relevant paths include `process_fork_current()`,
`process_exec_replace_current()`, and `process_resolve_cow()`.

Fork currently holds `process_lock` while creating an address space, looking
up and mapping user pages, retaining page references, changing parent PTE
permissions for COW, allocating/initializing a child slot, and rolling back
partial work on failure. It then drops the lock around VFS and pipe cloning;
the child remains unpublished as runnable until those operations succeed.
Exec validates and retires sibling threads, changes the current address space,
updates the kernel stack, and releases old pages/address-space resources while
the process lock is held. COW resolution also reads and updates process-owned
page metadata under the same lock.

**Lifetime requirement:** moving page-table or physical-page work outside the
lock requires explicit address-space references, serialization of PTE changes,
and a rollback protocol that cannot race exit, exec, fork, or reaping.

### 3. Exit, zombie state, waitpid, join, and reaping

Relevant paths include `process_exit_current()`, `process_waitpid_options()`,
`process_join_thread()`, `process_reap_pid()`, `process_kernel_thread_reaped()`,
`reap_process()`, and the scheduler's deferred-reap scan.

The lock protects terminal-state publication, parent/child relationships,
join registrations, reap eligibility, and the check that a zombie is no longer
current on any CPU. Reaping releases user pages, thread stacks, kernel stacks,
and address spaces, and clears the reusable process slot. Those operations
are currently performed while the caller holds `process_lock`. The scheduler
also defers reclamation of a zombie until no `current_by_cpu[]` entry points
to it; deferred kernel-stack release is handled after the outgoing context is
no longer current.

Process exit deliberately releases the lock around pipe/VFS descriptor
teardown, after retiring sibling threads, and reacquires it before publishing
the zombie state. This boundary is part of the lifetime protocol, not merely
a performance optimization.

**Lifetime requirement:** a process must not be freed or have its slot reused
while any CPU, wait path, VFS/pipe operation, syscall, or deferred cleanup still
holds a pointer to it or to resources owned by it. A future design needs an
explicit exiting state/ownership protocol, references or epochs, and a
quiescence condition before reclamation.

### 4. Scheduling, CPU ownership, and migration

Relevant paths include `process_schedule()`,
`process_transition_state_locked()`, `process_try_steal_locked()`,
`process_runqueues_validate_all_locked()`, `process_set_current()`, and
the scheduler execution-guard probes.

The global lock serializes RUNNING/READY/WAITING/STOPPED/ZOMBIE transitions,
current-task ownership, queue insertion/removal, migration eligibility,
migration counters, and scheduler-side reaping. The scheduler may hold it while
saving/restoring FPU state, selecting a task, validating queue metadata,
switching address spaces, and updating the TSS stack. The execution guard
checks that a process context is not dispatched on two CPUs, but it is test
instrumentation and does not replace lifecycle synchronization.

**Lifetime requirement:** a queue-local lock alone cannot authorize dispatch.
The transition must atomically establish unique CPU ownership, detach the
task from every queue, preserve its saved frame and address-space lifetime,
and prevent another CPU from reaping or reusing its slot.

### 5. Wait channels, futexes, signals, and process groups

Relevant paths include `process_sleep_on_kind()`, `process_wakeup()`,
`process_futex_wakeup()`, `process_futex_tick()`, signal delivery/return,
and process-group/session operations.

Wait registration and process-state transitions are serialized by
`process_lock`. `process_sleep_on_kind()` enters with a caller-owned
`held_lock` and acquires `process_lock` before publishing WAITING, then
releases both locks. This is a distinct nested-lock handoff and must be
included in any lock-rank audit; it must not be silently reordered. Wakeup
paths publish READY under `process_lock` and send remote reschedule IPIs only
after releasing it. The futex timeout clock advances independently, while
the process-table scan uses a non-blocking process-lock attempt.

**Lifetime requirement:** wait-object lifetime, lost-wakeup prevention,
timeout ownership, signal interruption, and process-slot references must be
specified together. Any new wait-queue lock must have a documented global
rank and a proven wake/sleep handoff.

## Lock-scope hazards observed in this snapshot

1. Several critical sections include page allocation/free, page-table mapping,
   address-space destruction, or full-page zeroing. These can make IRQ-disabled
   lock hold times substantially longer than a state-only critical section.
2. Process reaping combines logical slot retirement with physical resource
   destruction. Splitting those steps without a stable REAPING state and
   resource ownership transfer risks double-free, use-after-free, or premature
   slot reuse.
3. Fork, exec, and exit cross process, paging, VFS, and pipe ownership
   boundaries. Their existing unlock/relock points need explicit references
   and failure rollback before further lock reduction.
4. The wait path nests a caller-provided lock before `process_lock`; every
   caller and corresponding wake path must be included in lock-order analysis.
5. `current_by_cpu[]`, saved frames, FPU state, address spaces, and kernel
   stacks have CPU-local lifetime constraints. Atomic pointer loads/stores
   alone do not guarantee that the pointed-to object remains alive.
6. The scheduler's non-blocking fallback preserves interrupt progress under
   contention, but does not provide a starvation bound or prove sustained
   production convergence.

## Required prerequisites before reducing the global lock

1. Build a complete call-site/lock-rank graph, including VFS, pipe, paging,
   page allocator, wait-object, signal, and scheduler callbacks.
2. Define stable process handles (slot plus generation) or reference-counted
   process objects; specify which APIs may return borrowed pointers and for
   how long those pointers remain valid.
3. Separate process-table identity/lifecycle protection from address-space,
   thread-group, file-descriptor, wait-object, and per-CPU run-queue ownership.
   Document one total lock order and reject cycles.
4. Introduce an explicit exit/reap state machine and transfer resource ownership
   to a reaper only after all CPUs and external users have released references.
5. Move allocation, page clearing, page-table teardown, and VFS/pipe work out
   of IRQ-disabled critical sections only after reservation/commit/rollback
   semantics are defined and tested.
6. Add debug lock-rank assertions and tests for slot reuse, concurrent
   exit/waitpid/join, fork/exec/exit races, wake-vs-timeout, and deferred stack
   reclamation.
7. Add production-kernel sustained-load tests that measure queue depth,
   dispatch progress, lock-contention duration, and starvation bounds across
   multiple vCPU counts. Host pthread queue tests remain complementary only.

## Acceptance boundary

Until every prerequisite above has implementation and regression evidence,
`process_lock` remains the authoritative process-lifetime lock. Do not split
or remove it based solely on per-CPU run-queue tests, queue ownership checks,
or successful QEMU boot tests. The current host-side destination-owned
dispatch regression validates a queue model; it does not exercise this
production lifecycle path.
