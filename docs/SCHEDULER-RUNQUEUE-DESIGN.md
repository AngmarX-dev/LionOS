# Per-CPU Scheduler Run-Queue Design

## Status and scope

This document records the synchronization and ownership contract for the per-CPU run-queue scheduler. Process lifecycle transitions maintain queue membership and `process_schedule()` selects from local priority lanes. The single-CPU fallback and 2-, 4-, 8-, and 16-vCPU SMP regressions, including topology-aware multi-process work stealing and zombie-reap retries, passed Build, Phase 26, and xHCI CI on exact commit `b8829f4a7ea2d919e24b4d9d5f6c6bdb1f646452`. A follow-up test repeats the bounded migration batch for three rounds to exercise repeated scheduling, process exit, and reclamation cycles without allowing any individual process to migrate more than once. Build, Phase 26, and xHCI workflows passed on exact commit `831e6580b964041df1070fb475178a1043a30399`. The milestone remains partial pending broader load-balancing stress and physical hardware verification.

A first controlled work-stealing slice is enabled only for explicitly opted-in READY user processes. Each opted-in process may migrate at most once; ordinary processes remain pinned by default, as do kernel threads, user threads, and leaders with unreaped sibling threads. Broader migration remains partial.

The implementation now connects the intrusive run-queue core in
`include/runqueue.h` and `src/runqueue.c` to process lifecycle transitions and
`process_schedule()`. Each CPU has four FIFO priority lanes. READY transitions
enqueue exactly once; transitions away from READY dequeue before publishing the
new state; scheduler selection removes the highest-priority head. The global
`process_lock` remains in place and is always acquired before a queue lock.
The timer scheduler uses a non-blocking try-acquire for its local queue selection.
The scheduler may steal one explicitly opted-in READY user process when its local queue is empty; ordinary processes are not migratable by default, and the initial policy permits at most one migration per process.

Host tests cover queue invariants, FIFO/priority ordering, destination-lane FIFO
append semantics across multiple READY-task priority changes, and concurrent lock
contention across two independent per-CPU queues. Runtime validation checks queue
membership against process states during scheduler decisions. A boot-time AP
run-queue probe places one short-lived kernel task on every online AP queue,
blocks each task, and has the BSP wake it through the normal wait-channel path.
Each AP uses its own probe wait lock so a simultaneous wake does not create an
unfair shared test-lock convoy with interrupts disabled. The resumed task then performs a bounded wait on its own AP-local timer and checks that the timeout result is delivered exactly once. Each AP probe next stops itself through the normal process-state transition; the BSP waits until every AP is running its idle context, continues the probes, changes each AP probe's READY-task priority through the shared process-layer helper, validates the resulting owner-queue lane metadata, and sends owner-local reschedule IPIs. Each probe verifies that it resumed on its original AP before exiting through deferred reclamation. Before freeing the probe's kernel stack, the reaper verifies that the task is no longer current on any CPU, its run-queue node is fully detached, and the stack top is page-aligned. After slot cleanup it verifies UNUSED state and reset queue links before counting the probe as reaped. The QEMU stability suite requires AP lifecycle, cross-CPU wakeup, AP-local timeout, AP-local stop/continue, AP-owned READY-task priority-requeue, and AP reclamation-validation markers for multi-CPU boots. A dedicated, size-bounded userspace futex regression also verifies that two blocked waiters are released one at a time when `maximum=1` is passed to futex wake. Futex wake follows the same queue-publication contract: a waiter is counted only after its READY transition succeeds, wait metadata is retained on enqueue failure, and affected CPU owners receive reschedule IPIs after process_lock is released.
Fork and process-group stop/continue userland regressions execute during the same SMP boot suite. An additional AP-owned kernel worker launches the full `process_test.elf` lifecycle suite from CPU 1; the process is initially queued on CPU 1 and may be stolen once by another idle AP before or during its lifecycle test; forked children inherit the executing CPU as their initial owner. The BSP observes the top-level exit status and reaps the AP-launched test process. The QEMU acceptance test is configured to launch the same lifecycle program from CPU 0, explicitly opt that process into migration, kick an idle AP, and require it to steal that READY process; it checks the migration counter, final AP owner, and a nonzero per-CPU steal counter before reaping. The multi-process migration regression now opts in at most four independent READY user processes, capped at the number of online APs, plus one pinned control process; this avoids requiring a task to migrate when there is no AP available to steal it. Zombie reaping checks every CPU's current-context pointer before freeing a process stack or address space; an explicit migration-test reap reports a busy result while the zombie is still current, and the test retries after the scheduler switches away. Regular process exit completes descriptor and session cleanup with interrupts disabled before publishing the blocked-reschedule request and sending its owner-local IPI, preventing a zombie frame from being resumed or reclaimed prematurely. The restricted migration path and expanded QEMU topology matrix are validated only when all required workflows pass on the exact current commit; they do not establish general load balancing or unrestricted migration. Kernel threads, user threads, and leaders with unreaped sibling threads remain pinned, and each process is limited to one migration. Broader SMP coverage and physical hardware verification remain outstanding.


## Current implementation

The current scheduler has these relevant properties:

- The process table is a fixed array protected by the global `process_lock`.
- `process_schedule()` disables local interrupts and uses `spinlock_try_acquire()` on `process_lock`. If another CPU owns the lock, the timer scheduling attempt is dropped rather than spinning in interrupt context.
- Each CPU has a small scheduler state record (scan cursor, schedule count, steal count, and timer ticks). The local timer count advances atomically before the timeout scan tries `process_lock`, so lock contention may defer a scan but cannot discard elapsed local ticks or indefinitely postpone an AP-local timeout.
- Runnable selection dequeues from the current CPU's highest-priority non-empty FIFO run-queue lane; process-table scans are retained only for lifecycle and relationship operations.
- New processes and threads are initially assigned to the creating CPU. An idle CPU may steal an explicitly opted-in READY user process once; ordinary processes remain pinned by default, as do kernel threads, user threads, and process leaders with unreaped sibling threads.
- APs complete CPU-local initialization and receive a dedicated kernel idle context during `process_init()`. Their Local APIC timer enters `process_schedule()`, which switches an AP with no current task into that idle context. APs can receive explicitly opted-in READY user processes through the restricted one-time work-stealing path; ordinary processes and other task types remain pinned by default.
- Process state transitions, wakeups, creation, exit, reaping, and priority changes are currently serialized by `process_lock`.

Consequently, `cpu_owner` is both the affinity owner and the CPU whose queue contains a READY task. The per-CPU scan cursor remains only as diagnostic/compatibility state; it no longer selects runnable tasks.

## Goals

1. Make runnable-task ownership explicit and auditable.
2. Keep the common scheduling decision CPU-local.
3. Avoid unbounded spinning in timer/interrupt context.
4. Make enqueue, dequeue, wakeup, and priority-change operations have a single documented synchronization contract.
5. Prevent a task from being runnable on two CPUs or running concurrently on two CPUs.
6. Keep migration restricted to READY user processes until thread-group, address-space, and retirement synchronization support broader movement.

## Non-goals for this milestone

- Do not migrate a running task.
- Do not migrate kernel threads, user threads, or a process group with unreaped sibling threads in this initial slice.
- Do not migrate the same process more than once.
- Do not enable unrestricted cross-CPU migration. The initial path may transfer only eligible READY user processes once; broader migration requires separate thread-group, address-space, and lifetime verification.
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
- Initial stealing is limited to explicitly opted-in READY, independent user processes with no unreaped sibling threads, no active wait metadata, no deferred stack, and no prior migration. New processes and forked children are ineligible by default.

## Locking contract

### Phase 1: safe conversion while retaining process-table serialization

Use the following global lock order:

1. `process_lock`
2. One run-queue lock

Code holding a run-queue lock must never acquire `process_lock`. Code that needs to inspect or mutate process lifetime/state and queue membership acquires `process_lock` first, then the owning run-queue lock.

When an operation touches one queue, it holds only that queue lock. A two-queue transfer holds `process_lock` first, then acquires both run-queue locks in ascending CPU-index order and releases them in reverse order. It must never hold two queue locks while calling code that can acquire `process_lock`. The low-level `runqueue_move_locked()` primitive assumes both queue locks are already held and performs a validated unlink/append while preserving priority and destination FIFO order.

The scheduler's interrupt path must not spin indefinitely. It disables local interrupts and try-acquires `process_lock`, then try-acquires its local run-queue lock. If `process_lock` is contended, the existing process-lock fallback applies. If the local queue lock is contended, no queue/state mutation is committed and a reschedule is requested for the next tick. The current frame may continue only while its process remains RUNNING; any non-RUNNING current process (including a READY process already queued) must instead enter its CPU-local idle/bootstrap fallback. Every successful acquisition releases locks in reverse order and restores the original interrupt flags.

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
- `runqueue_move_locked(source, destination, nodes, capacity, index)`: transfers one READY node between distinct CPU queues. It requires both queue locks, preserves priority, appends to the destination lane tail, and rejects malformed queues or invalid ownership without modifying either queue.

The process layer always acquires `process_lock` before queue locks. The primitive is used by the scheduler's initial work-stealing path. The QEMU acceptance test transfers independent READY user processes created on CPU 0 to AP queues and verifies their execution/reaping while a non-opted-in control process remains pinned. The expanded topology-aware scenario (up to four migratable READY user processes, capped by online AP count, plus one pinned process) passed Build, Phase 26, and xHCI CI on exact commit `b8829f4a7ea2d919e24b4d9d5f6c6bdb1f646452`. A three-round repeat of that bounded batch is being added to stress repeated queue transfer and process retirement; each individual process still migrates at most once. The policy excludes current tasks, kernel threads, user threads, and process groups with unreaped sibling threads. This is not general process/thread migration or sustained load balancing; those remain outstanding.

## CPU selection policy

For the first implementation, the preferred CPU is the process's current owner when that CPU is online; otherwise use the creating/current CPU if online, then CPU 0 as a final valid fallback. The policy must never select an offline or out-of-range CPU.

Creation and wakeup must not migrate a RUNNING task. A WAITING task is re-enqueued on its previous owner. The current one-time stealing policy is the only permitted migration path; broader load balancing requires a separate change with explicit migration tests.

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
- The QEMU migration regression now enables a test-only atomic per-process execution-owner guard. A running task owns one CPU token from dispatch until the next scheduling decision; the outgoing token is released only after acquiring `process_lock`, and the selected task claims its token before that lock is released. The final audit compares every token with `current_by_cpu[]`, rejects duplicate CPU ownership, and verifies that each online CPU points either to a RUNNING process owned by that CPU or to its own RUNNING idle context. This catches stale current pointers that still reference WAITING, READY, STOPPED, or ZOMBIE tasks. This directly checks the no-double-dispatch invariant during the migration workload; longer-duration kernel scheduler contention remains outstanding.
- Explicit queue membership and synchronized enqueue/dequeue before any task migration.
- A tested two-queue transfer primitive with fixed CPU-index lock ordering; restricted user-process migration is integrated, while broader process/thread migration remains outstanding.

Until the broader prerequisites pass, only the restricted one-time READY user-process stealing path is enabled for explicitly opted-in processes; ordinary processes, kernel threads, user threads, and process groups with unreaped sibling threads remain pinned by default.

## Validation and acceptance criteria

The queue conversion is not complete until tests demonstrate:

1. FIFO ordering among tasks at equal priority.
2. Priority-lane selection and priority changes for READY and RUNNING tasks.
3. Exactly-one queue membership through create, fork, thread creation, wakeup, timeout, stop/continue, exit, and reap paths.
4. No queue entry for WAITING, STOPPED, ZOMBIE, UNUSED, or RUNNING tasks.
5. Queue counts and head/tail/link consistency after every stress iteration.
6. Host-side concurrent enqueue/dequeue stress uses four independently locked per-CPU queues and eight workers; a second deterministic stress phase performs 10,000 pseudo-random, bidirectional node transfers per worker across all four queues while acquiring queue locks in ascending CPU order, validating both touched queues after every transfer, and checking exactly-one global membership under all queue locks at completion. The boot-time AP probe verifies one task create/enqueue/dispatch/exit/reap lifecycle and queue invariants on every online AP; QEMU coverage exercises cross-CPU wakeup, timeout, stop/continue, and AP-owned priority requeue across 2-, 4-, 8-, and 16-CPU topologies; AP-owned fork/waitpid is checked at all four SMP sizes; the earlier restricted two-process migration test checks atomic execution ownership at all SMP sizes and requires two distinct AP owners at 4, 8, and 16 CPUs; the topology-aware workload tests up to four independently migrated processes, never exceeding the online AP count, plus one pinned control process, and passed exact-commit CI on `b8829f4a7ea2d919e24b4d9d5f6c6bdb1f646452`; the three-round repeat extension now requires exactly three round-success markers in every multi-CPU `run_qemu` test. Longer-duration kernel scheduler contention, broad load-distribution stress, and physical hardware verification remain outstanding.
7. Timer scheduling remains bounded when a lock is contended.
8. Existing process, waitpid, futex, pipe, TTY, FPU/SSE, and xHCI regressions pass.
9. QEMU tests cover one CPU, multiple CPUs, and the maximum supported CPU topology.
10. Physical hardware claims remain explicitly unverified until tested on hardware.

A successful build alone is insufficient. The implementation must pass both code reviews and all required CI workflows for the exact commit before the README item can be marked complete.
