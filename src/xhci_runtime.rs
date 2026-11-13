//! xHCI runtime event routing and interrupter register planning.
//!
//! This module is deliberately software-only. It does not write MMIO,
//! enable PCI bus mastering, enable interrupters, ring doorbells, or start
//! the host controller. Hardware activation requires a separately reviewed
//! sequencing layer and hardware validation.

use crate::xhci_dma::{EventAckToken, EventRecord, EventRing, EventType};

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
