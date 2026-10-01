#include <stdint.h>
#include "user_libc.h"
#include "vfs_uapi.h"
int main(void){
    static const char path[]="userspace-vfs-test.txt";
    static const char payload[]="LionOS userspace VFS integration OK\n";
    char buffer[64];struct lion_stat st;
    int32_t fd=lion_open(path,LIONOS_O_WRITE);if(fd<0)return 1;
    if(lion_fwrite(fd,payload,sizeof(payload)-1u)!=(int32_t)(sizeof(payload)-1u)){lion_close(fd);return 2;}
    if(lion_close(fd)<0)return 3;if(lion_stat(path,&st)<0||st.size!=sizeof(payload)-1u)return 4;
    fd=lion_open(path,LIONOS_O_READ);if(fd<0)return 5;
    int32_t n=lion_fread(fd,buffer,sizeof(buffer)-1u);if(n!=(int32_t)(sizeof(payload)-1u)){lion_close(fd);return 6;}
    buffer[n]=0;if(strcmp(buffer,payload)!=0){lion_close(fd);return 7;}
    if(lion_close(fd)<0||lion_remove(path)<0)return 8;if(lion_stat(path,&st)>=0)return 9;return 0;
}
