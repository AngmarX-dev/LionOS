# Per-CPU Scheduler Run-Queue Design

## Status and scope

This document records the synchronization and ownership contract for the per-CPU run-queue scheduler. The first scheduler-integration slice is implemented: process lifecycle transitions maintain queue membership and `process_schedule()` selects from local priority lanes. The milestone remains partial until QEMU SMP lifecycle stress and AP task-execution verification pass.

Cross-CPU migration and work stealing remain disabled.

The implementation now connects the intrusive run-queue core in
`include/runqueue.h` and `src/runqueue.c` to process lifecycle transitions and
`process_schedule()`. Each CPU has four FIFO priority lanes. READY transitions
enqueue exactly once; transitions away from READY dequeue before publishing the
new state; scheduler selection removes the highest-priority head. The global
`process_lock` remains in place and is always acquired before a queue lock.
The timer scheduler uses a non-blocking try-acquire for its local queue selection.
Work stealing and task migration remain disabled.

Host tests cover queue invariants, FIFO/priority ordering, and concurrent lock
contention across two independent per-CPU queues. Runtime validation checks queue
membership against process states during scheduler decisions. QEMU SMP lifecycle
stress and AP task-execution verification are still required before this milestone
can be considered complete.


## Current implementation

The current scheduler has these relevant properties:

- The process table is a fixed array protected by the global `process_lock`.
- `process_schedule()` disables local interrupts and uses `spinlock_try_acquire()` on `process_lock`. If another CPU owns the lock, the timer scheduling attempt is dropped rather than spinning in interrupt context.
- Each CPU has a small scheduler state record (scan cursor, schedule count, steal count, and timer ticks).
- Runnable selection dequeues from the current CPU's highest-priority non-empty FIFO run-queue lane; process-table scans are retained only for lifecycle and relationship operations.
- New processes and threads are initially assigned to the CPU that creates them. The scheduler keeps them pinned to that CPU; no work stealing or migration is enabled.
- APs complete CPU-local initialization and receive a dedicated kernel idle context during `process_init()`. Their Local APIC timer enters `process_schedule()`, which switches an AP with no current task into that idle context. APs still do not receive ordinary tasks through migration or work stealing.
- Process state transitions, wakeups, creation, exit, reaping, and priority changes are currently serialized by `process_lock`.

Consequently, `cpu_owner` is both the affinity owner and the CPU whose queue contains a READY task. The per-CPU scan cursor remains only as diagnostic/compatibility state; it no longer selects runnable tasks.

## Goals

1. Make runnable-task ownership explicit and auditable.
2. Keep the common scheduling decision CPU-local.
3. Avoid unbounded spinning in timer/interrupt context.
4. Make enqueue, dequeue, wakeup, and priority-change operations have a single documented synchronization contract.
5. Prevent a task from being runnable on two CPUs or running concurrently on two CPUs.
6. Prepare for migration without enabling migration before AP scheduling, idle contexts, and task retirement are safe.

## Non-goals for this milestone

- Do not enable work stealing.
- Do not migrate a running task.
- Do not enable cross-CPU migration or work stealing. An AP may select only tasks already assigned to its own local queue; this local AP task-execution path still requires QEMU SMP verification.
- Do not remove `process_lock` in the first queue conversion. Process-table lifetime and complex process-group/parent-child operations still require a stable serialization mechanism.
- Do not change xHCI, interrupt routing, or unrelated driver code.

## Data model

Each CPU owns one `struct runqueue` in `runqueues[LIONOS_MAX_CPUS]`, containing four FIFO priority lanes. Each lane stores head/tail process-table indices and a runnable count. Intrusive links use a typed invalid-index sentinel rather than raw pointers, so process-slot reuse cannot leave dangling references.

The run-queue object contains its spinlock, CPU owner, four lane head/tail pairs, per-lane counts, and total runnable count. Timer and scheduler statistics remain in the separate per-CPU scheduler state.

Membership metadata is kept in the parallel `runqueue_nodes[LIONOS_PROCESS_MAX]` array, indexed by process-table slot. Each node stores queued state, owning CPU, priority, and next/previous indices. `clear_process()` resets the node when a process slot is reclaimed; scheduler idle contexts are outside the process table and are never queued.

The process state and queue membership must agree:

| Process state | Queue membership | CPU ownership |
|---|---|---|
| UNUSED | Not queued | None |
| READY | Exactly one queue entry | That queue's CPU |
| RUNNING | Not queued | CPU currently executing it |
| WAITING / STOPPED / ZOMBIE | Not queued | Last owner may be retained for affinity/accounting, but it is not runnable |

The bootstrap process is a special CPU-0 idle/bootstrap context and must never appear in a normal runnable lane.

## Ownership invariants

- A READY process is present in exactly one run queue.
- A process is removed from its queue before changing from READY to RUNNING, WAITING, STOPPED, or ZOMBIE.
- A RUNNING process is owned by exactly one CPU and is absent from every run queue.
- A RUNNING process that remains runnable after preemption is enqueued exactly once on its owner CPU.
- A wakeup transitions WAITING to READY and enqueues the process atomically with respect to other state transitions.
- A priority change of a READY process removes it from its old lane and inserts it into the new lane while preserving exactly-one membership.
- Slot cleanup clears queue metadata only after the process is no longer current on any CPU and no queue can reference it.
- No code may write `cpu_owner` to transfer ownership while a task is RUNNING.

## Locking contract

### Phase 1: safe conversion while retaining process-table serialization

Use the following global lock order:

1. `process_lock`
2. One run-queue lock

Code holding a run-queue lock must never acquire `process_lock`. Code that needs to inspect or mutate process lifetime/state and queue membership acquires `process_lock` first, then the owning run-queue lock.

When an operation touches one queue, it holds only that queue lock. If an operation ever needs two queues (reserved for a future migration implementation), it must acquire them in ascending CPU-index order and release them in reverse order. It must never hold two queue locks while calling code that can acquire `process_lock`.

The scheduler's interrupt path must not spin indefinitely. It should disable local interrupts, try-acquire `process_lock`, then try-acquire its local run-queue lock. If either try-acquire fails, it restores the saved interrupt state and returns the current frame without partially changing process state. Every successful acquisition must have a single cleanup path that releases locks in reverse order and restores the original flags.

This phase deliberately retains the global lock. It introduces queue correctness and ownership first; it does not yet claim parallel scheduler scalability.

### Phase 2: reduce global-lock scope

Only after queue invariants and stress tests pass may independent operations move to queue-local synchronization. Process-table slot allocation/reaping, parent-child relationships, process groups/sessions, shared address-space/thread-group operations, and file-descriptor ownership must remain protected by appropriate lifetime and subsystem locks.

A future lock-scope reduction must document object lifetime and lock ordering for each operation before removing `process_lock` from that path.

## Required transition helpers

Queue mutation must be centralized. Direct writes to `PROCESS_READY` outside these helpers are prohibited after conversion.

- `process_transition_state_locked(process, state)`: the process-layer transition helper. It adds READY tasks to their owner queue and removes queued tasks before publishing a non-READY state.
- `runqueue_enqueue_locked(queue, nodes, capacity, index, priority)`: requires the owning queue lock; validates membership, lane integrity, bounds, and priority before insertion.
- `runqueue_dequeue_locked(queue, nodes, capacity, index)`: requires the owning queue lock; validates owner and links before unlinking and resetting node metadata.
- `runqueue_requeue_priority_locked(queue, nodes, capacity, index, priority)`: moves a queued node between priority lanes while preserving queue ownership.
- `runqueue_pick_next_locked(queue, nodes, capacity, index)`: removes the head of the highest-priority non-empty lane.

The process layer always acquires `process_lock` before a queue lock. Future migration helpers remain out of scope.

## CPU selection policy

For the first implementation, the preferred CPU is the process's current owner when that CPU is online; otherwise use the creating/current CPU if online, then CPU 0 as a final valid fallback. The policy must never select an offline or out-of-range CPU.

Creation and wakeup must not silently migrate a RUNNING task. A WAITING task may be re-enqueued on its previous owner because it is no longer executing. Any later load-balancing policy must be a separate change with explicit migration tests.

## Scheduler algorithm

On a scheduling event for CPU N:

1. Save the interrupted frame to the current task.
2. Preserve existing wait-timeout, signal, deferred-stack, and FPU/SSE handling.
3. If the outgoing task is still RUNNING and must yield/preempt, transition it to READY and enqueue it once on CPU N.
4. Select and dequeue the head task from the highest-priority non-empty lane for CPU N.
5. Mark the selected task RUNNING and owned by CPU N.
6. Switch address space and kernel stack, restore FPU/SSE state, and return the selected saved frame.
7. If no runnable task exists, return to a CPU-local idle context. The bootstrap context on CPU 0 must remain a special case until a real idle-thread model is implemented.

The scheduler must not perform debug console I/O while holding scheduler locks.

## AP scheduling prerequisite

The AP main loop services early-boot stress work and halts; the Local APIC timer transfers it into a dedicated scheduler idle context. Per-CPU run queues are now integrated into the common scheduler, so an AP can select work only from its own queue. The dedicated AP idle context and stack, AP timer entry, per-CPU TSS/address-space switch, and FPU/SSE restoration for idle entry are implemented. CI must still verify AP idle-entry markers and actual local-queue task execution on the configured QEMU SMP topology.

The remaining prerequisites are:

- FPU/SSE save/restore under real AP task switches.
- Timer tick accounting and reschedule signaling under AP task load.
- Safe task exit/reap on APs, including deferred reclamation only after a task stack is no longer current on any CPU.
- SMP tests proving two CPUs never execute the same task simultaneously.
- Explicit queue membership and synchronized enqueue/dequeue before any task migration.

Until these prerequisites pass, normal tasks remain pinned to their current owner and work stealing remains disabled.

## Validation and acceptance criteria

The queue conversion is not complete until tests demonstrate:

1. FIFO ordering among tasks at equal priority.
2. Priority-lane selection and priority changes for READY and RUNNING tasks.
3. Exactly-one queue membership through create, fork, thread creation, wakeup, timeout, stop/continue, exit, and reap paths.
4. No queue entry for WAITING, STOPPED, ZOMBIE, UNUSED, or RUNNING tasks.
5. Queue counts and head/tail/link consistency after every stress iteration.
6. Host-side concurrent enqueue/dequeue stress across two per-CPU queues; QEMU SMP lifecycle stress across all online CPUs is still required.
7. Timer scheduling remains bounded when a lock is contended.
8. Existing process, waitpid, futex, pipe, TTY, FPU/SSE, and xHCI regressions pass.
9. QEMU tests cover one CPU, multiple CPUs, and the maximum supported CPU topology.
10. Physical hardware claims remain explicitly unverified until tested on hardware.

A successful build alone is insufficient. The implementation must pass both code reviews and all required CI workflows for the exact commit before the README item can be marked complete.
