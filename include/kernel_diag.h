#ifndef LIONOS_KERNEL_DIAG_H
#define LIONOS_KERNEL_DIAG_H

#include <stdint.h>

enum kernel_log_level {
    KLOG_DEBUG = 0u,
    KLOG_INFO = 1u,
    KLOG_WARN = 2u,
    KLOG_ERROR = 3u,
    KLOG_PANIC = 4u
};

void kernel_log(enum kernel_log_level level, const char *message);
void kernel_panic(uint32_t vector, uint32_t error, uint32_t eip, uint32_t cs, uint32_t eflags);
void kernel_diag_print(void);
uint32_t kernel_diag_log_count(enum kernel_log_level level);
void kernel_diag_record_interrupt(uint32_t cpu, uint32_t vector);
uint32_t kernel_diag_interrupt_count(uint32_t cpu, uint32_t vector);

#endif
