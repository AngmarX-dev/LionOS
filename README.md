# LionOS — Rust Rewrite Track

This branch is an isolated Rust rewrite experiment based on the existing
`feature/graphics-engine-v2` boot contract. It does not modify the C branch
or the repository's default branch.

## Implemented foundations in this branch

- 32-bit x86 Multiboot2 entry point, dedicated bootstrap stack and linker script.
- Freestanding `no_std` Rust kernel with VGA text and COM1 serial diagnostics.
- Flat ring-0 GDT setup and CPUID feature discovery.
- 256-entry IDT with normalized stubs for CPU exception vectors and legacy IRQ vectors 0–15.
- Legacy PIC remapping, per-vector dispatch, fail-closed exception handling and master/slave EOI support.
- PIT channel 0 timer at nominal 100 Hz; IRQ0 is the only unmasked legacy PIC line.
- Initial supervisor-only 4 GiB identity mapping using 4 MiB PSE pages, guarded by a CPUID PSE capability check.
- Fixed physical-page allocator with contiguous allocation, power-of-two alignment and boundary-constrained page runs; allocator metadata is serialized with an interrupt-safe spinlock. Kernel image,
  boot information, low memory and Multiboot2 modules are reserved.
- Page-backed Rust global allocator (allocations own contiguous 4 KiB pages).
- Validated Multiboot2 tag traversal.
- Fixed-capacity 4 KiB physical frame allocator for the 32-bit physical address
  space, initialized from usable memory-map entries.
- PCI configuration-space enumeration, command/status and interrupt metadata, passive BAR decoding, reversible memory-BAR sizing and safe 16-bit Command-register control.
- Identity-mapped PCI MMIO window access with 4 MiB PDE PCD/PWT cache-disable marking; current implementation is limited to 32-bit physical BAR ranges and the bootstrap page map.
- Zeroed, physically contiguous DMA buffer allocator with page alignment, optional power-of-two boundary constraints and exclusive maximum physical address; current DMA addresses are limited below 4 GiB.
- xHCI PCI discovery, reversible memory-BAR sizing, bounded 4 KiB capability inspection, BIOS/OS ownership request, bounded controller halt/reset and CNR readiness checks; controller remains halted; DMA ring/base registers are now configured while PCI bus mastering and Run/Stop remain disabled.
- Fixed-capacity device registry (256 entries) and driver match/probe API (16 slots); no hardware drivers are registered yet.
- RGB 24/32-bpp Multiboot2 framebuffer metadata validation and CPU pixel/rectangle
  drawing when the framebuffer is addressable below 4 GiB.
- Bounded in-memory file store (RAMFS core).
- Static task registry and priority-based ready-task selection metadata.
- PS/2 keyboard data polling primitive.
- Ethernet frame-header parser.
- Initial syscall-number ABI draft.
- Basic framebuffer window rectangle compositor primitive.
- BIOS bootable Multiboot2 ISO packaging via GRUB, with a hybrid-ISO workflow intended for USB writing.
- Manually dispatched GitHub Actions workflow that builds the kernel and ISO and uploads the image artifact.

## USB boot scope and limitations

The current USB image is a **legacy BIOS / x86 32-bit boot image**. Firmware
and GRUB load the kernel from the USB device into RAM; the kernel does not
currently need a USB mass-storage driver just to be loaded. This is different
from supporting USB devices after the kernel starts.

- The image build uses `make iso` or `scripts/build-usb-image.sh`.
- Output: `dist/lionos-bios-usb.iso`.
- On Windows, write the ISO with Rufus and choose **DD mode** if Rufus asks.
  Writing the image erases the selected USB drive.
- In firmware setup, enable legacy BIOS/CSM boot if available and select the USB
  device from the one-time boot menu.
- This image does not currently provide UEFI boot support.
- xHCI validates its memory BAR, requests OS ownership, halts and resets the controller with bounded waits. A separate DMA-layout module allocates DCBAA, command/transfer producer rings, a single-segment event ring, ERST, slot-1 Device/Input Contexts, and optional scratchpad buffer arrays/storage using HCSParams2 and the context-size capability. It deliberately does not program these addresses into the controller, enable Run/Stop, ring doorbells, enumerate devices or handle ports.
- USB keyboard and mouse HID drivers are not implemented. Firmware USB legacy
  emulation may work on some machines, but is not guaranteed.
- The kernel currently has no persistent block storage or USB filesystem
  access after handoff. Its RAMFS is volatile; removing the USB after boot
  does not provide a persistent storage path.
- The PIT timer currently uses legacy PIC IRQ0 only. Keyboard IRQ1 and all other
  legacy IRQs remain masked; xHCI MSI/MSI-X and APIC/IOAPIC routing are not
  implemented, so this does not yet enable USB controller interrupts.
- The ISO pipeline and image itself have not been boot-tested in QEMU or on
  physical hardware in this development session. Do not use it as a production
  or recovery operating system.

## Important scope boundary

This is **not yet a complete or boot-verified operating system**. The modules
above provide real foundations, but several subsystems are intentionally
limited and must not be represented as production-ready:

| Subsystem | Current state |
|---|---|
| Boot / Multiboot2 | 32-bit kernel entry; BIOS GRUB ISO packaging added; boot verification pending |
| UEFI | Not implemented |
| GDT / IDT | Flat GDT; dedicated exception/IRQ entry stubs; exceptions fail-stop (no recovery) |
| PIC / IRQ | PIC remapped; IRQ0 enabled for PIT; remaining IRQs masked; legacy PIC only |
| Physical memory | Fixed bitmap allocator, aligned/boundary-constrained contiguous allocations and 4 GiB identity map; no isolation or demand paging |
| Heap | Page-backed global allocator; page-granular allocations only |
| PCI / devices | PCI enumeration, BAR metadata and reversible xHCI BAR sizing; bounded registry and driver-binding API, no general PCI resource allocator or active device drivers |
| USB boot | BIOS firmware + GRUB loads the kernel; post-handoff USB storage is not supported |
| DMA / MMIO | Zeroed contiguous DMA allocator below 4 GiB; identity MMIO windows with coarse 4 MiB uncached mapping; no IOMMU/cache-coherent abstraction |
| USB controllers | xHCI BAR sizing, ownership handoff and bounded halt/reset; DMA rings, DCBAA, slot-1 contexts and optional scratchpads allocated; DCBAAP/CRCR/ERST registers configured while halted; bus mastering and Run/Stop disabled; no USB enumeration |
| USB HID | Keyboard/mouse drivers not implemented |
| Framebuffer | CPU software drawing; no accelerated GPU command submission |
| Intel UHD | PCI discovery only; native display/GPU driver not implemented |
| NVIDIA RTX | PCI discovery only; native driver not implemented |
| Tasks | Registry only; no context switch, scheduler tick or user mode |
| Syscalls | ABI draft; dispatch intentionally returns not-implemented |
| Filesystem | RAM-only bounded file store; no VFS/block storage/persistent disk FS |
| Input | PS/2 scancode polling; IRQ1 remains masked; no IRQ-driven input or USB HID |
| Networking | Ethernet header parser only; no NIC driver, ARP, IP, TCP or sockets |
| Desktop | Basic rectangle drawing; no font system, windows/events or Liquid Glass |
| SMP / APIC | Not implemented |
| Power management | Not implemented |

Native GPU acceleration is a substantial independent driver project. Intel
and NVIDIA require different hardware initialization and memory-management
paths; PCI detection or a framebuffer does not imply GPU acceleration. The
NVIDIA laptop GPU path also depends on platform firmware, display routing,
and documented device-specific interfaces.

## Build prerequisites

On Debian/Kali/Ubuntu:

```sh
sudo apt-get update
sudo apt-get install -y build-essential gcc-multilib nasm grub-common grub-pc-bin xorriso binutils
rustup target add i686-unknown-linux-gnu
```

Build and validate the kernel:

```sh
make
make check
```

Build the BIOS USB image:

```sh
make iso
# or
./scripts/build-usb-image.sh
```

Expected outputs:

```
target/i686-unknown-linux-gnu/release/lionos-rust
dist/lionos-bios-usb.iso
```

The GitHub Actions workflow is manually dispatched from the Actions tab; it
builds the kernel and ISO and uploads `lionos-bios-usb-image` as an artifact.
It is not triggered automatically on every push.

## Remaining engineering work before meaningful end-to-end testing

1. Verify and correct the current freestanding build/link contract.
2. Replace the bring-up identity map with a deliberate kernel virtual-memory layout,
   page-level permissions, guard pages and memory protection.
3. Add APIC/IOAPIC support, spurious IRQ handling, robust exception diagnostics/recovery and interrupt-safe synchronization.
4. Add a real scheduler and architecture context-switch implementation.
5. Add user-mode transition, process address spaces and syscall entry/return.
6. Add a VFS abstraction, initramfs and persistent block-device support.
7. Add storage drivers appropriate to supported hardware (NVMe/AHCI/ATA).
8. Continue xHCI from the prepared DMA structures and halted register programming: implement safe controller start, command completion/event processing, then port reset and device enumeration before USB HID and mass storage.
9. Add UEFI boot support with an architecture-compatible loader and handoff.
10. Add NIC drivers and a tested network stack.
11. Expand the graphics stack: framebuffer backend, fonts, compositor, window/event
    system, then device-specific Intel graphics support.
12. Define a realistic NVIDIA support boundary and implement only documented,
    testable device paths.
13. Add power management, ACPI, SMP and regression tests.
14. Boot-test the ISO in QEMU, then validate BIOS/UEFI and USB controller variants
    on physical hardware before declaring the image ready.

The existing C implementation remains the reference branch until the Rust
rewrite has demonstrated feature parity through repeatable tests.

## MMIO and DMA constraints

The current MMIO accessor relies on the temporary 4 GiB identity map and marks whole 4 MiB PDE regions uncached using PCD/PWT. This is intentionally coarse and must be replaced by 4 KiB page mappings with explicit cache types before broader driver use. BAR sizes are probed reversibly for xHCI validation; resources are not reassigned and MMIO remains limited to verified windows. The DMA allocator returns physically contiguous, zeroed pages below an exclusive 4 GiB limit and supports page-granular alignment and power-of-two boundary constraints. It assumes x86 cache-coherent DMA and identity mapping; there is no IOMMU, bounce-buffer layer, >4 GiB DMA, or device-ownership tracking. Buffers must not be released until hardware has stopped using them, and allocator calls are not permitted from interrupt handlers.

## xHCI discovery and initialization scope

src/xhci.rs identifies PCI functions with class 0x0C, subclass 0x03, programming interface 0x30, validates a reversible memory-BAR size probe, and reads a bounded 4 KiB MMIO capability window. It requests OS ownership through the Legacy Support capability, waits for BIOS release, disables PCI bus mastering, halts the controller, performs a bounded host reset and waits for CNR to clear. It deliberately leaves Run/Stop disabled. The separate src/xhci_dma.rs module prepares DCBAA, command/transfer rings, Event Ring, ERST, slot-1 Device/Input Contexts and optional scratchpad buffers in DMA memory, but does not program them into the controller or enable transfers.

## xHCI ownership and reset stage

The xHCI bring-up now performs a reversible PCI memory-BAR size probe before mapping the register window. It temporarily disables PCI I/O decode, memory decode and bus mastering, probes the BAR mask (including a 64-bit BAR pair), restores both BAR dwords and restores the original 16-bit Command register. It rejects a BAR that is smaller than the bounded capability window or extends beyond the current 32-bit identity map. It does not allocate or reassign PCI resources.

If the xHCI Legacy Support capability is present and BIOS Owned is set, the kernel sets OS Owned and waits a bounded interval for BIOS Owned to clear. A timeout aborts the reset path. Once ownership is available, the kernel clears USBCMD.Run/Stop, waits for USBSTS.Halted, sets Host Controller Reset, waits for HCRST to clear, then waits for USBSTS.Controller Not Ready to clear. All waits are bounded. The controller is intentionally left halted; PCI bus mastering remains disabled, and no DMA structures, command rings, event rings, interrupters or port operations are started. This is an initialization foundation, not USB device support. The BAR probe and hardware state transitions have not been build- or hardware-tested in this development session.


## xHCI DMA memory-layout stage

A dedicated src/xhci_dma.rs module defines 16-byte TRBs, a 256-entry DCBAA, command and transfer producer rings with a terminal Link TRB (Toggle Cycle enabled), a 256-entry single-segment Event Ring, and a one-entry Event Ring Segment Table. It also allocates a slot-1 Device Context (32 contexts) and Input Context (33 contexts), using 32-byte or 64-byte context sizing from HCCPARAMS1.CSZ. When required, a scratchpad pointer array and page-backed scratchpad storage are allocated from the HCSParams2 count. Ring allocations are page-backed, physically contiguous, below 4 GiB, 64-byte aligned by the page allocator, and constrained not to cross a 64 KiB boundary.

This is a memory-layout foundation only. It does not program DCBAAP, CRCR, ERSTBA, ERSTSZ or ERDP; it does not set CONFIG, enable PCI bus mastering, start Run/Stop, enable interrupters or ring doorbells. Scratchpad support is limited to a single contiguous page-backed storage region and pointer array; it is allocated only after the controller reports ReadyStopped. No context or scratchpad addresses are programmed into hardware. The next stage must validate the context and ring layouts against the controller's supported limits, program DCBAAP/CRCR/ERSTBA/ERDP in specification order, configure interrupters, and only then consider enabling bus mastering and starting the controller. These structures have not been build-tested or hardware-tested in this session.


## xHCI device-context and scratchpad memory stage

The DMA allocator now reads HCSParams2 to derive the controller's scratchpad count and HCCPARAMS1.CSZ to select 32-byte or 64-byte contexts. It allocates the 256-entry DCBAA, writes entry 0 to the optional scratchpad pointer array, and reserves DCBAA entry 1 for a first Device Context. It also allocates the corresponding 32-context Device Context and 33-context Input Context for slot 1. Scratchpad buffers are represented by one page-aligned contiguous region plus a pointer array containing each page's physical address.

All addresses remain below 4 GiB, allocations are zeroed and page-backed, and ring allocations retain 64 KiB boundary constraints. The implementation decodes the HCSParams2 scratchpad Hi/Lo fields in their specification-defined bit positions and refuses DMA setup unless the operational PAGESIZE register advertises 4 KiB support. This stage only prepares memory while the controller is halted. It does not write DCBAAP/CRCR/ERST registers, configure a slot, set CONFIG, enable PCI bus mastering, start Run/Stop, or issue commands. The slot-1 context is a reserved zeroed allocation, not an enabled USB slot. No build or hardware test has been run for this stage.


## xHCI halted DMA-register programming stage

After the controller has completed the bounded halt/reset sequence and DMA structures have been allocated, the kernel now revalidates the PCI function, BAR size, controller Halted/CNR state, command-ring-running bit, DMA address ranges and alignment. It then programs DCBAAP, CRCR (with RCS=1), interrupter-0 ERSTSZ, ERSTBA and ERDP. The 64-bit register pairs are written as ordered 32-bit high/low writes for the 32-bit kernel. Interrupter 0's IMAN and IMOD are initialized with delivery disabled.

This is intentionally a halted configuration stage: PCI bus mastering is explicitly kept disabled, USBCMD.RS and USBCMD.INTE are not set, CONFIG.MaxSlotsEn is not changed, no command is submitted, and no port/device enumeration occurs. The DMA allocations remain reserved after programming. A partial MMIO write reports failure and does not attempt to start the controller. No build or hardware test has been run.



## xHCI event decoding foundation

The DMA event-ring abstraction now exposes the physical dequeue pointer and a
cycle-checked `next_event` operation. It returns a copied record with the TRB
type, completion code, endpoint ID and slot ID decoded for Transfer,
Command Completion, Port Status Change, Host Controller and MFINDEX Wrap
events. Command Completion records expose the referenced command-TRB pointer
and a success predicate.

This is a parser/consumer primitive only. It does not acknowledge ERDP.EHB,
write ERDP, enable an interrupter, submit commands, correlate completions with
outstanding commands, or start the controller. The event ring is not yet
connected to a live hardware event loop. No build or hardware test has been run.


## xHCI command tracking foundation

Added a fixed-capacity `CommandTracker` for software-side Command TRB
submission bookkeeping. It records each enqueued TRB's physical address and
matches decoded Command Completion Events by the event's command pointer.
Unknown, duplicate, stale, and non-command events are rejected; the table is
bounded and does not allocate dynamically.

This is not yet a live command engine: no doorbell is rung, no command is
submitted to active hardware, no timeout polling is performed, and no
Enable Slot or Address Device command is issued. The controller remains
halted and PCI bus mastering remains disabled. Build and hardware tests remain
deferred.


## xHCI timeout and recovery bookkeeping

The command tracker now supports absolute PIT-tick deadlines through
`submit_with_timeout`, expiration scans, and timed-out command lookup.
Expired command records remain in the table: timeout never frees or reuses a
TRB while hardware ownership is uncertain. Duplicate outstanding TRB pointers
are rejected to prevent a wrapped producer ring from silently aliasing a
pending command.

A bounded software `RecoveryManager` models timeout detection, abort request,
abort-in-progress, reset-required, and quarantine states. It is deliberately
policy/bookkeeping only: it does not write Command Abort, reset registers,
enable bus mastering, or free DMA. DMA must remain pinned until controller
halt/reset is independently confirmed. No build or hardware test has run.


## xHCI status classification and Command Abort preflight

Added a read-only `ControllerHealth` decoder for USBSTS.HCH, HSE, EINT,
PCD, SRE, CNR and HCE. A conservative `recovery_action` maps fatal controller
errors to DMA quarantine/reset-required policy, CNR to wait, and halted state
to inspection. It never writes the W1C-sensitive USBSTS register.

Added `command_abort_preflight` to validate fresh controller status, CRCR.CRR,
and PCI bus-master state before a future abort implementation. It only returns
an eligibility result; it does not set CRCR.CA, ring doorbells, enable bus
mastering, or start the controller. Real Command Abort remains deferred until
the command/event execution path and interrupt/polling ownership are validated.


## xHCI command pipeline coordinator

Added the missing typed Event TRB decoder definitions (`EventType` and
`EventRecord`) and a software `CommandEngine` that coordinates command-ring
submission, deadline expiration, and one-event-at-a-time completion matching.
Non-command events are returned intact to the caller for routing; invalid or
stale command completions are surfaced as errors. This stage is deliberately
not a live hardware execution engine: no doorbell is rung, no PCI bus mastering
is enabled, no interrupter is enabled, and no command TRB is handed to an
active controller. Real command execution remains blocked pending a verified
polling/interrupt ownership design and hardware-safe bring-up sequence.


## xHCI Enable Slot / Address Device preparation

Added software builders for Enable Slot and Address Device command TRBs, a
bounded slot-state registry, and Input Context field preparation for the Slot
Context and default control endpoint (EP0). Address Device construction
validates the input-context alignment/range and nonzero slot ID. Input Context
preparation requires caller-supplied speed, root port, EP0 max packet size and
transfer-ring dequeue pointer; these values must be derived from actual port
status and device descriptors, not guessed.

This remains preparation-only: no command is submitted to active hardware,
no doorbell is rung, and no slot is allocated by the controller. Port-status
change handling, descriptor control transfers, real Enable Slot completion,
and Address Device execution are still pending. Build and hardware tests have
not been run.


## xHCI root-port status and USB descriptor parsing

Added a read-only PORTSC decoder and bounded root-port status accessor. PORTSC
change bits are exposed as status only; this code deliberately does not write
the W1C-sensitive register or initiate a port reset. Added bounds-checked USB
descriptor stream traversal and a parser for the standard 18-byte Device
Descriptor, including little-endian VID/PID and device metadata.

The next required hardware-facing work is a carefully sequenced port-reset
state machine and control-transfer execution for the initial 8-byte
GET_DESCRIPTOR request, followed by the full Device Descriptor request.
No USB control transfer or live enumeration is performed by this stage.


## xHCI port reset policy and control-transfer TRB preparation

Added a software-only port-reset state machine with connection/over-current
checks, a minimum reset interval (five nominal 100 Hz PIT ticks), a bounded
timeout, and a recovery interval. It consumes fresh PORTSC snapshots but does
not write PORTSC or assert reset itself.

Added USB Setup Packet encoding and a three-TRB builder for an IN
GET_DESCRIPTOR(Device) control transfer: Setup Stage, Data Stage, and Status
Stage. It validates descriptor length and the 32-bit DMA address range.
The builder does not enqueue the TRBs, ring a doorbell, or access a live
controller. The data buffer must be a valid DMA buffer whose lifetime is
maintained by the eventual transfer owner.

The next stage must implement verified transfer-ring ownership, event
completion correlation, and safe PORTSC write semantics before live
enumeration. Build and hardware tests remain deferred.


## xHCI Transfer Event correlation and conservative transfer submission

Added Transfer Event decoding, completion-code classification (including
Success, Short Packet, Stall and common controller/transfer errors), residual
length extraction, and correlation by event TRB pointer, Slot ID and Endpoint
ID. Added a bounded TransferTracker and a TransferEngine for the initial
GET_DESCRIPTOR(Device) control TD.

The transfer engine is intentionally one-shot: it refuses to wrap or reuse
transfer-ring entries because a safe hardware dequeue/reclamation protocol is
not yet implemented. It only builds and records the three-TRB TD; it does not
ring a doorbell, enable PCI bus mastering, or start the controller. Live event
delivery, ERDP acknowledgement, and transfer-ring reclamation remain pending.
Build and hardware tests have not been run.


## xHCI Event Ring dequeue and acknowledgement bookkeeping

Extended EventRing with a monotonic consumed-event counter, wrap-aware dequeue
pointer/cycle-state updates, bounded batch draining into caller-provided
storage, and an ERDP acknowledgement value that combines the current dequeue
pointer with EHB acknowledgement bit 3. Event consumption and hardware ERDP
writes remain deliberately separate: this stage does not write ERDP, enable
interrupters, ring doorbells, or start the controller. The caller must perform
the register write only after validating the runtime-register mapping and
controller state. Event Ring DMA remains owned by XhciDma for its full lifetime;
no release path is exposed while hardware ownership could be uncertain.

Build and hardware tests have not been run.


## xHCI software event routing and interrupter planning

Added `src/xhci_runtime.rs` with a bounded event pump that consumes up to
16 cycle-valid Event Ring entries and classifies them as Transfer, Command
Completion, Port Status Change, Host Controller, or other events. The returned
batch preserves the original event records so the existing command and
transfer correlation layers can process them without losing unrelated events.

The module also validates runtime interrupter register offsets against the
reported BAR size and exposes separate IMAN values for enabling IE,
acknowledging the W1C IP bit, and disabling delivery. These are plans only:
there are no MMIO writes, no interrupt routing changes, no PCI bus-master
enable, no doorbell writes, and no controller Run/Stop transition. The event
pump does not itself write ERDP; hardware acknowledgement remains a separate,
reviewed operation.

Build and hardware validation remain deferred. The Rust rewrite is still not
a boot-verified or production-ready operating system.


## xHCI Event Ring acknowledgement transactions and runtime lifecycle

Added opaque EventAckToken snapshots to EventRing. A token captures the Event
Ring identity, consumed-event sequence, dequeue pointer, and ERDP value. The
software commit method rejects foreign, stale, replayed, and future tokens;
the caller must first perform and verify the hardware ERDP write. A later
batch of consumed events remains pending when an earlier snapshot is committed.

Added a software RuntimeLifecycle model with explicit Halted,
DmaConfigured, InterrupterPrepared, Running, Recovering, and Faulted phases.
Transitions are evidence-recording APIs only; they do not write USBCMD, IMAN,
ERDP, PCI command registers, or any other MMIO. Returning to Halted requires
the caller to verify controller halt and DMA quiescence.

This stage does not activate the controller or enable interrupts. Correct
hardware register ordering, W1C semantics, DMA visibility, and controller
quiescence still require a dedicated hardware-reviewed layer. Build and
hardware tests remain deferred.


## xHCI bounded runtime register access

Added `XhciRuntimeRegisters` in `src/xhci_runtime.rs`, scoped to a validated
`InterrupterPlan` and an existing bounded `MmioRegion`.

- IMAN enable writes IE without echoing the W1C IP bit.
- IMAN disable clears IE without acknowledging IP.
- Pending acknowledgement reads the current IE state and writes only IE plus
  the IP W1C bit, avoiding a generic read-modify-write of the register.
- ERDP writes validate the opaque EventAckToken value and use high-dword then
  low-dword ordering for this 32-bit access path, with CPU/compiler fences.
- EventRing acknowledgement bookkeeping remains a separate commit step and
  must occur only after the ERDP write succeeds.

These accessors do not enable PCI bus mastering, start the controller, configure
MSI/MSI-X, or activate interrupt delivery. The caller remains responsible for
controller-state preconditions, interrupt synchronization, correct capability
offset discovery, and ensuring the MMIO mapping covers the full register set.
The code has been source-reviewed only; build and hardware validation remain
deferred. In particular, the 64-bit register access ordering and IMAN W1C
behavior must be verified against the target xHCI revision before activation.


## xHCI DMA ownership and recovery safety

The DMA allocator now tracks each allocation as CPU-owned, device-owned, or
quarantined. A DMA buffer can be released only while CPU-owned; attempting to
release a device-owned or quarantined buffer refuses to return its pages to the
physical allocator. Quarantine intentionally leaks the allocation rather than
risking reuse while a bus master may still access it.

- `DmaBuffer::handoff_to_device` publishes initialized memory and performs a
  one-way CPU-to-device ownership transition.
- `XhciDma::handoff_all_to_device` applies the transition to the complete
  DCBAA/ring/context/scratchpad allocation set. Any partial or inconsistent
  transition quarantines the entire set.
- `RecoveryEvidence` records independently verified recovery facts:
  Run/Stop cleared, controller halted, PCI bus mastering disabled, and DMA idle.
- Aggregate DMA reclamation is rejected until all four conditions are recorded.
  An inconsistent ownership state quarantines the full allocation set.
- Recovery evidence is software bookkeeping only. It does not read hardware,
  stop the controller, disable PCI bus mastering, or prove quiescence by itself.
  Callers must record each fact only after a real hardware read-back.

This stage does not activate xHCI or submit commands. The ownership API is a
conservative guardrail, not a substitute for device-specific synchronization,
interrupt exclusion, posted-write flushing, or hardware validation. Build and
hardware tests remain deferred.
