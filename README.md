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
- IRQ-safe fixed-capacity task registry with priority-based ready-task selection metadata; context switching and preemption remain unimplemented.
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
- xHCI validates its memory BAR, requests OS ownership, halts and resets the controller with bounded waits. A separate DMA-layout module allocates DCBAA, command/transfer producer rings, a single-segment event ring, ERST, slot-1 Device/Input Contexts, and optional scratchpad buffer arrays/storage using HCSParams2 and the context-size capability. The current setup programs selected DMA base/ring registers while the controller is halted and PCI bus mastering is disabled. It does not enable Run/Stop, ring doorbells, enumerate devices or handle ports.
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

## PS/2 mouse input and desktop pointer

Added `src/mouse.rs`, a bounded legacy 8042 auxiliary-port driver. Initialization disables the auxiliary port during setup, reads and updates the controller configuration byte, enables the auxiliary clock and IRQ12, sends the mouse defaults command, negotiates the IntelliMouse wheel protocol (and Explorer extension when available), and enables data reporting. Every controller wait has a finite iteration limit; a missing or unresponsive mouse reports an initialization error rather than hanging boot.

IRQ12 (PIC vector 44) consumes one data byte, feeds a packet decoder, and acknowledges the slave and master PICs. The decoder synchronizes on the PS/2 packet header, rejects X/Y overflow packets, sign-extends movement deltas, inverts the vertical axis for screen coordinates, clamps the pointer to the framebuffer, and exposes left/right/middle plus optional side buttons and wheel movement in an atomic snapshot. Keyboard polling ignores auxiliary-port bytes so it cannot steal mouse packets.

The desktop refresh loop draws a software arrow cursor using framebuffer save-under storage. The previous cursor footprint is restored before a new one is drawn, avoiding cursor trails and preserving the pixels underneath it. The PS/2 mouse is initialized during subsystem bring-up; IRQ12 is unmasked only after the device acknowledges enable-data-reporting. If initialization fails, IRQ12 remains masked. The desktop still has no full window hit-testing or click-action dispatcher, and USB HID mice are not yet supported because live xHCI enumeration/class drivers remain inactive.

This stage has source read-back only. Cargo build, QEMU boot and physical mouse validation have not been run; hardware behavior remains unverified.
 
## Staged USB configuration enumeration

Configuration Descriptor retrieval is split into two bounded control transfers. The first request fetches exactly the 9-byte descriptor header using **GET_DESCRIPTOR(Configuration)** (request 6, descriptor type 2); this is distinct from the USB **GET_CONFIGURATION** request (request 8). The header parser validates descriptor length/type and reads `wTotalLength`. The second request is limited to the validated total length and a 4096-byte software cap. The full descriptor parser then validates the returned descriptor chain before the manager exposes its configuration value.

After parsing, `TransferEngine::submit_set_configuration` builds the standard no-data SET_CONFIGURATION request. `XhciControlTransferDispatcher` also exposes guarded EP0 helpers for the header, bounded body, and SET_CONFIGURATION stages; these retain the runtime/DMA ownership checks and ring the endpoint doorbell only through the existing guarded path. Enumeration is marked complete only when the matching successful EP0 Transfer Result belongs to the expected Slot ID and Endpoint ID 1. Descriptor payloads are likewise checked against their slot/endpoint result before parsing.

Disconnect and enumeration failure no longer release a Slot immediately. The manager retains the Slot in `DisconnectPending` or `Quarantined`; a later independent DMA-quiescence proof is required before the Slot ID can be reused. A failed proof can be retried after the controller state has been independently revalidated.

## USB endpoint recovery and disconnect safety\n\n`src/usb_recovery.rs` adds a fixed-capacity software recovery coordinator. A correlated STALL result transitions an endpoint through Reset Endpoint, then Set TR Dequeue Pointer; each command requires its own successful completion before the next state is accepted. Endpoint identity is the controller-assigned Slot ID plus Endpoint ID. The module also decodes Port Status Change Event port IDs, but correctly requires a separate PortSC MMIO snapshot to determine current connection and speed.\n\nDisconnect handling is conservative: Disable Slot completion does not by itself authorize freeing DMA. The disconnect guard requires an independently verified DMA-quiescence fact before marking memory safe to reclaim; otherwise the slot remains quarantined. No commands are submitted, no doorbells are rung, and no DMA memory is released by this module.\n\n## Build prerequisites

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


## xHCI controller lifecycle and bounded reset model

`src/xhci_runtime.rs` now contains an explicit `ControllerLifecycle` software
state machine:

`Offline -> Discovered -> OwnershipGranted -> Halted -> Resetting -> Ready
-> DmaConfigured -> InterrupterPrepared -> Running`.

Recovery moves supported active phases to `Recovering`. It cannot return to
`Halted` through the ordinary halt transition: `complete_recovery` requires
`RecoveryEvidence` to prove Run/Stop cleared, controller halted, PCI bus
mastering disabled, DMA idle, and DMA reclamation completed. Uncertain memory
can be moved to `Quarantined`; fatal errors can be recorded as `Faulted`.

Reset attempts are bounded to two per lifecycle generation. Exceeding the
limit moves the lifecycle to `Faulted`. Successful evidence-gated recovery
advances the generation and resets the retry counter.

This is a software state model only. It does not perform PCI/MMIO reads,
controller resets, timeouts, interrupt masking, or DMA synchronization itself.
The caller must supply truthful hardware read-back evidence. The existing
controller bring-up path and hardware sequencing still require a separate
audit before enabling live USB operation. No build or hardware test was run;
those remain deferred as requested.


## xHCI health diagnostics and error policy

The controller health decoder interprets the read-only USBSTS snapshot and
prioritizes Host System Error, Host Controller Error and Save/Restore Error as
fatal. Controller Not Ready and an unexpected Halted state are classified as
recoverable; event-interrupt and port-change indications are informational.
The diagnostic policy itself does not clear USBSTS or PORTSC write-one-to-clear
bits.

`XhciDiagnosticLog` is a fixed-capacity, allocation-free 16-record history.
Each record stores a monotonically wrapping sequence, caller-supplied timer
tick, raw USBSTS value, primary diagnostic code and severity. When full, the
oldest entry is overwritten and a counter records the number of overwritten
records.

`ControllerLifecycle::observe_health` connects health classification to the
lifecycle model: fatal errors move it to `Quarantined`; recoverable errors move
active phases to `Recovering`, and recoverable errors outside an active phase
fail closed to `Faulted`. The boot diagnostic currently displays one
read-only discovery-time USBSTS snapshot. It does not yet poll continuously,
acknowledge status bits, run recovery, or perform live interrupt handling.

No build, QEMU, or physical hardware test was run for this stage; those remain
deferred until the planned implementation work is complete.


## USB device-enumeration foundation

`src/usb.rs` adds a bounded, allocation-free software model for USB device
enumeration:

- Parses the standard 18-byte Device Descriptor with length/type checks.
- Parses a complete Configuration Descriptor blob using `wTotalLength`,
  validating each descriptor boundary before reading fields.
- Records Interface and Endpoint descriptors in fixed-capacity arrays
  (16 interfaces and 32 endpoints per parsed configuration).
- Tracks up to 32 root ports and 32 software device/slot records.
- Models the ordered progression from connection and completed port reset
  through Enable Slot, Address Device, descriptor retrieval, Set Configuration,
  and the final Enumerated state.
- Rejects invalid transitions, duplicate slot assignments, invalid port/slot
  identifiers, truncated descriptors and capacity overflow.
- Releases software slot bookkeeping on disconnect or an explicitly failed
  enumeration.

This is an enumeration architecture and descriptor parser, not a live USB
driver. It does not issue Enable Slot, Address Device, control-transfer,
Set Configuration, PORTSC, or doorbell operations. No USB device is currently
enumerated by this module, and class drivers (HID, mass storage, hub) remain
unimplemented. The boot output reports framework capacity only.

No build, QEMU, or physical hardware test was run for this stage; those remain
deferred until the planned implementation work is complete.


## xHCI guarded command dispatch and doorbell stage

Added a bounded doorbell access layer to `src/xhci_runtime.rs`:

- Validates the capability-reported DBOFF, alignment, BAR extent and complete
  DB0-through-DBMaxSlots register window before any doorbell access.
- Provides separate command-ring DB0 and device-slot endpoint doorbell methods.
- Requires the software lifecycle to be in `Running` and the caller to confirm
  that the DMA allocation set is device-owned before writing a doorbell.
- Validates slot and endpoint identifiers and constructs endpoint target values
  from endpoint and stream IDs.
- Adds `XhciCommandDispatcher`, which submits a command to the tracked producer
  ring and then notifies DB0. It exposes timeout polling and command-completion
  event correlation, plus convenience methods for Enable Slot and Address
  Device commands.
- If a doorbell write fails after enqueue, the command remains tracked and its
  TRB must not be reused; recovery must retain DMA ownership until quiescence.

The boot path only reports that this guarded API is present. It does not create
an active doorbell mapping, transition the controller to Running, hand DMA to
the device, ring DB0, or submit Enable Slot/Address Device. Those operations
require a separately verified hardware bring-up sequence, interrupt/polling
ownership, register-order review and hardware validation. No build, QEMU, or
physical-device test was run for this stage.


## Generic USB control-transfer execution foundation

The xHCI DMA layer now supports generic USB control-transfer TD construction:

- `UsbSetupPacket` includes helpers for GET_DESCRIPTOR (including arbitrary
  descriptor type/index), GET_CONFIGURATION and SET_CONFIGURATION.
- `control_transfer_trbs` builds Setup/Data/Status Stage TRBs for IN and OUT
  requests, and correctly builds a two-TRB Setup/Status TD for requests with
  no data stage (`wLength = 0`).
- Data buffers are range-checked against the current below-4-GiB DMA limit.
  Direction bits and Setup Stage transfer-type fields are derived from the
  setup packet; Status Stage direction is the opposite of the data stage (IN
  for no-data requests).
- `TransferEngine::submit_control` tracks the Setup, optional Data and Status
  TRB pointers and appends only the required number of TRBs. The one-shot ring
  policy remains: entries are not wrapped or reused without a proven dequeue
  and reclamation protocol.
- `XhciControlTransferDispatcher` connects this engine to the guarded
  slot/endpoint doorbell path. It requires a Running lifecycle and an explicit
  caller confirmation that DMA is device-owned before submission and doorbell
  notification. Transfer Events are correlated through the existing tracker.

This adds the software path needed for descriptor reads and no-data requests
such as SET_CONFIGURATION. The later USB DMA payload section adds bounded
payload allocation and descriptor acceptance helpers, but there is still no
live controller event loop or automatic end-to-end enumeration. The boot
sequence does not instantiate this dispatcher, transition xHCI to Running,
ring an endpoint doorbell, or enumerate a connected device. Event-ring ERDP
acknowledgement and transfer-ring reclamation remain separate prerequisites.

No build, QEMU, or physical hardware test was run for this stage.


## USB DMA payload ownership and completion processing

Added an owned `UsbPayloadBuffer` abstraction for arbitrary USB control-transfer
payloads. Buffers are allocated below 4 GiB, bounds-checked, and writable only
while CPU-owned. The guarded dispatcher has a dedicated `submit_payload` path:
it transfers payload ownership before publishing the TD, and quarantines the
buffer if enqueue or doorbell notification fails after handoff.

Transfer tracking now retains the payload physical address and requested
length. A correlated Transfer Event produces a result containing residual and
actual lengths; residual values larger than the requested transfer are rejected
without releasing the pending record. Payload reclamation is a separate,
explicit unsafe operation and is permitted only after the caller has verified
that the result came from the matching TransferEngine completion. The payload
can then be copied into CPU memory, and released only while CPU-owned.

`UsbDeviceManager` now accepts a completed payload directly for Device
Descriptor parsing, and can copy a completed Configuration Descriptor payload
into caller-provided scratch storage before parsing. These helpers require a
successful/short-packet result and a payload already reclaimed to CPU ownership.

This stage still does not implement a live xHCI event loop, automatic payload
reclamation, event-ring ERDP acknowledgement in the polling path, transfer-ring
dequeue/reuse, or full end-to-end enumeration. The boot path does not instantiate
the payload manager or activate the controller. The caller must retain payloads
until their matching completion arrives; uncertain buffers are quarantined.

No build, QEMU, or physical hardware test was run for this stage.


### Bounded payload pool

The payload owner is also exposed as `UsbPayloadPool`, a fixed-capacity pool
of eight DMA buffers. It uses generation-checked opaque handles, rejects stale
handles after slot reuse, tracks in-use capacity, and refuses to release any
buffer that is not CPU-owned. The control-transfer dispatcher accepts a pool
handle directly through `submit_pool_payload`; descriptor handoff can resolve
the same handle through the pool before copying the completed bytes.

This remains a software ownership and submission framework. It is not yet a
live USB DMA service and must not be treated as proof that a connected device
has been enumerated.


## Per-slot USB Endpoint 0 transfer rings

Added a bounded `Ep0TransferRings` owner with a separate DMA Transfer Ring for
each registered xHCI Slot (up to 32 software-managed slots). Registration
requires a nonzero, unique Slot ID and allocates a below-4-GiB ring constrained
to a 64 KiB boundary. Each ring exposes its initial dequeue pointer and cycle
state for the matching Address Device Input Context. The DCS value is encoded
in bit 0 of the low dequeue-pointer dword, as required by the endpoint context
layout.

The Address Device context helper now resolves the EP0 ring by the same Slot ID,
rather than accepting an unrelated ring pointer from its caller. The guarded
control-transfer dispatcher also has EP0-specific submission methods that bind
the selected Slot to endpoint target ID 1 and stream ID 0.

Rings are deliberately not removed or recycled. A future implementation must
prove controller halt/quiescence and endpoint dequeue safety before releasing
or reusing ring memory. This stage does not instantiate the ring manager in the
boot path, issue Address Device, submit USB transfers, ring doorbells, or enable
the controller. Event delivery, Transfer Event correlation across all event
consumers, hardware ERDP acknowledgement, and endpoint recovery remain
separate work.

No Cargo build, QEMU boot, or physical USB hardware test was run; validation
was limited to GitHub source read-back after each edit, as requested.


## xHCI event runtime dispatch and ERDP acknowledgement

The bounded EventPump can now drain a cycle-valid batch once and route the
copied records to both command and transfer engines without either engine
consuming another Event Ring entry. `CommandEngine::process_event` and
`TransferEngine::process_event` preserve their existing correlation checks;
unrelated events remain available to port-status and controller-health
consumers. The dispatch result retains successful completions, correlation
errors, and unhandled event records in fixed-capacity storage.

`EventPump::poll_and_ack` connects the drain path to the selected runtime
interrupter's ERDP register accessor. It snapshots the acknowledgement token,
writes ERDP, and commits EventRing acknowledgement bookkeeping only after the
MMIO accessor reports success. If the write fails, the copied batch is still
returned to the caller and the acknowledgement remains pending for retry.
A rejected software commit is reported separately. The caller must serialize
event-ring consumption and acknowledgement; this API does not establish
interrupt exclusion or hardware synchronization.

The runtime API is not instantiated in the boot path. No controller Run/Stop,
PCI bus mastering, interrupt delivery, doorbell, or USB transfer was activated.
Port Status Change and Host Controller events are retained, not yet connected
to automatic port-reset or controller-recovery actions. Endpoint Stall still
requires a future Reset Endpoint command/recovery path.

No Cargo build, QEMU boot, or physical hardware test was run; development
testing remains deferred as requested.


## Kernel synchronization primitives

Added `src/sync.rs` with two allocation-free synchronization types:

- `SpinLock<T>`: acquire/release atomic spin lock with blocking lock and
  non-blocking `try_lock` acquisition.
- `IrqSpinLock<T>`: saves the current IF state, disables local maskable
  interrupts before acquiring the atomic lock, and restores the prior IF state
  when its guard is dropped. This prevents same-CPU maskable interrupt
  re-entry from deadlocking on a lock held by interrupted code; the atomic
  operation also serializes access between CPUs.

The fixed-capacity task registry now uses `IrqSpinLock` for registration,
state changes, ready-task selection, snapshots and task counting. Task IDs
remain nonzero and callers cannot transition a task back to the Empty state
through the public state API.

These primitives do not yet provide a scheduler, context switching, SMP
startup, NMI synchronization, or protection against callers holding locks
across blocking operations. No build, QEMU boot, or hardware test was run;
validation for this stage is GitHub source read-back only.


## Physical and virtual memory management

The physical frame allocator retains its fixed 4 KiB bitmap model for the
32-bit physical address space and now exposes additional ownership operations:

- Runtime `reserve_region` rounds a physical interval outward and refuses to
  reserve frames already owned by an allocation. This is used to keep the
  Multiboot framebuffer out of the general frame pool.
- `is_frame_allocated`, `is_ready`, and allocator statistics expose bounded
  state queries.
- `alloc_zeroed_pages` and `alloc_zeroed_frame` provide cleared physical
  pages after paging is active. Size overflow is rejected before allocation.
- Existing aligned and boundary-constrained contiguous allocations remain the
  basis for DMA buffers.

The paging layer retains the initial supervisor-only 4 GiB identity map using
4 MiB PSE entries, and can split an individual large-page PDE into a 4 KiB page
table while preserving the unaffected mappings. It now provides:

- 4 KiB `map_page`, explicit `remap_page`, and `unmap_page` operations.
- Contiguous `map_range` with rollback of mappings on failure, plus validated
  `unmap_range`.
- Virtual-to-physical `translate` for both 4 MiB and 4 KiB mappings.
- MMIO cache-disable marking for either large pages or 4 KiB PTEs.
- IRQ-safe serialization of page-table mutations and local TLB invalidation.

Important boundaries: the kernel still uses one shared page directory; there
are no per-process address spaces, CR3 context switching, demand paging,
copy-on-write, swap, kernel/user isolation, or page-fault-based lazy allocation.
The initial identity map remains writable and supervisor-only. This is a
managed 4 KiB mapping foundation, not complete virtual-memory isolation.

No Cargo build, QEMU boot, or physical-memory stress test was run. Source
read-back from the Rust branch is the only validation performed in this stage.
