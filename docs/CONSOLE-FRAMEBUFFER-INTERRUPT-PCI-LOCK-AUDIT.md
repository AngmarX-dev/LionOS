# Console, Framebuffer, Interrupt, and PCI Lock Audit

## Scope and status

This bounded source audit records synchronization and lifecycle observations on
`feature/graphics-engine-v2` at parent commit
`8ba463757e2f9b5a70cb08108127cd6374dc6066`.

Inspected files:

- `src/console.c`
- `src/framebuffer.c`
- `src/idt.c`
- `src/pci.c`
- `src/net.c`
- `src/kernel.c`
- `src/lapic.c`
- `src/ioapic.c`
- `src/keyboard.c`
- `src/mouse.c`

This is a source-level audit, not a proof of interrupt safety, deadlock
freedom, or complete device-lifecycle correctness. No runtime code is changed.

## Console state and execution contexts

`src/console.c` keeps `row`, `col`, `color`, and `graphics` as shared
file-static state. `console_putc()`, `console_clear()`,
`console_set_color()`, and `console_use_framebuffer()` access that state
without a lock or an atomic record protocol. `console_write()` emits a string
one character at a time, so even a single logical message is not serialized
against another writer.

The exception dispatcher in `src/idt.c` calls console routines for user and
kernel exceptions; `page_fault_dump()` also writes to the console. Those
paths execute in exception context and can run on different CPUs. The inspected
console implementation has no per-CPU buffer, panic-only output path, or
cross-CPU serialization. Concurrent console writers can therefore interleave
messages and race the shared cursor/color state. This is a concrete
source-level concurrency hazard; no reproducer was run in this documentation
change.

Do not add a blocking console lock mechanically: exception, panic, and
interrupt-adjacent paths may re-enter output while another CPU or the current
CPU owns the lock. A safe design needs an explicit policy for normal output,
non-blocking exception output, panic takeover, and framebuffer/VGA fallback.

## Framebuffer shared state

`src/framebuffer.c` maintains global renderer state including the mapped
framebuffer pointer and geometry, desktop/layer buffers, layer origin and
dimensions, clip rectangle, wallpaper cache, and mode flags. The inspected
drawing, clipping, layer, desktop, and console-rendering entry points access
these fields without a common lock. Layer and clip operations are stateful:
`framebuffer_begin_layer()` saves and replaces the global clip state, while
`framebuffer_end_layer()` restores it. Concurrent rendering callers could
therefore overwrite one another's layer/clip state or draw while another
caller changes the active target.

The source review did not establish that all current desktop rendering calls
are concurrent, nor did it establish a safe caller-ownership contract. Treat
the renderer as single-owner until a documented compositor ownership model or
synchronization protocol is implemented and tested. A lock-scope change must
also account for allocation paths such as `framebuffer_begin_desktop()`,
which calls `kmalloc()`; do not introduce an allocator/renderer lock cycle.

The initialization path maps the framebuffer page-by-page and sets
`enabled` only after all mappings succeed. A failure after one or more
successful page mappings returns immediately; the inspected function does
not roll back those earlier mappings. This is a partial-initialization cleanup
question, not evidence that the framebuffer is currently reinitialized or
unmapped at runtime. No framebuffer teardown API was identified in the
inspected public implementation.

## Interrupt and exception interaction

- The timer and input IRQ paths in `src/idt.c` dispatch keyboard/mouse work
  and scheduling operations. The exception paths can write to the console.
- The AP timer diagnostic uses `debug_write()`, not the shared console
  cursor, and formats a per-CPU first-tick marker.
- The source review did not find a general console lock or a documented
  guarantee that console APIs are forbidden from exception/interrupt context.
- The current audit does not prove that framebuffer drawing is invoked from
  a hard-IRQ handler; the direct evidence is the exception-to-console path.

Keep the distinction between observed exception output and unproven
framebuffer-from-IRQ use. Any future output lock must be checked against
interrupt state, panic recursion, and cross-CPU ownership.

## PCI configuration serialization

The generic PCI accessors in `src/pci.c` and the RTL8139-private accessors in
`src/net.c` both use the legacy CF8/CFC configuration mechanism. The address
selector and data access are separate port operations. Neither implementation
uses a shared lock or a common PCI configuration transaction API.

The inspected RTL8139 discovery is called during network initialization, and
the generic PCI enumeration is also an initialization path. The inspected
source does not prove these operations overlap on separate CPUs today.
Nevertheless, any future concurrent config-space caller could change CF8
between another caller's address write and CFC data access, causing the data
operation to target the wrong BDF/register. This is an unresolved serialization
boundary, not a reproduced runtime race.

Do not add independent locks to each driver. The appropriate integration point
is a shared PCI configuration transaction layer that serializes the complete
address-plus-data sequence and is safe for the contexts that are allowed to
access PCI configuration space.

## Device teardown and remaining gaps

The inspected framebuffer and PCI paths are initialization-oriented; no
general device detach protocol or framebuffer unmap/release lifecycle was
established. The selected input files contain their own event-buffer locks,
but this audit does not cover all driver callbacks, DMA ownership, or
controller teardown paths.

Required follow-up work:

1. Define console ownership and exception/panic output semantics before
   introducing synchronization.
2. Establish compositor/framebuffer single-owner or lock ownership rules,
   including layer/clip state and buffer lifetime.
3. Design rollback for partial framebuffer mapping failure and define whether
   framebuffer reinitialization is supported.
4. Consolidate CF8/CFC access behind one serialized PCI transaction API.
5. Continue the remaining driver, interrupt callback, and device teardown
   audit, then add targeted host/QEMU concurrency and failure-injection tests.
6. Keep physical-hardware validation separate from QEMU and source-level
   evidence.

## Review boundary

This audit records shared-state and lifecycle risks for follow-up design. It
does not authorize a mechanical lock insertion, lock-scope reduction, device
teardown change, or claim of SMP-safe console/framebuffer/PCI operation.
