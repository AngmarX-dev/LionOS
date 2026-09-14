#ifndef LIONOS_PROCESS_H
#define LIONOS_PROCESS_H

#include <stdint.h>

#define LIONOS_PROCESS_MAX 16u
#define PROCESS_UNUSED 0u
#define PROCESS_READY 1u
#define PROCESS_RUNNING 2u

struct process {
    uint32_t pid;
    uint32_t state;
    uint32_t page_directory;
    uint32_t entry;
    uint32_t user_stack;
};

void process_init(void);
struct process *process_current(void);
uint32_t process_current_pid(void);

#endif
