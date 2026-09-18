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

void pic_init(void); void keyboard_init(void); void syscall_init(void);
static void boot_dec(uint32_t value){console_write_dec(value);}
static void vfs_boot_test(void){
    static const char path[]="/tests/../vfs-selftest.txt";
    static const char payload[]="LionOS VFS integration OK\n";
    char buffer[sizeof(payload)];
    int fd=vfs_open(path,LIONOS_O_WRITE);
    if(fd<0||vfs_write(fd,payload,sizeof(payload)-1u)!=(int)(sizeof(payload)-1u)||vfs_close(fd)<0){debug_write("LIONOS:VFS-TEST-FAIL\n");console_write("[ERR] VFS integration self-test\n");return;}
    fd=vfs_open("/vfs-selftest.txt",LIONOS_O_READ);
    if(fd<0||vfs_read(fd,buffer,sizeof(buffer))!=(int)(sizeof(payload)-1u)||vfs_close(fd)<0){debug_write("LIONOS:VFS-TEST-FAIL\n");console_write("[ERR] VFS integration self-test\n");return;}
    for(uint32_t i=0;i<sizeof(payload)-1u;++i)if(buffer[i]!=payload[i]){debug_write("LIONOS:VFS-TEST-FAIL\n");console_write("[ERR] VFS integration self-test\n");return;}
    debug_write("LIONOS:VFS-TEST-OK\n");console_write("[ OK ] VFS path / per-process descriptor integration\n");
}
static void diskfs_boot_test(void){static const char marker[]="LionOS persistent storage online\n";char buffer[sizeof(marker)];int size=diskfs_read(".boot",buffer,sizeof(buffer));if(size==(int)(sizeof(marker)-1u)){uint32_t ok=1;for(uint32_t i=0;i<sizeof(marker)-1u;++i)if(buffer[i]!=marker[i]){ok=0;break;}if(ok){debug_write("LIONOS:PERSIST-OK\n");console_write("[ OK ] Persistent data survived reboot\n");return;}}if(diskfs_write(".boot",marker,sizeof(marker)-1u)==0){debug_write("LIONOS:PERSIST-INIT\n");console_write("[ OK ] Persistent filesystem initialized\n");}else{debug_write("LIONOS:PERSIST-FAIL\n");console_write("[ERR] Persistent filesystem self-test\n");}}
void kernel_main(uint32_t magic,uint32_t multiboot_info){debug_write("LIONOS:BOOT\n");console_init();console_write("LionOS kernel booting...\n\n");if(magic!=0x36D76289u){debug_write("LIONOS:BAD-MULTIBOOT\n");console_write("ERROR: invalid Multiboot2 magic.\n");for(;;)__asm__ volatile("cli; hlt");}gdt_init();console_write("[ OK ] GDT / ring-3 segments\n");tss_init();console_write("[ OK ] TSS / ring-0 stack\n");idt_init();console_write("[ OK ] IDT / CPU exceptions / DPL3 syscall\n");pic_init();console_write("[ OK ] PIC remapped\n");pit_init(100);console_write("[ OK ] PIT 100 Hz / preemptive scheduler clock\n");keyboard_init();console_write("[ OK ] PS/2 keyboard / scancode input buffer\n");if(mouse_init()==0)console_write("[ OK ] PS/2 mouse / polled input\n");else console_write("[ -- ] PS/2 mouse unavailable\n");memory_init(multiboot_info);console_write("[ OK ] Physical memory manager\n       Total pages: ");boot_dec(memory_total_pages());console_write("  Free pages: ");boot_dec(memory_free_pages());console_putc('\n');paging_init();if(framebuffer_init(multiboot_info)==0)console_write("[ OK ] Framebuffer detected / GUI renderer staged\n");console_write("[ OK ] Paging / supervisor kernel mappings\n");heap_init();console_write("[ OK ] Kernel heap / PMM-backed kmalloc + kfree\n");cpu_init(multiboot_info);console_write("[ OK ] CPU topology / BSP APIC ID ");boot_dec(cpu_get(0)->apic_id);console_write(" / logical hint ");boot_dec(cpu_count_hint());console_write("\n");uint32_t lapic_ready=0;if(lapic_init()==0){lapic_ready=1;console_write("[ OK ] Local APIC / BSP enabled\n");smp_init();console_write("[ OK ] SMP online CPUs: ");boot_dec(smp_online_count());console_write("\n");console_write(smp_lock_selftest()?"[ OK ] SMP spinlock / IRQ-save synchronization\n":"[ERR] SMP spinlock self-test\n");}else{console_write("[ -- ] Local APIC unavailable; SMP disabled\n");}syscall_init();console_write("[ OK ] Syscall ABI / process / VFS / IPC / signals / networking\n");process_init();console_write("[ OK ] Process table / scheduler / ring-3 address spaces\n");if(lapic_ready){lapic_timer_init();pit_disable_timer();console_write("[ OK ] LAPIC timer / BSP preemption clock\n");}ramfs_init();console_write("[ OK ] RAM filesystem / embedded programs\n");if(diskfs_init()==0){console_write("[ OK ] ATA PIO / persistent LionFS\n");diskfs_boot_test();}else{console_write("[ -- ] Persistent disk unavailable (RAMFS only)\n");debug_write("LIONOS:NO-DISK\n");}vfs_init();console_write("[ OK ] VFS / unified file descriptor layer\n");vfs_boot_test();ipc_init();console_write("[ OK ] IPC / kernel message queues\n");signal_init();console_write("[ OK ] Signals / process control\n");net_init();console_write("[ OK ] Networking / loopback IPv4 transport\n");console_write("\nLionOS is ready.\n");debug_write("LIONOS:READY\n");__asm__ volatile("sti");gui_run();shell_run();for(;;)__asm__ volatile("hlt");}
