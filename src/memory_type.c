#include <stdint.h>
#include "memory_type.h"

#define MTRR_VARIABLE_VALID (1ULL << 11)
#define MTRR_BASE_RESERVED_MASK 0xF00ULL
#define MTRR_MASK_RESERVED_LOW 0x7FFULL
#define MTRR_PAGE_MASK 0xFFFULL
#define MTRR_FIXED_LIMIT 0x100000ULL

static int memory_type_valid_mtrr(uint8_t type, uint8_t wc_supported)
{
    if (type == MEMORY_TYPE_UC || type == MEMORY_TYPE_WT ||
        type == MEMORY_TYPE_WP || type == MEMORY_TYPE_WB)
        return 1;
    return type == MEMORY_TYPE_WC && wc_supported;
}

static int memory_type_valid_pat(uint8_t type)
{
    return type == MEMORY_TYPE_UC || type == MEMORY_TYPE_UC_MINUS ||
           type == MEMORY_TYPE_WC || type == MEMORY_TYPE_WT ||
           type == MEMORY_TYPE_WP || type == MEMORY_TYPE_WB;
}

static int memory_type_fixed_lookup(const cpu_mtrr_info_t *mtrr,
                                    uint64_t address, uint8_t *type_out)
{
    uint32_t reg;
    uint32_t slot;
    uint64_t offset;

    if (!mtrr->enabled || !mtrr->fixed_enabled ||
        !mtrr->fixed_supported || address >= MTRR_FIXED_LIMIT)
        return 0;

    if (address < 0x80000ULL) {
        reg = 0u;
        slot = (uint32_t)(address >> 16);
    } else if (address < 0xA0000ULL) {
        reg = 1u;
        slot = (uint32_t)((address - 0x80000ULL) >> 14);
    } else if (address < 0xC0000ULL) {
        reg = 2u;
        slot = (uint32_t)((address - 0xA0000ULL) >> 14);
    } else {
        offset = address - 0xC0000ULL;
        reg = 3u + (uint32_t)(offset >> 15);
        slot = (uint32_t)((offset & 0x7FFFULL) >> 12);
    }

    if (reg >= CPU_MTRR_FIXED_REGISTER_COUNT || slot >= 8u)
        return -1;
    *type_out = (uint8_t)((mtrr->fixed[reg] >> (slot * 8u)) & 0xFFu);
    return 1;
}

static int memory_type_variable_match(const cpu_mtrr_info_t *mtrr,
                                      uint32_t index, uint64_t address,
                                      uint64_t address_mask,
                                      uint8_t *type_out)
{
    uint64_t base = mtrr->variable[index].base;
    uint64_t mask = mtrr->variable[index].mask;
    uint64_t range_mask;
    uint64_t inverse;
    uint64_t size;
    uint64_t base_address;
    uint8_t type;

    if ((mask & MTRR_VARIABLE_VALID) == 0u)
        return 0;

    if ((mask & MTRR_MASK_RESERVED_LOW) != 0u ||
        (mask & ~(address_mask | MTRR_VARIABLE_VALID)) != 0u ||
        (base & MTRR_BASE_RESERVED_MASK) != 0u ||
        (base & ~(address_mask | MTRR_BASE_RESERVED_MASK | 0xFFULL)) != 0u)
        return -1;

    type = (uint8_t)(base & 0xFFu);
    if (!memory_type_valid_mtrr(type, mtrr->write_combining_supported))
        return -1;

    range_mask = mask & address_mask;
    inverse = address_mask & ~range_mask;
    size = inverse + 1u;
    if (size < 0x1000ULL || (size & (size - 1u)) != 0u ||
        range_mask != (address_mask & ~(size - 1u)))
        return -1;

    base_address = base & address_mask;
    if ((base_address & (size - 1u)) != 0u)
        return -1;
    if ((address & range_mask) != base_address)
        return 0;

    *type_out = type;
    return 1;
}

static int memory_type_resolve_mtrr(const cpu_mtrr_info_t *mtrr,
                                    uint64_t address,
                                    uint64_t address_mask,
                                    uint8_t *type_out,
                                    uint8_t *source_out,
                                    uint8_t *match_count_out)
{
    uint8_t fixed_type;
    uint8_t first_type = 0u;
    uint32_t matches = 0u;
    uint32_t seen_uc = 0u;
    uint32_t seen_wb = 0u;
    uint32_t seen_wt = 0u;
    uint32_t seen_other = 0u;
    uint32_t all_same = 1u;
    uint8_t type;

    if (!mtrr->enabled) {
        *type_out = MEMORY_TYPE_UC;
        *source_out = MEMORY_TYPE_SOURCE_MTRR_DISABLED;
        *match_count_out = 0u;
        return MEMORY_TYPE_RESOLVE_OK;
    }

    {
        int fixed = memory_type_fixed_lookup(mtrr, address, &fixed_type);
        if (fixed < 0 ||
            (fixed > 0 &&
             !memory_type_valid_mtrr(fixed_type,
                                     mtrr->write_combining_supported)))
            return MEMORY_TYPE_RESOLVE_INVALID_MTRR;
        if (fixed > 0) {
            *type_out = fixed_type;
            *source_out = MEMORY_TYPE_SOURCE_FIXED;
            *match_count_out = 0u;
            return MEMORY_TYPE_RESOLVE_OK;
        }
    }

    if ((mtrr->default_type & ~0xCFFULL) != 0u ||
        mtrr->truncated ||
        mtrr->captured_variable_count != mtrr->variable_count ||
        mtrr->captured_variable_count > CPU_MTRR_MAX_VARIABLE_RANGES)
        return MEMORY_TYPE_RESOLVE_INCOMPLETE_SNAPSHOT;

    for (uint32_t i = 0u; i < mtrr->captured_variable_count; ++i) {
        int matched = memory_type_variable_match(
            mtrr, i, address, address_mask, &type);
        if (matched < 0)
            return MEMORY_TYPE_RESOLVE_INVALID_MTRR;
        if (matched == 0)
            continue;

        if (matches == 0u) {
            first_type = type;
        } else if (type != first_type) {
            all_same = 0u;
        }
        if (type == MEMORY_TYPE_UC) seen_uc = 1u;
        else if (type == MEMORY_TYPE_WB) seen_wb = 1u;
        else if (type == MEMORY_TYPE_WT) seen_wt = 1u;
        else seen_other = 1u;
        ++matches;
    }

    if (matches == 0u) {
        type = (uint8_t)(mtrr->default_type & 0xFFu);
        if (!memory_type_valid_mtrr(type,
                                    mtrr->write_combining_supported))
            return MEMORY_TYPE_RESOLVE_INVALID_MTRR;
        *type_out = type;
        *source_out = MEMORY_TYPE_SOURCE_DEFAULT;
    } else if (all_same) {
        *type_out = first_type;
        *source_out = MEMORY_TYPE_SOURCE_VARIABLE;
    } else if (seen_uc) {
        *type_out = MEMORY_TYPE_UC;
        *source_out = MEMORY_TYPE_SOURCE_VARIABLE;
    } else if (seen_wb && seen_wt && !seen_other) {
        *type_out = MEMORY_TYPE_WT;
        *source_out = MEMORY_TYPE_SOURCE_VARIABLE;
    } else {
        return MEMORY_TYPE_RESOLVE_UNDEFINED_OVERLAP;
    }

    *match_count_out = (uint8_t)(matches > 255u ? 255u : matches);
    return MEMORY_TYPE_RESOLVE_OK;
}

static int memory_type_combine(uint8_t mtrr_type, uint8_t pat_type,
                               uint8_t *effective_out)
{
    /* Intel SDM, effective page-level memory-type table, normal caching. */
    switch (mtrr_type) {
    case MEMORY_TYPE_UC:
        *effective_out = (pat_type == MEMORY_TYPE_WC)
                       ? MEMORY_TYPE_WC : MEMORY_TYPE_UC;
        return 0;
    case MEMORY_TYPE_WC:
        if (pat_type == MEMORY_TYPE_UC ||
            pat_type == MEMORY_TYPE_WT ||
            pat_type == MEMORY_TYPE_WP)
            *effective_out = MEMORY_TYPE_UC;
        else
            *effective_out = MEMORY_TYPE_WC;
        return 0;
    case MEMORY_TYPE_WT:
        if (pat_type == MEMORY_TYPE_UC ||
            pat_type == MEMORY_TYPE_UC_MINUS)
            *effective_out = MEMORY_TYPE_UC;
        else if (pat_type == MEMORY_TYPE_WC)
            *effective_out = MEMORY_TYPE_WC;
        else if (pat_type == MEMORY_TYPE_WP)
            *effective_out = MEMORY_TYPE_WP;
        else
            *effective_out = MEMORY_TYPE_WT;
        return 0;
    case MEMORY_TYPE_WB:
        if (pat_type == MEMORY_TYPE_UC ||
            pat_type == MEMORY_TYPE_UC_MINUS)
            *effective_out = MEMORY_TYPE_UC;
        else if (pat_type == MEMORY_TYPE_WC)
            *effective_out = MEMORY_TYPE_WC;
        else if (pat_type == MEMORY_TYPE_WT)
            *effective_out = MEMORY_TYPE_WT;
        else if (pat_type == MEMORY_TYPE_WP)
            *effective_out = MEMORY_TYPE_WP;
        else
            *effective_out = MEMORY_TYPE_WB;
        return 0;
    case MEMORY_TYPE_WP:
        if (pat_type == MEMORY_TYPE_UC)
            *effective_out = MEMORY_TYPE_UC;
        else if (pat_type == MEMORY_TYPE_UC_MINUS ||
                 pat_type == MEMORY_TYPE_WC)
            *effective_out = MEMORY_TYPE_WC;
        else if (pat_type == MEMORY_TYPE_WT)
            *effective_out = MEMORY_TYPE_WT;
        else
            *effective_out = MEMORY_TYPE_WP;
        return 0;
    default:
        return -1;
    }
}

int memory_type_resolve(const cpu_mtrr_info_t *mtrr,
                        const cpu_memory_type_state_t *cpu_state,
                        uint64_t physical_address, uint8_t pat_index,
                        memory_type_result_t *out)
{
    uint64_t physical_mask;
    uint64_t address_mask;
    uint8_t pat_type;
    uint8_t mtrr_type;
    uint8_t source;
    uint8_t match_count;
    uint8_t effective_type;
    int rc;

    if (!mtrr || !cpu_state || !out || pat_index >= 8u)
        return MEMORY_TYPE_RESOLVE_INVALID_ARGUMENT;
    *out = (memory_type_result_t){0};

    if (!mtrr->msr_supported || !mtrr->supported ||
        !cpu_state->pat_supported || mtrr->physical_address_bits < 32u ||
        mtrr->physical_address_bits > 52u)
        return MEMORY_TYPE_RESOLVE_UNSUPPORTED;

    if (cpu_state->cr0_not_write_through &&
        !cpu_state->cr0_cache_disable)
        return MEMORY_TYPE_RESOLVE_INVALID_CPU_STATE;

    physical_mask = (1ULL << mtrr->physical_address_bits) - 1ULL;
    address_mask = physical_mask & ~MTRR_PAGE_MASK;
    if (physical_address > physical_mask)
        return MEMORY_TYPE_RESOLVE_ADDRESS_RANGE;

    pat_type = (uint8_t)((cpu_state->pat >> (pat_index * 8u)) & 0xFFu);
    if (!memory_type_valid_pat(pat_type))
        return MEMORY_TYPE_RESOLVE_INVALID_PAT;

    rc = memory_type_resolve_mtrr(mtrr, physical_address, address_mask,
                                  &mtrr_type, &source, &match_count);
    if (rc != MEMORY_TYPE_RESOLVE_OK)
        return rc;

    if (cpu_state->cr0_cache_disable)
        effective_type = MEMORY_TYPE_UC;
    else if (memory_type_combine(mtrr_type, pat_type,
                                 &effective_type) != 0)
        return MEMORY_TYPE_RESOLVE_INVALID_MTRR;

    out->mtrr_type = mtrr_type;
    out->pat_type = pat_type;
    out->effective_type = effective_type;
    out->source = source;
    out->matched_variable_count = match_count;
    return MEMORY_TYPE_RESOLVE_OK;
}

int memory_type_resolver_selftest(void)
{
    cpu_mtrr_info_t mtrr = {0};
    cpu_memory_type_state_t cpu = {0};
    memory_type_result_t result;
    const uint64_t address_mask = ((1ULL << 36) - 1ULL) & ~0xFFFULL;
    int rc;

    mtrr.msr_supported = 1u;
    mtrr.supported = 1u;
    mtrr.enabled = 1u;
    mtrr.write_combining_supported = 1u;
    mtrr.physical_address_bits = 36u;
    mtrr.default_type = MEMORY_TYPE_WB;
    cpu.pat_supported = 1u;
    cpu.pat = 0x0007040600070406ULL;

    rc = memory_type_resolve(&mtrr, &cpu, 0x200000ULL, 0u, &result);
    if (rc != MEMORY_TYPE_RESOLVE_OK ||
        result.effective_type != MEMORY_TYPE_WB ||
        result.source != MEMORY_TYPE_SOURCE_DEFAULT)
        return -1;

    /* MTRR UC + PAT WC is WC according to the architectural table. */
    mtrr.default_type = MEMORY_TYPE_UC;
    cpu.pat = (cpu.pat & ~(0xFFULL << 8)) | (MEMORY_TYPE_WC << 8);
    rc = memory_type_resolve(&mtrr, &cpu, 0x200000ULL, 1u, &result);
    if (rc != MEMORY_TYPE_RESOLVE_OK ||
        result.effective_type != MEMORY_TYPE_WC)
        return -1;

    mtrr.default_type = MEMORY_TYPE_UC;
    mtrr.variable_count = 1u;
    mtrr.captured_variable_count = 1u;
    mtrr.variable[0].base = 0x400000ULL | MEMORY_TYPE_WB;
    mtrr.variable[0].mask = (address_mask & ~0x3FFFFFULL) |
                            MTRR_VARIABLE_VALID;
    rc = memory_type_resolve(&mtrr, &cpu, 0x401000ULL, 0u, &result);
    if (rc != MEMORY_TYPE_RESOLVE_OK ||
        result.mtrr_type != MEMORY_TYPE_WB ||
        result.source != MEMORY_TYPE_SOURCE_VARIABLE)
        return -1;

    mtrr.variable[0].mask = (address_mask & ~0x2FFFFFULL) |
                            MTRR_VARIABLE_VALID;
    rc = memory_type_resolve(&mtrr, &cpu, 0x401000ULL, 0u, &result);
    if (rc != MEMORY_TYPE_RESOLVE_INVALID_MTRR)
        return -1;

    mtrr.variable[0].mask = (address_mask & ~0x3FFFFFULL) |
                            MTRR_VARIABLE_VALID;
    mtrr.fixed_supported = 1u;
    mtrr.fixed_enabled = 1u;
    mtrr.fixed[0] = 0x0606060606060606ULL;
    rc = memory_type_resolve(&mtrr, &cpu, 0x1000ULL, 0u, &result);
    if (rc != MEMORY_TYPE_RESOLVE_OK ||
        result.source != MEMORY_TYPE_SOURCE_FIXED ||
        result.mtrr_type != MEMORY_TYPE_WB)
        return -1;

    cpu.cr0_cache_disable = 1u;
    rc = memory_type_resolve(&mtrr, &cpu, 0x1000ULL, 0u, &result);
    if (rc != MEMORY_TYPE_RESOLVE_OK ||
        result.effective_type != MEMORY_TYPE_UC)
        return -1;
    cpu.cr0_cache_disable = 0u;

    mtrr.fixed_supported = 0u;
    mtrr.variable_count = 2u;
    mtrr.captured_variable_count = 2u;
    mtrr.variable[0].base = 0x400000ULL | MEMORY_TYPE_WB;
    mtrr.variable[0].mask = (address_mask & ~0x3FFFFFULL) |
                            MTRR_VARIABLE_VALID;
    mtrr.variable[1].base = 0x400000ULL | MEMORY_TYPE_WT;
    mtrr.variable[1].mask = (address_mask & ~0x3FFFFFULL) |
                            MTRR_VARIABLE_VALID;
    rc = memory_type_resolve(&mtrr, &cpu, 0x401000ULL, 0u, &result);
    if (rc != MEMORY_TYPE_RESOLVE_OK ||
        result.mtrr_type != MEMORY_TYPE_WT)
        return -1;

    mtrr.variable[1].base = 0x400000ULL | MEMORY_TYPE_UC;
    rc = memory_type_resolve(&mtrr, &cpu, 0x401000ULL, 0u, &result);
    if (rc != MEMORY_TYPE_RESOLVE_OK ||
        result.mtrr_type != MEMORY_TYPE_UC)
        return -1;

    mtrr.variable[1].base = 0x400000ULL | MEMORY_TYPE_WC;
    rc = memory_type_resolve(&mtrr, &cpu, 0x401000ULL, 0u, &result);
    if (rc != MEMORY_TYPE_RESOLVE_UNDEFINED_OVERLAP)
        return -1;

    mtrr.truncated = 1u;
    rc = memory_type_resolve(&mtrr, &cpu, 0x401000ULL, 0u, &result);
    if (rc != MEMORY_TYPE_RESOLVE_INCOMPLETE_SNAPSHOT)
        return -1;

    mtrr.truncated = 0u;
    mtrr.variable_count = 0u;
    mtrr.captured_variable_count = 0u;
    rc = memory_type_resolve(&mtrr, &cpu, 1ULL << 36, 0u, &result);
    if (rc != MEMORY_TYPE_RESOLVE_ADDRESS_RANGE)
        return -1;

    return 0;
}
