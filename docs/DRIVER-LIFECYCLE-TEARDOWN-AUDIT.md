# Driver Initialization, Failure Recovery, and Teardown Audit

## Scope

This bounded source review records initialization and lifecycle observations
on `feature/graphics-engine-v2` at parent commit
`afbefc15a77ba42d5e815fd51d6e65030be2b47d`.

Inspected implementation files:

- `src/ata.c`
- `src/ata_block.c`
- `src/block_device.c`
- `src/keyboard.c`
- `src/mouse.c`
- `src/net.c`
- `src/pci.c`
- `src/idt.c`

The xHCI implementation was deliberately excluded. This is a source-level
lifecycle audit; no runtime code was changed and no hardware teardown behavior
was exercised.

## ATA and block-device lifecycle

- ATA initialization is serialized by the channel-wide `ata_io_lock`.
  Master and slave identification reset their respective cached sector counts
  before probing, and publish a nonzero capacity only after successful IDENTIFY
  parsing.
- ATA read/write operations validate the buffer and LBA against the cached
  capacity, acquire the channel lock, perform the PIO transaction, and release
  the lock on both DRQ failure and normal completion.
- The block-device adapter installs static operation tables and stores a
  caller-owned `struct block_device`; it allocates no backing object and
  exposes no unregister or teardown operation.
- No ATA channel shutdown, device detach, or block-device unregister API was
  identified in the inspected public implementation. The static lock and
  cached device capacities are boot-lifetime state in the reviewed code.

These observations do not establish whether a future hot-unplug design is
required. If detach is introduced, callers must first stop new I/O and drain
in-flight operations before invalidating the device descriptor or capacity.

## Keyboard initialization and input path

- `keyboard_init()` resets software parser/buffer state, probes the i8042,
  disables both ports, flushes output, configures the controller, enables the
  keyboard port, sends reset and scanning commands, then unmasks the legacy
  keyboard IRQ.
- If the controller probe fails, initialization returns before publishing
  the controller as present. If later controller/device commands fail, the
  implementation reports diagnostic status but does not expose a general
  keyboard shutdown or detach operation.
- `keyboard_rearm_after_mouse_init()` rewrites controller configuration and
  re-enables scanning; it is a re-arm path, not a teardown path.
- `keyboard_poll()` directly invokes `keyboard_irq_handler()`. The timer
  vector in `src/idt.c` calls this compatibility polling entry point, while
  the dedicated keyboard vector also invokes the handler. This means the
  handler is reachable from both timer and keyboard interrupt paths. The
  reviewed source does not document an explicit single-consumer or
  controller-status ownership contract for those entry paths.

The dual entry path should be resolved by defining whether timer polling is
still required and by ensuring only one serialized consumer drains the
controller output buffer. Do not add a blocking lock to an IRQ path without
proving reentrancy and interrupt-state behavior.

## Mouse initialization and recovery

- `mouse_init()` initializes its event spinlock and ring indices before
  enabling the PS/2 auxiliary port and configuring the controller.
- On controller-configuration response failure, it clears
  `ps2_controller_present` and `mouse_event_lock_ready`, then returns. In
  this branch, the inspected code does not call
  `keyboard_rearm_after_mouse_init()` and does not explicitly restore the
  previous i8042 configuration or disable the auxiliary port.
- Later PS/2 defaults/enable failures do call the keyboard re-arm routine,
  but leave the event lock initialized and the controller-present flag set.
  These are distinct failure paths and should not be collapsed into one
  generic cleanup claim.
- USB mouse initialization and retry update static state and call into xHCI;
  the xHCI implementation and its resource lifecycle were not inspected here.
- No PS/2 mouse shutdown, USB mouse detach, event-channel unregister, or
  general mouse teardown API was identified in the reviewed public interface.

The early PS/2 configuration failure is a concrete partial-initialization
cleanup candidate. A future fix should preserve the original controller
configuration and use one carefully ordered rollback path, but must first
account for keyboard re-arm semantics and shared i8042 ownership.

## Network device lifecycle

- `net_init()` initializes the loopback queue lock and queue entries, clears
  `rtl_ready`, and then invokes RTL8139 initialization.
- RTL8139 initialization resets the controller, reads the MAC, programs RX/TX
  registers and publishes `rtl_ready` only after the setup sequence completes.
  A reset timeout returns failure before publishing readiness.
- No RTL8139 shutdown/reset-to-quiescent API, DMA-buffer release path,
  network-device unregister, or driver detach operation was identified in
  `src/net.c`. RX/TX buffers and the controller state are static storage.
- The RTL8139 polling and transmit paths share static ring offsets, TX index,
  frame buffers, and IP identification state; the previous network/storage
  lock audit already recorded the absence of a common lock for this physical
  path. This review does not repeat that finding as a new defect.

If device teardown is added, the driver must first prevent new callers, stop
the device's RX/TX activity, wait for in-flight polling/transmit users to
quiesce, and only then release or reuse DMA-visible storage. No such protocol
is currently established by the inspected code.

## PCI enumeration lifecycle

- `pci_init()` resets its static counters and repopulates the static device
  array during a full configuration-space scan.
- The recorded device list is exposed through indexed copy-out accessors.
  No device object allocation, unregister, hotplug notification, or teardown
  path is present in the inspected implementation.
- Re-running enumeration while readers use the static array would require an
  explicit publication/reader-lifetime protocol. The inspected source does
  not document concurrent re-enumeration as supported.

This is an API/lifecycle limitation, not evidence that current boot-time
enumeration races with a runtime reader.

## Interrupt callback and context boundary

The timer interrupt path in `src/idt.c` invokes both `keyboard_poll()` and,
depending on the active LAPIC frame-timer mode, `mouse_poll()`. The dedicated
keyboard/mouse vectors also process input. The source therefore has multiple
interrupt-context entry points into input processing. The current review
establishes the call graph but does not prove that any specific controller
transaction is concurrently executed or that a deadlock is reproducible.

The input path needs a documented ownership model: which vector drains PS/2
bytes, which context polls USB reports, and which operations are deferred to
foreground service. Avoid moving controller recovery or lengthy polling into
a hard interrupt without explicit bounded-work and reentrancy guarantees.

## Follow-up requirements

1. Define a device lifecycle contract: initialized, operational, quiescing,
   detached, and failed states, with caller-visible semantics.
2. Add rollback for the PS/2 mouse configuration failure while preserving
   shared keyboard-controller state.
3. Define whether input polling is timer-IRQ work or dedicated-IRQ work;
   avoid duplicate consumers of the same controller/event stream.
4. Specify quiescence and DMA-buffer lifetime before implementing network or
   USB device teardown. The xHCI path requires a separate focused audit.
5. Decide whether PCI re-enumeration and block-device unregister are supported;
   if so, add stable references and reader/I/O draining before invalidation.
6. Add failure-injection tests for each initialization stage and tests proving
   that failed initialization leaves the previous controller state usable.
7. Keep QEMU tests, source-level evidence, and physical-hardware verification
   reported separately.

## Review boundary

No lock was added, no driver behavior was changed, and no hardware support
claim is made. The findings identify lifecycle contracts and cleanup work
that must be designed before implementing detach or hotplug.
