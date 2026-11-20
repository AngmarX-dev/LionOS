# Lazy FPU Evaluation

## Decision

**Keep eager FPU/SSE context switching. Do not enable lazy FPU state switching yet.**

The current implementation is conservative and has a small, explicit ownership model: every schedulable process context owns an aligned 512-byte FXSAVE image, and the scheduler saves the outgoing context and restores the incoming context on an actual task switch. This costs memory per process and performs FXSAVE/FXRSTOR even when a task did not use floating-point or SIMD instructions, but it avoids deferred ownership and cross-CPU synchronization.

This document records an evaluation, not an implementation of lazy FPU switching. The existing eager path remains the only supported path.

## Existing implementation observed

- `struct process` contains a 512-byte `fpu_state` aligned to 16 bytes and an `fpu_valid` flag.
- `cpu_fpu_sse_init()` clears CR0.EM, sets CR0.MP, and enables CR4.OSFXSR and CR4.OSXMMEXCPT when the BSP-discovered SSE feature is available.
- Each AP invokes `cpu_fpu_sse_init()` before it is marked online.
- `process_init()` initializes a shared immutable default FXSAVE image using `FNINIT; FXSAVE`.
- The scheduler saves the outgoing process with `FXSAVE` and restores the incoming process with `FXRSTOR`. The same eager handling is present in bootstrap and AP-idle fallback transitions.
- `fork` saves the parent's live FPU/SSE state and copies the image into the child.
- The current exception dispatcher treats vector 7 as an ordinary exception. There is no dedicated `#NM` (Device Not Available) handler, and the kernel does not currently set CR0.TS as a lazy-switch trigger.
- The scheduler currently permits a deliberately narrow READY-user-process migration path. A process may move between CPUs, so a future lazy design cannot assume that its previous FPU owner is the same CPU on which it next runs.

## Eager versus lazy trade-off

| Concern | Current eager path | Lazy path requirements |
|---|---|---|
| Switch cost | FXSAVE/FXRSTOR on real switches | Avoids transfers for tasks that do not use FPU/SIMD |
| Ownership | State is in the process object | Per-CPU live-owner tracking and a valid saved-image state |
| First FPU use | No trap required | Correct #NM handling and CR0.TS management |
| SMP migration | Saved image travels with the process | Must revoke or synchronize ownership on the old CPU before a new CPU can claim the task |
| fork | Explicitly snapshots and copies the parent image | Must materialize a live hardware-owned image before copying |
| Threads | Each process/thread object has its own saved image | Must define whether each schedulable thread has an independent architectural state |
| Kernel SIMD/FPU use | Eager task state remains isolated at switch boundaries | Requires a kernel-FPU begin/end protocol or a strict prohibition on kernel FPU use |
| Failure behavior | No deferred owner state to reconcile | A stale owner or missed remote synchronization can corrupt another task's state |

Lazy switching is not automatically faster for this workload. The benefit depends on how many tasks actually execute FPU/SSE instructions between switches, while the cost includes trap handling, owner tracking, and SMP coordination. No performance claim is made without workload measurements.

## Blocking requirements before implementation

1. **Feature and control-register validation**
   - Check the relevant CPUID capabilities on every logical processor, not only the BSP.
   - Define and verify the required CR0 and CR4 bits on every CPU before publishing it online.
   - Keep a safe fallback for CPUs that do not satisfy the required feature set.
2. **Dedicated #NM path**
   - Install a dedicated vector-7 handler and preserve the exception frame contract.
   - Distinguish user-mode first use from kernel-mode faults.
   - Define behavior for unexpected or nested #NM faults; do not silently retry an invalid kernel instruction.
3. **Per-CPU ownership protocol**
   - Track the current hardware FPU owner independently on every CPU.
   - Save the previous owner before assigning a different task.
   - Define locking, interrupt state, memory ordering, and remote-CPU invalidation rules.
   - Ensure that a process can never be considered the live owner on two CPUs simultaneously.
4. **Migration and lifecycle**
   - Materialize/revoke old-CPU ownership before a migratable process can execute on another CPU.
   - Handle process exit, reaping, slot reuse, fork, exec, thread creation, and thread exit without leaving stale owner pointers.
   - Decide and test the architectural initial-state semantics for newly created tasks and exec.
5. **Kernel FPU policy**
   - Audit compiler output and all kernel code for FPU/SSE use.
   - Either prohibit kernel FPU/SIMD use with enforceable build/runtime checks or add a nested-safe kernel-FPU ownership protocol.
6. **Regression and performance evidence**
   - Add userland tests that exercise x87 and SSE state independently across preemption, yield, blocking, fork, exec, and migration.
   - Add multi-vCPU tests that force ownership changes and cross-CPU movement.
   - Compare eager and lazy paths using a repeatable workload; retain eager switching unless measurements show a meaningful improvement without regressions.

## Acceptance criteria for a future implementation

Lazy FPU may be considered only after all requirements above have dedicated tests, the full QEMU topology matrix passes, both correctness and integration reviews pass, and the measured benefit justifies the additional SMP complexity. Until then, `FXSAVE/FXRSTOR` eager switching remains the required fallback and production path.

## Scope and verification note

This evaluation is based on the current `feature/graphics-engine-v2` source for `src/process.c`, `src/cpu.c`, `src/idt.c`, `src/smp.c`, and `include/process.h`. It does not claim physical hardware validation or measured performance.
