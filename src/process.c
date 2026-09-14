#include "process.h"

static struct process processes[LIONOS_PROCESS_MAX];
static struct process *current;

void process_init(void) {
    for (uint32_t i = 0; i < LIONOS_PROCESS_MAX; ++i) {
        processes[i].pid = 0;
        processes[i].state = PROCESS_UNUSED;
        processes[i].page_directory = 0;
        processes[i].entry = 0;
        processes[i].user_stack = 0;
    }

    processes[0].pid = 1;
    processes[0].state = PROCESS_RUNNING;
    current = &processes[0];
}

struct process *process_current(void) {
    return current;
}

uint32_t process_current_pid(void) {
    return current ? current->pid : 0;
}
