# Scheduler Starvation and Dispatch Fairness Audit

## Scope and evidence

This is a bounded source-level audit of the production scheduler and its
existing host/QEMU regressions on `feature/graphics-engine-v2` at commit
`438517da6a01bf3d483891ca5ab85cbbb1746ddb`.

Reviewed sources:

- `src/runqueue.c` and `include/runqueue.h`
- `src/scheduler_policy.c`
- `src/process.c` (priority updates, timer accounting, scheduling, and
  migration eligibility)
- `src/kernel.c` (production busy-destination integration fixture)
- `user/scheduler_stress_test.c`
- `user/busy_balance_candidate_test.c`
- `scripts/test.sh`
- `tests/runqueue_concurrency_test.c` and
  `tests/scheduler_policy_test.c`

This document records source behavior and test coverage. It does not claim
that a starvation bug was reproduced, that a finite starvation bound exists,
or that physical hardware was tested.

## Current dispatch policy

### Priority selection

`runqueue_pick_next_locked()` scans priority lanes in ascending numeric
order and selects the head of the first non-empty lane. Enqueue appends to a
lane's tail, so FIFO order is preserved within a priority lane. The selector
does not rotate between non-empty priority lanes and does not age queued
entries.

The scheduler policy assigns quanta of 2, 4, 8, and 12 ticks to priority
levels 0 through 3. When a running task exhausts its quantum,
`scheduler_tick_charge()` resets its used-tick count and demotes it by one
level. This limits uninterrupted execution by a CPU-bound task, but it does
not itself bound the wait of a task in a lower-priority lane while higher
priority work continues to arrive.

The inspected priority update path explicitly repositions a READY task and
sets both its current and base priority. No automatic READY-age promotion or
periodic priority-aging path was found in the reviewed scheduler sources.

**Consequence:** FIFO gives an ordering guarantee only among tasks in the
same priority lane. Strict priority selection provides no source-level
finite dispatch bound for a lower-priority lane under an indefinitely
replenished higher-priority lane. This is a policy limitation to test and
resolve, not evidence that current default workloads necessarily starve.

### Process-lock contention

The production interrupt scheduler uses a non-blocking try-acquire for
`process_lock`. If acquisition fails, it defers the scheduling decision
instead of spinning in interrupt context. Timer ticks are accumulated in a
per-CPU pending counter and drained on a later successful scheduler entry,
which prevents those ticks from being silently discarded.

This preserves bounded work in the interrupt handler, but the inspected path
does not establish a maximum number of consecutive failed acquisitions, a
maximum elapsed delay before the next successful acquisition, or a
cross-CPU fairness guarantee for the global lock.

### Busy-queue balancing and migration

After selecting a local task, the scheduler may transfer at most one eligible
READY process from a more-loaded source queue when the source backlog exceeds
the destination backlog by the configured threshold. Queue-pair acquisition
is non-blocking and uses ascending CPU order. A failed try-lock defers that
transfer.

The current eligibility rules deliberately exclude kernel threads, user
threads, live thread groups, and processes that have already migrated. This
is a conservative migration boundary. It does not establish a bound for
ineligible tasks or prove that every eligible task will be dispatched within
a fixed interval.

## What the existing tests establish

- Host run-queue concurrency tests exercise concurrent queue operations,
  queue-pair transfers, membership invariants, and destination service in a
  queue model. They do not execute the production process lifecycle.
- Scheduler policy tests cover quantum transitions, tick accumulation,
  counter concurrency, and wraparound behavior.
- The QEMU production integration fixture keeps AP workers runnable while it
  publishes fresh candidate waves. It requires at least two successful
  migrations per wave, destination spread, ownership/execution-guard
  invariants, candidate completion, and worker reaping.
- The separate user scheduler stress test checks timer preemption of two
  CPU-bound peers and repeated timer-driven futex timeouts.

These tests provide useful production-path and host-model coverage. They do
not measure per-process READY wait duration, maximum dispatch latency,
continuous high-priority arrival pressure, or the maximum consecutive
`process_lock` deferrals. Five finite candidate waves are not a proof of
arbitrary sustained-load convergence.

## Required next implementation and acceptance work

1. Define the fairness contract separately for priority scheduling,
   process-lock acquisition, and cross-CPU migration. State whether the
   bound is measured in local timer ticks, successful scheduler decisions,
   or both; do not mix these units.
2. Add low-overhead diagnostics for each continuously READY task's enqueue
   age and eventual dispatch, plus per-CPU scheduler-lock deferral counts.
   Preserve age across queue migration. Use wrap-safe bounded deltas and
   avoid non-lock-free 64-bit atomics in the 32-bit freestanding kernel.
3. Add a production QEMU workload with a continuously replenished higher
   priority lane and a lower-priority sentinel that must still make progress.
   Add a separate contention workload that repeatedly exercises the
   `process_lock` try-lock fallback while checking that blocked tasks and
   timer accounting remain correct.
4. Run the workload across the configured 2/4/8/16-vCPU matrix. Assert a
   documented maximum READY age, successful dispatch of every sentinel,
   exact-once completion/reaping, queue invariants, and no lost pending ticks.
5. Only if measured results require it, design a priority-aging or weighted
   service policy and test its CPU-share and latency consequences before
   changing production selection semantics.

Until those tests pass, keep scheduler convergence and starvation bounds
marked partial. Do not infer a bound from queue-transfer counts or host
pthread tests alone.

## Verification boundary

This was a source-level audit only. No runtime code was changed by the audit,
no local build or regression suite was run for it, and no physical hardware
verification is claimed.
