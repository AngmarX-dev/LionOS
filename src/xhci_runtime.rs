//! xHCI runtime event routing and interrupter register planning.
//!
//! This module is deliberately software-only. It does not write MMIO,
//! enable PCI bus mastering, enable interrupters, ring doorbells, or start
//! the host controller. Hardware activation requires a separately reviewed
//! sequencing layer and hardware validation.

use crate::xhci_dma::{EventAckToken, EventRecord, EventRing, EventType};
use crate::mmio::MmioRegion;
use crate::dma::DmaBuffer;
use crate::xhci_dma::XhciDma;
use core::sync::atomic::{compiler_fence, fence, Ordering};

const IMAN_IE: u32 = 1 << 1;
const IMAN_IP: u32 = 1 << 0;
const INTERRUPTER_STRIDE: usize = 0x20;
const INTERRUPTER_BASE: usize = 0x20;
const INTERRUPTER_REGISTER_BYTES: usize = 0x20;
const EVENT_BATCH_CAPACITY: usize = 16;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InterrupterPlanError {
    IndexOutOfRange,
    OffsetOverflow,
    OutsideBar,
}

/// Validated offsets for one xHCI runtime interrupter register set.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct InterrupterPlan {
    pub index: u16,
    pub iman: usize,
    pub imod: usize,
    pub erstsz: usize,
    pub erstba: usize,
    pub erdp: usize,
}

impl InterrupterPlan {
    /// Build a register plan from the capability-reported runtime offset.
    /// This only validates offsets; it performs no MMIO access.
    pub fn new(
        runtime_offset: u32,
        index: u16,
        max_interrupters: u16,
        bar_size: u64,
    ) -> Result<Self, InterrupterPlanError> {
        if index >= max_interrupters {
            return Err(InterrupterPlanError::IndexOutOfRange);
        }
        let base = (runtime_offset as usize)
            .checked_add(INTERRUPTER_BASE)
            .and_then(|v| v.checked_add((index as usize).checked_mul(INTERRUPTER_STRIDE)?))
            .ok_or(InterrupterPlanError::OffsetOverflow)?;
        let end = base
            .checked_add(INTERRUPTER_REGISTER_BYTES)
            .ok_or(InterrupterPlanError::OffsetOverflow)?;
        if end as u64 > bar_size {
            return Err(InterrupterPlanError::OutsideBar);
        }
        Ok(Self {
            index,
            iman: base,
            imod: base + 0x04,
            erstsz: base + 0x08,
            erstba: base + 0x10,
            erdp: base + 0x18,
        })
    }

    /// IMAN.IE is a normal RW bit. IP is W1C and is intentionally not echoed.
    pub const fn enable_value(&self) -> u32 {
        IMAN_IE
    }

    /// Write-one-to-clear acknowledgement value for IMAN.IP only.
    /// This does not enable interrupt delivery.
    pub const fn acknowledge_pending_value(&self) -> u32 {
        IMAN_IP
    }

    /// Value to mask interrupt delivery without acknowledging IP.
    pub const fn disable_value(&self) -> u32 {
        0
    }
}

#[derive(Clone, Copy)]
pub enum RoutedEvent {
    Transfer(EventRecord),
    CommandCompletion(EventRecord),
    PortStatusChange(EventRecord),
    HostController(EventRecord),
    Other(EventRecord),
}

impl RoutedEvent {
    pub const fn from_record(record: EventRecord) -> Self {
        match record.event_type {
            EventType::Transfer => Self::Transfer(record),
            EventType::CommandCompletion => Self::CommandCompletion(record),
            EventType::PortStatusChange => Self::PortStatusChange(record),
            EventType::HostController => Self::HostController(record),
            _ => Self::Other(record),
        }
    }
}

#[derive(Clone, Copy)]
pub struct EventBatch {
    entries: [Option<RoutedEvent>; EVENT_BATCH_CAPACITY],
    len: usize,
}

impl EventBatch {
    pub const fn new() -> Self {
        Self {
            entries: [None; EVENT_BATCH_CAPACITY],
            len: 0,
        }
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn get(&self, index: usize) -> Option<RoutedEvent> {
        if index >= self.len {
            None
        } else {
            self.entries[index]
        }
    }

    fn push(&mut self, event: RoutedEvent) -> bool {
        if self.len == EVENT_BATCH_CAPACITY {
            return false;
        }
        self.entries[self.len] = Some(event);
        self.len += 1;
        true
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EventPumpReport {
    pub consumed: usize,
    pub remaining_capacity: usize,
    pub event_ring_consumed_total: u64,
}

/// Bounded software event pump. It consumes only cycle-valid Event Ring entries
/// and returns typed records; it does not acknowledge ERDP or touch MMIO.
pub struct EventPump;

impl EventPump {
    pub const fn new() -> Self {
        Self
    }

    pub fn poll(&mut self, ring: &mut EventRing) -> (EventBatch, EventPumpReport) {
        let mut batch = EventBatch::new();
        let mut raw = [None; EVENT_BATCH_CAPACITY];
        let count = ring.drain(EVENT_BATCH_CAPACITY, &mut raw);
        for record in raw[..count].iter().flatten() {
            let _ = batch.push(RoutedEvent::from_record(*record));
        }
        (
            batch,
            EventPumpReport {
                consumed: count,
                remaining_capacity: EVENT_BATCH_CAPACITY - count,
                event_ring_consumed_total: ring.consumed_count(),
            },
        )
    }
}


/// Software model of controller runtime activation. Transitions represent
/// externally verified facts; this type performs no hardware operations.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RuntimePhase {
    Halted,
    DmaConfigured,
    InterrupterPrepared,
    Running,
    Recovering,
    Faulted,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RuntimeTransitionError {
    InvalidTransition,
    PendingEvents,
}

pub struct RuntimeLifecycle {
    phase: RuntimePhase,
    generation: u32,
}

impl RuntimeLifecycle {
    pub const fn new_halted() -> Self {
        Self { phase: RuntimePhase::Halted, generation: 0 }
    }

    pub fn phase(&self) -> RuntimePhase { self.phase }
    pub fn generation(&self) -> u32 { self.generation }

    /// Call only after the caller has independently verified DMA register setup.
    pub fn note_dma_configured(&mut self) -> Result<(), RuntimeTransitionError> {
        if self.phase != RuntimePhase::Halted {
            return Err(RuntimeTransitionError::InvalidTransition);
        }
        self.phase = RuntimePhase::DmaConfigured;
        Ok(())
    }

    /// Call only after the caller has independently verified ERST/ERDP and
    /// interrupter register programming while the controller remains halted.
    pub fn note_interrupter_prepared(&mut self) -> Result<(), RuntimeTransitionError> {
        if self.phase != RuntimePhase::DmaConfigured {
            return Err(RuntimeTransitionError::InvalidTransition);
        }
        self.phase = RuntimePhase::InterrupterPrepared;
        Ok(())
    }

    /// Records a verified external Run/Stop transition. Does not set USBCMD.RS.
    pub fn note_running(&mut self) -> Result<(), RuntimeTransitionError> {
        if self.phase != RuntimePhase::InterrupterPrepared {
            return Err(RuntimeTransitionError::InvalidTransition);
        }
        self.phase = RuntimePhase::Running;
        Ok(())
    }

    pub fn note_recovery(&mut self) -> Result<(), RuntimeTransitionError> {
        if self.phase != RuntimePhase::Running {
            return Err(RuntimeTransitionError::InvalidTransition);
        }
        self.phase = RuntimePhase::Recovering;
        Ok(())
    }

    /// Return to Halted only after hardware halt and DMA quiescence are verified.
    pub fn note_quiesced(&mut self) -> Result<(), RuntimeTransitionError> {
        if self.phase != RuntimePhase::Recovering && self.phase != RuntimePhase::Faulted {
            return Err(RuntimeTransitionError::InvalidTransition);
        }
        self.phase = RuntimePhase::Halted;
        self.generation = self.generation.wrapping_add(1);
        Ok(())
    }

    pub fn note_fault(&mut self) {
        self.phase = RuntimePhase::Faulted;
    }
}

/// Snapshot an Event Ring acknowledgement transaction. The caller must
/// perform the correctly ordered ERDP MMIO write, then commit the token.
pub fn prepare_event_acknowledgement(ring: &EventRing) -> Option<EventAckToken> {
    ring.prepare_acknowledgement()
}

/// Commit bookkeeping only after a successful ERDP write was confirmed.
pub fn commit_event_acknowledgement(
    ring: &mut EventRing,
    token: EventAckToken,
) -> bool {
    ring.commit_acknowledgement(token)
}


#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RuntimeRegisterError {
    InvalidErdpValue,
    MmioWriteFailed,
    MmioReadFailed,
}

/// Narrow xHCI runtime-register accessors. The caller must ensure that the
/// controller is in the required state and that the supplied MMIO mapping
/// covers the capability-reported runtime register window.
pub struct XhciRuntimeRegisters<'a> {
    mmio: &'a MmioRegion,
    plan: InterrupterPlan,
}

impl<'a> XhciRuntimeRegisters<'a> {
    pub const fn new(mmio: &'a MmioRegion, plan: InterrupterPlan) -> Self {
        Self { mmio, plan }
    }

    pub fn read_iman(&self) -> Result<u32, RuntimeRegisterError> {
        self.mmio.read32(self.plan.iman).ok_or(RuntimeRegisterError::MmioReadFailed)
    }

    /// Set IE without echoing the W1C IP bit.
    pub fn enable_interrupt(&self) -> Result<(), RuntimeRegisterError> {
        if self.mmio.write32(self.plan.iman, IMAN_IE) {
            Ok(())
        } else {
            Err(RuntimeRegisterError::MmioWriteFailed)
        }
    }

    /// Clear IE without acknowledging a pending IP indication.
    pub fn disable_interrupt(&self) -> Result<(), RuntimeRegisterError> {
        if self.mmio.write32(self.plan.iman, 0) {
            Ok(())
        } else {
            Err(RuntimeRegisterError::MmioWriteFailed)
        }
    }

    /// Acknowledge IP while preserving the currently observed IE state.
    /// IMAN.IP is W1C; no unrelated bits are echoed to the register.
    pub fn acknowledge_pending(&self) -> Result<(), RuntimeRegisterError> {
        let current = self.read_iman()?;
        let ie = current & IMAN_IE;
        if self.mmio.write32(self.plan.iman, ie | IMAN_IP) {
            Ok(())
        } else {
            Err(RuntimeRegisterError::MmioWriteFailed)
        }
    }

    /// Write a validated ERDP snapshot. On this 32-bit kernel the high
    /// dword is written before the low dword, which commits the pointer update.
    /// The EventRing token must be committed separately only after this succeeds.
    pub fn write_erdp(&self, token: &EventAckToken) -> Result<(), RuntimeRegisterError> {
        let value = token.value();
        let pointer = value & !0xF;
        if pointer == 0 || pointer & 0xF != 0 || value & 0x7 != 0 || value & (1 << 3) == 0 {
            return Err(RuntimeRegisterError::InvalidErdpValue);
        }
        fence(Ordering::SeqCst);
        compiler_fence(Ordering::SeqCst);
        if !self.mmio.write32(self.plan.erdp + 4, (value >> 32) as u32) {
            return Err(RuntimeRegisterError::MmioWriteFailed);
        }
        fence(Ordering::SeqCst);
        compiler_fence(Ordering::SeqCst);
        if !self.mmio.write32(self.plan.erdp, value as u32) {
            return Err(RuntimeRegisterError::MmioWriteFailed);
        }
        compiler_fence(Ordering::SeqCst);
        fence(Ordering::SeqCst);
        Ok(())
    }
}


#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RecoveryError {
    InvalidState,
    NotQuiesced,
}

/// Software evidence gate for DMA reclamation. Each evidence method must be
/// called only after the corresponding hardware condition was independently
/// read back and verified by the controller-management layer.
pub struct RecoveryEvidence {
    active: bool,
    run_stop_cleared: bool,
    controller_halted: bool,
    bus_master_disabled: bool,
    dma_idle: bool,
    dma_reclaimed: bool,
    generation: u32,
}

impl RecoveryEvidence {
    pub const fn new() -> Self {
        Self {
            active: false,
            run_stop_cleared: false,
            controller_halted: false,
            bus_master_disabled: false,
            dma_idle: false,
            dma_reclaimed: false,
            generation: 0,
        }
    }

    pub fn begin(&mut self) -> Result<(), RecoveryError> {
        if self.active {
            return Err(RecoveryError::InvalidState);
        }
        self.active = true;
        self.run_stop_cleared = false;
        self.controller_halted = false;
        self.bus_master_disabled = false;
        self.dma_idle = false;
        self.dma_reclaimed = false;
        Ok(())
    }

    pub fn note_run_stop_cleared(&mut self) -> Result<(), RecoveryError> {
        if !self.active { return Err(RecoveryError::InvalidState); }
        self.run_stop_cleared = true;
        Ok(())
    }

    pub fn note_controller_halted(&mut self) -> Result<(), RecoveryError> {
        if !self.active || !self.run_stop_cleared {
            return Err(RecoveryError::InvalidState);
        }
        self.controller_halted = true;
        Ok(())
    }

    pub fn note_bus_master_disabled(&mut self) -> Result<(), RecoveryError> {
        if !self.active { return Err(RecoveryError::InvalidState); }
        self.bus_master_disabled = true;
        Ok(())
    }

    pub fn note_dma_idle(&mut self) -> Result<(), RecoveryError> {
        if !self.active || !self.controller_halted || !self.bus_master_disabled {
            return Err(RecoveryError::InvalidState);
        }
        self.dma_idle = true;
        Ok(())
    }

    pub fn safe_to_reclaim(&self) -> bool {
        self.active && self.run_stop_cleared && self.controller_halted
            && self.bus_master_disabled && self.dma_idle
    }

    /// This is the only runtime helper that returns a device-owned DMA buffer
    /// to CPU ownership. Safety depends on truthful, hardware-verified evidence.
    pub unsafe fn reclaim_buffer(&self, buffer: &DmaBuffer) -> Result<(), RecoveryError> {
        if !self.safe_to_reclaim() {
            return Err(RecoveryError::NotQuiesced);
        }
        if buffer.reclaim_after_quiescence() {
            Ok(())
        } else {
            Err(RecoveryError::InvalidState)
        }
    }

    /// Preserve memory permanently when controller activity cannot be ruled out.
    pub fn quarantine_buffer(&self, buffer: &DmaBuffer) {
        buffer.quarantine();
    }

    pub fn finish(&mut self) -> Result<(), RecoveryError> {
        if !self.safe_to_reclaim() || !self.dma_reclaimed {
            return Err(RecoveryError::NotQuiesced);
        }
        self.active = false;
        self.generation = self.generation.wrapping_add(1);
        Ok(())
    }

    pub fn generation(&self) -> u32 { self.generation }
}

impl RecoveryEvidence {
    /// Reclaim the complete xHCI DMA set only after all quiescence evidence
    /// has been recorded. Any inconsistent ownership state quarantines all buffers.
    pub unsafe fn reclaim_xhci_dma(&mut self, dma: &XhciDma) -> Result<(), RecoveryError> {
        if !self.safe_to_reclaim() {
            dma.quarantine_all();
            return Err(RecoveryError::NotQuiesced);
        }
        if dma.reclaim_all_after_quiescence() {
            self.dma_reclaimed = true;
            Ok(())
        } else {
            dma.quarantine_all();
            Err(RecoveryError::InvalidState)
        }
    }
}


/// Explicit controller lifecycle model. Every transition records an externally
/// verified hardware fact; this state machine itself performs no register I/O.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ControllerPhase {
    Offline,
    Discovered,
    OwnershipGranted,
    Halted,
    Resetting,
    Ready,
    DmaConfigured,
    InterrupterPrepared,
    Running,
    Recovering,
    Quarantined,
    Faulted,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LifecycleError {
    InvalidTransition,
    ResetLimitReached,
    RecoveryIncomplete,
}

const MAX_RESET_ATTEMPTS: u8 = 2;

pub struct ControllerLifecycle {
    phase: ControllerPhase,
    reset_attempts: u8,
    generation: u32,
}

impl ControllerLifecycle {
    pub const fn new() -> Self {
        Self { phase: ControllerPhase::Offline, reset_attempts: 0, generation: 0 }
    }

    pub fn phase(&self) -> ControllerPhase { self.phase }
    pub fn reset_attempts(&self) -> u8 { self.reset_attempts }
    pub fn generation(&self) -> u32 { self.generation }

    pub fn note_discovered(&mut self) -> Result<(), LifecycleError> {
        if self.phase != ControllerPhase::Offline {
            return Err(LifecycleError::InvalidTransition);
        }
        self.phase = ControllerPhase::Discovered;
        Ok(())
    }

    pub fn note_ownership_granted(&mut self) -> Result<(), LifecycleError> {
        if self.phase != ControllerPhase::Discovered {
            return Err(LifecycleError::InvalidTransition);
        }
        self.phase = ControllerPhase::OwnershipGranted;
        Ok(())
    }

    pub fn note_halted(&mut self) -> Result<(), LifecycleError> {
        if self.phase != ControllerPhase::OwnershipGranted
            && self.phase != ControllerPhase::Resetting
        {
            return Err(LifecycleError::InvalidTransition);
        }
        self.phase = ControllerPhase::Halted;
        Ok(())
    }

    pub fn begin_reset(&mut self) -> Result<(), LifecycleError> {
        if self.phase != ControllerPhase::Halted
            && self.phase != ControllerPhase::Faulted
        {
            return Err(LifecycleError::InvalidTransition);
        }
        if self.reset_attempts >= MAX_RESET_ATTEMPTS {
            self.phase = ControllerPhase::Faulted;
            return Err(LifecycleError::ResetLimitReached);
        }
        self.reset_attempts += 1;
        self.phase = ControllerPhase::Resetting;
        Ok(())
    }

    pub fn note_controller_ready(&mut self) -> Result<(), LifecycleError> {
        if self.phase != ControllerPhase::Resetting
            && self.phase != ControllerPhase::Halted
        {
            return Err(LifecycleError::InvalidTransition);
        }
        self.phase = ControllerPhase::Ready;
        Ok(())
    }

    pub fn note_dma_configured(&mut self) -> Result<(), LifecycleError> {
        if self.phase != ControllerPhase::Ready {
            return Err(LifecycleError::InvalidTransition);
        }
        self.phase = ControllerPhase::DmaConfigured;
        Ok(())
    }

    pub fn note_interrupter_prepared(&mut self) -> Result<(), LifecycleError> {
        if self.phase != ControllerPhase::DmaConfigured {
            return Err(LifecycleError::InvalidTransition);
        }
        self.phase = ControllerPhase::InterrupterPrepared;
        Ok(())
    }

    pub fn note_running(&mut self) -> Result<(), LifecycleError> {
        if self.phase != ControllerPhase::InterrupterPrepared {
            return Err(LifecycleError::InvalidTransition);
        }
        self.phase = ControllerPhase::Running;
        Ok(())
    }

    pub fn begin_recovery(&mut self) -> Result<(), LifecycleError> {
        if self.phase != ControllerPhase::Running
            && self.phase != ControllerPhase::DmaConfigured
            && self.phase != ControllerPhase::InterrupterPrepared
        {
            return Err(LifecycleError::InvalidTransition);
        }
        self.phase = ControllerPhase::Recovering;
        Ok(())
    }

    pub fn quarantine(&mut self) {
        self.phase = ControllerPhase::Quarantined;
    }

    pub fn fault(&mut self) {
        self.phase = ControllerPhase::Faulted;
    }

    /// Recovery is complete only when hardware quiescence and DMA reclamation
    /// have both been independently verified by RecoveryEvidence.
    pub fn complete_recovery(
        &mut self,
        evidence: &RecoveryEvidence,
    ) -> Result<(), LifecycleError> {
        if self.phase != ControllerPhase::Recovering {
            return Err(LifecycleError::InvalidTransition);
        }
        if !evidence.safe_to_reclaim() || !evidence.dma_reclaimed {
            return Err(LifecycleError::RecoveryIncomplete);
        }
        self.phase = ControllerPhase::Halted;
        self.generation = self.generation.wrapping_add(1);
        self.reset_attempts = 0;
        Ok(())
    }
}


/// Severity assigned to a read-only controller health snapshot.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DiagnosticSeverity {
    Informational,
    Recoverable,
    Fatal,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DiagnosticCode {
    HostSystemError,
    HostControllerError,
    SaveRestoreError,
    ControllerNotReady,
    UnexpectedHalt,
    EventInterruptPending,
    PortChangePending,
    Healthy,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ControllerDiagnostic {
    pub sequence: u32,
    pub tick: u32,
    pub raw_status: u32,
    pub code: DiagnosticCode,
    pub severity: DiagnosticSeverity,
}

const DIAGNOSTIC_CAPACITY: usize = 16;

/// Fixed-size, allocation-free diagnostic history. Oldest entries are
/// overwritten when full; the dropped counter makes loss explicit.
pub struct XhciDiagnosticLog {
    entries: [Option<ControllerDiagnostic>; DIAGNOSTIC_CAPACITY],
    next: usize,
    len: usize,
    sequence: u32,
    dropped: u32,
}

impl XhciDiagnosticLog {
    pub const fn new() -> Self {
        Self {
            entries: [None; DIAGNOSTIC_CAPACITY],
            next: 0,
            len: 0,
            sequence: 0,
            dropped: 0,
        }
    }

    pub fn len(&self) -> usize { self.len }
    pub fn is_empty(&self) -> bool { self.len == 0 }
    pub fn dropped_count(&self) -> u32 { self.dropped }

    /// Return the newest record at age zero, then progressively older records.
    pub fn newest(&self, age: usize) -> Option<ControllerDiagnostic> {
        if age >= self.len { return None; }
        let index = (self.next + DIAGNOSTIC_CAPACITY - 1 - age) % DIAGNOSTIC_CAPACITY;
        self.entries[index]
    }

    pub fn record(
        &mut self,
        tick: u32,
        raw_status: u32,
        code: DiagnosticCode,
        severity: DiagnosticSeverity,
    ) -> ControllerDiagnostic {
        if self.len == DIAGNOSTIC_CAPACITY {
            self.dropped = self.dropped.saturating_add(1);
        } else {
            self.len += 1;
        }
        self.sequence = self.sequence.wrapping_add(1);
        let entry = ControllerDiagnostic {
            sequence: self.sequence,
            tick,
            raw_status,
            code,
            severity,
        };
        self.entries[self.next] = Some(entry);
        self.next = (self.next + 1) % DIAGNOSTIC_CAPACITY;
        entry
    }
}

/// Select one primary diagnostic reason from a USBSTS snapshot. Fatal
/// conditions take precedence over readiness and informational change bits.
pub const fn diagnose_controller_health(
    health: crate::xhci::ControllerHealth,
) -> (DiagnosticCode, DiagnosticSeverity) {
    if health.host_system_error {
        (DiagnosticCode::HostSystemError, DiagnosticSeverity::Fatal)
    } else if health.host_controller_error {
        (DiagnosticCode::HostControllerError, DiagnosticSeverity::Fatal)
    } else if health.save_restore_error {
        (DiagnosticCode::SaveRestoreError, DiagnosticSeverity::Fatal)
    } else if health.controller_not_ready {
        (DiagnosticCode::ControllerNotReady, DiagnosticSeverity::Recoverable)
    } else if health.halted {
        (DiagnosticCode::UnexpectedHalt, DiagnosticSeverity::Recoverable)
    } else if health.event_interrupt {
        (DiagnosticCode::EventInterruptPending, DiagnosticSeverity::Informational)
    } else if health.port_change_detect {
        (DiagnosticCode::PortChangePending, DiagnosticSeverity::Informational)
    } else {
        (DiagnosticCode::Healthy, DiagnosticSeverity::Informational)
    }
}

impl ControllerLifecycle {
    /// Apply a decoded status snapshot to the software lifecycle model.
    /// A fatal USBSTS condition quarantines DMA ownership immediately.
    pub fn observe_health(
        &mut self,
        health: crate::xhci::ControllerHealth,
        log: &mut XhciDiagnosticLog,
        tick: u32,
    ) -> ControllerDiagnostic {
        let (code, severity) = diagnose_controller_health(health);
        let record = log.record(tick, health.raw, code, severity);
        match severity {
            DiagnosticSeverity::Fatal => self.quarantine(),
            DiagnosticSeverity::Recoverable => {
                if self.phase == ControllerPhase::Running
                    || self.phase == ControllerPhase::DmaConfigured
                    || self.phase == ControllerPhase::InterrupterPrepared
                {
                    let _ = self.begin_recovery();
                } else if self.phase != ControllerPhase::Recovering
                    && self.phase != ControllerPhase::Quarantined
                {
                    self.fault();
                }
            }
            DiagnosticSeverity::Informational => {}
        }
        record
    }
}
