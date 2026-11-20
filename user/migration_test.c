#include <stdint.h>
#include "user_api.h"

/*
 * A deliberately side-effect-free user process for scheduler migration tests.
 * Volatile state keeps the bounded workload observable to the compiler while
 * avoiding shared filesystem or console state between simultaneous instances.
 * Periodic yields leave READY windows for an idle AP to steal the task.
 */
int main(void){
    volatile uint32_t state=0x4C494F4Eu;
    for(uint32_t round=0u;round<32u;++round){
        for(uint32_t i=0u;i<2000000u;++i)
            state=(state*1664525u)+1013904223u+i;
        (void)lion_yield();
    }
    (void)state;
    return 0;
}
