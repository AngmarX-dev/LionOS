# Scheduler Instrumentation Source Map

## Scope and status

This is a source-level integration map for the next Phase A scheduler-fairness
change. It records the existing production paths and their synchronization
contracts; it does not claim that READY-age or lock-deferral instrumentation
has been implemented or that starvation freedom has been established.

The audit is based on the current `feature/graphics-engine-v2` source at
commit `6d0360eb41fd38afdb02288863a0fbe1cfe5991b`.

## Production path map

| Concern | Production location | Existing synchronization and behavior |
|---|---|---|
| General READY transition | `process_transition_state_locked()` in `src/process.c` | Caller holds `process_lock`. READY insertion acquires the selected per-CPU run-queue lock after the process lock. The process state is published only after enqueue succeeds. |
| Scheduler requeue of the outgoing task | `process_schedule()` in `src/process.c` | After a successful non-blocking acquisition of `process_lock`, the current RUNNING non-idle task is appended directly to its local run queue. This is the second production enqueue path and does not call the general transition helper. |
| READY dispatch | `process_schedule()` in `src/process.c` | Local selection uses `runqueue_pick_next_locked()`; idle-destination stealing can also select the just-moved candidate inside `process_try_steal_locked()`. Both return to the same scheduler decision path, which installs the selected process as RUNNING while `process_lock` remains held. |
| READY migration | `process_try_steal_locked()` in `src/process.c` and `runqueue_move_locked()` in `src/runqueue.c` | The caller holds `process_lock` and both queue locks in ascending CPU order. The move updates intrusive queue links and queue ownership; it does not rewrite the process record. READY-age metadata stored in the process record can therefore remain unchanged across a successful move. |
| Scheduler lock deferral | The `spinlock_try_acquire(&process_lock)` failure branch in `process_schedule()` | This is the production interrupt-context scheduling deferral point. It returns without spinning, preserves accumulated timer ticks in the per-CPU pending counter, and may send a retry IPI for an explicit reschedule. Count this failure once per CPU here, not at unrelated probe try-lock sites. |
| Process-slot lifecycle | `clear_process()` and `process_init()` in `src/process.c` | `clear_process()` resets the process record and intrusive run-queue node. New READY-age fields must be reset here. Global epoch and per-CPU deferral statistics must be initialized in `process_init()`. |

## Instrumentation ownership rules

1. Keep the global 32-bit decision epoch under `process_lock`. Do not use
   independent per-CPU epochs for READY age.
2. Stamp a process only after a READY enqueue succeeds. The central transition
   helper and the direct scheduler requeue path must both be covered.
3. Preserve the stamp during `runqueue_move_locked()`; a queue transfer is not
   a new READY transition and is not a dispatch.
4. Measure age at the common production selection/dispatch point, including
   candidates selected by idle-destination stealing. Use
   `scheduler_epoch_delta()` and treat an invalid half-range delta as an
   instrumentation fault rather than silently accepting it.
5. Advance the global decision epoch exactly once for each completed
   production dispatch decision, with the ordering documented alongside the
   measurement. A failed queue operation, a lock deferral, and a mere migration
   must not advance it.
6. Count failed scheduler try-lock acquisitions in the per-CPU scheduler state.
   Keep consecutive-deferral and timer-delay measurements separate from READY
   age. Do not infer elapsed time from a deferral count.
7. Use naturally aligned 32-bit fields and the existing process-lock ownership
   for process metadata. Do not introduce 64-bit atomics into the 32-bit kernel.
8. Keep all statistics reset and process-slot reuse behavior explicit; a reused
   process-table slot must not inherit a previous process's READY age.

## Required implementation and verification follow-up

- Add the epoch and per-process READY metadata, then cover both enqueue paths.
- Add per-CPU scheduler lock-deferral accounting at the single production
  failure branch.
- Add deterministic host tests for epoch initialization, wraparound,
  half-range rejection, metadata reset, and preservation across migration.
- Add production QEMU reporting/assertions at the actual dispatch point and
  under controlled process-lock contention.
- Re-run the complete regression matrix and inspect both the correctness and
  integration surfaces before changing any scheduling policy.

The strict-priority FIFO policy remains unchanged. In particular, this source
map does not establish a finite service bound for lower-priority work under
continuous replenishment of higher-priority lanes.
