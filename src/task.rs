//! Fixed-capacity task registry.
//!
//! This module stores task metadata only. It does not yet perform context
//! switching, address-space switching, blocking, or preemptive scheduling.

use crate::sync::IrqSpinLock;

pub const MAX_TASKS: usize = 32;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum State {
    Empty,
    Ready,
    Running,
    Blocked,
    Exited,
}

#[derive(Clone, Copy, Debug)]
pub struct Task {
    pub id: u32,
    pub state: State,
    pub priority: u8,
    pub ticks: u64,
}

impl Task {
    const EMPTY: Self = Self {
        id: 0,
        state: State::Empty,
        priority: 0,
        ticks: 0,
    };
}

impl State {
    /// Validate scheduler metadata transitions; Exited is terminal.
    fn allows(self, next: Self) -> bool {
        if self == next {
            return true;
        }

        match self {
            State::Empty | State::Exited => false,
            State::Ready => matches!(next, State::Running | State::Blocked | State::Exited),
            State::Running => matches!(next, State::Ready | State::Blocked | State::Exited),
            State::Blocked => matches!(next, State::Ready | State::Exited),
        }
    }
}

struct TaskRegistry {
    tasks: [Task; MAX_TASKS],
    next_id: u32,
}

impl TaskRegistry {
    const fn new() -> Self {
        Self {
            tasks: [Task::EMPTY; MAX_TASKS],
            next_id: 1,
        }
    }
}

static REGISTRY: IrqSpinLock<TaskRegistry> = IrqSpinLock::new(TaskRegistry::new());

/// Register a task metadata record and return its nonzero identifier.
pub fn register(priority: u8) -> Option<u32> {
    crate::write_str("[TRACE] task register: before registry lock\n");
    let mut registry = REGISTRY.lock();
    crate::write_str("[TRACE] task register: registry lock acquired\n");
    let mut slot_index=0usize;
    while slot_index<MAX_TASKS {
        crate::write_str("[TRACE] task register: inspect slot ");
        crate::write_dec(slot_index as u32);
        crate::write_str("\n");
        let state=registry.tasks[slot_index].state;
        crate::write_str("[TRACE] task register: slot state read\n");
        if state==State::Empty { break; }
        slot_index+=1;
    }
    if slot_index==MAX_TASKS {
        crate::write_str("[WARN] task register: registry full\n");
        return None;
    }
    crate::write_str("[TRACE] task register: free slot found\n");

    let id = registry.next_id;
    registry.next_id = registry.next_id.wrapping_add(1).max(1);

    registry.tasks[slot_index] = Task {
        id,
        state: State::Ready,
        priority,
        ticks: 0,
    };

    crate::write_str("[TRACE] task register: task record committed\n");
    Some(id)
}

/// Change the state of an existing task.
pub fn set_state(id: u32, state: State) -> bool {
    if id == 0 || state == State::Empty {
        return false;
    }

    let mut registry = REGISTRY.lock();
    if let Some(task) = registry.tasks.iter_mut().find(|task| task.id == id && task.state != State::Empty) {
        if !task.state.allows(state) {
            return false;
        }
        task.state = state;
        true
    } else {
        false
    }
}

/// Return the highest-priority Ready task identifier.
pub fn next_ready() -> Option<u32> {
    let registry = REGISTRY.lock();
    registry.tasks
        .iter()
        .filter(|task| task.state == State::Ready)
        .max_by_key(|task| task.priority)
        .map(|task| task.id)
}

/// Select the next Ready task in round-robin ID order after `after_id`.
/// Priority is the primary key; equal-priority tasks rotate deterministically.
pub fn next_ready_after(after_id: u32) -> Option<u32> {
    let registry = REGISTRY.lock();
    let max_priority = registry.tasks.iter()
        .filter(|task| task.state == State::Ready)
        .map(|task| task.priority)
        .max()?;
    registry.tasks.iter()
        .filter(|task| task.state == State::Ready && task.priority == max_priority && task.id > after_id)
        .map(|task| task.id)
        .min()
        .or_else(|| registry.tasks.iter()
            .filter(|task| task.state == State::Ready && task.priority == max_priority)
            .map(|task| task.id)
            .min())
}

/// Return a consistent copy of a task record.
pub fn get(id: u32) -> Option<Task> {
    if id == 0 {
        return None;
    }

    let registry = REGISTRY.lock();
    registry.tasks.iter()
        .find(|task| task.id == id && task.state != State::Empty)
        .copied()
}

/// Count currently occupied task slots.
pub fn task_count() -> usize {
    let registry = REGISTRY.lock();
    registry.tasks.iter().filter(|task| task.state != State::Empty).count()
}
