#include <stdint.h>
#include "memory.h"
#include "paging.h"
#include "process.h"
#include "tss.h"

static struct process processes[LIONOS_PROCESS_MAX];
static struct process *current;
static uint32_t next_pid;

static struct process *find_free_slot(void) {
    for (uint32_t i = 0; i < LIONOS_PROCESS_MAX; ++i)
        if (processes[i].state == PROCESS_UNUSED) return &processes[i];
    return 0;
}

static uint32_t process_index(const struct process *process) { return (uint32_t)(process - processes); }

static void init_interrupt_frame(struct process *process) {
    uint32_t *frame = (uint32_t *)(uintptr_t)(process->kernel_stack_top - PROCESS_CONTEXT_WORDS * 4u);
    for (uint32_t i = 0; i < PROCESS_CONTEXT_WORDS; ++i) frame[i] = 0;
    frame[0] = 0x2Bu; frame[1] = 0x2Bu; frame[2] = 0x2Bu; frame[3] = 0x2Bu;
    frame[14] = process->entry; frame[15] = 0x23u; frame[16] = 0x202u;
    frame[17] = process->user_stack; frame[18] = 0x2Bu;
    process->saved_frame = (uint32_t)(uintptr_t)frame;
}

static void init_bootstrap_frame(void) {
    struct process *bootstrap = &processes[0];
    void *stack_page = page_alloc();
    if (!stack_page) { bootstrap->kernel_stack_top = 0; bootstrap->saved_frame = 0; return; }
    bootstrap->kernel_stack_top = (uint32_t)(uintptr_t)stack_page + 4096u;
    bootstrap->page_directory = paging_kernel_directory();
    bootstrap->entry = (uint32_t)(uintptr_t)&process_schedule;
    bootstrap->user_stack = bootstrap->kernel_stack_top;
    uint32_t *frame = (uint32_t *)(uintptr_t)(bootstrap->kernel_stack_top - PROCESS_CONTEXT_WORDS * 4u);
    for (uint32_t i = 0; i < PROCESS_CONTEXT_WORDS; ++i) frame[i] = 0;
    frame[0] = 0x10u; frame[1] = 0x10u; frame[2] = 0x10u; frame[3] = 0x10u;
    frame[14] = (uint32_t)(uintptr_t)&scheduler_idle; frame[15] = 0x08u; frame[16] = 0x202u;
    frame[17] = bootstrap->kernel_stack_top; frame[18] = 0x10u;
    bootstrap->saved_frame = (uint32_t)(uintptr_t)frame;
}

void process_init(void) {
    for (uint32_t i = 0; i < LIONOS_PROCESS_MAX; ++i) {
        processes[i].pid = 0; processes[i].state = PROCESS_UNUSED; processes[i].page_directory = 0;
        processes[i].entry = 0; processes[i].user_stack = 0; processes[i].kernel_stack_top = 0;
        processes[i].saved_frame = 0; processes[i].user_code_page = 0; processes[i].user_stack_page = 0;
    }
    next_pid = 2; processes[0].pid = 1; processes[0].state = PROCESS_RUNNING;
    current = &processes[0]; init_bootstrap_frame();
}

struct process *process_current(void) { return current; }
struct process *process_at(uint32_t index) { return index < LIONOS_PROCESS_MAX ? &processes[index] : 0; }
const char *process_state_name(uint32_t state) {
    switch (state) {
        case PROCESS_READY: return "READY";
        case PROCESS_RUNNING: return "RUNNING";
        case PROCESS_ZOMBIE: return "ZOMBIE";
        default: return "UNUSED";
    }
}
uint32_t process_current_pid(void) { return current ? current->pid : 0; }

struct process *process_create(uint32_t entry, uint32_t user_stack, uint32_t page_directory,
                               uint32_t user_code_page, uint32_t user_stack_page) {
    struct process *process = find_free_slot();
    if (!process || page_directory == 0 || user_code_page == 0 || user_stack_page == 0) return 0;
    void *kernel_stack = page_alloc();
    if (!kernel_stack) return 0;
    process->pid = next_pid++; if (next_pid == 0) next_pid = 2;
    process->state = PROCESS_READY; process->page_directory = page_directory; process->entry = entry;
    process->user_stack = user_stack; process->kernel_stack_top = (uint32_t)(uintptr_t)kernel_stack + 4096u;
    process->saved_frame = 0; process->user_code_page = user_code_page; process->user_stack_page = user_stack_page;
    init_interrupt_frame(process); return process;
}

int process_set_current(struct process *process) {
    if (!process || process->state == PROCESS_UNUSED || process->page_directory == 0) return -1;
    if (current && current != process && current->state == PROCESS_RUNNING) current->state = PROCESS_READY;
    current = process; current->state = PROCESS_RUNNING;
    paging_switch_address_space(current->page_directory); tss_set_kernel_stack(current->kernel_stack_top); return 0;
}

void process_exit_current(void) { if (current && current != &processes[0]) current->state = PROCESS_ZOMBIE; }

static void reap_process(struct process *process) {
    if (!process || process->state != PROCESS_ZOMBIE) return;
    if (process->user_code_page) page_free((void *)(uintptr_t)process->user_code_page);
    if (process->user_stack_page) page_free((void *)(uintptr_t)process->user_stack_page);
    if (process->page_directory) paging_destroy_address_space(process->page_directory);
    if (process->kernel_stack_top) page_free((void *)(uintptr_t)(process->kernel_stack_top - 4096u));
    process->pid = 0; process->state = PROCESS_UNUSED; process->page_directory = 0; process->entry = 0;
    process->user_stack = 0; process->kernel_stack_top = 0; process->saved_frame = 0;
    process->user_code_page = 0; process->user_stack_page = 0;
}

uint32_t process_count(void) {
    uint32_t count = 0; for (uint32_t i = 0; i < LIONOS_PROCESS_MAX; ++i)
        if (processes[i].state != PROCESS_UNUSED) ++count; return count;
}
void process_set_saved_frame(struct process *process, uint32_t *frame) { if (process) process->saved_frame = (uint32_t)(uintptr_t)frame; }
uint32_t *process_saved_frame(struct process *process) { return process ? (uint32_t *)(uintptr_t)process->saved_frame : 0; }
uint32_t process_kernel_stack_top(struct process *process) { return process ? process->kernel_stack_top : 0; }

uint32_t *process_schedule(uint32_t *frame) {
    if (!current) return frame;
    struct process *previous = current;
    if (current->state == PROCESS_RUNNING) current->saved_frame = (uint32_t)(uintptr_t)frame;
    uint32_t start = process_index(current);
    for (uint32_t step = 1; step <= LIONOS_PROCESS_MAX; ++step) {
        uint32_t index = (start + step) % LIONOS_PROCESS_MAX;
        struct process *candidate = &processes[index];
        if (candidate->state != PROCESS_READY) continue;
        if (current->state == PROCESS_RUNNING) current->state = PROCESS_READY;
        current = candidate; current->state = PROCESS_RUNNING;
        paging_switch_address_space(current->page_directory); tss_set_kernel_stack(current->kernel_stack_top);
        if (previous != current) reap_process(previous);
        return process_saved_frame(current);
    }
    if (current->state == PROCESS_ZOMBIE) {
        current = &processes[0]; current->state = PROCESS_RUNNING;
        paging_switch_address_space(current->page_directory); tss_set_kernel_stack(current->kernel_stack_top);
        reap_process(previous); return process_saved_frame(current);
    }
    current->state = PROCESS_RUNNING; tss_set_kernel_stack(current->kernel_stack_top); return frame;
}

void scheduler_idle(void) { for (;;) __asm__ volatile ("sti; hlt"); }
