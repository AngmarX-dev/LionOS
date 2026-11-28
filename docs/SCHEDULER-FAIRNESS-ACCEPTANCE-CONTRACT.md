# Scheduler Fairness Acceptance Contract

## Status and scope

This document turns the open scheduler-fairness findings into testable
acceptance requirements. It does not claim that the current scheduler meets
these requirements, and it does not authorize a production policy change by
itself.

The current implementation has four strict-priority FIFO lanes. A lower
numeric priority is selected before every higher-numbered lane, and queue
selection does not age entries. The scheduler also uses a non-blocking
`process_lock` attempt in interrupt context. These are separate fairness
questions and must not be combined into one latency number.

The contract applies to the existing x86 32-bit freestanding kernel and the
configured QEMU 2/4/8/16-vCPU matrix. A one-vCPU run is useful as a control,
but does not replace SMP acceptance. Host pthread tests remain supplemental.

## Terms and measurement units

- **READY age:** number of successful production scheduler decisions globally
  since a process entered READY state, until it is selected for execution.
  Because process-state scheduling is serialized by `process_lock`, the
  measurement uses one global 32-bit decision epoch rather than unrelated
  per-CPU counters. This is a decision-count metric, not elapsed time.
- **Lock deferral:** one failed non-blocking acquisition of `process_lock`
  by a scheduler entry. Count attempts per CPU; do not infer elapsed time
  from the count.
- **Timer delay:** local timer ticks accumulated while a CPU cannot complete
  the process scheduling critical section. This is a separate per-CPU metric.
- **Migration:** transfer of one eligible READY process between run queues.
  A migration must preserve the process's original READY-age measurement.
- **Dispatch:** the production scheduler changes a READY process to RUNNING
  and installs it as the unique current process on exactly one CPU.

The implementation must use a wrap-safe 32-bit counter or epoch for kernel
instrumentation. Comparisons must reject or explicitly handle deltas greater
than `INT32_MAX`; unsigned wraparound must not be mistaken for a small age.
The shared scheduler policy now provides `scheduler_epoch_delta()` for this
half-range validation, with host tests for wraparound, the maximum accepted
delta, ambiguous intervals, and unchanged output on failure. This is only a
measurement primitive: it is not yet wired into production READY transitions
or scheduler dispatch instrumentation. Do not introduce 64-bit atomics unless
the target guarantees they are lock-free and the 32-bit toolchain/runtime
contract is documented.

The current production call-site map and lock/ownership constraints are
recorded in [the scheduler instrumentation source map](SCHEDULER-INSTRUMENTATION-SOURCE-MAP.md).

## Fairness properties to establish

### 1. Priority-lane service

A continuously READY sentinel must be dispatched while the scheduler is
under sustained queue pressure. For sentinel lanes 1 through 3, replenish
higher-priority work continuously without stopping producers to let the
sentinel run. For lane 0, which has no higher-priority lane, test FIFO service
under continuous same-priority replenishment. The test must not weaken the
workload after observing a delayed sentinel.

Acceptance requires:

1. The policy documents a finite maximum READY age in global successful
   scheduler decisions for every supported priority lane.
2. The test records each sentinel's enqueue epoch, dispatch epoch, and
   observed age; the maximum must not exceed the documented bound. The global
   epoch is advanced only while `process_lock` protects a completed production
   scheduling decision.
3. Every sentinel is dispatched and completed exactly once.
4. Queue membership, process state, CPU ownership, and execution-guard
   invariants remain valid throughout the workload.
5. The test reports per-priority and aggregate maximum READY age.
6. A test failure prints the CPU count, priority, enqueue epoch, dispatch
   epoch, and observed age without relying on console output from interrupt
   context.

A bound must be derived from the selected service policy and the maximum
supported workload population. It must not be chosen after observing a
passing run. If the current strict-priority policy cannot provide such a
bound under replenished higher-priority arrivals, the test must expose that
failure; it must not weaken the workload or silently classify the run as
passed.

### 2. Process-lock progress

The scheduler's try-lock fallback must be tested independently of priority
selection. A contention workload must repeatedly create real overlap between
scheduler entries and a controlled process-lock holder, while preserving
the existing rule that interrupt context never spins on the global lock.

Acceptance requires:

1. Per-CPU failed try-acquire counts and successful scheduler-entry counts
   are reported separately.
2. Timer ticks accumulated during deferral are eventually charged exactly
   once to the running task after a successful scheduler entry, subject to
   the existing saturating-counter contract.
3. A CPU that experiences deferrals later completes a successful scheduling
   decision; the test checks progress, not merely that the fallback branch
   executed.
4. No READY, WAITING, STOPPED, or ZOMBIE process is lost, duplicated, or
   incorrectly reaped.
5. The test records a per-CPU maximum consecutive-deferral count and the
   local timer-tick interval between first deferral and the next successful
   scheduling decision.
6. Any proposed finite bound must be justified by the lock-holder protocol
   and workload assumptions. A counter alone is not proof of a wall-clock
   bound.

The process lock must remain authoritative for process-table identity,
lifecycle, and queue/state publication until a separate stable-reference
and reclamation design is implemented and verified.

### 3. Migration age and destination service

READY age must not reset when an eligible process moves between queues.
The source and destination queue locks continue to be acquired in ascending
CPU order, with the existing process-lock-before-runqueue-lock order.

Acceptance requires:

- The age observed at destination dispatch uses the same global decision
  epoch and includes all decisions completed before and after migration.
- A transfer does not count as a dispatch.
- A moved process is dispatched at most once and remains owned by exactly
  one queue or one running CPU.
- A destination service assertion is based on production dispatch, not only
  successful queue insertion or a host-model dequeue.
- Ineligible processes remain excluded according to the current migration
  rules; fairness instrumentation must not broaden migration eligibility.

## Required test workloads

### Replenished-priority workload

- Keep at least one sentinel continuously READY.
- For sentinel priorities 1–3, continuously replenish all higher-priority
  lanes; for priority 0, continuously replenish its own lane.
- Use bounded producers and explicit completion/reaping accounting.
- Repeat with each priority lane used as the sentinel lane.
- Run with 2, 4, 8, and 16 vCPUs; retain a 1-vCPU control run.
- Capture per-CPU and per-priority READY-age maxima and verify the contract
  before reporting success.

### Lock-contention workload

- Exercise the production scheduler try-lock fallback under controlled
  process-lock contention.
- Keep timer delivery active on participating CPUs.
- Verify pending-tick accounting, timeout progress, successful reacquisition,
  exact-once completion, and deferred reaping.
- Run the same vCPU matrix and report per-CPU deferral and timer-delay data.

### Regression controls

Every run must also preserve the existing boot, exception, interrupt,
memory, process lifecycle, run-queue integrity, futex timeout, migration,
USB keyboard-discovery, and xHCI mouse checks. The software test matrix does
not establish physical-hardware behavior.

## Implementation sequencing

1. Wire the validated epoch-delta primitive into a production-owned global
   decision epoch and READY enqueue/dispatch metadata, without resetting age
   during migration; separately count per-CPU lock deferrals.
2. Add deterministic host tests for global epoch wraparound, reset/initialization,
   migration-age preservation, and invalid measurement deltas; then add
   production QEMU assertions for the same metadata.
3. Add production QEMU workloads and parsers for the two independent
   acceptance scenarios.
4. Run the complete build and regression suite, then both correctness and
   integration reviews.
5. Only after the tests expose the current policy's actual behavior, decide
   whether strict priority must be replaced by aging, weighted service, or
   another policy. Test latency and CPU-share regressions before enabling it.

Until all required production workloads pass on the exact tested commit,
scheduler convergence, starvation freedom, and process-lock progress remain
partial. QEMU success is software validation only; physical hardware
verification remains a separate requirement.
