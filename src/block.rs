//! Fixed-capacity block-device interface and registry.
//!
//! Drivers provide synchronous block callbacks. This module validates every
//! request before dispatch but does not itself initialize storage hardware.

pub const MAX_BLOCK_DEVICES: usize = 8;
pub const MIN_BLOCK_SIZE: u32 = 512;
pub const MAX_BLOCK_SIZE: u32 = 4096;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum BlockError {
    InvalidGeometry,
    InvalidRequest,
    OutOfRange,
    BufferTooSmall,
    ReadOnly,
    NoDeviceSlots,
    StaleHandle,
    Io,
    Unsupported,
}

#[derive(Clone, Copy)]
pub struct BlockDeviceInfo {
    pub block_size: u32,
    pub block_count: u64,
    pub removable: bool,
    pub read_only: bool,
}

impl BlockDeviceInfo {
    pub fn capacity_bytes(&self) -> Option<u64> {
        self.block_count.checked_mul(self.block_size as u64)
    }

    fn valid(&self) -> bool {
        self.block_size >= MIN_BLOCK_SIZE
            && self.block_size <= MAX_BLOCK_SIZE
            && self.block_size.is_power_of_two()
            && self.block_count != 0
            && self.capacity_bytes().is_some()
    }
}

/// Driver callbacks operate on exactly count consecutive logical blocks.
/// The driver must not retain the caller's buffer after returning.
#[derive(Clone, Copy)]
pub struct BlockDeviceOps {
    pub context: *mut (),
    pub read: unsafe fn(*mut (), u64, u32, *mut u8) -> Result<(), BlockError>,
    pub write: unsafe fn(*mut (), u64, u32, *const u8) -> Result<(), BlockError>,
    pub flush: unsafe fn(*mut ()) -> Result<(), BlockError>,
}

#[derive(Clone, Copy)]
pub struct BlockDeviceHandle {
    index: u8,
    generation: u32,
}

#[derive(Clone, Copy)]
struct DeviceSlot {
    active: bool,
    generation: u32,
    info: BlockDeviceInfo,
    ops: Option<BlockDeviceOps>,
}

impl DeviceSlot {
    const EMPTY: Self = Self {
        active: false,
        generation: 0,
        info: BlockDeviceInfo {
            block_size: 512,
            block_count: 0,
            removable: false,
            read_only: true,
        },
        ops: None,
    };
}

static mut DEVICES: [DeviceSlot; MAX_BLOCK_DEVICES] = [DeviceSlot::EMPTY; MAX_BLOCK_DEVICES];
static mut NEXT_GENERATION: u32 = 1;

/// Register a driver-owned callback table. The callback context and any
/// referenced driver state must remain valid until the device is unregistered.
/// Registration is expected during serialized kernel initialization.
pub unsafe fn register(
    info: BlockDeviceInfo,
    ops: BlockDeviceOps,
) -> Result<BlockDeviceHandle, BlockError> {
    if !info.valid() { return Err(BlockError::InvalidGeometry); }
    let mut index = 0usize;
    while index < MAX_BLOCK_DEVICES && DEVICES[index].active { index += 1; }
    if index == MAX_BLOCK_DEVICES { return Err(BlockError::NoDeviceSlots); }

    let generation = NEXT_GENERATION;
    NEXT_GENERATION = NEXT_GENERATION.wrapping_add(1).max(1);
    DEVICES[index] = DeviceSlot {
        active: true,
        generation,
        info,
        ops: Some(ops),
    };
    Ok(BlockDeviceHandle { index: index as u8, generation })
}

fn resolve(handle: BlockDeviceHandle) -> Result<(BlockDeviceInfo, BlockDeviceOps), BlockError> {
    let index = handle.index as usize;
    if index >= MAX_BLOCK_DEVICES { return Err(BlockError::StaleHandle); }
    unsafe {
        let slot = &DEVICES[index];
        if !slot.active || slot.generation != handle.generation {
            return Err(BlockError::StaleHandle);
        }
        let ops = slot.ops.ok_or(BlockError::StaleHandle)?;
        Ok((slot.info, ops))
    }
}

fn validate_request(
    info: BlockDeviceInfo,
    lba: u64,
    count: u32,
    buffer_len: usize,
) -> Result<(), BlockError> {
    if count == 0 { return Err(BlockError::InvalidRequest); }
    let end = lba.checked_add(count as u64).ok_or(BlockError::OutOfRange)?;
    if end > info.block_count { return Err(BlockError::OutOfRange); }
    let required = (count as usize)
        .checked_mul(info.block_size as usize)
        .ok_or(BlockError::InvalidRequest)?;
    if buffer_len < required { return Err(BlockError::BufferTooSmall); }
    Ok(())
}

pub fn info(handle: BlockDeviceHandle) -> Result<BlockDeviceInfo, BlockError> {
    resolve(handle).map(|pair| pair.0)
}

pub fn read_blocks(
    handle: BlockDeviceHandle,
    lba: u64,
    count: u32,
    output: &mut [u8],
) -> Result<(), BlockError> {
    let (device, ops) = resolve(handle)?;
    validate_request(device, lba, count, output.len())?;
    unsafe { (ops.read)(ops.context, lba, count, output.as_mut_ptr()) }
}

pub fn write_blocks(
    handle: BlockDeviceHandle,
    lba: u64,
    count: u32,
    input: &[u8],
) -> Result<(), BlockError> {
    let (device, ops) = resolve(handle)?;
    if device.read_only { return Err(BlockError::ReadOnly); }
    validate_request(device, lba, count, input.len())?;
    unsafe { (ops.write)(ops.context, lba, count, input.as_ptr()) }
}

pub fn flush(handle: BlockDeviceHandle) -> Result<(), BlockError> {
    let (_, ops) = resolve(handle)?;
    unsafe { (ops.flush)(ops.context) }
}

/// Remove a device only after its driver has stopped all I/O and DMA.
/// The registry does not establish hardware quiescence on the driver's behalf.
pub unsafe fn unregister(handle: BlockDeviceHandle) -> Result<(), BlockError> {
    let index = handle.index as usize;
    if index >= MAX_BLOCK_DEVICES { return Err(BlockError::StaleHandle); }
    let slot = &DEVICES[index];
    if !slot.active || slot.generation != handle.generation {
        return Err(BlockError::StaleHandle);
    }
    DEVICES[index] = DeviceSlot::EMPTY;
    Ok(())
}

pub fn device_count() -> usize {
    let mut count = 0usize;
    unsafe {
        let mut i = 0usize;
        while i < MAX_BLOCK_DEVICES {
            if DEVICES[i].active { count += 1; }
            i += 1;
        }
    }
    count
}
