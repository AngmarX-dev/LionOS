#include <stdint.h>
#include "user_libc.h"

int main(void){
    volatile uint32_t shared_value=0x11223344u;
    uint32_t child=lion_fork();
    if(child==LIONOS_SYSCALL_ERR)return 1;
    if(child==0u){
        shared_value=0x55667788u;
        if(shared_value!=0x55667788u)lion_exit_code(2u);
        lion_exit_code(0u);
    }
    for(uint32_t i=0u;i<4u;++i)(void)lion_yield();
    if(shared_value!=0x11223344u)return 3;
    int32_t status=-1;
    if(lion_waitpid(child,&status)!=(int32_t)child)return 4;
    if(status!=0)return 5;
    if(shared_value!=0x11223344u)return 6;

    /* A nested fork must not let a reaped grandchild release the parent's
       still-mapped stack page. Follow it with allocation pressure so stale
       physical-page ownership is observable rather than merely latent. */
    volatile uint32_t nested_guard=0xA55A3CC3u;
    uint32_t nested=lion_fork();
    if(nested==LIONOS_SYSCALL_ERR)return 7;
    if(nested==0u){
        volatile uint32_t child_guard=0x13579BDFu;
        uint32_t grandchild=lion_fork();
        if(grandchild==LIONOS_SYSCALL_ERR)lion_exit_code(20u);
        if(grandchild==0u){
            child_guard=0x2468ACE0u;
            if(child_guard!=0x2468ACE0u)lion_exit_code(21u);
            lion_exit_code(0u);
        }
        int32_t nested_status=-1;
        if(lion_waitpid(grandchild,&nested_status)!=(int32_t)grandchild||
           nested_status!=0)lion_exit_code(22u);
        if(child_guard!=0x13579BDFu)lion_exit_code(23u);

        for(uint32_t i=0u;i<6u;++i){
            uint32_t pressure=lion_fork();
            if(pressure==LIONOS_SYSCALL_ERR)lion_exit_code(24u);
            if(pressure==0u){
                volatile uint32_t scratch[32];
                for(uint32_t j=0u;j<32u;++j)scratch[j]=i+j;
                if(scratch[0]!=i||scratch[31]!=i+31u)
                    lion_exit_code(27u);
                lion_exit_code(0u);
            }
            int32_t pressure_status=-1;
            if(lion_waitpid(pressure,&pressure_status)!=(int32_t)pressure||
               pressure_status!=0)lion_exit_code(25u);
        }
        if(child_guard!=0x13579BDFu)lion_exit_code(26u);
        lion_exit_code(0u);
    }
    status=-1;
    if(lion_waitpid(nested,&status)!=(int32_t)nested||status!=0)return 8;
    if(nested_guard!=0xA55A3CC3u)return 9;
    return 0;
}
