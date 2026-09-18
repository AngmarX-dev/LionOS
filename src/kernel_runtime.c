#include "gui.h"
#include "shell.h"

static void gui_desktop_run(void);\nstatic void lionos_gui_bootstrap(void);
static void lionos_gui_or_shell_loop(void);

#define gui_run lionos_gui_bootstrap
#define shell_run lionos_gui_or_shell_loop
#include "kernel.c"
#undef shell_run
#undef gui_run

static void lionos_gui_bootstrap(void){
    gui_start();
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
        __asm__ volatile("sti; hlt");
    }
}
