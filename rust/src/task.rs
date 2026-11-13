//! Static task registry. Context switching and preemption are not enabled yet.
pub const MAX_TASKS: usize = 32;
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum State { Empty, Ready, Running, Blocked, Exited }
#[derive(Clone, Copy, Debug)]
pub struct Task { pub id: u32, pub state: State, pub priority: u8, pub ticks: u64 }
impl Task { const EMPTY: Self = Self { id:0, state:State::Empty, priority:0, ticks:0 }; }
static mut TASKS: [Task; MAX_TASKS] = [Task::EMPTY; MAX_TASKS];
static mut NEXT_ID: u32 = 1;
pub fn register(priority: u8) -> Option<u32> {
    unsafe {
        let slot = TASKS.iter_mut().find(|t| t.state == State::Empty)?;
        let id = NEXT_ID; NEXT_ID = NEXT_ID.wrapping_add(1).max(1);
        *slot = Task { id, state:State::Ready, priority, ticks:0 }; Some(id)
    }
}
pub fn set_state(id: u32, state: State) -> bool {
    unsafe { if let Some(t) = TASKS.iter_mut().find(|t| t.id == id && t.state != State::Empty) { t.state=state; true } else { false } }
}
pub fn next_ready() -> Option<u32> {
    unsafe { TASKS.iter().filter(|t| t.state == State::Ready).max_by_key(|t| t.priority).map(|t| t.id) }
}
