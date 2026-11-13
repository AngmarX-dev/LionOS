#![no_std]
#![no_main]

use core::panic::PanicInfo;
use core::ptr::{read_unaligned, write_volatile};

mod arch {
    pub mod x86;
    pub mod cpu;
    pub mod gdt;
    pub mod interrupts;
}
mod memory;
mod paging;
mod dma;
mod mmio;
mod xhci;
mod xhci_dma;
mod xhci_runtime;
mod usb;
mod heap;
mod pci;
mod device;
mod graphics;
mod fs;
mod task;
mod input;
mod net;
mod syscall;
mod desktop;

const MULTIBOOT2_BOOT_MAGIC: u32 = 0x36D7_6289;
const VGA_TEXT: *mut u16 = 0xB8000 as *mut u16;
const VGA_COLUMNS: usize = 80;
const VGA_ROWS: usize = 25;
const MAX_BOOT_INFO: u32 = 16 * 1024 * 1024;
static mut CURSOR: usize = 0;

fn serial_init() {
    unsafe {
        arch::x86::outb(0x3F9, 0x00);
        arch::x86::outb(0x3FB, 0x80);
        arch::x86::outb(0x3F8, 0x01);
        arch::x86::outb(0x3F9, 0x00);
        arch::x86::outb(0x3FB, 0x03);
        arch::x86::outb(0x3FA, 0xC7);
        arch::x86::outb(0x3FC, 0x0B);
    }
}
fn serial_byte(byte: u8) {
    unsafe {
        for _ in 0..100_000 {
            if arch::x86::inb(0x3FD) & 0x20 != 0 {
                arch::x86::outb(0x3F8, byte);
                return;
            }
            arch::x86::pause();
        }
    }
}
fn put_byte(byte: u8) {
    serial_byte(byte);
    unsafe {
        if byte == b'\n' {
            CURSOR = (CURSOR / VGA_COLUMNS + 1) * VGA_COLUMNS;
        } else if byte == b'\r' {
            CURSOR -= CURSOR % VGA_COLUMNS;
        } else {
            if CURSOR < VGA_COLUMNS * VGA_ROWS {
                write_volatile(VGA_TEXT.add(CURSOR), 0x0F00 | byte as u16);
            }
            CURSOR += 1;
        }
        if CURSOR >= VGA_COLUMNS * VGA_ROWS {
            for i in 0..(VGA_COLUMNS * (VGA_ROWS - 1)) {
                let cell = core::ptr::read_volatile(VGA_TEXT.add(i + VGA_COLUMNS));
                write_volatile(VGA_TEXT.add(i), cell);
            }
            for i in (VGA_COLUMNS * (VGA_ROWS - 1))..(VGA_COLUMNS * VGA_ROWS) {
                write_volatile(VGA_TEXT.add(i), 0x0F20);
            }
            CURSOR = VGA_COLUMNS * (VGA_ROWS - 1);
        }
    }
}
fn write_str(value: &str) { for byte in value.bytes() { put_byte(byte); } }
fn write_hex(value: u32) {
    const DIGITS: &[u8;16] = b"0123456789ABCDEF";
    write_str("0x");
    for shift in (0..8).rev() { put_byte(DIGITS[((value >> (shift*4)) & 0xF) as usize]); }
}
fn write_dec(mut value:u32) {
    let mut digits=[0u8;10]; let mut used=0usize;
    if value==0 { put_byte(b'0'); return; }
    while value!=0 && used<digits.len() { digits[used]=b'0'+(value%10) as u8; value/=10; used+=1; }
    while used>0 { used-=1; put_byte(digits[used]); }
}
unsafe fn read_u8(base:*const u8,offset:usize)->u8 { *base.add(offset) }
unsafe fn read_u32(base:*const u8,offset:usize)->u32 { read_unaligned(base.add(offset).cast::<u32>()) }
unsafe fn read_u64(base:*const u8,offset:usize)->u64 { read_unaligned(base.add(offset).cast::<u64>()) }

fn parse_boot_info(address:u32)->(u64,bool) {
    if address==0 || (address & 7) != 0 { write_str("[ERR] Null or unaligned Multiboot2 info pointer\n"); return (0,false); }
    let info=address as *const u8;
    let total=unsafe{read_u32(info,0)};
    if total<16 || total>MAX_BOOT_INFO || (total & 7) != 0 || (address as u64 + total as u64)>0x1_0000_0000 { write_str("[ERR] Invalid Multiboot2 info size\n"); return (0,false); }
    let memory_ready=unsafe{memory::init(info,total as usize)};
    let mut usable=0u64;
    let mut offset=8usize;
    while offset+8<=total as usize {
        let tag=unsafe{info.add(offset)};
        let kind=unsafe{read_u32(tag,0)};
        let size=unsafe{read_u32(tag,4)} as usize;
        if size<8 || offset.checked_add(size).map_or(true,|end|end>total as usize) { write_str("[WARN] Malformed boot tag\n"); break; }
        if kind==0 { break; }
        if kind==6 && size>=16 {
            let entry_size=unsafe{read_u32(tag,8)} as usize;
            if entry_size>=24 {
                let mut pos=16usize;
                while pos+entry_size<=size {
                    let entry=unsafe{tag.add(pos)};
                    let length=unsafe{read_u64(entry,8)};
                    let typ=unsafe{read_u32(entry,16)};
                    if typ==1 { usable=usable.saturating_add(length); }
                    pos+=entry_size;
                }
            }
        } else if kind==8 && size>=38 {
            let address=unsafe{read_u64(tag,8)};
            let pitch=unsafe{read_u32(tag,16)};
            let width=unsafe{read_u32(tag,20)};
            let height=unsafe{read_u32(tag,24)};
            let bpp=unsafe{read_u8(tag,28)};
            let fb_type=unsafe{read_u8(tag,29)};
            if fb_type==1 {
                let red_pos=unsafe{read_u8(tag,32)};
                let red_size=unsafe{read_u8(tag,33)};
                let green_pos=unsafe{read_u8(tag,34)};
                let green_size=unsafe{read_u8(tag,35)};
                let blue_pos=unsafe{read_u8(tag,36)};
                let blue_size=unsafe{read_u8(tag,37)};
                if unsafe{graphics::configure(address,pitch,width,height,bpp,red_pos,red_size,green_pos,green_size,blue_pos,blue_size)} {
                    write_str("[ OK ] RGB framebuffer configured: ");
                    write_dec(width); put_byte(b'x'); write_dec(height); write_str("\n");
                } else { write_str("[WARN] Framebuffer unavailable to 32-bit renderer\n"); }
            } else { write_str("[WARN] Unsupported framebuffer pixel format\n"); }
        }
        offset=(offset+size+7)&!7;
    }
    (usable,memory_ready)
}

fn report_pci() {
    unsafe { device::discover(); }
    let mut count=0u32;
    write_str("\nPCI devices (enumeration only):\n");
    unsafe {
        pci::enumerate(|device| {
            count+=1;
            if device.class==0x03 {
                write_str("  Display ");
                write_hex(device.vendor_id as u32); put_byte(':'); write_hex(device.device_id as u32);
                write_str(" at "); write_hex(((device.bus as u32)<<8)|((device.device as u32)<<3)|device.function as u32);
                write_str("\n");
            }
        });
    }
    write_str("PCI functions enumerated: "); write_dec(count); write_str("\n");
    write_str("Device registry entries: "); write_dec(device::device_count() as u32); write_str(" / "); write_dec(device::MAX_DEVICES as u32); write_str("\n");
    write_str("Registered driver slots: "); write_dec(device::driver_count() as u32); write_str("; bound devices: "); write_dec(device::bound_count() as u32); write_str("\n");
    if device::dropped_count()!=0 { write_str("[WARN] Registry overflow, dropped: "); write_dec(device::dropped_count() as u32); write_str("\n"); }
}

fn report_xhci(){
    write_str("\nXHCI CONTROLLER DISCOVERY\n");
    match xhci::discover(){
        Some(info)=>{
            write_str("[ OK ] xHCI PCI function found at ");
            write_hex(((info.bus as u32)<<8)|((info.device as u32)<<3)|info.function as u32);
            write_str(" vendor/device ");write_hex(info.vendor_id as u32);put_byte(':');write_hex(info.device_id as u32);write_str("\n");
            write_str("  BAR");write_dec(info.bar_index as u32);write_str(" MMIO ");write_hex(info.bar_base);write_str(" version ");write_hex(info.version as u32);write_str("\n");
            write_str("  Slots ");write_dec(info.max_slots as u32);write_str(" interrupters ");write_dec(info.max_interrupters as u32);write_str(" ports ");write_dec(info.max_ports as u32);write_str("\n");
            write_str("  USBSTS ");write_hex(info.usb_status);write_str("\n");
            let health=xhci::ControllerHealth::decode(info.usb_status);
            let mut diagnostics=xhci_runtime::XhciDiagnosticLog::new();
            let mut lifecycle=xhci_runtime::ControllerLifecycle::new();
            let record=lifecycle.observe_health(health,&mut diagnostics,0);
            write_str("  Health: ");
            match record.code {
                xhci_runtime::DiagnosticCode::Healthy=>write_str("HEALTHY"),
                xhci_runtime::DiagnosticCode::HostSystemError=>write_str("HOST SYSTEM ERROR"),
                xhci_runtime::DiagnosticCode::HostControllerError=>write_str("HOST CONTROLLER ERROR"),
                xhci_runtime::DiagnosticCode::SaveRestoreError=>write_str("SAVE/RESTORE ERROR"),
                xhci_runtime::DiagnosticCode::ControllerNotReady=>write_str("CONTROLLER NOT READY"),
                xhci_runtime::DiagnosticCode::UnexpectedHalt=>write_str("UNEXPECTED HALT"),
                xhci_runtime::DiagnosticCode::EventInterruptPending=>write_str("EVENT INTERRUPT PENDING"),
                xhci_runtime::DiagnosticCode::PortChangePending=>write_str("PORT CHANGE PENDING"),
            }
            write_str(" (");
            match record.severity {
                xhci_runtime::DiagnosticSeverity::Informational=>write_str("INFO"),
                xhci_runtime::DiagnosticSeverity::Recoverable=>write_str("RECOVERABLE"),
                xhci_runtime::DiagnosticSeverity::Fatal=>write_str("FATAL"),
            }
            write_str(")\n");
            write_str("  DMA policy: ");
            write_str(if record.severity==xhci_runtime::DiagnosticSeverity::Fatal {"QUARANTINE REQUIRED"} else {"NO DMA ACTION"});
            write_str("\n");
            if info.legacy_support_found{
                write_str("  BIOS ownership: ");write_str(if info.bios_owned{"held"}else{"not held"});
                write_str("; OS ownership: ");write_str(if info.os_owned{"claimed"}else{"not claimed"});write_str("\n");
            }else{write_str("  Legacy Support capability not found\n");}
            if !info.ext_caps_valid{write_str("[WARN] Extended capability chain invalid or outside probe window\n");}
            write_str("  Diagnostic records: ");write_dec(diagnostics.len() as u32);
            write_str("; overwritten: ");write_dec(diagnostics.dropped_count());write_str("\n");
            write_str("[INFO] Snapshot is read-only; no live interrupt polling or recovery was started\n");
        },
        None=>write_str("[INFO] No usable xHCI controller found in the current 32-bit MMIO window\n"),
    }
}

fn report_usb_enumeration(){
    let manager=usb::UsbDeviceManager::new();
    write_str("\nUSB ENUMERATION FRAMEWORK\n");
    write_str("[ OK ] Bounded USB device manager initialized\n");
    write_str("  Root-port capacity: ");write_dec(usb::MAX_ROOT_PORTS as u32);write_str("\n");
    write_str("  Device/slot capacity: ");write_dec(usb::MAX_USB_DEVICES as u32);write_str("\n");
    write_str("  Interface capacity/device: ");write_dec(usb::MAX_INTERFACES as u32);write_str("\n");
    write_str("  Endpoint capacity/device: ");write_dec(usb::MAX_ENDPOINTS as u32);write_str("\n");
    write_str("  Current enumerated devices: ");write_dec(manager.device_count() as u32);write_str("\n");
    write_str("[INFO] Descriptor parser and enumeration state machine are software-only; no USB commands submitted\n");
}

fn initialize_subsystems() {
    if fs::create("BOOT.TXT") {
        let _=fs::write("BOOT.TXT",b"LionOS Rust kernel booted.\n");
        write_str("[ OK ] RAMFS core initialized\n");
    }
    if let Some(id)=task::register(1) {
        write_str("[ OK ] Static task registry initialized; bootstrap task id ");
        write_dec(id); write_str("\n");
    }
    if let Some(framebuffer)=graphics::info() {
        // CPU-rendered diagnostic desktop surface; no GPU acceleration is claimed.
        graphics::fill_rect(0,0,framebuffer.width,framebuffer.height,(18,24,38));
        desktop::draw_window(desktop::Window{
            bounds:desktop::Rect{x:24,y:24,width:framebuffer.width.saturating_sub(48).max(1),height:64},
            color:(35,53,78),visible:true,
        });
        write_str("[ OK ] Software framebuffer renderer active\n");
    }
    if let Some(scancode)=input::poll_scancode() {
        write_str("[ OK ] PS/2 input byte observed: "); write_hex(scancode as u32); write_str("\n");
    } else {
        write_str("[INFO] No PS/2 scancode pending (poll-only input path)\n");
    }
    let _ = net::parse_ethernet(&[]);
    let _ = syscall::dispatch(u32::MAX,0,0,0);
}

#[no_mangle]
pub extern "C" fn rust_kernel_main(magic:u32,multiboot_info:u32)->! {
    unsafe {
        CURSOR=0;
        for i in 0..(VGA_COLUMNS*VGA_ROWS) { write_volatile(VGA_TEXT.add(i),0x0F20); }
        arch::x86::interrupts_disable();
        arch::gdt::init();
    }
    serial_init();
    write_str("\nLionOS Rust Kernel - modular rewrite\n");
    write_str("====================================\n");
    if magic!=MULTIBOOT2_BOOT_MAGIC {
        write_str("[FATAL] Invalid Multiboot2 boot magic: "); write_hex(magic); write_str("\n");
        halt_forever();
    }
    write_str("[ OK ] Multiboot2 entry verified\n");
    unsafe { arch::interrupts::init(); }
    write_str("[ OK ] IDT exception/IRQ stubs installed; legacy PIC remapped and masked\n");
    unsafe { arch::interrupts::timer_init(); }
    write_str("[ OK ] PIT channel 0 programmed at 100 Hz; IRQ0 unmasked\n");
    let cpu=arch::cpu::features();
    write_str("[INFO] CPU features: PSE="); write_str(if cpu.pse {"yes"} else {"no"});
    write_str(" PAE="); write_str(if cpu.pae {"yes"} else {"no"});
    write_str(" APIC="); write_str(if cpu.apic {"yes"} else {"no"});
    write_str(" SSE2="); write_str(if cpu.sse2 {"yes"} else {"no"}); write_str("\n");

    let (usable,memory_ready)=parse_boot_info(multiboot_info);
    if unsafe{paging::init_identity_4g()} {
        write_str("[ OK ] 4 GiB supervisor identity paging enabled (4 MiB PSE pages)\n");
    } else { write_str("[WARN] Paging initialization failed\n"); }
    if memory_ready {
        write_str("[ OK ] Physical frame allocator initialized; usable bytes ");
        write_hex((usable>>32) as u32); write_hex(usable as u32); write_str("\n");
        let stats=memory::stats(usable);
        write_str("[INFO] Free 4 KiB frames: "); write_dec(stats.free_frames); write_str("\n");
        if let Some(frame)=memory::alloc_frame() {
            write_str("[ OK ] Physical frame allocation probe: "); write_hex(frame); write_str("\n");
            unsafe { let _=memory::free_frame(frame); }
        }
    } else { write_str("[WARN] Physical frame allocator not initialized\n"); }

    report_pci();
    report_xhci();
    let xhci_init=xhci::initialize();
    write_str("xHCI initialization result: ");
    match xhci_init.status {
        xhci::InitStatus::NoController=>write_str("no controller"),
        xhci::InitStatus::InvalidBar=>write_str("BAR validation failed"),
        xhci::InitStatus::InvalidCapabilities=>write_str("invalid capability chain/register window"),
        xhci::InitStatus::UnsupportedPageSize=>write_str("controller does not advertise 4 KiB page support; DMA setup skipped"),
        xhci::InitStatus::OwnershipTimeout=>write_str("BIOS ownership timeout; reset skipped"),
        xhci::InitStatus::StopTimeout=>write_str("controller halt timeout; reset skipped"),
        xhci::InitStatus::ResetTimeout=>write_str("host reset timeout"),
        xhci::InitStatus::NotReadyTimeout=>write_str("controller-not-ready timeout"),
        xhci::InitStatus::ReadyStopped=>write_str("ready and halted; DMA not started"),
    }
    write_str("\\n");
    write_str("  OS ownership requested: ");write_str(if xhci_init.ownership_requested{"yes"}else{"not needed"});
    write_str("; BIOS released: ");write_str(if xhci_init.bios_released{"yes"}else{"no/unknown"});
    write_str("; halted: ");write_str(if xhci_init.controller_halted{"yes"}else{"no"});
    write_str("; reset: ");write_str(if xhci_init.reset_completed{"complete"}else{"not complete"});
    write_str("; CNR clear: ");write_str(if xhci_init.controller_ready{"yes"}else{"no/unknown"});write_str("\\n");
    if xhci_init.status==xhci::InitStatus::ReadyStopped {
        if let Some(info)=xhci::discover() {
            match xhci_dma::XhciDma::allocate(info.scratchpad_count,info.context_size) {
                Ok(layout)=>{
                    write_str("[ OK ] xHCI DMA context layout allocated; scratchpads: ");
                    write_dec(layout.scratchpad_count() as u32);
                    write_str("; context bytes: ");
                    write_dec(layout.device_context.device_context_bytes() as u32);
                    write_str("\\n");
                    let programmed=xhci::program_dma_registers(&info,&layout);
                    write_str("  xHCI DMA register setup: ");
                    match programmed {
                        xhci::DmaProgramStatus::ProgrammedStopped=>write_str("DCBAAP/CRCR/ERST configured; controller remains halted"),
                        xhci::DmaProgramStatus::NoController=>write_str("controller disappeared"),
                        xhci::DmaProgramStatus::InvalidCapabilities=>write_str("capability or BAR range validation failed"),
                        xhci::DmaProgramStatus::InvalidDmaLayout=>write_str("DMA alignment/range validation failed"),
                        xhci::DmaProgramStatus::BusMasterDisableFailed=>write_str("could not keep PCI bus mastering disabled"),
                        xhci::DmaProgramStatus::ControllerNotHalted=>write_str("controller is not confirmed halted"),
                        xhci::DmaProgramStatus::CommandRingBusy=>write_str("command ring reports running"),
                        xhci::DmaProgramStatus::MmioUnavailable=>write_str("MMIO register window unavailable"),
                        xhci::DmaProgramStatus::RegisterWriteFailed=>write_str("one or more register writes failed; controller remains halted"),
                    }
                    write_str("\\n");
                    core::mem::forget(layout);
                }
                Err(_)=>write_str("[WARN] xHCI DMA context/scratchpad allocation failed; controller remains halted\\n"),
            }
        }
    }
    initialize_subsystems();

    write_str("\nRUST PORT INVENTORY\n");
    write_str("[ OK ] x86 port I/O, GDT, IDT exception/IRQ dispatch, PIC and PIT timer\n");
    write_str("[ OK ] Multiboot2 memory map and fixed physical-frame allocator\n");
    write_str("[ OK ] PCI discovery, BAR metadata, bounded MMIO and DMA allocation primitives\n");
    write_str("[ OK ] RGB framebuffer software drawing (when supplied below 4 GiB)\n");
    write_str("[ OK ] Bounded RAMFS, static task registry, PS/2 polling primitives\n");
    write_str("[ -- ] Preemptive context switching, user mode and syscall entry not implemented\n");
    write_str("[ OK ] xHCI PCI capability discovery, BAR sizing and ownership/reset state machine\n");
    write_str("[ OK ] xHCI DMA memory layout and halted-register programming: DCBAAP, CRCR, ERSTSZ, ERSTBA, ERDP; no bus mastering, interrupter enable or Run/Stop\\n");
    write_str("[ -- ] xHCI controller DMA activation, interrupter setup, port handling and USB enumeration not implemented\\n");
    write_str("[ -- ] Intel/NVIDIA native GPU command submission not implemented\n");
    write_str("\nLionOS Rust kernel reached subsystem bring-up.\n");
    idle_forever()
}

fn idle_forever()->! { loop { unsafe { arch::x86::interrupts_enable(); arch::x86::halt(); } } }
fn halt_forever()->! { loop { unsafe { arch::x86::interrupts_disable(); arch::x86::halt(); } } }
#[panic_handler]
fn panic(_info:&PanicInfo)->! { serial_byte(b'!'); write_str("\n[FATAL] Rust kernel panic\n"); halt_forever() }

#[no_mangle]
pub unsafe extern "C" fn memset(dst:*mut u8,value:i32,count:usize)->*mut u8 {
    for i in 0..count { write_volatile(dst.add(i),value as u8); } dst
}
#[no_mangle]
pub unsafe extern "C" fn memcpy(dst:*mut u8,src:*const u8,count:usize)->*mut u8 {
    for i in 0..count { write_volatile(dst.add(i),core::ptr::read_volatile(src.add(i))); } dst
}
#[no_mangle]
pub unsafe extern "C" fn memmove(dst:*mut u8,src:*const u8,count:usize)->*mut u8 {
    if (dst as usize)<=(src as usize) {
        for i in 0..count { write_volatile(dst.add(i),core::ptr::read_volatile(src.add(i))); }
    } else {
        for i in (0..count).rev() { write_volatile(dst.add(i),core::ptr::read_volatile(src.add(i))); }
    } dst
}
#[no_mangle]
pub unsafe extern "C" fn memcmp(a:*const u8,b:*const u8,count:usize)->i32 {
    for i in 0..count { let x=core::ptr::read_volatile(a.add(i)); let y=core::ptr::read_volatile(b.add(i)); if x!=y{return x as i32-y as i32;} } 0
}
