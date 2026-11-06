# Alder Lake GGTT Ownership Contract — Research Checkpoint

## Status

**Decision: NO-GO for GGTT writes.** This document records the current evidence and the minimum contract required before LionOS may reserve, map, or unmap any GGTT entry.

Target currently observed on the test laptop:

- Display PCI function: Intel `8086:468B`, revision `0x0C`, BDF `00:02.0`.
- Platform classification: Alder Lake-S.
- GTTMMADR BAR0: `0x0000006202000000`, reported size 16 MiB.
- Read-only GGTT aperture: BAR0 + 8 MiB; current diagnostics report an 8 MiB aperture.
- Firmware scanout remains active.
- LionOS has not established exclusive ownership of any GGTT entry. The allocator is disabled and the mapping-candidate API must continue to fail closed.

These values are observations from one machine, not a portable Alder Lake contract.

## Source cross-check performed

The current upstream Linux i915 source was inspected as an implementation cross-check:

- `drivers/gpu/drm/i915/gt/intel_ggtt.c`
  - `gen8_ggtt_pte_encode()` constructs the Gen8+ PTE from the DMA address and Present bit, and conditionally sets the Local Memory bit.
  - The Meteor Lake-specific encoder additionally handles PAT bits 52 and 53. This is a warning not to copy a newer platform's PAT encoding into Alder Lake code.
  - `gen8_ggtt_insert_page()` writes a PTE and then calls the platform-selected GGTT invalidate callback.
  - The GGTT invalidate path is selected by platform and may involve GuC TLB invalidation or generation-specific GT registers. It is not a universal single-register operation.
  - The source explicitly discusses write-combining buffer flush ordering for applicable GGTT mappings and uses readback checks to detect dropped writes on affected platforms.
- `drivers/gpu/drm/i915/gt/intel_gtt.h`
  - Defines `GEN12_GGTT_PTE_ADDR_MASK` as bits 45:12 and `GEN12_GGTT_PTE_LM` as bit 1.
  - Defines separate PPGTT PAT fields and Meteor Lake GGTT PAT fields. These must not be conflated.
  - Defines Gen8 Present as bit 0.

References (moving upstream source; pin a reviewed kernel commit before implementation):

- https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/gt/intel_ggtt.c
- https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/gt/intel_gtt.h

**Important limitation:** Linux source is valuable evidence of a working driver implementation, but it is not a substitute for the exact Intel Alder Lake platform/volume programming reference or the firmware interface contract of this machine. The current source review does not prove that LionOS may take over the firmware's GGTT, nor does it settle every Alder Lake cacheability and invalidation detail.

## Findings and unresolved questions

### 1. Ownership and firmware hand-off — unresolved, blocking

A zero-valued PTE proves only that the sampled entry read as zero. It does not prove that firmware, display hardware, a management engine, or another driver has relinquished the address. A nonzero PTE also cannot safely be overwritten merely because it appears unused by LionOS.

Before any range can be allocated, authoritative evidence must identify:

- Who owns the GGTT during firmware boot and after the OS loader transfers control.
- Whether the firmware explicitly hands over the entire GGTT or only a defined subrange.
- Which entries/ranges are reserved for firmware framebuffer scanout, display planes, cursors, overlays, or other agents.
- Whether a documented hand-off flag, mailbox, ownership register, or firmware table exists and how it is validated.
- Whether the display engine continues consuming GGTT translations after the hand-off.
- Whether LionOS can preserve active scanout while managing a disjoint range.

No such hand-off contract has been verified for this target. Therefore **no GGTT range is currently allocatable**.

### 2. PTE format — partially cross-checked, not implementation-ready

The Linux Gen8+ definitions support the address field at bits 45:12, Present at bit 0, and Local Memory at bit 1. However, the exact Alder Lake GGTT PTE requirements still need confirmation from the matching platform documentation, including:

- All reserved and must-be-zero bits.
- System-memory versus local-memory encoding.
- Whether any platform-specific PAT or cacheability bits apply to this generation and GGTT path.
- Required address width and physical-address restrictions.
- Whether the firmware-configured GGTT mode changes the interpretation.

The existing pure encoder self-test is software validation only. It does not authorize PTE writes.

### 3. Cacheability and CPU/GPU coherency — unresolved, blocking

A valid address/PRESENT encoding alone is insufficient. Before mapping CPU-written backing pages, LionOS must establish the Alder Lake policy for:

- GGTT cacheability attributes and the applicable PAT/PPAT configuration.
- CPU cache visibility and GPU cache visibility for the selected memory type.
- Required flush, invalidate, or fence operations when CPU and GPU access the same pages.
- Ordering between page contents, PTE writes, MMIO readback, and invalidation.
- Whether the chosen physical pages are suitable for GPU access under the platform's memory and DMA rules.

Do not infer a cache policy from the kernel's default PTE value or from another Intel generation.

### 4. PTE update and invalidation sequence — unresolved, blocking

Linux i915 demonstrates that invalidation is platform-selected and may differ depending on GuC support and graphics generation. Its implementation also couples write ordering to the GGTT mapping type.

LionOS must identify, from the exact Alder Lake specification and a pinned, matching i915 implementation:

1. The permitted access width and atomicity requirements for a 64-bit PTE update.
2. Required MMIO mapping attributes for the GGTT aperture.
3. Required write barriers and readback behavior.
4. The correct Alder Lake GGTT/GT TLB invalidation mechanism and completion condition.
5. Required GT power/forcewake references and safe release behavior.
6. Whether display-side translation caches require separate treatment.
7. The correct clear/unmap sequence and proof that no in-flight GPU/display access can still use the mapping.

No invalidation register should be guessed from a neighboring generation or copied without confirming the exact device path.

### 5. Transactional mapping and lifetime — required design

Once the platform contract is established, the mapping layer must satisfy all of the following:

- Reserve only a firmware-authorized, explicitly LionOS-owned range.
- Revalidate the selected entries immediately before modifying them.
- Record the prior PTE values and ownership token for every entry.
- Map only pages owned by the corresponding live allocation.
- Write entries using the documented width/order and verify readback.
- Invalidate and wait for documented completion before reporting the mapping usable.
- On partial failure, roll back only entries still proven to be owned by this allocation.
- If rollback, readback, or invalidation is uncertain, poison the allocation and retain its backing pages; never return potentially GPU-referenced pages to the PMM.
- Before unmapping, prove all GPU work and display references to the range have completed.
- Clear only entries owned by the allocation, invalidate again, and free backing pages only after completion is confirmed.

The current BSP-only/external-serialization restriction remains until a concurrency model is designed and tested.

## Go/no-go checklist

GGTT PTE writes remain prohibited until every item below has evidence attached to the code review:

- [ ] Exact Alder Lake platform documentation and applicable stepping identified.
- [ ] Firmware-to-OS GGTT ownership/handoff contract identified and validated at runtime.
- [ ] Firmware scanout and all reserved GGTT ranges accounted for.
- [ ] Exact GGTT PTE bit layout and address constraints confirmed.
- [ ] Alder Lake cacheability/PAT and CPU/GPU coherency policy confirmed.
- [ ] Exact PTE write width, ordering, readback, and MMIO mapping requirements confirmed.
- [ ] Exact invalidation path and completion semantics confirmed.
- [ ] Transactional map/unmap, rollback, and page-lifetime design reviewed.
- [ ] Hardware test plan can verify mapping and unmapping without disrupting firmware scanout.

Until all items are checked, the expected runtime state is:

- `ggtt_ownership_verified = 0`
- `address_space_ownership_verified = 0`
- no GGTT slot reservation
- no GGTT PTE writes
- no GPU-visible allocation
- CPU software compositor remains the active graphics path

## Next safe work item

The next implementation work, if authoritative documents become available, is a read-only contract validator and test plan—not a PTE writer. It should report the exact evidence and unsupported conditions, and preserve fail-closed behavior. If the firmware hand-off cannot be proven, the project must keep GGTT mapping disabled and pursue a different supported interface rather than guessing.


## Pinned upstream implementation cross-check

To avoid relying on a moving `master` URL, the source observations above were rechecked against Linux commit:

- Commit: `8b1858aaaa20255a42d8eefe7e913c161331af0c`
- Date: 2026-04-08
- Files:
  - https://github.com/torvalds/linux/blob/8b1858aaaa20255a42d8eefe7e913c161331af0c/drivers/gpu/drm/i915/gt/intel_ggtt.c
  - https://github.com/torvalds/linux/blob/8b1858aaaa20255a42d8eefe7e913c161331af0c/drivers/gpu/drm/i915/gt/intel_gtt.h

Specific observations from that pinned revision:

1. `gen8_ggtt_pte_encode()` returns the DMA address with `GEN8_PAGE_PRESENT`, and conditionally adds `GEN12_GGTT_PTE_LM`. The separate `mtl_ggtt_pte_encode()` adds MTL GGTT PAT bits 52/53. This confirms that the MTL PAT extension must not be applied to Alder Lake merely because both use a Gen12-era PTE family.
2. `GEN12_GGTT_PTE_ADDR_MASK` is bits 45:12; `GEN12_GGTT_PTE_LM` is bit 1; `GEN8_PAGE_PRESENT` is bit 0. This is an upstream implementation definition, not a replacement for the Alder Lake programming reference or a complete reserved-bit specification.
3. The ordinary Gen8 GGTT path uses `writeq()` / `readq()` for PTE access. The implementation's mapping-type comments explain why certain generations cannot safely use write-combining mappings for GGTT writes. LionOS must not assume its current uncached BAR mapping alone establishes every required ordering/coherency property.
4. The pinned `guc_ggtt_invalidate()` first invokes the generic GGTT flush helper, then iterates GTs: it uses GuC TLB invalidation when available; otherwise, for graphics version 12 and later, it writes `GEN12_GUC_TLB_INV_CR`; older paths use `GEN8_GTCR`. This is conditional on GT/GuC state and is not a single unconditional Alder Lake register recipe.
5. The source includes a readback check discussion for dropped writes with WC mappings, and notes that an uncached MMIO write flushes the write-combining buffer before the relevant invalidate on platforms where that flush is needed. That does not establish LionOS's own BAR mapping attributes, CPU cache policy, or completion/wait contract.

This pinned-source review narrows the software implementation comparison, but it does **not** close the ownership, firmware hand-off, Alder Lake cacheability, or invalidation-completion blockers. The no-go decision is unchanged: no GGTT PTE writes and no ownership claims.


## GRUB / Multiboot2 hand-off audit

The active boot path was inspected at the current `feature/graphics-engine-v2` branch revision:

- `boot/grub.cfg` loads `/boot/lionos.bin` with `multiboot2`, selects `gfxmode=auto`, and uses `gfxpayload=keep`. These settings preserve the firmware/GRUB-selected framebuffer mode; they do not describe ownership transfer of Intel GGTT entries.
- `boot/boot.asm` contains a Multiboot2 header with a framebuffer request tag. The 32-bit entry point passes the Multiboot2 information pointer (EBX) and boot magic (EAX) to `kernel_main`.
- `src/kernel.c` validates the Multiboot2 boot magic before proceeding.
- `src/memory.c` consumes the Multiboot2 memory-map and module tags for physical-memory accounting.
- `src/framebuffer.c` consumes the Multiboot2 framebuffer tag and reserves the reported framebuffer physical range before mapping it for CPU display output.

The inspected boot contract exposes memory-map, module, and framebuffer information to LionOS. No Intel-specific GGTT ownership token, reserved-entry map, firmware arbitration result, or documented GGTT hand-off structure is requested or consumed by this boot path.

**Conclusion:** the current GRUB/Multiboot2 hand-off provides no evidence that firmware has relinquished any GGTT range. `gfxpayload=keep` is a framebuffer continuity setting, not a GPU memory-ownership guarantee. This audit therefore confirms the existing no-go decision; it does not prove that firmware retains every GGTT entry, and it does not authorize LionOS to modify any entry.

### Required follow-up

A future ownership implementation must obtain an authoritative Alder Lake firmware/platform hand-off mechanism independent of the generic Multiboot2 framebuffer and memory-map tags, or use a documented OS-visible interface that explicitly grants a disjoint GGTT range. Until such evidence exists, the runtime must continue to report ownership as unverified and keep GGTT mapping disabled.
