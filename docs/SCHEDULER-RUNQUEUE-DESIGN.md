# Per-CPU Scheduler Run-Queue Design

## Status and scope

This document defines the synchronization and ownership contract for replacing the current process-table scan with explicit per-CPU run queues. It is a design milestone, not a claim that queue-based scheduling, CPU migration, or AP task execution is already implemented.

The implementation must preserve the existing scheduler behavior until the queue implementation and its regression tests are ready. Cross-CPU work stealing remains disabled.

The first implementation slice now provides a standalone intrusive run-queue core in
`include/runqueue.h` and `src/runqueue.c`, with host-side invariant and ordering tests
in `tests/runqueue_test.c`. These primitives are not yet connected to the process
table or scheduler. The scheduler still uses the legacy process-table scan, and all
tasks remain pinned to their current CPU. The module's locked API requires callers
to hold the queue lock; scheduler integration must also preserve the documented
`process_lock`-before-run-queue-lock order.


## Current implementation

The current scheduler has these relevant properties:

- The process table is a fixed array protected by the global `process_lock`.
- `process_schedule()` disables local interrupts and uses `spinlock_try_acquire()` on `process_lock`. If another CPU owns the lock, the timer scheduling attempt is dropped rather than spinning in interrupt context.
- Each CPU has a small scheduler state record (scan cursor, schedule count, steal count, and timer ticks).
- Runnable selection scans the process table and filters by `PROCESS_READY`, priority, and `cpu_owner`.
- New processes and threads are initially assigned to the CPU that creates them. The scheduler currently keeps them pinned to that CPU.
- APs complete CPU-local initialization and receive a dedicated kernel idle context during `process_init()`. Their Local APIC timer enters `process_schedule()`, which switches an AP with no current task into that idle context. APs still do not receive ordinary tasks through migration or work stealing.
- Process state transitions, wakeups, creation, exit, reaping, and priority changes are currently serialized by `process_lock`.

Consequently, `cpu_owner` is currently an affinity/ownership field, not a queue membership structure. The existing per-CPU scan cursor is not a run queue.

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
- Do not let APs schedule tasks until each AP has a valid idle context, per-CPU scheduler state, TSS stack handling, and tested FPU/SSE context switching.
- Do not remove `process_lock` in the first queue conversion. Process-table lifetime and complex process-group/parent-child operations still require a stable serialization mechanism.
- Do not change xHCI, interrupt routing, or unrelated driver code.

## Data model

Each CPU will own one `run_queue` containing four FIFO priority lanes, one lane per `PROCESS_PRIORITY_LEVELS` value. Each lane has head and tail process indices and a runnable count. Queue links should use process-table indices (or a clearly typed invalid-index sentinel), not raw pointers, so that slot reuse cannot leave dangling heap references.

The per-CPU scheduler state will contain:

- A run-queue spinlock.
- Four lane head/tail pairs and per-lane counts.
- A total runnable count.
- A monotonically increasing enqueue sequence or equivalent deterministic FIFO bookkeeping if intrusive links alone cannot preserve FIFO order.
- Existing timer and scheduler statistics.

Each process will have explicit run-queue membership metadata: queued/not-queued state, owning queue CPU, and next/previous indices (or the chosen equivalent). The metadata must be reset by process-slot initialization and reaping.

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

- `runqueue_enqueue_locked(process, cpu)`: requires `process_lock`; validates READY state, valid online CPU, no existing membership, and priority range; inserts once and updates counts/owner metadata.
- `runqueue_dequeue_locked(process)`: requires `process_lock`; validates membership and queue owner; unlinks once and updates counts/metadata.
- `runqueue_requeue_priority_locked(process, new_priority)`: handles READY processes by unlinking and reinserting under the same queue lock; non-READY processes only update their priority metadata.
- `runqueue_pick_next_locked(cpu)`: called by that CPU's scheduler with the required locks held; removes the head of the highest-priority non-empty lane and returns the process.
- `process_make_ready_locked(process, preferred_cpu)`: single state-transition entry point for creation completion, wakeups, waitpid events, futex wakeups, signal continuation, and preemption.

The helper names are design identifiers; they are not present APIs until implemented.

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

The AP main loop services early-boot stress work and halts; the Local APIC timer now transfers it into a dedicated scheduler idle context. Before an AP can consume normal runnable work, implement and test:

The dedicated AP idle context and stack, AP timer entry into the common scheduler, per-CPU TSS/address-space switch on idle entry, and FPU/SSE restoration for idle entry are now implemented. CI must still verify the AP idle-entry markers on the configured QEMU SMP topology.

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
6. Concurrent enqueue/dequeue/wakeup stress across all online CPUs without duplicate ownership or lost wakeups.
7. Timer scheduling remains bounded when a lock is contended.
8. Existing process, waitpid, futex, pipe, TTY, FPU/SSE, and xHCI regressions pass.
9. QEMU tests cover one CPU, multiple CPUs, and the maximum supported CPU topology.
10. Physical hardware claims remain explicitly unverified until tested on hardware.

A successful build alone is insufficient. The implementation must pass both code reviews and all required CI workflows for the exact commit before the README item can be marked complete.
