#include <stdint.h>
#include <stddef.h>
#include "gdt.h"
#include "idt.h"
#include "paging.h"
#include "heap.h"
#include "pit.h"
#include "memory.h"
#include "tss.h"
#include "process.h"
#include "user.h"
#include "console.h"
#include "framebuffer.h"
#include "ramfs.h"
#include "diskfs.h"
#include "vfs.h"
#include "ipc.h"
#include "pipe.h"
#include "signal.h"
#include "net.h"
#include "shell.h"
#include "gui.h"
#include "debug.h"
#include "cpu.h"
#include "lapic.h"
#include "smp.h"
#include "mouse.h"
#include "xhci.h"
#include "intel_gpu.h"
#include "exec.h"
#include "kernel_diag.h"
#include "pci.h"

void pic_init(void);
void keyboard_init(void);
void syscall_init(void);

static void boot_dec(uint32_t value){
    console_write_dec(value);
}

static void boot_hex32(uint32_t value){
    static const char digits[] = "0123456789ABCDEF";
    for(int shift=28; shift>=0; shift-=4)
        console_putc(digits[(value >> (uint32_t)shift) & 0x0Fu]);
}

static void boot_hex64(uint64_t value){
    boot_hex32((uint32_t)(value >> 32));
    boot_hex32((uint32_t)value);
}

static void cpu_intel_hardware_report(void){
    cpu_identification_t cpu;
    intel_gpu_info_t gpu;
    uint64_t first=0u,last=0u;
    uint32_t ggtt_last=0u;
    cpu_get_identification(&cpu);
    console_write("\n========== CPU / INTEL GRAPHICS HARDWARE REPORT ==========\n");
    console_write("CPU: "); console_write(cpu.brand); console_write("\n");
    console_write("Vendor: "); console_write(cpu.vendor);
    console_write(" Family: "); boot_dec(cpu.family);
    console_write(" Model: "); boot_dec(cpu.model);
    console_write(" Stepping: "); boot_dec(cpu.stepping);
    console_write(" Signature: 0x"); boot_hex32(cpu.signature);
    console_write("\nPhysical cores: ");
    if(cpu.physical_cores) boot_dec(cpu.physical_cores); else console_write("UNKNOWN");
    console_write("  Logical CPUs: "); boot_dec(cpu.logical_processors);
    console_write("  APIC ID: "); boot_dec(cpu.apic_id);
    console_write("\nL1D: "); boot_dec(cpu.l1_data_kb);
    console_write(" KB  L1I: "); boot_dec(cpu.l1_instruction_kb);
    console_write(" KB  L2: "); boot_dec(cpu.l2_kb);
    console_write(" KB  L3: "); boot_dec(cpu.l3_kb); console_write(" KB");
    console_write("\nBase: "); boot_dec(cpu.base_mhz);
    console_write(" MHz  Max: "); boot_dec(cpu.max_mhz);
    console_write(" MHz  BCLK: "); boot_dec(cpu.bus_mhz); console_write(" MHz");
    console_write("\nArchitecture: ");
    console_write(cpu.x86_64 ? "x86-64 capable" : "32-bit only");
    console_write("\nCPU features:");
    if(cpu.feature_flags & CPU_FEATURE_SSE) console_write(" SSE");
    if(cpu.feature_flags & CPU_FEATURE_SSE2) console_write(" SSE2");
    if(cpu.feature_flags & CPU_FEATURE_SSE3) console_write(" SSE3");
    if(cpu.feature_flags & CPU_FEATURE_SSSE3) console_write(" SSSE3");
    if(cpu.feature_flags & CPU_FEATURE_SSE41) console_write(" SSE4.1");
    if(cpu.feature_flags & CPU_FEATURE_SSE42) console_write(" SSE4.2");
    if(cpu.feature_flags & CPU_FEATURE_AVX) console_write(" AVX");
    if(cpu.feature_flags & CPU_FEATURE_APIC) console_write(" APIC");
    console_write("\nCPU feature mask: 0x"); boot_hex32(cpu.feature_flags);
    console_write("\n");

    if(intel_gpu_get_info(&gpu)==0){
        console_write("Intel GPU: 0x"); boot_hex32(gpu.device_id);
        console_write(" Rev 0x"); boot_hex32(gpu.revision);
        console_write(" PCI "); boot_dec(gpu.bus); console_putc(':');
        boot_dec(gpu.slot); console_putc('.'); boot_dec(gpu.function);
        console_write("\nGPU BAR0: 0x"); boot_hex64(gpu.bar0);
        console_write("  BAR0 size: "); boot_dec((uint32_t)(gpu.bar0_size/(1024u*1024u)));
        console_write(" MiB\nMMIO: ");
        console_write(gpu.mmio_mapped ? "MAPPED" : "NOT MAPPED");
        console_write("  Virtual base: 0x"); boot_hex32(gpu.mmio_virtual);
        console_write("\nGraphics acceleration: NOT ACTIVE (firmware scanout + CPU compositor)\n");
        debug_write("LIONOS:INTEL-GPU-ACCELERATION-NOT-ACTIVE\n");
        if(gpu.host_bridge_found){
            console_write("Host bridge: Intel 00:00.0\nGGC: 0x");
            boot_hex32(gpu.host_ggc);
            console_write("  BDSM: 0x"); boot_hex32(gpu.host_bdsm);
            console_write("  BGSM: 0x"); boot_hex32(gpu.host_bgsm);
            console_write("  TOLUD: 0x"); boot_hex32(gpu.host_tolud);
            console_write("\n");
        }else console_write("Host bridge: NOT FOUND\n");
        if(gpu.ggtt_ready){
            console_write("GGTT aperture: BAR0 + 0x");
            boot_hex32(gpu.ggtt_offset);
            console_write("\nGGTT size: ");
            boot_dec(gpu.ggtt_size/(1024u*1024u));
            console_write(" MiB  Read test: ");
            ggtt_last=gpu.ggtt_size/4096u-1u;
            if(intel_gpu_ggtt_read_entry(0u,&first)==0 &&
               intel_gpu_ggtt_read_entry(ggtt_last,&last)==0){
                console_write("PASS\nGGTT first PTE: 0x"); boot_hex64(first);
                console_write("\nGGTT last PTE: 0x"); boot_hex64(last);
                console_write("\n");
                debug_write("LIONOS:HW-REPORT-GGTT-READ-OK\n");
            }else{
                console_write("FAIL\n");
                debug_write("LIONOS:HW-REPORT-GGTT-READ-FAIL\n");
            }
        }else console_write("GGTT: NOT IDENTIFIED\n");
    }else{
        console_write("Intel GPU: NOT FOUND\n");
        debug_write("LIONOS:HW-REPORT-IGPU-NOT-FOUND\n");
    }
    console_write("Framebuffer: ");
    if(framebuffer_available()){
        boot_dec(framebuffer_width()); console_putc('x'); boot_dec(framebuffer_height());
        console_write(" ACTIVE\n");
    }else console_write("UNAVAILABLE\n");
    console_write("High RAM available: ");
    boot_dec((uint32_t)(memory_high_available_bytes()/(1024u*1024u)));
    console_write(" MiB  High RAM free: ");
    boot_dec((uint32_t)(((uint64_t)memory_high_free_pages()*4096u)/(1024u*1024u)));
    console_write(" MiB\n=========================================================\n");
    debug_write("LIONOS:CPU-INTEL-HARDWARE-REPORT-DONE\n");
}

static void vfs_boot_test(void){
    static const char path[]    = "/tests/../vfs-selftest.txt";
    static const char payload[] = "LionOS VFS integration OK\n";
    char buffer[sizeof(payload)];
    int fd = vfs_open(path, 2u);
    if(fd < 0 ||
       vfs_write(fd, payload, sizeof(payload)-1u) != (int)(sizeof(payload)-1u) ||
       vfs_close(fd) < 0){
        debug_write("LIONOS:VFS-TEST-FAIL\n");
        console_write("[ERR] VFS integration self-test\n");
        return;
    }
    fd = vfs_open("/vfs-selftest.txt", 1u);
    if(fd < 0 ||
       vfs_read(fd, buffer, sizeof(buffer)) != (int)(sizeof(payload)-1u) ||
       vfs_close(fd) < 0){
        debug_write("LIONOS:VFS-TEST-FAIL\n");
        console_write("[ERR] VFS integration self-test\n");
        return;
    }
    for(uint32_t i=0;i<sizeof(payload)-1u;++i){
        if(buffer[i] != payload[i]){
            debug_write("LIONOS:VFS-TEST-FAIL\n");
            console_write("[ERR] VFS integration self-test\n");
            return;
        }
    }
    debug_write("LIONOS:VFS-TEST-OK\n");
    console_write("[ OK ] VFS path / per-process descriptor integration\n");
}

static void diskfs_boot_test(void){
    static const char marker[] = "LionOS persistent storage online\n";
    char buffer[sizeof(marker)];
    int size = diskfs_read(".boot", buffer, sizeof(buffer));
    if(size == (int)(sizeof(marker)-1u)){
        uint32_t ok = 1;
        for(uint32_t i=0;i<sizeof(marker)-1u;++i){
            if(buffer[i] != marker[i]){ ok = 0; break; }
        }
        if(ok){
            debug_write("LIONOS:PERSIST-OK\n");
            console_write("[ OK ] Persistent data survived reboot\n");
            return;
        }
    }
    if(diskfs_write(".boot", marker, sizeof(marker)-1u) == 0){
        debug_write("LIONOS:PERSIST-INIT\n");
        console_write("[ OK ] Persistent filesystem initialized\n");
    } else {
        debug_write("LIONOS:PERSIST-FAIL\n");
        console_write("[ERR] Persistent filesystem self-test\n");
    }
}
static void high_memory_selftest(void){
    uint64_t physical=0u;
    uint32_t virtual_address=0u;
    if(page_alloc_high(&physical)!=0){
        console_write("[ -- ] High physical memory allocator: no usable page above 256 MiB\n");
        return;
    }
    if(paging_map_high_page(physical,0x2u,&virtual_address)!=0){
        page_free_high(physical);
        console_write("[ERR] High memory mapping self-test: map failed\n");
        return;
    }
    volatile uint32_t *words=(volatile uint32_t *)(uintptr_t)virtual_address;
    uint32_t ok=1u;
    for(uint32_t i=0u;i<1024u;++i)words[i]=0x4C494F4Eu^(i*0x9E3779B9u);
    for(uint32_t i=0u;i<1024u;++i){
        if(words[i]!=(0x4C494F4Eu^(i*0x9E3779B9u))){ok=0u;break;}
    }
    if(paging_unmap_high_page(virtual_address)!=0){
        console_write("[ERR] High memory mapping self-test: unmap failed; page retained\n");
        return;
    }
    page_free_high(physical);
    if(ok){
        debug_write("LIONOS:HIGH-MEMORY-TEST-OK\n");
        console_write("[ OK ] High physical memory allocate/map/write/read/unmap/free\n");
    }else{
        debug_write("LIONOS:HIGH-MEMORY-TEST-FAIL\n");
        console_write("[ERR] High physical memory data verification failed\n");
    }
}

static int integration_stage;
static int integration_done;
static int integration_pid=-1;
static int integration_vfs_ok;
static int integration_cow_ok;

void lionos_user_integration_step(void){
    if(integration_done)return;

    if(integration_stage==0){
        integration_pid=exec_run_file("vfs_test.elf");
        if(integration_pid>0){
            debug_write("LIONOS:USER-VFS-LAUNCHED\n");
        }
        integration_stage=(integration_pid>0)?1:-1;
        if(integration_stage<0){
            integration_done=1;
            debug_write("LIONOS:USER-VFS-LAUNCH-FAIL\n");
        }
        return;
    }

    if(integration_stage==1){
        int32_t s=process_get_state((uint32_t)integration_pid);
        if(s==PROCESS_ZOMBIE){
            uint32_t code=0xFFFFFFFFu;
            integration_vfs_ok=(process_get_exit_code((uint32_t)integration_pid,&code)==0&&code==0u);
            process_reap_pid((uint32_t)integration_pid);
            integration_pid=-1;
            debug_write("LIONOS:USER-VFS-DONE\n");
            integration_stage=2;
        }else if(s<0){
            integration_done=1;
            debug_write("LIONOS:USER-VFS-STATE-FAIL\n");
        }
        return;
    }

    if(integration_stage==2){
        integration_pid=exec_run_file("cow_test.elf");
        if(integration_pid>0)debug_write("LIONOS:USER-COW-LAUNCHED\n");
        if(integration_pid<0){
            integration_done=1;
            debug_write("LIONOS:USER-COW-LAUNCH-FAIL\n");
            return;
        }
        integration_stage=3;
        return;
    }

    if(integration_stage==3){
        int32_t s=process_get_state((uint32_t)integration_pid);
        if(s==PROCESS_ZOMBIE){
            uint32_t code=0xFFFFFFFFu;
            integration_cow_ok=(process_get_exit_code((uint32_t)integration_pid,&code)==0&&code==0u);
            process_reap_pid((uint32_t)integration_pid);
            integration_done=1;
            if(integration_vfs_ok&&integration_cow_ok){
                debug_write("LIONOS:VFS-USER-TEST-OK\n");
                debug_write("LIONOS:COW-TEST-OK\n");
                console_write("[ OK ] Automated VFS + userspace COW integration tests\n");
            }else{
                debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
                console_write("[ERR] Automated userspace integration tests\n");
            }
        }else if(s<0){
            integration_done=1;
            debug_write("LIONOS:USER-COW-STATE-FAIL\n");
        }
    }
}


void kernel_main(uint32_t magic, uint32_t multiboot_info){
#define BOOT_STAGE(percent, label) framebuffer_boot_splash((percent), (label))

    debug_write("LIONOS:BOOT\n");
    console_init();
    console_write("LionOS kernel booting...\n\n");

    if(magic != 0x36D76289u){
        debug_write("LIONOS:BAD-MULTIBOOT\n");
        console_write("ERROR: invalid Multiboot2 magic.\n");
        for(;;) __asm__ volatile("cli; hlt");
    }

    /* ---- CPU / GDT / TSS ---- */
    gdt_init();
    console_write("[ OK ] GDT / ring-3 segments\n");
    BOOT_STAGE(6u, "GDT READY");

    tss_init();
    console_write("[ OK ] TSS / ring-0 stack\n");
    BOOT_STAGE(10u, "KERNEL STACK READY");

    /* ---- Interrupts / timers / input ---- */
    idt_init();
    console_write("[ OK ] IDT / CPU exceptions / DPL3 syscall\n");
    BOOT_STAGE(14u, "INTERRUPTS READY");

    pic_init();
    console_write("[ OK ] PIC remapped\n");
    BOOT_STAGE(18u, "CLOCK READY");

    pit_init(100);
    console_write("[ OK ] PIT 100 Hz / preemptive scheduler clock\n");

    keyboard_init();
    console_write("[ OK ] PS/2 keyboard / scancode input buffer\n");
    BOOT_STAGE(22u, "INPUT READY");

    mouse_init();
    console_write("[ OK ] PS/2 mouse / polled input\n");

    /* ---- Memory / paging ---- */
    memory_init(multiboot_info);
    console_write("[ OK ] Physical memory manager\n");
    console_write("       Total pages: ");
    boot_dec(memory_total_pages());
    console_write("  Free pages: ");
    boot_dec(memory_free_pages());
    console_putc('\n');
    BOOT_STAGE(28u, "MEMORY READY");

    if(framebuffer_prepare(multiboot_info) == 0)
        BOOT_STAGE(32u, "DISPLAY DETECTED");

    paging_init();

    /* Enumerate real PCI configuration space before device-driver probing. */
    if(pci_init() == 0)
        pci_print_devices();
    else
        console_write("[ -- ] PCI hardware enumeration unavailable\n");

    if(framebuffer_init(multiboot_info) == 0){
        console_write("[ OK ] Framebuffer ");
        boot_dec(framebuffer_width());
        console_putc('x');
        boot_dec(framebuffer_height());
        console_write(" desktop mode\n");
        /* Keep boot diagnostics on VGA while the full-screen splash remains visible. */
        BOOT_STAGE(34u, "DISPLAY READY");
    }

    console_write("[ .. ] Initializing Intel graphics PCI/MMIO bootstrap...\n");
    if (intel_gpu_init(framebuffer_available() ? 1u : 0u) == 0 && intel_gpu_mmio_ready()) {
        console_write("[ OK ] Intel GPU MMIO ready; CPU compositor remains active\n");
    } else {
        console_write("[ -- ] Intel GPU MMIO unavailable; firmware framebuffer remains active\n");
    }

    console_write("[ OK ] Paging / supervisor kernel mappings\n");
    BOOT_STAGE(38u, "PAGING READY");

    heap_init();
    console_write("[ OK ] Kernel heap / PMM-backed kmalloc + kfree\n");

    /* ---- USB xHCI HID input ---- */
    int usb_mouse_rc = mouse_usb_init();
    if(usb_mouse_rc == 0){
        console_write("[ OK ] USB xHCI / HID boot mouse\n");
    }else{
        xhci_mouse_debug_info_t dbg;
        if(xhci_mouse_debug_get(&dbg) == 0){
            console_write("[ -- ] USB xHCI stage: ");
            console_write(dbg.stage ? dbg.stage : "?");
            console_write(" (vid=0x");
            console_write_hex(dbg.vid);
            console_write(" pid=0x");
            console_write_hex(dbg.pid);
            console_write(" port=");
            console_write_dec(dbg.port);
            console_write(" speed=");
            console_write_dec(dbg.speed);
            console_write(" portsc=0x");
            console_write_hex(dbg.portsc);
            console_write(")\n");
        }
        console_write("[ -- ] USB xHCI / HID mouse unavailable; keyboard/input paths continue\n");
    }

    /* Keyboard discovery is independent of mouse discovery. This is
       important on machines with a USB keyboard but no usable USB mouse,
       and for composite HID devices where both interfaces share a slot. */
    if(xhci_keyboard_init() == 0)
        console_write("[ OK ] USB xHCI / HID boot keyboard\n");
    else
        console_write("[ -- ] USB xHCI / HID keyboard unavailable\n");

    BOOT_STAGE(42u, "HEAP READY");

    /* ---- CPU topology / APIC ---- */
    cpu_init(multiboot_info);

    /*
     * The boot splash is drawn directly into the framebuffer, while the
     * text console initially targets VGA text memory. Switch the console
     * before printing the hardware report so the report is actually visible
     * on the laptop panel. Clear the splash canvas once; later boot status
     * updates remain below the report in the framebuffer text console.
     */
    if(framebuffer_available()){
        console_use_framebuffer();
        console_clear();
        console_set_color(CONSOLE_COLOR_WHITE);
    }
    cpu_intel_hardware_report();
    console_write("[ OK ] CPU topology / BSP APIC ID ");
    boot_dec(cpu_get(0)->apic_id);
    console_write(" / logical hint ");
    boot_dec(cpu_count_hint());
    console_write("\n");
    console_write("[ OK ] CPU SIMD features: 0x");
    boot_hex32(cpu_feature_flags());
    console_write("\n");
    BOOT_STAGE(46u, "CPU TOPOLOGY READY");

    uint32_t lapic_ready = 0;
    if(lapic_init() == 0){
        lapic_ready = 1;
        console_write("[ OK ] Local APIC / BSP enabled\n");
        BOOT_STAGE(50u, "PROCESSORS STARTING");

        smp_init();
        console_write("[ OK ] SMP online CPUs: ");
        boot_dec(smp_online_count());
        console_write("\n");
        console_write(smp_lock_selftest()
            ? "[ OK ] SMP spinlock / IRQ-save synchronization\n"
            : "[ERR] SMP spinlock self-test\n");
        if(lapic_frame_timer_init(120u)==0)
            console_write("[ OK ] Calibrated 120 Hz BSP frame timer\n");
        else
            console_write("[ -- ] BSP frame timer unavailable; PIT fallback enabled\n");
        BOOT_STAGE(56u, "PROCESSORS READY");
    } else {
        console_write("[ -- ] Local APIC unavailable; SMP disabled\n");
        debug_write("LIONOS:SMP-DISABLED\n");
        BOOT_STAGE(56u, "SINGLE CPU MODE");
    }

    high_memory_selftest();

    /* ---- Syscalls / processes ---- */
    syscall_init();
    console_write("[ OK ] Syscall ABI / process / VFS / IPC / signals / networking\n");
    BOOT_STAGE(62u, "KERNEL SERVICES READY");

    process_init();
    console_write("[ OK ] Process table / MLFQ scheduler / per-CPU run queues\n");
    console_write(cpu_fpu_sse_init()==0 ? "[ OK ] FPU / SSE context switching enabled\n" : "[ -- ] FPU / SSE unavailable\n");
    kernel_log(KLOG_INFO,"kernel services online");
    kernel_diag_print();
    BOOT_STAGE(68u, "PROCESS MANAGER READY");

    if(lapic_ready){
        if(lapic_frame_timer_active())
            console_write("[ OK ] PIT 100 Hz scheduler / LAPIC 120 Hz GUI + USB input clock\n");
        else
            console_write("[ OK ] PIT 100 Hz scheduler + GUI/input fallback clock\n");
        BOOT_STAGE(72u, "SYSTEM TIMER READY");
    }
    debug_write(lapic_frame_timer_active()
        ? "LIONOS:GUI-FRAME-TIMER-120HZ\n"
        : "LIONOS:GUI-FRAME-TIMER-PIT-FALLBACK\n");

    /* ---- Filesystems ---- */
    ramfs_init();
    console_write("[ OK ] RAM filesystem / embedded programs\n");
    kernel_log(KLOG_INFO, "RAM filesystem initialized");
    BOOT_STAGE(78u, "RAM FILESYSTEM READY");

    if(diskfs_init() == 0){
        console_write("[ OK ] ATA PIO / persistent LionFS\n");
        kernel_log(KLOG_INFO, "Persistent LionFS available");
        diskfs_boot_test();
    } else {
        console_write("[ -- ] Persistent disk unavailable (RAMFS only)\n");
        debug_write("LIONOS:NO-DISK\n");
        kernel_log(KLOG_WARN, "Persistent disk unavailable; using RAMFS");
    }
    BOOT_STAGE(84u, "STORAGE READY");

    vfs_init();
    console_write("[ OK ] VFS / unified file descriptor layer\n");
    if (kernel_log_persistence_init() == 0) {
        console_write("[ OK ] System Log / RAM history + /var/log/lionos.log\n");
    } else {
        console_write("[ -- ] Persistent System Log unavailable; RAM history remains active\n");
        debug_write("LIONOS:PERSIST-LOG-INIT-FAIL\n");
    }
    kernel_log(KLOG_INFO, "Virtual filesystem initialized");
    vfs_boot_test();

    ipc_init();
    pipe_init();
    console_write("[ OK ] IPC / kernel message queues\n");
    BOOT_STAGE(89u, "FILESYSTEM SERVICES READY");

    signal_init();
    console_write("[ OK ] Signals / process control\n");

    net_init();
    if(net_physical_ready()){
        console_write("[ OK ] RTL8139 Ethernet / IPv4 / ARP / ICMP\n");
        kernel_log(KLOG_INFO, "Physical network interface initialized");
    }else{
        console_write("[ -- ] Physical NIC unavailable; loopback only\n");
        kernel_log(KLOG_WARN, "No physical NIC; loopback only");
    }
    BOOT_STAGE(94u, "NETWORK READY");

    /* ---- Desktop ---- */
    console_write("\nLionOS is ready.\n");
    kernel_log(KLOG_INFO, "LionOS boot completed; starting desktop");
    debug_write("LIONOS:READY\n");
    BOOT_STAGE(100u, "STARTING LIONOS DESKTOP");

    /* Switch to the graphical console only after the splash has finished. */
    console_use_framebuffer();
    __asm__ volatile("sti");

    /*
     * Sample interrupt counters only after IF is enabled. Wait briefly for
     * two timer events, with a hard bound so a missing timer cannot stall boot.
     */
    if(lapic_frame_timer_active()){
        uint32_t start_ticks=lapic_frame_ticks();
        for(uint32_t guard=0u;guard<50000000u &&
            (uint32_t)(lapic_frame_ticks()-start_ticks)<2u;++guard)
            __asm__ volatile("pause");
    }else{
        for(uint32_t guard=0u;guard<50000000u &&
            kernel_diag_interrupt_count(0u,32u)<2u;++guard)
            __asm__ volatile("pause");
    }
    kernel_diag_print_interrupts();
    gui_run();
    shell_run();
    for(;;) __asm__ volatile("hlt");

#undef BOOT_STAGE
}
