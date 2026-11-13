//! Bounded per-CPU bookkeeping for the future SMP runtime.
//!
//! Registration records CPUID's local APIC ID only. It does not start APs,
//! enable an APIC, route interrupts, or claim that multiple CPUs are online.
use core::sync::atomic::{AtomicU32, AtomicU8, Ordering};

pub const MAX_CPUS: usize = 8;
const NO_APIC_ID: u32 = u32::MAX;
const OFFLINE: u8 = 0;
const ONLINE: u8 = 1;

struct PerCpu {
    apic_id: AtomicU32,
    state: AtomicU8,
    current_task: AtomicU32,
    interrupt_depth: AtomicU32,
}
impl PerCpu {
    const fn new() -> Self {
        Self {
            apic_id: AtomicU32::new(NO_APIC_ID),
            state: AtomicU8::new(OFFLINE),
            current_task: AtomicU32::new(0),
            interrupt_depth: AtomicU32::new(0),
        }
    }
}
static CPUS: [PerCpu; MAX_CPUS] = [
    PerCpu::new(), PerCpu::new(), PerCpu::new(), PerCpu::new(),
    PerCpu::new(), PerCpu::new(), PerCpu::new(), PerCpu::new(),
];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RegisterError {
    ApicUnavailable,
    CapacityReached,
}

/// Register the calling logical CPU after its local kernel structures are ready.
pub fn register_current_cpu() -> Result<usize, RegisterError> {
    let id = crate::arch::cpu::local_apic_id().ok_or(RegisterError::ApicUnavailable)? as u32;
    for (index, cpu) in CPUS.iter().enumerate() {
        if cpu.apic_id.load(Ordering::Acquire) == id {
            cpu.state.store(ONLINE, Ordering::Release);
            return Ok(index);
        }
    }
    for (index, cpu) in CPUS.iter().enumerate() {
        if cpu.apic_id.compare_exchange(NO_APIC_ID, id, Ordering::AcqRel, Ordering::Acquire).is_ok() {
            cpu.state.store(ONLINE, Ordering::Release);
            return Ok(index);
        }
    }
    // Another CPU may have claimed this APIC ID between the two scans.
    for (index, cpu) in CPUS.iter().enumerate() {
        if cpu.apic_id.load(Ordering::Acquire) == id {
            cpu.state.store(ONLINE, Ordering::Release);
            return Ok(index);
        }
    }
    Err(RegisterError::CapacityReached)
}

/// Locate this CPU's registered slot by its local APIC ID.
pub fn current_cpu_index() -> Option<usize> {
    let id = crate::arch::cpu::local_apic_id()? as u32;
    CPUS.iter().position(|cpu| {
        cpu.apic_id.load(Ordering::Acquire) == id
            && cpu.state.load(Ordering::Acquire) == ONLINE
    })
}

pub fn online_count() -> usize {
    CPUS.iter().filter(|cpu| cpu.state.load(Ordering::Acquire) == ONLINE).count()
}

pub fn apic_id(index: usize) -> Option<u8> {
    let cpu = CPUS.get(index)?;
    if cpu.state.load(Ordering::Acquire) != ONLINE { return None; }
    let id = cpu.apic_id.load(Ordering::Acquire);
    if id == NO_APIC_ID { None } else { Some(id as u8) }
}

pub fn set_current_task(task_id: u32) -> bool {
    let Some(index) = current_cpu_index() else { return false; };
    CPUS[index].current_task.store(task_id, Ordering::Release);
    true
}
pub fn current_task() -> Option<u32> {
    let index = current_cpu_index()?;
    let id = CPUS[index].current_task.load(Ordering::Acquire);
    if id == 0 { None } else { Some(id) }
}

/// Track nested interrupt entry/exit independently on each logical CPU.
pub fn interrupt_enter() -> bool {
    let Some(index) = current_cpu_index() else { return false; };
    CPUS[index].interrupt_depth.fetch_add(1, Ordering::AcqRel);
    true
}
pub fn interrupt_exit() -> bool {
    let Some(index) = current_cpu_index() else { return false; };
    CPUS[index].interrupt_depth.fetch_update(Ordering::AcqRel, Ordering::Acquire, |depth| {
        if depth == 0 { None } else { Some(depth - 1) }
    }).is_ok()
}
pub fn interrupt_depth() -> Option<u32> {
    let index = current_cpu_index()?;
    Some(CPUS[index].interrupt_depth.load(Ordering::Acquire))
}
