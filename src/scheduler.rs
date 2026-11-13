//! Preemptive scheduler policy and PIT-driven reschedule requests.
//!
//! This module deliberately does not switch stacks from the current ISR:
//! the interrupt entry/return ABI still needs an interrupt-frame handoff path.
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use crate::task::{self, State};

pub const DEFAULT_QUANTUM_TICKS: u32 = 5; // 50 ms at the configured 100 Hz PIT.
static TICKS: AtomicU32 = AtomicU32::new(0);
static QUANTUM: AtomicU32 = AtomicU32::new(DEFAULT_QUANTUM_TICKS);
static ELAPSED: AtomicU32 = AtomicU32::new(0);
static RESCHEDULE: AtomicBool = AtomicBool::new(false);
static CURRENT_TASK: AtomicU32 = AtomicU32::new(0);

/// Called from IRQ0. Performs only bounded atomic operations; never takes a lock.
pub fn on_timer_tick() {
    TICKS.fetch_add(1, Ordering::Relaxed);
    let elapsed = ELAPSED.fetch_add(1, Ordering::Relaxed).wrapping_add(1);
    let quantum = QUANTUM.load(Ordering::Relaxed).max(1);
    if elapsed >= quantum {
        ELAPSED.store(0, Ordering::Relaxed);
        RESCHEDULE.store(true, Ordering::Release);
    }
}

/// Set a nonzero time slice in PIT ticks.
pub fn set_quantum(ticks: u32) -> bool {
    if ticks == 0 || ticks > 10_000 { return false; }
    QUANTUM.store(ticks, Ordering::Release);
    ELAPSED.store(0, Ordering::Relaxed);
    true
}

pub fn ticks() -> u32 { TICKS.load(Ordering::Acquire) }
pub fn current_task() -> Option<u32> {
    let id = CURRENT_TASK.load(Ordering::Acquire);
    if id == 0 { None } else { Some(id) }
}

/// Consume a pending scheduling request. This is a policy signal, not a switch.
pub fn take_reschedule_request() -> bool {
    RESCHEDULE.swap(false, Ordering::AcqRel)
}

/// Pick the next Ready task using priority-first round robin ordering.
pub fn choose_next() -> Option<u32> {
    task::next_ready_after(CURRENT_TASK.load(Ordering::Acquire))
}

/// Commit scheduler metadata after a future architecture switch succeeds.
/// Callers must not use this as a substitute for a real context switch.
pub fn commit_task_switch(next_id: u32) -> bool {
    if next_id == 0 { return false; }
    let next = match task::get(next_id) {
        Some(record) if record.state == State::Ready || record.state == State::Running => record,
        _ => return false,
    };
    if let Some(current) = current_task() {
        if current != next_id {
            if !task::set_state(current, State::Ready) { return false; }
        }
    }
    if next.state == State::Ready && !task::set_state(next_id, State::Running) {
        return false;
    }
    CURRENT_TASK.store(next_id, Ordering::Release);
    ELAPSED.store(0, Ordering::Relaxed);
    true
}
