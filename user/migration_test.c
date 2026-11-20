#include <stdint.h>

/*
 * A deliberately side-effect-free user process for scheduler migration tests.
 * Volatile state keeps the bounded workload observable to the compiler while
 * avoiding shared filesystem or console state between simultaneous instances.
 */
int main(void){
    volatile uint32_t state=0x4C494F4Eu;
    for(uint32_t i=0u;i<2000000u;++i)
        state=(state*1664525u)+1013904223u+i;
    (void)state;
    return 0;
}
