//! Software-only USB endpoint recovery and root-port event coordination.
//! This module builds commands and tracks verified completions; it never rings
//! a doorbell, performs MMIO, or releases DMA memory.

use crate::xhci_dma::{EventRecord, EventType, TransferCompletion, TransferResult, Trb};

const TRB_TYPE_RESET_ENDPOINT: u32 = 14;
const TRB_TYPE_SET_TR_DEQUEUE_POINTER: u32 = 16;
const TRB_TYPE_SHIFT: u32 = 10;
const DMA_LIMIT: u64 = 0x1_0000_0000;
pub const MAX_TRACKED_ENDPOINTS: usize = 64;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RecoveryError {
    InvalidSlot,
    InvalidEndpoint,
    InvalidPointer,
    CapacityReached,
    UnknownEndpoint,
    InvalidTransition,
    CommandFailed,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EndpointRecoveryPhase {
    Idle,
    StallObserved,
    ResetPending,
    ResetComplete,
    DequeueUpdatePending,
    Ready,
    Failed,
}

#[derive(Clone, Copy)]
struct EndpointEntry {
    slot_id: u8,
    endpoint_id: u8,
    phase: EndpointRecoveryPhase,
    dequeue_pointer: u64,
}

impl EndpointEntry {
    const EMPTY: Self = Self {
        slot_id: 0,
        endpoint_id: 0,
        phase: EndpointRecoveryPhase::Idle,
        dequeue_pointer: 0,
    };
}

/// Fixed-capacity recovery state keyed by the controller-assigned Slot ID and
/// Endpoint ID. A STALL never releases a transfer ring or its DMA buffers.
pub struct EndpointRecoveryManager {
    entries: [EndpointEntry; MAX_TRACKED_ENDPOINTS],
    len: usize,
}

impl EndpointRecoveryManager {
    pub const fn new() -> Self {
        Self { entries: [EndpointEntry::EMPTY; MAX_TRACKED_ENDPOINTS], len: 0 }
    }

    fn find(&self, slot_id: u8, endpoint_id: u8) -> Option<usize> {
        self.entries[..self.len].iter().position(|entry|
            entry.slot_id == slot_id && entry.endpoint_id == endpoint_id)
    }

    fn ensure(&mut self, slot_id: u8, endpoint_id: u8) -> Result<usize, RecoveryError> {
        if slot_id == 0 { return Err(RecoveryError::InvalidSlot); }
        if endpoint_id == 0 || endpoint_id > 31 { return Err(RecoveryError::InvalidEndpoint); }
        if let Some(index) = self.find(slot_id, endpoint_id) { return Ok(index); }
        if self.len == MAX_TRACKED_ENDPOINTS { return Err(RecoveryError::CapacityReached); }
        let index = self.len;
        self.entries[index] = EndpointEntry {
            slot_id, endpoint_id, phase: EndpointRecoveryPhase::Idle, dequeue_pointer: 0,
        };
        self.len += 1;
        Ok(index)
    }

    pub fn observe_transfer(&mut self, result: &TransferResult) -> Result<EndpointRecoveryPhase, RecoveryError> {
        let index = self.ensure(result.slot_id, result.endpoint_id)?;
        if result.completion == TransferCompletion::Stall {
            self.entries[index].phase = EndpointRecoveryPhase::StallObserved;
        }
        Ok(self.entries[index].phase)
    }

    pub fn phase(&self, slot_id: u8, endpoint_id: u8) -> Option<EndpointRecoveryPhase> {
        self.find(slot_id, endpoint_id).map(|index| self.entries[index].phase)
    }

    /// Build Reset Endpoint only after a correlated STALL has been observed.
    pub fn begin_reset(&mut self, slot_id: u8, endpoint_id: u8) -> Result<Trb, RecoveryError> {
        let index = self.find(slot_id, endpoint_id).ok_or(RecoveryError::UnknownEndpoint)?;
        if self.entries[index].phase != EndpointRecoveryPhase::StallObserved {
            return Err(RecoveryError::InvalidTransition);
        }
        self.entries[index].phase = EndpointRecoveryPhase::ResetPending;
        Ok(Trb {
            parameter: 0,
            status: 0,
            control: (TRB_TYPE_RESET_ENDPOINT << TRB_TYPE_SHIFT)
                | ((endpoint_id as u32) << 16)
                | ((slot_id as u32) << 24),
        })
    }

    pub fn note_reset_completion(&mut self, slot_id: u8, endpoint_id: u8, success: bool)
        -> Result<(), RecoveryError>
    {
        let index = self.find(slot_id, endpoint_id).ok_or(RecoveryError::UnknownEndpoint)?;
        if self.entries[index].phase != EndpointRecoveryPhase::ResetPending {
            return Err(RecoveryError::InvalidTransition);
        }
        self.entries[index].phase = if success {
            EndpointRecoveryPhase::ResetComplete
        } else {
            EndpointRecoveryPhase::Failed
        };
        if !success { return Err(RecoveryError::CommandFailed); }
        Ok(())
    }

    /// Set TR Dequeue Pointer is a distinct command after Reset Endpoint.
    /// Caller must provide a controller-valid dequeue pointer and DCS value.
    pub fn begin_dequeue_update(&mut self, slot_id: u8, endpoint_id: u8,
        dequeue_pointer: u64, cycle_state: bool) -> Result<Trb, RecoveryError>
    {
        if dequeue_pointer == 0 || dequeue_pointer & 0xF != 0 || dequeue_pointer >= DMA_LIMIT {
            return Err(RecoveryError::InvalidPointer);
        }
        let index = self.find(slot_id, endpoint_id).ok_or(RecoveryError::UnknownEndpoint)?;
        if self.entries[index].phase != EndpointRecoveryPhase::ResetComplete {
            return Err(RecoveryError::InvalidTransition);
        }
        self.entries[index].dequeue_pointer = dequeue_pointer;
        self.entries[index].phase = EndpointRecoveryPhase::DequeueUpdatePending;
        Ok(Trb {
            parameter: dequeue_pointer | if cycle_state { 1 } else { 0 },
            status: 0,
            control: (TRB_TYPE_SET_TR_DEQUEUE_POINTER << TRB_TYPE_SHIFT)
                | ((endpoint_id as u32) << 16)
                | ((slot_id as u32) << 24),
        })
    }

    pub fn note_dequeue_update_completion(&mut self, slot_id: u8, endpoint_id: u8,
        success: bool) -> Result<(), RecoveryError>
    {
        let index = self.find(slot_id, endpoint_id).ok_or(RecoveryError::UnknownEndpoint)?;
        if self.entries[index].phase != EndpointRecoveryPhase::DequeueUpdatePending {
            return Err(RecoveryError::InvalidTransition);
        }
        self.entries[index].phase = if success {
            EndpointRecoveryPhase::Ready
        } else {
            EndpointRecoveryPhase::Failed
        };
        if !success { return Err(RecoveryError::CommandFailed); }
        Ok(())
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PortStatusChange {
    pub port_id: u8,
}

impl PortStatusChange {
    /// Port Status Change Event: Port ID is in Parameter[31:24].
    /// PortSC.CCS and PORTSC speed must be read separately by a future MMIO layer;
    /// this event alone does not carry the live port status register snapshot.
    pub fn decode(event: &EventRecord) -> Option<Self> {
        if event.event_type != EventType::PortStatusChange { return None; }
        let port_id = (event.trb.parameter >> 24) as u8;
        if port_id == 0 { return None; }
        Some(Self { port_id })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DisconnectPhase {
    Connected,
    DisconnectObserved,
    DisableSlotPending,
    SlotDisabled,
    SafeToReclaim,
    Quarantined,
}

/// Disconnect lifecycle gate. Slot disable completion alone is deliberately
/// insufficient: a separate controller/DMA quiescence proof is mandatory.
pub struct DisconnectGuard {
    slot_id: u8,
    phase: DisconnectPhase,
}

impl DisconnectGuard {
    pub const fn new(slot_id: u8) -> Self {
        Self { slot_id, phase: DisconnectPhase::Connected }
    }
    pub fn phase(&self) -> DisconnectPhase { self.phase }
    pub fn observe_disconnect(&mut self) -> Result<(), RecoveryError> {
        if self.slot_id == 0 || self.phase != DisconnectPhase::Connected {
            return Err(RecoveryError::InvalidTransition);
        }
        self.phase = DisconnectPhase::DisconnectObserved;
        Ok(())
    }
    pub fn begin_disable_slot(&mut self) -> Result<Trb, RecoveryError> {
        if self.phase != DisconnectPhase::DisconnectObserved {
            return Err(RecoveryError::InvalidTransition);
        }
        self.phase = DisconnectPhase::DisableSlotPending;
        Ok(Trb {
            parameter: 0,
            status: 0,
            control: (10u32 << TRB_TYPE_SHIFT) | ((self.slot_id as u32) << 24),
        })
    }
    pub fn note_slot_disabled(&mut self, success: bool) -> Result<(), RecoveryError> {
        if self.phase != DisconnectPhase::DisableSlotPending {
            return Err(RecoveryError::InvalidTransition);
        }
        self.phase = if success { DisconnectPhase::SlotDisabled } else { DisconnectPhase::Quarantined };
        if !success { return Err(RecoveryError::CommandFailed); }
        Ok(())
    }
    pub fn note_dma_quiescence_verified(&mut self, verified: bool) -> Result<(), RecoveryError> {
        if self.phase != DisconnectPhase::SlotDisabled {
            return Err(RecoveryError::InvalidTransition);
        }
        self.phase = if verified { DisconnectPhase::SafeToReclaim } else { DisconnectPhase::Quarantined };
        if !verified { return Err(RecoveryError::InvalidTransition); }
        Ok(())
    }
}
