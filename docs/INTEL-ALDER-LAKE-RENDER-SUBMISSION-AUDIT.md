# Alder Lake Render Submission Contract — Research Checkpoint

## Status

**Decision: NO-GO for GPU command submission.** This document audits the Intel render submission architecture against a pinned upstream Linux i915 implementation. It does not authorize MMIO writes, ring programming, context activation, GGTT modifications, or GPU command execution in LionOS.

Target currently observed by LionOS:

- Intel display function: PCI `8086:468B`, revision `0x0C`, BDF `00:02.0`.
- Platform classification: Alder Lake-S.
- Render engine reported present: `RCS0`.
- Current diagnostic policy: Execlists default; GuC submission disabled.
- GGTT ownership remains unverified.
- Command-buffer backing pages are CPU-mapped system RAM only; no GPU address is assigned.

The observations above are specific to the tested machine. They are not a complete Alder Lake platform specification.

## Pinned implementation used for cross-check

Linux i915 source commit:

- Commit: `8b1858aaaa20255a42d8eefe7e913c161331af0c`
- Date: 2026-04-08

Relevant source files:

- [intel_execlists_submission.c](https://github.com/torvalds/linux/blob/8b1858aaaa20255a42d8eefe7e913c161331af0c/drivers/gpu/drm/i915/gt/intel_execlists_submission.c)
- [intel_lrc.c](https://github.com/torvalds/linux/blob/8b1858aaaa20255a42d8eefe7e913c161331af0c/drivers/gpu/drm/i915/gt/intel_lrc.c)
- [intel_ring.c](https://github.com/torvalds/linux/blob/8b1858aaaa20255a42d8eefe7e913c161331af0c/drivers/gpu/drm/i915/gt/intel_ring.c)
- [intel_engine_cs.c](https://github.com/torvalds/linux/blob/8b1858aaaa20255a42d8eefe7e913c161331af0c/drivers/gpu/drm/i915/gt/intel_engine_cs.c)
- [intel_engine_regs.h](https://github.com/torvalds/linux/blob/8b1858aaaa20255a42d8eefe7e913c161331af0c/drivers/gpu/drm/i915/gt/intel_engine_regs.h)
- [intel_gpu_commands.h](https://github.com/torvalds/linux/blob/8b1858aaaa20255a42d8eefe7e913c161331af0c/drivers/gpu/drm/i915/gt/intel_gpu_commands.h)

Linux i915 is an implementation cross-check, not a substitute for the matching Intel Alder Lake programming reference, stepping-specific errata, or firmware hand-off contract.

## Findings

### 1. The observed submission model is Execlists, not a simple tail-register kick

The pinned `intel_execlists_submission.c` describes request scheduling through execution-list ports. The implementation maintains pending and in-flight requests, submits context descriptors through the ExecLists Submit Port (ELSP), and processes Context Status Buffer (CSB) events to retire completed requests.

The source comments describe two hardware submission ports. Each port is associated conceptually with a context's `RING_START`, `RING_HEAD`, and `RING_TAIL` state. A context is not interchangeable with an arbitrary CPU command buffer.

**LionOS consequence:** writing a render ring tail register is not an adequate substitute for implementing the observed Execlists path. A correct implementation needs context descriptors, request lifetime tracking, submission ordering, CSB processing, completion retirement, and recovery/error handling.

### 2. Context state contains the ring's GPU-visible address and offsets

In `intel_lrc.c`, the context register-state update writes:

- `CTX_RING_START` from `i915_ggtt_offset(ring->vma)`.
- `CTX_RING_HEAD` from the validated ring head.
- `CTX_RING_TAIL` from the ring tail.
- `CTX_RING_CTL` from the ring size and valid bit.

This confirms that the context image refers to a GPU address-space mapping, not the CPU virtual pointer returned by LionOS's high-memory mapper.

**LionOS consequence:** the current one-page command-buffer adapter cannot be passed to an Intel context as-is. Its CPU virtual address and physical address do not establish a GGTT address or a valid context ring start.

### 3. Ring storage has stricter allocation and lifetime requirements

In `intel_ring.c`, the reference implementation creates a ring VMA in the GGTT address space. The object allocation path considers local memory, stolen memory on applicable systems, and internal memory. Ring pinning establishes the GGTT mapping and a CPU mapping with a platform-selected coherent mapping type. The implementation also handles ring wrap, available space, pin counts, and request-dependent space reclamation.

**LionOS consequence:** the current generic GPU memory provider only owns CPU-accessible backing RAM and deliberately does not reserve GGTT entries. It does not yet provide the address-space mapping, cache/coherency policy, pin lifetime, or ring wrap/space contract needed by an engine ring.

### 4. Completion requires hardware-written status and interrupt/event processing

The reference implementation initializes an engine status page and associates request/timeline completion with hardware-visible status. Execlists completion is observed through CSB events and then reconciled with the software request queue. The source also contains ordering workarounds around CSB reads, demonstrating that a simple memory read or guessed interrupt flag is not a sufficient completion contract.

**LionOS consequence:** a software counter or a successful MMIO write must never be reported as GPU completion. A future fence must be backed by a documented hardware completion mechanism and validated against the corresponding request.

### 5. Legacy ring registers are not a safe fallback shortcut

The pinned `intel_engine_regs.h` defines the generic ring register offsets and fields, while `intel_ring_submission.c` contains a separate legacy submission path. That path performs explicit stop/empty/disable sequencing, status-page setup, posting reads, ring start programming, and engine-specific initialization.

The presence of `RING_HEAD`, `RING_TAIL`, `RING_START`, and `RING_CTL` definitions does not establish that legacy submission is the correct mode for this Alder Lake device. LionOS diagnostics currently report Execlists as the default policy.

**LionOS consequence:** do not program legacy ring registers merely because their offsets are known. The chosen submission mode must match the device generation, enabled engine mode, firmware state, and the validated driver contract.

### 6. Command opcodes are not ready for use

The pinned `intel_gpu_commands.h` defines MI and 3D command encodings, including batch-buffer and completion-related commands. These definitions are only one part of the command contract. Safe use also depends on engine class, graphics version, command length/field rules, address-space selection, privilege restrictions, parser policy, cache flush requirements, and valid GPU addresses.

**LionOS consequence:** the existing command buffer intentionally stores opaque dwords. Do not add guessed MI packets or treat arbitrary test dwords as executable commands. A command validator must be generation- and engine-aware and must reject unsupported packets.

## Blocking requirements before any hardware submission

The following requirements remain open:

- [ ] Authoritative Alder Lake platform programming reference and applicable stepping errata are available and reviewed.
- [ ] Firmware/OS hand-off explicitly establishes the GGTT range LionOS may own.
- [ ] Firmware scanout and all other GGTT consumers/reservations are accounted for.
- [ ] Exact GGTT PTE format, permitted address range, cacheability, coherency, write ordering, and invalidation completion are confirmed.
- [ ] The actual Alder Lake submission mode and engine enablement state are validated at runtime.
- [ ] Ring and context backing objects can be mapped into a proven LionOS-owned GPU address space.
- [ ] Context descriptor format, submission-port protocol, CSB layout, interrupt routing, and completion semantics are confirmed.
- [ ] Engine power/forcewake acquisition, reset, timeout, hang detection, and cleanup paths are designed.
- [ ] Command parsing/validation and address-space isolation prevent arbitrary or malformed GPU commands.
- [ ] Failure handling retains backing pages whenever the GPU may still reference them.
- [ ] Hardware tests can verify a harmless render operation and completion without disrupting firmware scanout.

## Safe next implementation boundary

Until the blocking requirements are closed, LionOS may continue with CPU-side abstractions and read-only diagnostics. The next code work must not:

- write GGTT PTEs;
- write render ring or Execlists submission registers;
- construct executable Intel MI/3D packets;
- claim GPU command submission or completion;
- mark the Intel backend OPERATIONAL.

The CPU software rasterizer remains the active graphics implementation. Intel hardware may be reported as detected/initialized for diagnostics only.

No xHCI source or code path is part of this audit or the proposed follow-up.
