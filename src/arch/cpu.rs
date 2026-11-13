//! CPUID feature discovery for the 32-bit x86 kernel bootstrap.
use core::arch::x86::__cpuid;

#[derive(Clone, Copy, Debug)]
pub struct CpuFeatures {
    pub pse: bool,
    pub pae: bool,
    pub apic: bool,
    pub msr: bool,
    pub tsc: bool,
    pub sse2: bool,
}

/// Read architectural feature bits from CPUID leaf 1.
/// The selected i686 target guarantees CPUID is available.
pub fn features() -> CpuFeatures {
    let leaf = unsafe { __cpuid(1) };
    CpuFeatures {
        tsc: (leaf.edx & (1 << 4)) != 0,
        msr: (leaf.edx & (1 << 5)) != 0,
        pae: (leaf.edx & (1 << 6)) != 0,
        apic: (leaf.edx & (1 << 9)) != 0,
        pse: (leaf.edx & (1 << 3)) != 0,
        sse2: (leaf.edx & (1 << 26)) != 0,
    }
}
