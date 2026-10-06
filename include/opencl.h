#ifndef LIONOS_OPENCL_H
#define LIONOS_OPENCL_H

#include <stdint.h>

/*
 * LionCL is LionOS's dependency-free OpenCL execution layer.
 *
 * It implements the execution primitives needed by the kernel today instead
 * of pretending to be a complete Khronos OpenCL implementation. The interface
 * is device-independent so a future Intel/NVIDIA backend can replace the CPU
 * backend without changing callers.
 */

enum {
    OPENCL_BACKEND_NONE = 0u,
    OPENCL_BACKEND_CPU  = 1u,
    OPENCL_BACKEND_GPU  = 2u
};

typedef struct {
    uint32_t available;
    uint32_t backend;
    uint32_t max_work_items;
    uint32_t local_mem_bytes;
    const char *platform_name;
    const char *device_name;
    const char *backend_name;
} opencl_device_info_t;

typedef void (*opencl_kernel_fn)(uint32_t global_id, void *args);

int opencl_init(void);
int opencl_available(void);
int opencl_get_device_info(opencl_device_info_t *out);
int opencl_enqueue_ndrange_1d(opencl_kernel_fn kernel, uint32_t global_size, void *args);
int opencl_vector_add_u32(const uint32_t *a, const uint32_t *b, uint32_t *out, uint32_t count);
int opencl_self_test(void);

#endif
