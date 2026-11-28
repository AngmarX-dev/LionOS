# Interrupt Dispatch, EOI Ownership, and Callback Audit

## Scope and revision

This source-level audit covers interrupt dispatch and the related APIC/IOAPIC
and paging shootdown paths on `feature/graphics-engine-v2`, parent commit
`759084bcf93e3e0b3d32275bde1118f46372f6b9`.

Inspected files:

- `src/idt.c`
- `src/ioapic.c`
- `src/lapic.c`
- `src/paging.c`
- `src/keyboard.c`
- `src/mouse.c`

No runtime code was changed. This review establishes source call paths and EOI
ownership only; it is not a hardware interrupt-routing test.

## Vector dispatch and acknowledgement ownership

| Vector | Source path | EOI owner observed |
| --- | --- | --- |
| 32 | Legacy timer/frame tick; keyboard and sometimes mouse polling | `legacy_irq_eoi(32)` selects LAPIC EOI after IOAPIC ISA activation, otherwise PIC master EOI |
| 33 | Keyboard IRQ; shared i8042 output service | `legacy_irq_eoi(33)` selects LAPIC EOI after IOAPIC activation, otherwise PIC master EOI |
| 44 | Mouse IRQ; shared i8042 output service | `legacy_irq_eoi(44)` selects LAPIC EOI after IOAPIC activation, otherwise PIC slave then master EOI |
| 48 | Local APIC timer | `lapic_timer_tick()` increments timer state and sends LAPIC EOI |
| 49 | TLB shootdown IPI | `paging_tlb_shootdown_interrupt()` invalidates pages, increments ACK, and sends LAPIC EOI |
| 50 | Reschedule IPI | `interrupt_dispatch()` sends LAPIC EOI before entering the scheduler |

The vector-49 path intentionally owns its EOI in the paging callback rather
than in the IDT dispatcher. The other APIC paths have their EOI in the
dispatcher or the timer helper. This split is valid in the inspected call
graph, but it is an important maintenance contract: moving the EOI into a
common dispatcher must not create a double EOI, and removing it from a helper
must not leave an APIC ISR bit in service.

## IOAPIC activation transition

`ioapic_activate_isa_routes()` verifies that the prepared ISA routes target the
BSP, unmasks the planned entries, then masks the legacy PIC. If PIC masking
fails, it attempts to restore all ISA IOAPIC routes to masked state before
returning failure. The active flag is published only after successful
activation. The IDT's legacy EOI helper consults this flag to choose LAPIC or
PIC acknowledgement.

The inspected activation sequence is BSP-oriented and relies on the caller's
interrupt-disabled handoff described in the implementation. This audit does
not establish behavior for concurrent route reconfiguration or hotplug; no
runtime route-reconfiguration API was identified in the inspected paths.

## Shared i8042 service and input callbacks

Both vector 33 and vector 44 ultimately drain the shared i8042 output buffer
through `keyboard_ps2_service()`. The service is bounded to at most 64 status
reads per invocation, discards parity/timeout bytes, and dispatches AUX bytes
to the mouse parser and non-AUX bytes to the keyboard parser. This is an
intentional shared-controller design: the mouse IRQ is not a separate
independent data FIFO.

The timer vector can also call `keyboard_poll()`, which delegates to the same
service. The source therefore has three entry paths (keyboard IRQ, mouse IRQ,
and timer polling) into one shared controller drain routine. On the BSP these
interrupts are locally serialized by interrupt execution, but the code does
not encode a single designated consumer or a cross-CPU ownership assertion.
The timer polling path is a compatibility/fallback path and should be
documented as such at the dispatch site, or removed only after verifying that
legacy and IOAPIC keyboard IRQ delivery is reliable on every supported boot
configuration.

The timer path also invokes `mouse_poll()` in the configured frame-timer
modes. That function polls xHCI, updates cursor/button state, pushes events,
and manages retry counters. The source inspection confirms the callback path
but does not prove that all xHCI polling work is bounded or safe for every
timer interrupt context; the xHCI implementation was not included in this
audit.

## TLB shootdown IPI

The paging shootdown callback reads the published address/page count, performs
one `invlpg` per page, increments the acknowledgement counter, and issues
LAPIC EOI. The BSP-side sender publishes the page range before sending IPIs,
counts only online APs with supported xAPIC IDs as targets, and waits until
the acknowledgement count reaches the target count before returning.

The global shootdown payload is a single shared transaction. Its safety
depends on the caller-side mapping lock and BSP-only mapping policy preventing
overlapping transactions. The callback does not independently identify a
transaction generation; this audit did not find a supported concurrent
shootdown producer. A future asynchronous or multi-producer design would need
a generation/sequence protocol rather than reusing the single global payload.

The APIC ID filter excludes IDs above 255 because the current IPI sender uses
xAPIC destination fields; x2APIC is separately rejected until an x2APIC driver
exists. This is a deliberate capability boundary, not evidence of x2APIC
support.

## Outstanding verification

1. Exercise all configured IRQ0/1/12 routes in QEMU and verify exactly one
   EOI per delivered interrupt after both PIC and IOAPIC activation.
2. Add a test or diagnostic assertion that detects duplicate/missing EOI
   ownership for vectors 48-50 and TLB vector 49.
3. Prove the timer polling fallback is needed for each supported legacy/IOAPIC
   configuration before changing the shared i8042 service entry paths.
4. Audit the full xHCI polling and retry call graph for hard-IRQ suitability,
   bounded execution, and lock interactions.
5. Add failure-injection coverage for IOAPIC activation rollback and verify
   that PIC/IOAPIC source ownership remains coherent after each failure.
6. Test TLB shootdown under concurrent mapping callers and delayed AP
   acknowledgements; keep the current synchronous BSP transaction until a
   transaction-generation design is justified.
7. Keep QEMU evidence separate from physical-hardware verification.

## Review boundary

This audit does not claim that IRQ routing is physically verified, that every
EOI path has been observed on hardware, or that xHCI polling is IRQ-safe. It
records the current source ownership model and the tests still needed before
changing interrupt dispatch.
