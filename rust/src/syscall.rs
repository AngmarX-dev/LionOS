//! Kernel syscall ABI draft. Dispatch is intentionally disabled until user mode exists.
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Number { Exit=0, Write=1, Yield=2, GetTime=3 }
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error { NotImplemented, InvalidNumber, PermissionDenied }
pub fn dispatch(number: u32, _arg0: u32, _arg1: u32, _arg2: u32) -> Result<u32, Error> {
    match number {
        0..=3 => Err(Error::NotImplemented),
        _ => Err(Error::InvalidNumber),
    }
}
