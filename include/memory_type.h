#ifndef LIONOS_MEMORY_TYPE_H
#define LIONOS_MEMORY_TYPE_H

#include <stdint.h>
#include "cpu.h"

typedef enum {
    MEMORY_TYPE_UC = 0u,
    MEMORY_TYPE_WC = 1u,
    MEMORY_TYPE_WT = 4u,
    MEMORY_TYPE_WP = 5u,
    MEMORY_TYPE_WB = 6u,
    MEMORY_TYPE_UC_MINUS = 7u
} memory_type_t;

typedef enum {
    MEMORY_TYPE_SOURCE_DEFAULT = 0u,
    MEMORY_TYPE_SOURCE_FIXED,
    MEMORY_TYPE_SOURCE_VARIABLE,
    MEMORY_TYPE_SOURCE_MTRR_DISABLED
} memory_type_source_t;

typedef enum {
    MEMORY_TYPE_RESOLVE_OK = 0,
    MEMORY_TYPE_RESOLVE_INVALID_ARGUMENT = -1,
    MEMORY_TYPE_RESOLVE_UNSUPPORTED = -2,
    MEMORY_TYPE_RESOLVE_INCOMPLETE_SNAPSHOT = -3,
    MEMORY_TYPE_RESOLVE_INVALID_MTRR = -4,
    MEMORY_TYPE_RESOLVE_UNDEFINED_OVERLAP = -5,
    MEMORY_TYPE_RESOLVE_INVALID_PAT = -6,
    MEMORY_TYPE_RESOLVE_ADDRESS_RANGE = -7,
    MEMORY_TYPE_RESOLVE_INVALID_CPU_STATE = -8
} memory_type_resolve_status_t;

typedef struct {
    uint8_t mtrr_type;
    uint8_t pat_type;
    uint8_t effective_type;
    uint8_t source;
    uint8_t matched_variable_count;
} memory_type_result_t;

/* pat_index is the decoded 3-bit PAT:PCD:PWT index. */
int memory_type_resolve(const cpu_mtrr_info_t *mtrr,
                        const cpu_memory_type_state_t *cpu_state,
                        uint64_t physical_address,
                        uint8_t pat_index,
                        memory_type_result_t *out);

/* Pure host-side tests; no privileged instructions or hardware access. */
int memory_type_resolver_selftest(void);

#endif
