//! Kernel synchronization primitives for the 32-bit x86 target.
//!
//! IrqSpinLock disables local maskable interrupts before acquiring its
//! atomic lock. This prevents same-CPU interrupt handlers from re-entering
//! a lock held by the interrupted code. The atomic flag also serializes
//! access between CPUs if SMP support is added later.
//!
//! Do not hold these locks across blocking operations or unbounded waits.

use core::cell::UnsafeCell;
use core::hint::spin_loop;
use core::ops::{Deref, DerefMut};
use core::sync::atomic::{AtomicBool, Ordering};
use core::arch::asm;

/// A spin lock for data that is shared between execution contexts.
pub struct SpinLock<T> {
    held: AtomicBool,
    value: UnsafeCell<T>,
}

unsafe impl<T: Send> Send for SpinLock<T> {}
unsafe impl<T: Send> Sync for SpinLock<T> {}

impl<T> SpinLock<T> {
    pub const fn new(value: T) -> Self {
        Self {
            held: AtomicBool::new(false),
            value: UnsafeCell::new(value),
        }
    }

    pub fn lock(&self) -> SpinLockGuard<'_, T> {
        while self.held.compare_exchange_weak(
            false,
            true,
            Ordering::Acquire,
            Ordering::Relaxed,
        ).is_err() {
            while self.held.load(Ordering::Relaxed) {
                spin_loop();
            }
        }
        SpinLockGuard { lock: self }
    }

    pub fn try_lock(&self) -> Option<SpinLockGuard<'_, T>> {
        self.held.compare_exchange(
            false,
            true,
            Ordering::Acquire,
            Ordering::Relaxed,
        ).ok().map(|_| SpinLockGuard { lock: self })
    }

    pub fn is_locked(&self) -> bool {
        self.held.load(Ordering::Relaxed)
    }
}

pub struct SpinLockGuard<'a, T> {
    lock: &'a SpinLock<T>,
}

impl<T> Deref for SpinLockGuard<'_, T> {
    type Target = T;

    fn deref(&self) -> &T {
        unsafe { &*self.lock.value.get() }
    }
}

impl<T> DerefMut for SpinLockGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        unsafe { &mut *self.lock.value.get() }
    }
}

impl<T> Drop for SpinLockGuard<'_, T> {
    fn drop(&mut self) {
        self.lock.held.store(false, Ordering::Release);
    }
}

/// A spin lock that saves and disables the local CPU's interrupt-enable flag.
pub struct IrqSpinLock<T> {
    held: AtomicBool,
    value: UnsafeCell<T>,
}

unsafe impl<T: Send> Send for IrqSpinLock<T> {}
unsafe impl<T: Send> Sync for IrqSpinLock<T> {}

impl<T> IrqSpinLock<T> {
    pub const fn new(value: T) -> Self {
        Self {
            held: AtomicBool::new(false),
            value: UnsafeCell::new(value),
        }
    }

    #[inline]
    fn disable_interrupts() -> bool {
        let flags: u32;
        unsafe {
            asm!("pushfd; pop {0}; cli", out(reg) flags, options(nomem));
        }
        flags & (1 << 9) != 0
    }

    #[inline]
    fn restore_interrupts(enabled: bool) {
        if enabled {
            unsafe {
                asm!("sti", options(nomem, nostack));
            }
        }
    }

    pub fn lock(&self) -> IrqSpinLockGuard<'_, T> {
        let restore_interrupts = Self::disable_interrupts();

        while self.held.compare_exchange_weak(
            false,
            true,
            Ordering::Acquire,
            Ordering::Relaxed,
        ).is_err() {
            while self.held.load(Ordering::Relaxed) {
                spin_loop();
            }
        }

        IrqSpinLockGuard {
            lock: self,
            restore_interrupts,
        }
    }

    pub fn try_lock(&self) -> Option<IrqSpinLockGuard<'_, T>> {
        let restore_interrupts = Self::disable_interrupts();

        match self.held.compare_exchange(
            false,
            true,
            Ordering::Acquire,
            Ordering::Relaxed,
        ) {
            Ok(_) => Some(IrqSpinLockGuard {
                lock: self,
                restore_interrupts,
            }),
            Err(_) => {
                Self::restore_interrupts(restore_interrupts);
                None
            }
        }
    }

    pub fn is_locked(&self) -> bool {
        self.held.load(Ordering::Relaxed)
    }
}

pub struct IrqSpinLockGuard<'a, T> {
    lock: &'a IrqSpinLock<T>,
    restore_interrupts: bool,
}

impl<T> Deref for IrqSpinLockGuard<'_, T> {
    type Target = T;

    fn deref(&self) -> &T {
        unsafe { &*self.lock.value.get() }
    }
}

impl<T> DerefMut for IrqSpinLockGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        unsafe { &mut *self.lock.value.get() }
    }
}

impl<T> Drop for IrqSpinLockGuard<'_, T> {
    fn drop(&mut self) {
        self.lock.held.store(false, Ordering::Release);
        IrqSpinLock::<T>::restore_interrupts(self.restore_interrupts);
    }
}
