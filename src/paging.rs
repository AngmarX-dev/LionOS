//! 32-bit x86 paging: bootstrap 4 MiB map plus managed 4 KiB mappings.
//!
//! The active address space remains one shared supervisor kernel directory.
//! This module does not yet create per-process page directories or enable
//! user-mode execution. Physical addresses are limited to the 32-bit range.

use core::arch::asm;
use core::ptr::{read_volatile, write_volatile};
use crate::sync::IrqSpinLock;

#[repr(align(4096))]
struct PageDirectory([u32; 1024]);

static mut DIRECTORY: PageDirectory = PageDirectory([0; 1024]);
static PAGING_READY: core::sync::atomic::AtomicBool =
    core::sync::atomic::AtomicBool::new(false);
static PAGING_LOCK: IrqSpinLock<()> = IrqSpinLock::new(());

const LARGE_PAGE_SIZE: u64 = 4 * 1024 * 1024;
const PAGE_MASK: u32 = 0xFFFF_F000;
const PRESENT: u32 = 1 << 0;
const WRITABLE: u32 = 1 << 1;
const USER: u32 = 1 << 2;
const PWT: u32 = 1 << 3;
const PCD: u32 = 1 << 4;
const ACCESSED: u32 = 1 << 5;
const DIRTY: u32 = 1 << 6;
const PAGE_SIZE_BIT: u32 = 1 << 7;
const GLOBAL: u32 = 1 << 8;
const LARGE_PAT: u32 = 1 << 12;

/// Supported 4 KiB PTE flags. PRESENT is always set by the mapper.
pub const MAP_WRITABLE: u32 = WRITABLE;
pub const MAP_USER: u32 = USER;
pub const MAP_WRITE_THROUGH: u32 = PWT;
pub const MAP_CACHE_DISABLE: u32 = PCD;
pub const MAP_GLOBAL: u32 = GLOBAL;
const MAP_ALLOWED: u32 = MAP_WRITABLE | MAP_USER | MAP_WRITE_THROUGH | MAP_CACHE_DISABLE | MAP_GLOBAL;

#[inline]
fn directory_ptr() -> *mut u32 {
    core::ptr::addr_of_mut!(DIRECTORY.0).cast::<u32>()
}

#[inline]
unsafe fn invalidate_page(address: u32) {
    asm!("invlpg [{}]", in(reg) address, options(nostack, preserves_flags));
}

#[inline]
unsafe fn reload_cr3() {
    let physical = directory_ptr() as u32;
    asm!("mov cr3, {}", in(reg) physical, options(nostack, preserves_flags));
}

/// Enable a supervisor-only, writable 4 GiB identity map using 4 MiB pages.
pub unsafe fn init_identity_4g() -> bool {
    if PAGING_READY.load(core::sync::atomic::Ordering::Acquire) {
        return true;
    }
    if !crate::arch::cpu::features().pse {
        return false;
    }

    let directory = directory_ptr();
    for index in 0..1024usize {
        write_volatile(directory.add(index), ((index as u32) << 22) | 0x083);
    }

    let physical = directory as u32;
    asm!(
        "mov eax, cr4",
        "or eax, 0x10",
        "mov cr4, eax",
        "mov cr3, ecx",
        "mov eax, cr0",
        "or eax, 0x80000000",
        "mov cr0, eax",
        "jmp 2f",
        "2:",
        in("ecx") physical,
        out("eax") _,
        options(nostack),
    );

    PAGING_READY.store(true, core::sync::atomic::Ordering::Release);
    true
}

pub fn is_ready() -> bool {
    PAGING_READY.load(core::sync::atomic::Ordering::Acquire)
}

/// Split an existing 4 MiB PDE into a page table, preserving its mapping and
/// cache/access attributes. Caller must hold PAGING_LOCK.
unsafe fn ensure_page_table_locked(directory_index: usize) -> Option<*mut u32> {
    let directory = directory_ptr();
    let pde_ptr = directory.add(directory_index);
    let pde = read_volatile(pde_ptr);

    if pde & PRESENT != 0 && pde & PAGE_SIZE_BIT == 0 {
        return Some((pde & PAGE_MASK) as *mut u32);
    }

    let frame = crate::memory::alloc_frame()?;
    let table = frame as *mut u32;

    if pde & PRESENT != 0 && pde & PAGE_SIZE_BIT != 0 {
        let large_base = pde & 0xFFC0_0000;
        let inherited = (pde & 0x7F) | (pde & GLOBAL);
        let pat = if pde & LARGE_PAT != 0 { PAGE_SIZE_BIT } else { 0 };

        for index in 0..1024usize {
            let physical = large_base.wrapping_add((index as u32) * 4096);
            write_volatile(table.add(index), physical | inherited | pat);
        }

        let mut table_flags = (pde & 0x1F) | WRITABLE | PRESENT;
        if pde & USER != 0 {
            table_flags |= USER;
        }
        write_volatile(pde_ptr, frame | table_flags);
    } else {
        for index in 0..1024usize {
            write_volatile(table.add(index), 0);
        }
        write_volatile(pde_ptr, frame | PRESENT | WRITABLE);
    }

    reload_cr3();
    Some(table)
}

unsafe fn map_page_locked(virtual_address: u32, physical_address: u32, flags: u32, replace: bool) -> bool {
    if virtual_address & 0xFFF != 0
        || physical_address & 0xFFF != 0
        || flags & !MAP_ALLOWED != 0
    {
        return false;
    }

    let directory_index = (virtual_address >> 22) as usize;
    let table_index = ((virtual_address >> 12) & 0x3FF) as usize;
    let Some(table) = ensure_page_table_locked(directory_index) else {
        return false;
    };

    let entry = table.add(table_index);
    let old = read_volatile(entry);
    if old & PRESENT != 0 && !replace {
        return false;
    }

    write_volatile(entry, (physical_address & PAGE_MASK) | PRESENT | flags);

    let pde_ptr = directory_ptr().add(directory_index);
    let mut pde = read_volatile(pde_ptr) | WRITABLE | PRESENT;
    if flags & USER != 0 {
        pde |= USER;
    }
    write_volatile(pde_ptr, pde);

    invalidate_page(virtual_address);
    true
}

/// Map a previously unmapped 4 KiB virtual page to a physical page.
/// The mapping is installed in the shared kernel address space.
pub fn map_page(virtual_address: u32, physical_address: u32, flags: u32) -> bool {
    if !is_ready() {
        return false;
    }
    let _guard = PAGING_LOCK.lock();
    unsafe { map_page_locked(virtual_address, physical_address, flags, false) }
}

/// Explicitly replace an existing 4 KiB mapping or install a new one.
pub fn remap_page(virtual_address: u32, physical_address: u32, flags: u32) -> bool {
    if !is_ready() {
        return false;
    }
    let _guard = PAGING_LOCK.lock();
    unsafe { map_page_locked(virtual_address, physical_address, flags, true) }
}

/// Translate a virtual address through the active page directory.
pub fn translate(virtual_address: u32) -> Option<u32> {
    if !is_ready() {
        return None;
    }
    let _guard = PAGING_LOCK.lock();
    unsafe {
        let pde = read_volatile(directory_ptr().add((virtual_address >> 22) as usize));
        if pde & PRESENT == 0 {
            return None;
        }
        if pde & PAGE_SIZE_BIT != 0 {
            let base = pde & 0xFFC0_0000;
            return Some(base | (virtual_address & 0x003F_FFFF));
        }

        let table = (pde & PAGE_MASK) as *const u32;
        let pte = read_volatile(table.add(((virtual_address >> 12) & 0x3FF) as usize));
        if pte & PRESENT == 0 {
            None
        } else {
            Some((pte & PAGE_MASK) | (virtual_address & 0xFFF))
        }
    }
}

unsafe fn unmap_page_locked(virtual_address: u32) -> bool {
    let directory_index = (virtual_address >> 22) as usize;
    let table_index = ((virtual_address >> 12) & 0x3FF) as usize;
    let Some(table) = ensure_page_table_locked(directory_index) else {
        return false;
    };

    let entry = table.add(table_index);
    if read_volatile(entry) & PRESENT == 0 {
        return false;
    }
    write_volatile(entry, 0);
    invalidate_page(virtual_address);

    let mut empty = true;
    for index in 0..1024usize {
        if read_volatile(table.add(index)) & PRESENT != 0 {
            empty = false;
            break;
        }
    }

    if empty {
        let pde_ptr = directory_ptr().add(directory_index);
        let pde = read_volatile(pde_ptr);
        write_volatile(pde_ptr, 0);
        reload_cr3();
        let table_frame = pde & PAGE_MASK;
        let _ = crate::memory::free_frame(table_frame);
    }
    true
}

/// Remove a 4 KiB mapping. A large-page PDE is split first, preserving all
/// unaffected identity mappings. Empty page-table frames are reclaimed.
pub fn unmap_page(virtual_address: u32) -> bool {
    if !is_ready() || virtual_address & 0xFFF != 0 {
        return false;
    }
    let _guard = PAGING_LOCK.lock();
    unsafe { unmap_page_locked(virtual_address) }
}

/// Map a contiguous physical run into a contiguous virtual range. If any page
/// fails, mappings installed by this call are rolled back before returning.
pub fn map_range(virtual_start: u32, physical_start: u32, pages: u32, flags: u32) -> bool {
    if !is_ready() || pages == 0 || virtual_start & 0xFFF != 0 || physical_start & 0xFFF != 0 {
        return false;
    }
    let Some(bytes) = (pages as u64).checked_mul(4096) else { return false; };
    if (virtual_start as u64).checked_add(bytes).map_or(true, |end| end > 0x1_0000_0000)
        || (physical_start as u64).checked_add(bytes).map_or(true, |end| end > 0x1_0000_0000)
        || flags & !MAP_ALLOWED != 0
    {
        return false;
    }

    let _guard = PAGING_LOCK.lock();
    unsafe {
        let mut mapped = 0u32;
        while mapped < pages {
            let virtual_address = virtual_start + mapped * 4096;
            let physical_address = physical_start + mapped * 4096;
            if !map_page_locked(virtual_address, physical_address, flags, false) {
                for rollback in 0..mapped {
                    let _ = unmap_page_locked(virtual_start + rollback * 4096);
                }
                return false;
            }
            mapped += 1;
        }
    }
    true
}

/// Unmap a contiguous virtual range. The operation first verifies that every
/// page is present, so a missing page cannot cause a partially unmapped range.
pub fn unmap_range(virtual_start: u32, pages: u32) -> bool {
    if !is_ready() || pages == 0 || virtual_start & 0xFFF != 0 {
        return false;
    }
    let Some(bytes) = (pages as u64).checked_mul(4096) else { return false; };
    if (virtual_start as u64).checked_add(bytes).map_or(true, |end| end > 0x1_0000_0000) {
        return false;
    }

    let _guard = PAGING_LOCK.lock();
    unsafe {
        for index in 0..pages {
            let address = virtual_start + index * 4096;
            let pde = read_volatile(directory_ptr().add((address >> 22) as usize));
            if pde & PRESENT == 0 {
                return false;
            }
            if pde & PAGE_SIZE_BIT == 0 {
                let table = (pde & PAGE_MASK) as *const u32;
                let pte = read_volatile(table.add(((address >> 12) & 0x3FF) as usize));
                if pte & PRESENT == 0 {
                    return false;
                }
            }
        }
        for index in 0..pages {
            if !unmap_page_locked(virtual_start + index * 4096) {
                return false;
            }
        }
    }
    true
}

/// Mark an MMIO range uncached. Handles both large PDEs and 4 KiB PTEs.
pub unsafe fn mark_mmio_uncacheable(base: u64, length: usize) -> bool {
    if !is_ready() || length == 0 {
        return false;
    }
    let end = match base.checked_add(length as u64) {
        Some(value) => value,
        None => return false,
    };
    if end > 0x1_0000_0000 || base >= end {
        return false;
    }

    let _guard = PAGING_LOCK.lock();
    let first = (base / LARGE_PAGE_SIZE) as usize;
    let last = ((end - 1) / LARGE_PAGE_SIZE) as usize;
    let directory = directory_ptr();

    for directory_index in first..=last {
        let pde_ptr = directory.add(directory_index);
        let mut pde = read_volatile(pde_ptr);
        if pde & PRESENT == 0 {
            return false;
        }

        if pde & PAGE_SIZE_BIT != 0 {
            write_volatile(pde_ptr, pde | PWT | PCD);
            continue;
        }

        let Some(table) = ensure_page_table_locked(directory_index) else {
            return false;
        };
        let segment_start = base.max((directory_index as u64) * LARGE_PAGE_SIZE);
        let segment_end = end.min(((directory_index as u64) + 1) * LARGE_PAGE_SIZE);
        let first_pte = ((segment_start >> 12) & 0x3FF) as usize;
        let last_pte = (((segment_end - 1) >> 12) & 0x3FF) as usize;

        for pte_index in first_pte..=last_pte {
            let pte_ptr = table.add(pte_index);
            let pte = read_volatile(pte_ptr);
            if pte & PRESENT == 0 {
                return false;
            }
            write_volatile(pte_ptr, pte | PWT | PCD);
        }
        pde = read_volatile(pde_ptr);
        let _ = pde;
    }

    reload_cr3();
    true
}
