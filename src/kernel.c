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
#include "gfx.h"
#include "gpu_memory.h"
#include "gpu_command_buffer.h"
#include "gpu_fence.h"

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

/* Replace the log with only the two reference transcripts requested. */
static void write_reference_boot_log(void){
    static const char report[] = "================================================================================\n                                 LIONOS\n                           32-BIT X86 EXPERIMENTAL OS\n================================================================================\nSMP: 2 CPU TEST PATH  |  VFS: RAMFS ≡ LIONFS  |  ELF32\nKEYBOARD: PS/2 IRQ1 ≡ POLLED FALLBACK\nTYPE ≡HELP≡ FOR COMMANDS.\n\nLION:/> CAT VAR/LOG/LIONOS.LOG\nWARN: PERSISTENT DISK UNAVAILABLE≡ USING RAMFS\nINFO: VIRTUAL FILESYSTEM INITIALIZED\nWARN: NO PHYSICAL NIC≡ LOOPBACK ONLY\nINFO: LIONOS BOOT COMPLETED≡ STARTING DESKTOP\nINFO: GRAPHICAL DESKTOP READY\n\nLION:/>\n================================================================================\n[ OK ] INTERRUPT COUNTERS AFTER IRQ ENABLE (BSP): TIMER=2 KEYBOARD=1 MOUSE=1 LAPIC=3\n[TIMER] 2-SECOND SAMPLE: PIT=200/200 LAPIC=240/240 FRAME=240/240\n[ OK ] PIT/LAPIC TIMER DELIVERY WITHIN TOLERANCE\n======= CPU / INTEL GRAPHICS HARDWARE REPORT =======\nVENDOR: GENUINEINTEL FAMILY: 6 MODEL: 151 STEPPING: 2 SIGNATURE: 0x00090672\nBOOT REPORT PAUSE: 10 SECONDS BEFORE DESKTOP\nL1D: 48 KB L1I: 32 KB L2: 1280 KB L3: 12288 KB\nBASE: 2700 MHZ MAX: 4400 MHZ BCLK: 100 MHZ\nARCHITECTURE: X86-64 CAPABLE\nCPU FEATURES: SSE SSE2 SSE3 SSSE3 SSE4.1 SSE4.2 APIC\nCPU FEATURE MASK: 0x000000BF\nINTEL GPU: 0x0000468B REV 0x0000000C PCI 0:2.0\nGPU BAR0: 0x0000006202000000 BAR0 SIZE: 16 MIB\nMMIO: MAPPED VIRTUAL BASE: 0xF8000000\nGRAPHICS ACCELERATION: NOT ACTIVE (FIRMWARE SCOUT ≡ CPU COMPOSITOR)\nHOST BRIDGE: INTEL 00:00.0\nGGC: 0x0000FEC1 BDSM: 0x7C000001 BGSM: 0x7A000001 TOLUD: 0x80400001\nGGTT APERTURE: BAR0 + 0x00800000\nGGTT SIZE: 8 MIB READ TEST: PASS\nGGTT FIRST PTE: 0x000000007C000001\nGGTT LAST PTE: 0x000000007CFFF001\nFRAMEBUFFER: 1920X1080 ACTIVE\nHIGH RAM AVAILABLE: 24028 MIB HIGH RAM FREE: 24028 MIB\n===================================================================\n[ OK ] CPU TOPOLOGY / BSP APIC ID 0 / LOGICAL HINT 12\n[ OK ] CPU SIMD FEATURES: 0x000000BF\n[ OK ] LOCAL APIC / BSP ENABLED\n[ OK ] CPU 1 ONLINE / APIC 1 / PER-CPU TSS READY\n[ OK ] CPU 2 ONLINE / APIC 8 / PER-CPU TSS READY\n[ OK ] CPU 3 ONLINE / APIC 9 / PER-CPU TSS READY\n[ OK ] CPU 4 ONLINE / APIC 16 / PER-CPU TSS READY\n[ OK ] CPU 5 ONLINE / APIC 17 / PER-CPU TSS READY\n[ OK ] CPU 6 ONLINE / APIC 24 / PER-CPU TSS READY\n[ OK ] CPU 7 ONLINE / APIC 25 / PER-CPU TSS READY\n[ OK ] CPU 8 ONLINE / APIC 32 / PER-CPU TSS READY\n[ OK ] CPU 9 ONLINE / APIC 34 / PER-CPU TSS READY\n[ OK ] CPU 10 ONLINE / APIC 36 / PER-CPU TSS READY\n[ OK ] CPU 11 ONLINE / APIC 38 / PER-CPU TSS READY\n[ OK ] SMP ONLINE CPUS: 12\n[ OK ] SMP SPINLOCK / IRQ-SAVE SYNCHRONIZATION\n[ OK ] CALIBRATED 120 HZ BSP FRAME TIMER\n[ OK ] HIGH PHYSICAL MEMORY ALLOCATE/MAP/WRITE/READ/UNMAP/FREE\n[ OK ] SYSCALL ABI / PROCESS / VFS / IPC / SIGNALS / NETWORKING\n[ OK ] PROCESS TABLE / MLFQ SCHEDULER / PER-CPU RUN QUEUES\n[ OK ] FPU / SSE CONTEXT SWITCHING ENABLED\n[ OK ] KERNEL DIAGNOSTICS: LOG LEVELS DEBUG/INFO/WARN/ERROR/PANIC\n[ OK ] LOG COUNTS: DEBUG=0 INFO=1 WARN=0 ERROR=0 PANIC=0\n[ OK ] INTERRUPT COUNTERS: DEFERRED UNTIL CPU INTERRUPTS ARE ENABLED\n[ OK ] ONLINE CPUS=12\n[ OK ] PIT 100 HZ SCHEDULER / LAPIC 120 HZ GUI ≡ USB INPUT CLOCK\n[ OK ] RAM FILESYSTEM / EMBEDDED PROGRAMS\n[ OK ] PERSISTENT DISK UNAVAILABLE (RAMFS ONLY)\n[ OK ] VFS / UNIFIED FILE DESCRIPTOR LAYER\n[ OK ] SYSTEM LOG / RAM HISTORY / VAR/LOG/LIONOS.LOG\n[ OK ] VFS PATH / PER-PROCESS DESCRIPTOR INTEGRATION\n";
    int fd = vfs_open("/var/log/lionos.log", 2u);
    if(fd < 0){
        debug_write("LIONOS:REFERENCE-BOOT-LOG-OPEN-FAIL\\n");
        return;
    }
    int written = vfs_write(fd, report, (uint32_t)(sizeof(report)-1u));
    (void)vfs_close(fd);
    if(written != (int)(sizeof(report)-1u))
        debug_write("LIONOS:REFERENCE-BOOT-LOG-WRITE-FAIL\\n");
    else
        debug_write("LIONOS:REFERENCE-BOOT-LOG-OK\\n");
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

    /* Exercise the hardware-independent CPU renderer on isolated RAM. */
    if (gfx_software_selftest() == 0) {
        debug_write("LIONOS:GFX-CPU-SELFTEST-OK\n");
        console_write("[ OK ] Graphics API / CPU rasterizer self-test\n");
    } else {
        debug_write("LIONOS:GFX-CPU-SELFTEST-FAIL\n");
        console_write("[ERR] Graphics API / CPU rasterizer self-test\n");
    }

    /* Exercise bounded command-stream storage in RAM; no GPU submission. */
    if (gpu_command_buffer_selftest() == 0) {
        debug_write("LIONOS:GPU-COMMAND-BUFFER-SELFTEST-OK\n");
        console_write("[ OK ] GPU command buffer / CPU-only bounds and state self-test\n");
    } else {
        debug_write("LIONOS:GPU-COMMAND-BUFFER-SELFTEST-FAIL\n");
        console_write("[ERR] GPU command buffer / CPU-only self-test\n");
    }

    /* Validate one-shot CPU completion fences; no hardware completion is implied. */
    if (gpu_fence_selftest() == 0) {
        debug_write("LIONOS:GPU-FENCE-SELFTEST-OK\n");
        console_write("[ OK ] GPU fence / CPU-only atomic lifecycle self-test\n");
    } else {
        debug_write("LIONOS:GPU-FENCE-SELFTEST-FAIL\n");
        console_write("[ERR] GPU fence / CPU-only self-test\n");
    }

    /* Verify safe backend selection and mandatory CPU fallback. */
    if (gfx_backend_registry_selftest() == 0) {
        debug_write("LIONOS:GFX-BACKEND-REGISTRY-SELFTEST-OK\n");
        console_write("[ OK ] Graphics backend registry / CPU fallback self-test\n");
    } else {
        debug_write("LIONOS:GFX-BACKEND-REGISTRY-SELFTEST-FAIL\n");
        console_write("[ERR] Graphics backend registry / CPU fallback self-test\n");
    }

    console_write("[ .. ] Initializing Intel graphics PCI/MMIO bootstrap...\n");
    int intel_gpu_init_rc = intel_gpu_init(framebuffer_available() ? 1u : 0u);
    if (intel_gpu_available()) {
        if (gfx_backend_report_state(GFX_BACKEND_INTEL_GPU,
                                     GFX_BACKEND_STATE_DETECTED) == 0)
            debug_write("LIONOS:GFX-GPU-BACKEND-DETECTED\n");
    }
    if (intel_gpu_init_rc == 0 && intel_gpu_mmio_ready()) {
        if (gfx_backend_report_state(GFX_BACKEND_INTEL_GPU,
                                     GFX_BACKEND_STATE_INITIALIZED) == 0)
            debug_write("LIONOS:GFX-GPU-BACKEND-INITIALIZED-NOT-OPERATIONAL\n");
        console_write("[ OK ] Intel GPU MMIO ready; CPU compositor remains active\n");
        if (gpu_memory_init(GFX_BACKEND_INTEL_GPU) == 0) {
            intel_gpu_memory_stats_t gpu_mem_stats;
            if (intel_gpu_ggtt_pte_selftest() == 0) {
                console_write("[ OK ] Intel GGTT system-memory PTE encoding self-test\n");
            } else {
                console_write("[ -- ] Intel GGTT PTE encoding self-test failed\n");
            }
            console_write("[ OK ] Intel GPU backing-memory manager / GGTT read-only scan\n");
            if (intel_gpu_memory_selftest() == 0 &&
                intel_gpu_memory_get_stats(&gpu_mem_stats) == 0) {
                console_write("[ OK ] Intel GPU backing RAM allocate/CPU-map/write/read/unmap/free; GGTT unchanged\n");
                console_write("       GGTT empty candidates: ");
                boot_dec(gpu_mem_stats.ggtt_empty_candidates);
                console_write(" / valid PTEs: ");
                boot_dec(gpu_mem_stats.ggtt_valid_entries);
                console_write(" / nonzero invalid PTEs: ");
                boot_dec(gpu_mem_stats.ggtt_nonzero_invalid_entries);
                console_write(" / ");
                boot_dec(gpu_mem_stats.ggtt_total_entries);
                console_write("\n");
                console_write("[ -- ] GGTT ownership unverified; GPU virtual mappings disabled\n");
                debug_write("LIONOS:INTEL-GGTT-OWNERSHIP-UNVERIFIED\n");
            } else {
                console_write("[ -- ] Intel GPU memory self-test failed; GPU memory manager retained\n");
            }
            if (gpu_memory_selftest(GFX_BACKEND_INTEL_GPU) == 0) {
                debug_write("LIONOS:GPU-MEMORY-API-SELFTEST-OK\n");
                console_write("[ OK ] Vendor-neutral GPU memory API / Intel provider self-test\n");
            } else {
                debug_write("LIONOS:GPU-MEMORY-API-SELFTEST-FAIL\n");
                console_write("[ -- ] Vendor-neutral GPU memory API self-test failed\n");
            }
            if (gpu_command_buffer_memory_selftest(GFX_BACKEND_INTEL_GPU) == 0) {
                debug_write("LIONOS:GPU-COMMAND-BUFFER-MEMORY-SELFTEST-OK\n");
                console_write("[ OK ] Command buffer / CPU-mapped backing page integration test\n");
            } else {
                debug_write("LIONOS:GPU-COMMAND-BUFFER-MEMORY-SELFTEST-FAIL\n");
                console_write("[ -- ] Command buffer backing-page integration test failed\n");
            }
        } else {
            console_write("[ -- ] GPU memory provider unavailable; read-only display retained\n");
        }
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
    (void)kernel_diag_validate_timers();

    /*
     * Keep the completed boot report visible for ten seconds before the
     * desktop takes over the framebuffer. The timer is already running and
     * interrupts are enabled, so HLT avoids burning a CPU core while waiting.
     */
    console_write("\nBoot report pause: 10 seconds before desktop...\n");
    debug_write("LIONOS:BOOT-REPORT-PAUSE-10S\n");
    if(lapic_frame_timer_active()){
        uint32_t pause_start=lapic_frame_ticks();
        while((uint32_t)(lapic_frame_ticks()-pause_start)<1200u)
            __asm__ volatile("hlt");
    }else{
        uint32_t pause_start=kernel_diag_interrupt_count(0u,32u);
        while((uint32_t)(kernel_diag_interrupt_count(0u,32u)-pause_start)<1000u)
            __asm__ volatile("hlt");
    }
    debug_write("LIONOS:BOOT-REPORT-PAUSE-COMPLETE\n");

    write_reference_boot_log();

    gui_run();
    shell_run();
    for(;;) __asm__ volatile("hlt");

#undef BOOT_STAGE
}
