#ifndef LIONOS_EXEC_H
#define LIONOS_EXEC_H

#include <stdint.h>

int exec_validate_image(const uint8_t *image, uint32_t size, uint32_t *entry, uint32_t *stack_top);
int exec_run_file(const char *name);

#endif
