#include <stdint.h>
#include "user_libc.h"
int main(void){
    volatile uint32_t shared_value=0x11223344u;
    uint32_t child=lion_fork();if(child==LIONOS_SYSCALL_ERR)return 1;
    if(child==0u){shared_value=0x55667788u;if(shared_value!=0x55667788u)lion_exit_code(2u);lion_exit_code(0u);}
    for(uint32_t i=0u;i<4u;++i)(void)lion_yield();
    if(shared_value!=0x11223344u)return 3;
    int32_t status=-1;if(lion_waitpid(child,&status)!=(int32_t)child)return 4;
    if(status!=0)return 5;if(shared_value!=0x11223344u)return 6;return 0;
}
