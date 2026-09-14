#ifndef LIONOS_PROCESS_H
#define LIONOS_PROCESS_H

#include <stdint.h>

#define LIONOS_PROCESS_MAX 16u
#define PROCESS_UNUSED 0u
#define PROCESS_READY 1u
#define PROCESS_RUNNING 2u

#define PROCESS_CONTEXT_WORDS 19u

struct process {
    uint32_t pid;
    uint32_t state;
    uint32_t page_directory;
    uint32_t entry;
    uint32_t user_stack;
    uint32_t kernel_stack_top;
    uint32_t saved_frame;
};

void process_init(void);
struct process *process_current(void);
uint32_t process_current_pid(void);
struct process *process_create(uint32_t entry, uint32_t user_stack, uint32_t page_directory);
int process_set_current(struct process *process);
void process_exit_current(void);
uint32_t process_count(void);
uint32_t *process_schedule(uint32_t *frame);
void process_set_saved_frame(struct process *process, uint32_t *frame);
uint32_t *process_saved_frame(struct process *process);
uint32_t process_kernel_stack_top(struct process *process);
void scheduler_idle(void);

#endif
