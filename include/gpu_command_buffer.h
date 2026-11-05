#ifndef LIONOS_GPU_COMMAND_BUFFER_H
#define LIONOS_GPU_COMMAND_BUFFER_H

#include <stdint.h>

/*
 * CPU-side command-stream storage only.
 *
 * The buffer owns no memory: storage is caller-provided, 32-bit aligned, and
 * remains valid for the buffer lifetime. This module does not define vendor
 * opcodes, map storage into a GPU address space, or submit commands.
 */
typedef enum {
    GPU_COMMAND_BUFFER_UNINITIALIZED = 0,
    GPU_COMMAND_BUFFER_OPEN = 1,
    GPU_COMMAND_BUFFER_SEALED = 2
} gpu_command_buffer_state_t;

typedef struct {
    uint32_t *storage;
    uint32_t capacity_dwords;
    uint32_t used_dwords;
    gpu_command_buffer_state_t state;
} gpu_command_buffer_t;

/* Read-only CPU view, available only after the command buffer is sealed. */
typedef struct {
    const uint32_t *words;
    uint32_t word_count;
} gpu_command_buffer_view_t;

/* Initialize an empty buffer over caller-owned storage. */
int gpu_command_buffer_init(gpu_command_buffer_t *buffer,
                            uint32_t *storage,
                            uint32_t capacity_dwords);

/* Append opaque dwords; no interpretation or hardware submission occurs. */
int gpu_command_buffer_append_u32(gpu_command_buffer_t *buffer,
                                  uint32_t value);
int gpu_command_buffer_append_words(gpu_command_buffer_t *buffer,
                                    const uint32_t *words,
                                    uint32_t word_count);

/* Sealing prevents further appends until reset. */
int gpu_command_buffer_seal(gpu_command_buffer_t *buffer);
int gpu_command_buffer_reset(gpu_command_buffer_t *buffer);
int gpu_command_buffer_get_view(const gpu_command_buffer_t *buffer,
                                  gpu_command_buffer_view_t *out_view);

/* Deterministic RAM-only bounds/state/aliasing test. */
int gpu_command_buffer_selftest(void);

#endif
