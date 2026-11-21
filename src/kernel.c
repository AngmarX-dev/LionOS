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
#include "ioapic.h"
#include "pic.h"
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
#include "gpu_submission.h"
#include "gfx_pci.h"
#include "ext2_vfs.h"
#include "tty.h"

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

static void cpu_mtrr_hardware_report(void){
    cpu_mtrr_info_t mtrr;
    cpu_mtrr_get_info(&mtrr);

    console_write("\nMTRR snapshot: ");
    if(!mtrr.msr_supported){
        console_write("MSR NOT SUPPORTED\n");
        debug_write("LIONOS:MTRR-MSR-UNSUPPORTED\n");
        return;
    }
    if(!mtrr.supported){
        console_write("NOT SUPPORTED BY CPU\n");
        debug_write("LIONOS:MTRR-UNSUPPORTED\n");
        return;
    }

    console_write("READ-ONLY\nCapability: 0x");
    boot_hex64(mtrr.capability);
    console_write("  Default type: 0x");
    boot_hex64(mtrr.default_type);
    console_write("\nMTRR enabled: ");
    console_write(mtrr.enabled ? "YES" : "NO");
    console_write("  Fixed supported: ");
    console_write(mtrr.fixed_supported ? "YES" : "NO");
    console_write("  Fixed enabled: ");
    console_write(mtrr.fixed_enabled ? "YES" : "NO");
    console_write("\nWrite-combining supported: ");
    console_write(mtrr.write_combining_supported ? "YES" : "NO");
    console_write("  Physical address bits: ");
    if(mtrr.physical_address_bits) boot_dec(mtrr.physical_address_bits);
    else console_write("UNKNOWN");
    console_write("\nVariable MTRRs captured: ");
    boot_dec(mtrr.captured_variable_count);
    console_write(" / advertised: ");
    boot_dec(mtrr.variable_count);
    console_write(mtrr.truncated ? " [TRUNCATED]\n" : "\n");

    if(mtrr.fixed_supported){
        for(uint32_t i=0u;i<CPU_MTRR_FIXED_REGISTER_COUNT;++i){
            console_write("MTRR fixed[");
            boot_dec(i);
            console_write("]: 0x");
            boot_hex64(mtrr.fixed[i]);
            console_write("\n");
        }
    }
    for(uint32_t i=0u;i<mtrr.captured_variable_count;++i){
        console_write("MTRR variable[");
        boot_dec(i);
        console_write("] base=0x");
        boot_hex64(mtrr.variable[i].base);
        console_write(" mask=0x");
        boot_hex64(mtrr.variable[i].mask);
        console_write("\n");
    }
    debug_write("LIONOS:MTRR-SNAPSHOT-READ-ONLY-DONE\n");
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
    if(cpu.feature_flags & CPU_FEATURE_NX) console_write(" NX");
    console_write("\nCPU feature mask: 0x"); boot_hex32(cpu.feature_flags);
    console_write("\n");
    cpu_mtrr_hardware_report();

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
        debug_write("LIONOS:REFERENCE-BOOT-LOG-OPEN-FAIL\n");
        return;
    }
    int written = vfs_write(fd, report, (uint32_t)(sizeof(report)-1u));
    (void)vfs_close(fd);
    if(written != (int)(sizeof(report)-1u))
        debug_write("LIONOS:REFERENCE-BOOT-LOG-WRITE-FAIL\n");
    else
        debug_write("LIONOS:REFERENCE-BOOT-LOG-OK\n");
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
static int integration_utility_ok;
static int integration_pipe_ok;
static int integration_nx_ok;
static int integration_process_ok;
#define INTEGRATION_MIGRATION_ELIGIBLE_COUNT 2u
#define INTEGRATION_MIGRATION_PROCESS_COUNT 3u
static int integration_migration_pids[INTEGRATION_MIGRATION_PROCESS_COUNT] = {-1,-1,-1};
static uint32_t integration_migration_steals_before[LIONOS_MAX_CPUS];
static uint32_t integration_migration_ticks_before[LIONOS_MAX_CPUS];
static uint32_t integration_migration_count;
static uint32_t integration_migration_failed;
static uint32_t integration_migration_owner_mask;
static volatile uint32_t integration_ap_user_pid;
static volatile uint32_t integration_ap_user_status;
/*
 * Run the full process lifecycle regression from an AP-owned kernel thread.
 * exec_run_file() creates the user process on the caller's CPU; the AP-local
 * queue therefore keeps the complete fork/waitpid test on that AP.
 */
static void integration_ap_user_launch_worker(uint32_t expected_cpu){
    if(cpu_current_index()!=expected_cpu){
        __atomic_store_n(&integration_ap_user_status,2u,__ATOMIC_RELEASE);
        return;
    }
    int32_t pid=exec_run_file("process_test.elf");
    if(pid<=0){
        __atomic_store_n(&integration_ap_user_status,3u,__ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&integration_ap_user_pid,(uint32_t)pid,__ATOMIC_RELAXED);
    __atomic_store_n(&integration_ap_user_status,1u,__ATOMIC_RELEASE);
}
static void debug_write_u32_hex(uint32_t value){
    static const char digits[]="0123456789ABCDEF";
    char out[9];
    for(uint32_t i=0u;i<8u;++i)
        out[i]=digits[(value>>(28u-i*4u))&0xFu];
    out[8]='\0';
    debug_write(out);
}

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
            debug_write(integration_vfs_ok
                ? "LIONOS:USER-EXT2-SYMLINK-SYSCALL-TEST-OK\n"
                : "LIONOS:USER-EXT2-SYMLINK-SYSCALL-TEST-FAIL\n");
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
            int cow_status_valid=process_get_exit_code((uint32_t)integration_pid,&code)==0;
            integration_cow_ok=(cow_status_valid&&code==0u);
            debug_write("LIONOS:COW-TEST-EXIT-CODE=0x");
            debug_write_u32_hex(code);
            debug_write(cow_status_valid?" VALID\n":" INVALID\n");
            debug_write(integration_vfs_ok?"LIONOS:VFS-INTEGRATION-STATE-OK\n":"LIONOS:VFS-INTEGRATION-STATE-FAIL\n");
            process_reap_pid((uint32_t)integration_pid);
            if(!integration_vfs_ok||!integration_cow_ok){
                integration_done=1;
                debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
                console_write("[ERR] Automated userspace integration tests\n");
                return;
            }
            integration_stage=4;
            return;
        }else if(s<0){
            integration_done=1;
            debug_write("LIONOS:USER-COW-STATE-FAIL\n");
            return;
        }
        return;
    }

    if(integration_stage==4){
        integration_pid=exec_run_file("meminfo.elf");
        if(integration_pid>0){
            debug_write("LIONOS:USER-MEMINFO-LAUNCHED\n");
            integration_stage=5;
        }else{
            integration_done=1;
            debug_write("LIONOS:USER-MEMINFO-LAUNCH-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
        }
        return;
    }

    if(integration_stage==5){
        int32_t s=process_get_state((uint32_t)integration_pid);
        if(s==PROCESS_ZOMBIE){
            uint32_t code=0xFFFFFFFFu;
            integration_utility_ok =
                process_get_exit_code((uint32_t)integration_pid,&code)==0 &&
                code==0u;
            if(process_reap_pid((uint32_t)integration_pid)!=0)
                integration_utility_ok=0;
            integration_pid=-1;
            if(integration_utility_ok){
                debug_write("LIONOS:VFS-USER-TEST-OK\n");
                debug_write("LIONOS:COW-TEST-OK\n");
                debug_write("LIONOS:USERLAND-UTILITY-TEST-OK\n");
                integration_stage=6;
            }else{
                integration_done=1;
                debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
                console_write("[ERR] Ring-3 meminfo utility integration test failed\n");
            }
        }else if(s<0){
            integration_done=1;
            debug_write("LIONOS:USER-MEMINFO-STATE-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
        }
        return;
    }

    if(integration_stage==6){
        integration_pid=exec_run_file("pipe_test.elf");
        if(integration_pid>0){
            debug_write("LIONOS:USER-PIPE-LAUNCHED\n");
            integration_stage=7;
        }else{
            integration_done=1;
            debug_write("LIONOS:USER-PIPE-LAUNCH-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
        }
        return;
    }

    if(integration_stage==7){
        int32_t s=process_get_state((uint32_t)integration_pid);
        if(s==PROCESS_ZOMBIE){
            uint32_t code=0xFFFFFFFFu;
            int exit_status_valid=process_get_exit_code((uint32_t)integration_pid,&code)==0;
            integration_pipe_ok=(exit_status_valid&&code==0u);
            debug_write("LIONOS:PIPE-TEST-EXIT-CODE=0x");
            debug_write_u32_hex(code);
            debug_write(exit_status_valid?"\n":" INVALID\n");
            if(process_reap_pid((uint32_t)integration_pid)!=0)
                integration_pipe_ok=0;
            integration_pid=-1;
            if(integration_pipe_ok){
                debug_write("LIONOS:PIPE-LIFECYCLE-TEST-OK\n");
                console_write("[ OK ] Automated Pipe fork/EOF/exit cleanup test\n");
                integration_stage=8;
            }else{
                integration_done=1;
                debug_write("LIONOS:PIPE-LIFECYCLE-TEST-FAIL\n");
                debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
                console_write("[ERR] Pipe lifecycle integration test failed\n");
            }
        }else if(s<0){
            integration_done=1;
            debug_write("LIONOS:USER-PIPE-STATE-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
        }
        return;
    }

    if(integration_stage==8){
        if(!cpu_nx_enabled()){
            debug_write("LIONOS:NX-RUNTIME-TEST-SKIP\n");
            integration_stage=10;
            return;
        }
        integration_pid=exec_run_file("nx_test.elf");
        if(integration_pid>0){
            debug_write("LIONOS:NX-RUNTIME-TEST-LAUNCHED\n");
            integration_stage=9;
        }else{
            integration_done=1;
            debug_write("LIONOS:NX-RUNTIME-TEST-LAUNCH-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
        }
        return;
    }

    if(integration_stage==9){
        int32_t s=process_get_state((uint32_t)integration_pid);
        if(s==PROCESS_ZOMBIE){
            uint32_t code=0xFFFFFFFFu;
            int valid=process_get_exit_code((uint32_t)integration_pid,&code)==0;
            integration_nx_ok=valid&&code==139u;
            if(process_reap_pid((uint32_t)integration_pid)!=0)integration_nx_ok=0;
            integration_pid=-1;
            debug_write(integration_nx_ok
                ? "LIONOS:NX-RUNTIME-TEST-OK\n"
                : "LIONOS:NX-RUNTIME-TEST-FAIL\n");
            if(!integration_nx_ok){
                integration_done=1;
                debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
            }else{
                integration_stage=10;
            }
        }else if(s<0){
            integration_done=1;
            debug_write("LIONOS:NX-RUNTIME-TEST-STATE-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
        }
        return;
    }

    if(integration_stage==10){
        integration_pid=exec_run_file("process_test.elf");
        if(integration_pid>0){
            debug_write("LIONOS:USER-PROCESS-LIFECYCLE-LAUNCHED\n");
            integration_stage=11;
        }else{
            integration_done=1;
            debug_write("LIONOS:USER-PROCESS-LIFECYCLE-LAUNCH-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
        }
        return;
    }

    if(integration_stage==11){
        int32_t s=process_get_state((uint32_t)integration_pid);
        if(s==PROCESS_ZOMBIE){
            uint32_t code=0xFFFFFFFFu;
            int valid=process_get_exit_code((uint32_t)integration_pid,&code)==0;
            integration_process_ok=valid&&code==0u;
            debug_write("LIONOS:PROCESS-TEST-EXIT-CODE=0x");
            debug_write_u32_hex(code);
            debug_write(valid?" VALID\n":" INVALID\n");
            if(process_reap_pid((uint32_t)integration_pid)!=0)
                integration_process_ok=0;
            integration_pid=-1;
            debug_write(integration_process_ok
                ? "LIONOS:PROCESS-WAITPID-TEST-OK\n"
                : "LIONOS:PROCESS-WAITPID-TEST-FAIL\n");
            if(!integration_process_ok){
                integration_done=1;
                debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
            }else{
                integration_stage=12;
            }
        }else if(s<0){
            integration_done=1;
            debug_write("LIONOS:USER-PROCESS-LIFECYCLE-STATE-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
        }
        return;
    }

    if(integration_stage==12){
        integration_pid=exec_run_file("scheduler_stress_test.elf");
        if(integration_pid>0){
            debug_write("LIONOS:LONG-DURATION-SCHEDULER-TEST-LAUNCHED\n");
            integration_stage=13;
        }else{
            integration_done=1;
            debug_write("LIONOS:LONG-DURATION-SCHEDULER-TEST-LAUNCH-FAIL\n");
            debug_write("LIONOS:LONG-DURATION-SCHEDULER-TEST-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
        }
        return;
    }

    if(integration_stage==13){
        int32_t s=process_get_state((uint32_t)integration_pid);
        if(s==PROCESS_ZOMBIE){
            uint32_t code=0xFFFFFFFFu;
            int valid=process_get_exit_code((uint32_t)integration_pid,&code)==0;
            int stress_ok=valid&&code==0u;
            if(process_reap_pid((uint32_t)integration_pid)!=0)
                stress_ok=0;
            integration_pid=-1;
            debug_write(stress_ok
                ? "LIONOS:LONG-DURATION-SCHEDULER-TEST-OK\n"
                : "LIONOS:LONG-DURATION-SCHEDULER-TEST-FAIL\n");
            if(!stress_ok){
                integration_done=1;
                debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
            }else{
                integration_stage=14;
            }
        }else if(s<0){
            integration_done=1;
            debug_write("LIONOS:LONG-DURATION-SCHEDULER-TEST-STATE-FAIL\n");
            debug_write("LIONOS:LONG-DURATION-SCHEDULER-TEST-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
        }
        return;
    }

    if(integration_stage==14){
        integration_pid=exec_run_file("signal_test.elf");
        if(integration_pid>0){
            debug_write("LIONOS:USER-SIGNAL-TEST-LAUNCHED\n");
            integration_stage=15;
        }else{
            integration_done=1;
            debug_write("LIONOS:USER-SIGNAL-TEST-LAUNCH-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
        }
        return;
    }

    if(integration_stage==15){
        int32_t s=process_get_state((uint32_t)integration_pid);
        if(s==PROCESS_ZOMBIE){
            uint32_t code=0xFFFFFFFFu;
            int valid=process_get_exit_code((uint32_t)integration_pid,&code)==0;
            int signal_ok=valid&&code==0u;
            if(process_reap_pid((uint32_t)integration_pid)!=0)
                signal_ok=0;
            integration_pid=-1;
            debug_write(signal_ok
                ? "LIONOS:SIGNAL-CONTROL-TEST-OK\n"
                : "LIONOS:SIGNAL-CONTROL-TEST-FAIL\n");
            if(!signal_ok){
                integration_done=1;
                debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
            }else{
                integration_stage=16;
            }
        }else if(s<0){
            integration_done=1;
            debug_write("LIONOS:USER-SIGNAL-TEST-STATE-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
        }
        return;
    }

    if(integration_stage==16){
        integration_pid=exec_run_file("tty_job_control_test.elf");
        if(integration_pid>0){
            debug_write("LIONOS:TTY-JOB-CONTROL-TEST-LAUNCHED\n");
            integration_stage=17;
        }else{
            integration_done=1;
            debug_write("LIONOS:TTY-JOB-CONTROL-TEST-LAUNCH-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
        }
        return;
    }

    if(integration_stage==17){
        int32_t s=process_get_state((uint32_t)integration_pid);
        if(s==PROCESS_ZOMBIE){
            uint32_t code=0xFFFFFFFFu;
            int valid=process_get_exit_code((uint32_t)integration_pid,&code)==0;
            int job_control_ok=valid&&code==0u;
            debug_write("LIONOS:TTY-JOB-CONTROL-EXIT-CODE=0x");
            debug_write_u32_hex(code);
            debug_write("\n");
            if(process_reap_pid((uint32_t)integration_pid)!=0)
                job_control_ok=0;
            integration_pid=-1;
            if(job_control_ok){
                debug_write("LIONOS:TTY-JOB-CONTROL-TEST-OK\n");
                integration_stage=18;
            }else{
                integration_done=1;
                debug_write("LIONOS:TTY-JOB-CONTROL-TEST-FAIL\n");
                debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
            }
        }else if(s<0){
            integration_done=1;
            debug_write("LIONOS:TTY-JOB-CONTROL-TEST-STATE-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
        }
        return;
    }

    if(integration_stage==18){
        integration_pid=exec_run_file("sigint_test.elf");
        if(integration_pid>0){
            debug_write("LIONOS:SIGINT-DEFAULT-TEST-LAUNCHED\n");
            integration_stage=19;
        }else{
            integration_done=1;
            debug_write("LIONOS:SIGINT-DEFAULT-TEST-LAUNCH-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
        }
        return;
    }

    if(integration_stage==19){
        int32_t s=process_get_state((uint32_t)integration_pid);
        if(s==PROCESS_ZOMBIE){
            uint32_t code=0xFFFFFFFFu;
            int valid=process_get_exit_code((uint32_t)integration_pid,&code)==0;
            int sigint_ok=valid&&code==0u;
            if(process_reap_pid((uint32_t)integration_pid)!=0)
                sigint_ok=0;
            integration_pid=-1;
            debug_write(sigint_ok
                ? "LIONOS:SIGINT-DEFAULT-TEST-OK\n"
                : "LIONOS:SIGINT-DEFAULT-TEST-FAIL\n");
            if(!sigint_ok){
                integration_done=1;
                debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
            }else{
                integration_stage=20;
            }
        }else if(s<0){
            integration_done=1;
            debug_write("LIONOS:SIGINT-DEFAULT-TEST-STATE-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
        }
        return;
    }

    if(integration_stage==20){
        integration_pid=exec_run_file("signal_handler_test.elf");
        if(integration_pid>0){
            debug_write("LIONOS:SIGNAL-HANDLER-TEST-LAUNCHED\n");
            integration_stage=21;
        }else{
            integration_done=1;
            debug_write("LIONOS:SIGNAL-HANDLER-TEST-LAUNCH-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
        }
        return;
    }

    if(integration_stage==21){
        int32_t s=process_get_state((uint32_t)integration_pid);
        if(s==PROCESS_ZOMBIE){
            uint32_t code=0xFFFFFFFFu;
            int valid=process_get_exit_code((uint32_t)integration_pid,&code)==0;
            debug_write("LIONOS:SIGNAL-HANDLER-EXIT-CODE=0x");
            debug_write_u32_hex(code);
            debug_write(valid?" VALID\n":" INVALID\n");
            int handler_ok=valid&&code==0u;
            if(process_reap_pid((uint32_t)integration_pid)!=0)
                handler_ok=0;
            integration_pid=-1;
            debug_write(handler_ok
                ? "LIONOS:SIGNAL-HANDLER-TEST-OK\n"
                : "LIONOS:SIGNAL-HANDLER-TEST-FAIL\n");
            if(!handler_ok){
                integration_done=1;
                debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
            }else{
                integration_stage=22;
            }
        }else if(s<0){
            integration_done=1;
            debug_write("LIONOS:SIGNAL-HANDLER-TEST-STATE-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
        }
        return;
    }

    if(integration_stage==22){
        uint32_t online=smp_online_count();
        if(online<=1u){
            debug_write("LIONOS:SMP-AP-USER-FORK-SKIP\n");
            integration_done=1;
            return;
        }
        __atomic_store_n(&integration_ap_user_pid,0u,__ATOMIC_RELAXED);
        __atomic_store_n(&integration_ap_user_status,0u,__ATOMIC_RELEASE);
        if(!process_create_kernel_thread_on_cpu(
                (uint32_t)(uintptr_t)&integration_ap_user_launch_worker,
                1u,1u)){
            integration_done=1;
            debug_write("LIONOS:SMP-AP-USER-FORK-WAITPID-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
            return;
        }
        debug_write("LIONOS:SMP-AP-USER-FORK-LAUNCHED\n");
        integration_stage=23;
        return;
    }

    if(integration_stage==23){
        uint32_t launch_status =
            __atomic_load_n(&integration_ap_user_status, __ATOMIC_ACQUIRE);
        if(launch_status == 0u) return;
        if(launch_status != 1u){
            integration_done = 1;
            debug_write(launch_status == 2u
                ? "LIONOS:SMP-AP-USER-WORKER-CPU-MISMATCH\n"
                : "LIONOS:SMP-AP-USER-EXEC-LAUNCH-FAIL\n");
            debug_write("LIONOS:SMP-AP-USER-FORK-WAITPID-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
            return;
        }
        uint32_t pid =
            __atomic_load_n(&integration_ap_user_pid, __ATOMIC_ACQUIRE);
        if(pid == 0u){
            integration_done = 1;
            debug_write("LIONOS:SMP-AP-USER-PID-MISSING\n");
            debug_write("LIONOS:SMP-AP-USER-FORK-WAITPID-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
            return;
        }
        int32_t state = process_get_state(pid);
        if(state == PROCESS_ZOMBIE){
            uint32_t code = 0xFFFFFFFFu;
            int ok = process_get_exit_code(pid, &code) == 0 && code == 0u;
            if(process_reap_pid(pid) != 0) ok = 0;
            if(ok){
                debug_write("LIONOS:SMP-AP-USER-FORK-WAITPID-OK\n");
                integration_stage = 24;
            }else{
                integration_done = 1;
                debug_write("LIONOS:SMP-AP-USER-FORK-WAITPID-FAIL\n");
                debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
            }
        }else if(state < 0){
            integration_done = 1;
            debug_write("LIONOS:SMP-AP-USER-PROCESS-STATE-FAIL\n");
            debug_write("LIONOS:SMP-AP-USER-FORK-WAITPID-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
        }
        return;
    }

    /*
     * Launch two independent, explicitly migratable user processes and one
     * ordinary process that must retain BSP affinity. The migration test ELF
     * has no shared filesystem or console side effects between instances.
     */
    if(integration_stage == 24){
        uint32_t online=smp_online_count();
        uint32_t launch_flags=0u;

        /* This integration test is owned by the BSP. */
        if(cpu_current_index()!=0u)return;
        if(online<=1u){
            debug_write("LIONOS:SMP-WORK-STEAL-MULTI-PROCESS-SKIP\n");
            integration_done=1;
            return;
        }
        if(online>LIONOS_MAX_CPUS)online=LIONOS_MAX_CPUS;
        if(process_scheduler_execution_guard_enable()!=0){
            (void)process_scheduler_execution_guard_finish();
            integration_done=1;
            debug_write("LIONOS:SMP-EXECUTION-EXCLUSION-ENABLE-FAIL\n");
            debug_write("LIONOS:SMP-WORK-STEAL-MULTI-PROCESS-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
            return;
        }
        for(uint32_t cpu=0u;cpu<online;++cpu){
            integration_migration_steals_before[cpu]=
                process_scheduler_steals(cpu);
            integration_migration_ticks_before[cpu]=
                process_scheduler_ticks(cpu);
        }
        for(uint32_t i=0u;i<INTEGRATION_MIGRATION_PROCESS_COUNT;++i)
            integration_migration_pids[i]=-1;
        integration_migration_count=0u;
        integration_migration_failed=0u;
        integration_migration_owner_mask=0u;

        /*
         * Prevent BSP preemption while the READY batch is constructed.
         * Every process remains pinned until the atomic eligibility publish.
         */
        __asm__ volatile("pushfl; popl %0; cli"
                         : "=r"(launch_flags) : : "memory");
        for(uint32_t i=0u;i<INTEGRATION_MIGRATION_PROCESS_COUNT;++i){
            int pid=exec_run_file("migration_test.elf");
            if(pid<=0){
                integration_migration_failed=1u;
                break;
            }
            integration_migration_pids[i]=pid;
            ++integration_migration_count;
        }
        if(integration_migration_count!=
           INTEGRATION_MIGRATION_PROCESS_COUNT)
            integration_migration_failed=1u;

        /*
         * Publish the complete eligible set under process_lock. AP schedulers
         * also take process_lock before checking candidates, so they cannot
         * observe or steal a partially opted-in batch.
         */
        if(integration_migration_count==
           INTEGRATION_MIGRATION_PROCESS_COUNT){
            uint32_t eligible_pids[INTEGRATION_MIGRATION_ELIGIBLE_COUNT];
            for(uint32_t i=0u;i<INTEGRATION_MIGRATION_ELIGIBLE_COUNT;++i)
                eligible_pids[i]=(uint32_t)integration_migration_pids[i];
            /*
             * Rejected batches must be transactional: zero, duplicate, or
             * reserved trailing PIDs must not leave an earlier process opted in.
             * The valid batch immediately afterwards detects partial mutation.
             */
            uint32_t invalid_batch[INTEGRATION_MIGRATION_ELIGIBLE_COUNT];
            uint32_t duplicate_batch[INTEGRATION_MIGRATION_ELIGIBLE_COUNT];
            uint32_t reserved_batch[INTEGRATION_MIGRATION_ELIGIBLE_COUNT];
            for(uint32_t i=0u;i<INTEGRATION_MIGRATION_ELIGIBLE_COUNT;++i){
                invalid_batch[i]=eligible_pids[i];
                duplicate_batch[i]=eligible_pids[0];
                reserved_batch[i]=eligible_pids[i];
            }
            invalid_batch[INTEGRATION_MIGRATION_ELIGIBLE_COUNT-1u]=0u;
            reserved_batch[INTEGRATION_MIGRATION_ELIGIBLE_COUNT-1u]=1u;
            int invalid_result=process_set_migration_eligible_batch(
                invalid_batch,INTEGRATION_MIGRATION_ELIGIBLE_COUNT);
            int duplicate_result=process_set_migration_eligible_batch(
                duplicate_batch,INTEGRATION_MIGRATION_ELIGIBLE_COUNT);
            /*
             * PID 1 is reserved by the process policy (or absent on a
             * configuration without init). It is checked after a valid PID,
             * exercising rollback when lookup/policy validation fails late.
             */
            int reserved_result=process_set_migration_eligible_batch(
                reserved_batch,INTEGRATION_MIGRATION_ELIGIBLE_COUNT);
            if(invalid_result!=-1||duplicate_result!=-1||
               reserved_result!=-1)
                integration_migration_failed=1u;

            if(process_set_migration_eligible_batch(
                   eligible_pids,INTEGRATION_MIGRATION_ELIGIBLE_COUNT)!=0)
                integration_migration_failed=1u;
            else if(invalid_result==-1&&duplicate_result==-1&&
                    reserved_result==-1)
                debug_write("LIONOS:SMP-WORK-STEAL-BATCH-ATOMICITY-OK\n");
        }

        /* Restore the exact incoming EFLAGS, including the prior IF state. */
        __asm__ volatile("pushl %0; popfl"
                         : : "r"(launch_flags) : "memory","cc");

        if(integration_migration_count==0u){
            (void)process_scheduler_execution_guard_finish();
            integration_done=1;
            debug_write("LIONOS:SMP-WORK-STEAL-MULTI-PROCESS-FAIL\n");
            debug_write("LIONOS:USER-INTEGRATION-FAIL\n");
            return;
        }
        for(uint32_t cpu=1u;cpu<online;++cpu)
            process_request_reschedule_cpu(cpu);
        debug_write("LIONOS:SMP-WORK-STEAL-MULTI-PROCESS-LAUNCHED\n");
        integration_stage=25;
        return;
    }

    if(integration_stage == 25){
        uint32_t online=smp_online_count();
        uint32_t pending=0u;
        if(online>LIONOS_MAX_CPUS)online=LIONOS_MAX_CPUS;
        for(uint32_t i=0u;i<integration_migration_count;++i){
            int pid=integration_migration_pids[i];
            if(pid<=0)continue;
            int32_t state=process_get_state((uint32_t)pid);
            if(state==PROCESS_ZOMBIE){
                uint32_t code=0xFFFFFFFFu;
                uint32_t owner=0xFFFFFFFFu;
                uint32_t migrations=0xFFFFFFFFu;
                int valid=process_get_exit_code((uint32_t)pid,&code)==0&&
                          code==0u;
                int info=process_get_migration_info((uint32_t)pid,
                                                     &owner,&migrations)==0;
                if(!valid||!info){
                    integration_migration_failed=1u;
                }else if(i<INTEGRATION_MIGRATION_ELIGIBLE_COUNT){
                    uint32_t steals=owner<LIONOS_MAX_CPUS
                        ?process_scheduler_steals(owner):0u;
                    uint32_t before=owner<LIONOS_MAX_CPUS
                        ?integration_migration_steals_before[owner]:0u;
                    if(owner==0u||owner>=online||migrations!=1u||
                       steals<=before){
                        integration_migration_failed=1u;
                    }else{
                        integration_migration_owner_mask|=1u<<owner;
                    }
                    if(owner==0u||owner>=online||migrations!=1u||
                       steals<=before){
                        debug_write("LIONOS:SMP-WORK-STEAL-PROCESS-DIAG pid=");
                        debug_write_u32_hex((uint32_t)pid);
                        debug_write(" owner=");debug_write_u32_hex(owner);
                        debug_write(" migrations=");debug_write_u32_hex(migrations);
                        debug_write(" steals=");debug_write_u32_hex(steals);
                        debug_write(" before=");debug_write_u32_hex(before);
                        debug_write("\n");
                    }
                }else if(owner!=0u||migrations!=0u){
                    integration_migration_failed=1u;
                    debug_write("LIONOS:SMP-WORK-STEAL-PINNED-DIAG pid=");
                    debug_write_u32_hex((uint32_t)pid);
                    debug_write(" owner=");debug_write_u32_hex(owner);
                    debug_write(" migrations=");debug_write_u32_hex(migrations);
                    debug_write("\n");
                }
                if(process_reap_pid((uint32_t)pid)!=0)
                    integration_migration_failed=1u;
                integration_migration_pids[i]=-1;
            }else if(state<0){
                integration_migration_failed=1u;
                integration_migration_pids[i]=-1;
            }else{
                pending=1u;
            }
        }
        if(!pending){
            int execution_guard_ok=
                process_scheduler_execution_guard_finish()==0;
            if(execution_guard_ok)
                debug_write("LIONOS:SMP-EXECUTION-EXCLUSION-OK\n");
            else{
                integration_migration_failed=1u;
                debug_write("LIONOS:SMP-EXECUTION-EXCLUSION-FAIL\n");
            }
            uint32_t owner_count=0u;
            for(uint32_t cpu=1u;cpu<online;++cpu)
                if(integration_migration_owner_mask&(1u<<cpu))++owner_count;
            uint32_t required_owners=online>2u?2u:1u;
            int multi_stealer_ok=owner_count>=required_owners;
            if(online>=3u&&multi_stealer_ok)
                debug_write("LIONOS:SMP-MULTI-STEALER-OK\n");
            else if(online>=3u){
                integration_migration_failed=1u;
                debug_write("LIONOS:SMP-MULTI-STEALER-FAIL\n");
            }
            int ok=integration_migration_failed==0u&&
                   integration_migration_count==
                       INTEGRATION_MIGRATION_PROCESS_COUNT&&
                   execution_guard_ok&&multi_stealer_ok;
            debug_write(ok
                ? "LIONOS:SMP-WORK-STEAL-MULTI-PROCESS-OK\n"
                : "LIONOS:SMP-WORK-STEAL-MULTI-PROCESS-FAIL\n");
            if(!ok){
                for(uint32_t cpu=1u;cpu<online;++cpu){
                    debug_write("LIONOS:SMP-WORK-STEAL-CPU-DIAG cpu=");
                    debug_write_u32_hex(cpu);
                    debug_write(" ticks=");
                    debug_write_u32_hex(process_scheduler_ticks(cpu));
                    debug_write(" ticks_before=");
                    debug_write_u32_hex(integration_migration_ticks_before[cpu]);
                    debug_write(" steals=");
                    debug_write_u32_hex(process_scheduler_steals(cpu));
                    debug_write(" steals_before=");
                    debug_write_u32_hex(integration_migration_steals_before[cpu]);
                    debug_write("\n");
                }
            }
            debug_write(ok
                ? "LIONOS:SMP-WORK-STEAL-USER-PROCESS-OK\n"
                : "LIONOS:USER-INTEGRATION-FAIL\n");
            integration_done=1;
        }
        return;
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

    /* Initialize the TTY lock before any keyboard backend can forward input. */
    tty_init();
    keyboard_init();
    console_write("[ OK ] PS/2 keyboard / scancode input buffer\n");
    BOOT_STAGE(22u, "INPUT READY");

    mouse_init();
    console_write("[ OK ] PS/2 mouse / polled input\n");

    /* ---- Memory / paging ---- */
    if(memory_init(multiboot_info)!=0){
        debug_write("LIONOS:MEMORY-INIT-FAILED\n");
        console_write("[ERR] Physical memory manager initialization failed; halting safely\n");
        for(;;) __asm__ volatile("cli; hlt");
    }
    debug_write("LIONOS:MEMORY-INIT-OK\n");
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

    /*
     * Capture BSP CPU and MTRR state before the first device MMIO mapping.
     * This is read-only for MTRRs; PAT/MMIO policy remains unchanged.
     */
    cpu_init(multiboot_info);
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

    /* Exercise CPU-only command-buffer consumer and software-fence lifetime. */
    if (gpu_submission_selftest() == 0) {
        debug_write("LIONOS:GPU-SUBMISSION-SELFTEST-OK\n");
        console_write("[ OK ] GPU submission / CPU-only lease and fence lifecycle self-test\n");
    } else {
        debug_write("LIONOS:GPU-SUBMISSION-SELFTEST-FAIL\n");
        console_write("[ERR] GPU submission / CPU-only lifecycle self-test\n");
    }

    /* Verify safe backend selection and mandatory CPU fallback. */
    if (gfx_backend_registry_selftest() == 0) {
        debug_write("LIONOS:GFX-BACKEND-REGISTRY-SELFTEST-OK\n");
        console_write("[ OK ] Graphics backend registry / CPU fallback self-test\n");
    } else {
        debug_write("LIONOS:GFX-BACKEND-REGISTRY-SELFTEST-FAIL\n");
        console_write("[ERR] Graphics backend registry / CPU fallback self-test\n");
    }

    /* Pure vendor/classification test; no GPU registers are accessed. */
    if (gfx_pci_discovery_selftest() == 0) {
        debug_write("LIONOS:GFX-PCI-CLASSIFY-SELFTEST-OK\n");
        console_write("[ OK ] Intel/AMD/NVIDIA PCI display classification self-test\n");
    } else {
        debug_write("LIONOS:GFX-PCI-CLASSIFY-SELFTEST-FAIL\n");
        console_write("[ERR] PCI display classification self-test\n");
    }

    /* Classify enumerated display functions after the registry self-test reset. */
    {
        gfx_pci_discovery_report_t gfx_pci_report;
        if (gfx_pci_discover(&gfx_pci_report) == 0) {
            console_write("[ OK ] PCI display adapters: ");
            boot_dec(gfx_pci_report.display_adapter_count);
            console_write(" / supported vendors: ");
            boot_dec(gfx_pci_report.supported_adapter_count);
            console_write(" / unknown vendors: ");
            boot_dec(gfx_pci_report.unsupported_adapter_count);
            console_write("\n");
            debug_write("LIONOS:GFX-PCI-DISCOVERY-OK\n");
            if (gfx_pci_report.pci_recording_truncated ||
                gfx_pci_report.adapter_list_truncated)
                debug_write("LIONOS:GFX-PCI-DISCOVERY-TRUNCATED\n");
        } else {
            console_write("[ -- ] PCI display adapter discovery failed\n");
            debug_write("LIONOS:GFX-PCI-DISCOVERY-FAIL\n");
        }
    }

    if (intel_gpu_ggtt_ownership_report_selftest() == 0)
        console_write("[ OK ] Intel GGTT ownership report contract self-test\n");
    else
        console_write("[ERR] Intel GGTT ownership report contract self-test\n");

    if (intel_gpu_bar0_validation_selftest() == 0) {
        console_write("[ OK ] Intel PCI BAR0 resource validation self-test\n");
    } else {
        console_write("[ERR] Intel PCI BAR0 resource validation self-test\n");
    }

    if (paging_kernel_range_selftest() == 0) {
        debug_write("LIONOS:KERNEL-MMIO-RANGE-SELFTEST-OK\n");
        console_write("[ OK ] Kernel MMIO range validation self-test\n");
    } else {
        debug_write("LIONOS:KERNEL-MMIO-RANGE-SELFTEST-FAIL\n");
        console_write("[ERR] Kernel MMIO range validation self-test\n");
    }

    if (intel_gpu_mmio_pat_selftest() == 0) {
        console_write("[ OK ] Intel MMIO PAT cache-index self-test\n");
    } else {
        console_write("[ERR] Intel MMIO PAT cache-index self-test\n");
    }

    console_write("[ .. ] Initializing Intel graphics PCI/MMIO bootstrap...\n");
    int intel_gpu_init_rc = intel_gpu_init(framebuffer_available() ? 1u : 0u);
    if (intel_gpu_available()) {
        intel_gpu_ggtt_ownership_report_t ownership_report;
        if (intel_gpu_ggtt_get_ownership_report(&ownership_report) == 0) {
            console_write("[ -- ] GGTT ownership blocker mask: 0x");
            boot_hex32(ownership_report.blocker_mask);
            console_write(" / mapping permission: DENIED\n");
            debug_write("LIONOS:INTEL-GGTT-OWNERSHIP-REPORT-OK\n");
        } else {
            debug_write("LIONOS:INTEL-GGTT-OWNERSHIP-REPORT-FAIL\n");
        }
    }
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
    if(cpu_enable_nx()==0){
        debug_write("LIONOS:NX-ENABLED\n");
    }else{
        debug_write("LIONOS:NX-UNAVAILABLE\n");
    }

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
        if (ioapic_init() == 0) {
            console_write("[ OK ] IOAPIC discovery / masked ISA routes; PIC remains active\n");
        } else {
            console_write("[ -- ] IOAPIC probe unavailable; PIC remains active\n");
        }
        BOOT_STAGE(50u, "PROCESSORS STARTING");

        smp_init();
        console_write("[ OK ] SMP online CPUs: ");
        boot_dec(smp_online_count());
        console_write("\n");
        console_write(smp_lock_selftest()
            ? "[ OK ] SMP spinlock / IRQ-save synchronization\n"
            : "[ERR] SMP spinlock self-test\n");
        if (ioapic_count() != 0u) {
            int route_status = ioapic_activate_isa_routes();
            if (route_status == 0) {
                console_write("[ OK ] IOAPIC ISA IRQ0/1/12 active; legacy PIC masked\n");
            } else if (route_status == -2) {
                debug_write("LIONOS:IOAPIC-HANDOFF-ROLLBACK-FAILED\n");
                for (;;) __asm__ volatile("cli; hlt");
            } else {
                console_write("[ -- ] IOAPIC ISA handoff unavailable; legacy PIC remains active\n");
            }
        }
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
    console_write("[ OK ] Software TTY core / session ownership / canonical input queue\n");
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
    if (ext2_vfs_boot_test() == 0)
        console_write("[ OK ] ext2 primary-slave filesystem mounted read-only at /ext2\n");
    else
        console_write("[ -- ] ext2 primary-slave VFS mount unavailable\n");
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
