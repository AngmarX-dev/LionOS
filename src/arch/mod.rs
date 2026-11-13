//! Architecture-specific code for the 32-bit x86 kernel target.
//! Keep CPU instructions, descriptor tables, interrupt entry and context ABI
//! behind this module boundary. Higher-level code should use these interfaces.
pub mod context;
pub mod cpu;
pub mod exception;
pub mod gdt;
pub mod init;
pub mod interrupts;
pub mod tss;
pub mod x86;
