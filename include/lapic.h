#ifndef LIONOS_LAPIC_H
#define LIONOS_LAPIC_H

#include <stdint.h>

#define LIONOS_LAPIC_PHYS 0xFEE00000u
#define LIONOS_LAPIC_VIRT 0xFEE00000u
#define LIONOS_LAPIC_TIMER_VECTOR 48u

int lapic_init(void);
uint32_t lapic_id(void);
void lapic_eoi(void);
void lapic_enable(void);
void lapic_send_init(uint32_t apic_id);
void lapic_send_startup(uint32_t apic_id, uint32_t vector);
void lapic_timer_init(void);
void lapic_timer_tick(void);
uint32_t lapic_timer_ticks(void);
int lapic_frame_timer_init(uint32_t hz);
int lapic_frame_timer_active(void);
uint32_t lapic_frame_ticks(void);
void lapic_frame_timer_tick(void);

#endif
