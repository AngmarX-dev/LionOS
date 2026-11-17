#include <stdint.h>
#include "paging.h"
#include "usercopy.h"

#define USER_MIN 0x10000000u
#define USER_MAX 0xC0000000u
#define PAGE_SIZE 4096u
#define PTE_WRITABLE 0x2u
#define PTE_COW 0x200u

int usercopy_range_valid(uint32_t address, uint32_t length) {
    if (address < USER_MIN || address >= USER_MAX) return 0;
    if (length == 0u) return 1;
    return length <= USER_MAX - address;
}

static int validate_pages(uint32_t address, uint32_t length, int write) {
    if (!usercopy_range_valid(address, length)) return 0;
    if (length == 0u) return 1;

    uint32_t pd = paging_current_address_space();
    if (!pd) return 0;

    uint32_t end = address + length;
    uint32_t page = address & ~(PAGE_SIZE - 1u);

    /* Preflight every page before resolving COW or allowing a caller to copy. */
    for (uint32_t current = page; current < end; current += PAGE_SIZE) {
        uint32_t flags = 0u;
        if (paging_get_user_page(pd, current, 0, &flags) != 0) return 0;
        if (write && !(flags & PTE_WRITABLE) && !(flags & PTE_COW))
            return 0;
    }

    if (!write) return 1;

    for (uint32_t current = page; current < end; current += PAGE_SIZE) {
        uint32_t flags = 0u;
        if (paging_get_user_page(pd, current, 0, &flags) != 0) return 0;
        if (!(flags & PTE_WRITABLE) &&
            (!(flags & PTE_COW) || paging_resolve_cow(pd, current) != 0))
            return 0;
    }

    for (uint32_t current = page; current < end; current += PAGE_SIZE) {
        uint32_t flags = 0u;
        if (paging_get_user_page(pd, current, 0, &flags) != 0 ||
            !(flags & PTE_WRITABLE))
            return 0;
    }
    return 1;
}

int usercopy_validate_read(uint32_t address, uint32_t length) {
    return validate_pages(address, length, 0);
}

int usercopy_validate_write(uint32_t address, uint32_t length) {
    return validate_pages(address, length, 1);
}

int usercopy_copy_from(void *kernel_dst, uint32_t user_src, uint32_t length) {
    if (length != 0u && !kernel_dst) return -1;
    if (!usercopy_validate_read(user_src, length)) return -1;

    uint8_t *dst = (uint8_t *)kernel_dst;
    const volatile uint8_t *src = (const volatile uint8_t *)(uintptr_t)user_src;
    for (uint32_t i = 0u; i < length; ++i) dst[i] = src[i];
    return 0;
}

int usercopy_copy_to(uint32_t user_dst, const void *kernel_src, uint32_t length) {
    if (length != 0u && !kernel_src) return -1;
    if (!usercopy_validate_write(user_dst, length)) return -1;

    volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)user_dst;
    const uint8_t *src = (const uint8_t *)kernel_src;
    for (uint32_t i = 0u; i < length; ++i) dst[i] = src[i];
    return 0;
}

int usercopy_copy_string(char *kernel_dst, uint32_t dst_size, uint32_t user_src) {
    if (!kernel_dst || dst_size < 2u ||
        !usercopy_range_valid(user_src, 1u))
        return -1;

    for (uint32_t i = 0u; i < dst_size - 1u; ++i) {
        if (i > USER_MAX - user_src - 1u) return -1;
        uint8_t ch = 0u;
        if (usercopy_copy_from(&ch, user_src + i, 1u) != 0) return -1;
        kernel_dst[i] = (char)ch;
        if (ch == 0u) return 0;
    }

    kernel_dst[dst_size - 1u] = '\0';
    return -1;
}
