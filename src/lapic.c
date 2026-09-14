#include <stdint.h>
#include "lapic.h"
#include "paging.h"
#include "memory.h"

#define IA32_APIC_BASE_MSR 0x1Bu
#define APIC_BASE_ENABLE 0x800u
#define LAPIC_REG_ID 0x020u
#define LAPIC_REG_SIVR 0x0F0u
#define LAPIC_REG_EOI 0x0B0u
#define LAPIC_SIVR_ENABLE 0x100u

static volatile uint32_t *lapic = (volatile uint32_t *)(uintptr_t)LIONOS_LAPIC_VIRT;
static uint32_t initialized;

static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi) : "memory");
}

static uint32_t read_reg(uint32_t reg) { return lapic[reg / 4u]; }
static void write_reg(uint32_t reg, uint32_t value) { lapic[reg / 4u] = value; }

void lapic_enable(void) {
    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);
    base |= APIC_BASE_ENABLE;
    wrmsr(IA32_APIC_BASE_MSR, base);
    write_reg(LAPIC_REG_SIVR, read_reg(LAPIC_REG_SIVR) | LAPIC_SIVR_ENABLE | 0xFFu);
    initialized = 1u;
}

int lapic_init(void) {
    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);
    if ((base & APIC_BASE_ENABLE) == 0u) {
        base |= APIC_BASE_ENABLE;
        wrmsr(IA32_APIC_BASE_MSR, base);
    }

    /* The first 256 MiB is identity mapped; add the APIC MMIO page above it. */
    if (paging_map_kernel_page(LIONOS_LAPIC_VIRT, LIONOS_LAPIC_PHYS, 0x3u) != 0)
        return -1;

    lapic_enable();
    return 0;
}

uint32_t lapic_id(void) {
    return initialized ? (read_reg(LAPIC_REG_ID) >> 24) : 0xFFFFFFFFu;
}

void lapic_eoi(void) {
    if (initialized) write_reg(LAPIC_REG_EOI, 0u);
}
