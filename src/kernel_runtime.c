#include "gui.h"
#include "shell.h"

void gui_desktop_run(void);
static void lionos_gui_bootstrap(void);
static void lionos_gui_or_shell_loop(void);
void lionos_kernel_idle(void);

#define gui_run lionos_gui_bootstrap
#define shell_run lionos_gui_or_shell_loop
#include "kernel.c"
#undef shell_run
#undef gui_run

static void lionos_gui_bootstrap(void){
    gui_start();
    lionos_user_integration_step();
}

static void lionos_gui_or_shell_loop(void){
    if(!gui_is_active()){
        shell_run();
        return;
    }

    for(;;){
        if(!gui_is_active()){
            shell_run();
            return;
        }
        gui_step();
        lionos_user_integration_step();
        __asm__ volatile("sti; hlt");
    }
}

void lionos_kernel_idle(void){
    lionos_gui_or_shell_loop();
}
