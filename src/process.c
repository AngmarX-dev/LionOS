#include "process.h"

static struct process processes[LIONOS_PROCESS_MAX];
static struct process *current;
static uint32_t next_pid;

static struct process *find_free_slot(void) {
    for (uint32_t i = 0; i < LIONOS_PROCESS_MAX; ++i) {
        if (processes[i].state == PROCESS_UNUSED) return &processes[i];
    }
    return 0;
}

void process_init(void) {
    for (uint32_t i = 0; i < LIONOS_PROCESS_MAX; ++i) {
        processes[i].pid = 0;
        processes[i].state = PROCESS_UNUSED;
        processes[i].page_directory = 0;
        processes[i].entry = 0;
        processes[i].user_stack = 0;
    }

    next_pid = 2;
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

struct process *process_create(uint32_t entry, uint32_t user_stack, uint32_t page_directory) {
    struct process *process = find_free_slot();
    if (!process) return 0;

    process->pid = next_pid++;
    if (next_pid == 0) next_pid = 2;
    process->state = PROCESS_READY;
    process->page_directory = page_directory;
    process->entry = entry;
    process->user_stack = user_stack;
    return process;
}

int process_set_current(struct process *process) {
    if (!process || process->state == PROCESS_UNUSED) return -1;
    if (current && current != process && current->state == PROCESS_RUNNING)
        current->state = PROCESS_READY;
    current = process;
    current->state = PROCESS_RUNNING;
    return 0;
}

void process_exit_current(void) {
    if (!current || current == &processes[0]) return;

    current->state = PROCESS_UNUSED;
    current->pid = 0;
    current->page_directory = 0;
    current->entry = 0;
    current->user_stack = 0;

    current = &processes[0];
    current->state = PROCESS_RUNNING;
}

uint32_t process_count(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < LIONOS_PROCESS_MAX; ++i)
        if (processes[i].state != PROCESS_UNUSED) ++count;
    return count;
}
