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

void pic_init(void);
void keyboard_init(void);
void syscall_init(void);

static void boot_dec(uint32_t value){
    console_write_dec(value);
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

    if(framebuffer_init(multiboot_info) == 0){
        console_write("[ OK ] Framebuffer ");
        boot_dec(framebuffer_width());
        console_putc('x');
        boot_dec(framebuffer_height());
        console_write(" desktop mode\n");
        console_use_framebuffer();
        BOOT_STAGE(34u, "DISPLAY READY");
    }

    console_write("[ -- ] Intel graphics native driver not active; probing firmware-backed display...\n");
    (void)intel_gpu_init(framebuffer_available() ? 1u : 0u);

    console_write("[ OK ] Paging / supervisor kernel mappings\n");
    BOOT_STAGE(38u, "PAGING READY");

    heap_init();
    console_write("[ OK ] Kernel heap / PMM-backed kmalloc + kfree\n");

    /* ---- USB xHCI mouse ---- */
    if(mouse_usb_init() == 0){
        console_write("[ OK ] USB xHCI / HID boot mouse\n");
        if(xhci_keyboard_init() == 0)
            console_write("[ OK ] USB xHCI / HID boot keyboard\n");
        else
            console_write("[ -- ] USB xHCI / HID keyboard unavailable\n");
    } else {
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
        console_write("[ -- ] USB xHCI / HID mouse unavailable; PS/2 fallback active\n");
    }
    BOOT_STAGE(42u, "HEAP READY");

    /* ---- CPU topology / APIC ---- */
    cpu_init(multiboot_info);
    console_write("[ OK ] CPU topology / BSP APIC ID ");
    boot_dec(cpu_get(0)->apic_id);
    console_write(" / logical hint ");
    boot_dec(cpu_count_hint());
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
        BOOT_STAGE(56u, "PROCESSORS READY");
    } else {
        console_write("[ -- ] Local APIC unavailable; SMP disabled\n");
        debug_write("LIONOS:SMP-DISABLED\n");
        BOOT_STAGE(56u, "SINGLE CPU MODE");
    }

    /* ---- Syscalls / processes ---- */
    syscall_init();
    console_write("[ OK ] Syscall ABI / process / VFS / IPC / signals / networking\n");
    BOOT_STAGE(62u, "KERNEL SERVICES READY");

    process_init();
    console_write("[ OK ] Process table / scheduler / ring-3 address spaces\n");
    BOOT_STAGE(68u, "PROCESS MANAGER READY");

    if(lapic_ready){
        /* Keep the PIT 100 Hz timer active for the desktop and input path. */
        console_write("[ OK ] PIT 100 Hz / desktop + input clock\n");
        BOOT_STAGE(72u, "SYSTEM TIMER READY");
    }

    /* ---- Filesystems ---- */
    ramfs_init();
    console_write("[ OK ] RAM filesystem / embedded programs\n");
    BOOT_STAGE(78u, "RAM FILESYSTEM READY");

    if(diskfs_init() == 0){
        console_write("[ OK ] ATA PIO / persistent LionFS\n");
        diskfs_boot_test();
    } else {
        console_write("[ -- ] Persistent disk unavailable (RAMFS only)\n");
        debug_write("LIONOS:NO-DISK\n");
    }
    BOOT_STAGE(84u, "STORAGE READY");

    vfs_init();
    console_write("[ OK ] VFS / unified file descriptor layer\n");
    vfs_boot_test();

    ipc_init();
    console_write("[ OK ] IPC / kernel message queues\n");
    BOOT_STAGE(89u, "FILESYSTEM SERVICES READY");

    signal_init();
    console_write("[ OK ] Signals / process control\n");

    net_init();
    if(net_physical_ready())
        console_write("[ OK ] RTL8139 Ethernet / IPv4 / ARP / ICMP\n");
    else
        console_write("[ -- ] Physical NIC unavailable; loopback only\n");
    BOOT_STAGE(94u, "NETWORK READY");

    /* ---- Desktop ---- */
    console_write("\nLionOS is ready.\n");
    debug_write("LIONOS:READY\n");
    BOOT_STAGE(100u, "STARTING LIONOS DESKTOP");

    __asm__ volatile("sti");
    gui_run();
    shell_run();
    for(;;) __asm__ volatile("hlt");

#undef BOOT_STAGE
}
