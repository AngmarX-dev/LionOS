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
    let mut registry = REGISTRY.lock();
    let slot_index = registry.tasks.iter().position(|task| task.state == State::Empty)?;

    let id = registry.next_id;
    registry.next_id = registry.next_id.wrapping_add(1).max(1);

    registry.tasks[slot_index] = Task {
        id,
        state: State::Ready,
        priority,
        ticks: 0,
    };

    Some(id)
}

/// Change the state of an existing task.
pub fn set_state(id: u32, state: State) -> bool {
    if id == 0 || state == State::Empty {
        return false;
    }

    let mut registry = REGISTRY.lock();
    if let Some(task) = registry.tasks.iter_mut().find(|task| task.id == id && task.state != State::Empty) {
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
