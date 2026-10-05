#include "user_libc.h"
static int cp(const char*s,const char*d){int32_t a=lion_open(s,LIONOS_O_READ),b;if(a<0)return-1;b=lion_open(d,LIONOS_O_WRITE);if(b<0){lion_close(a);return-1;}char x[512];int ok=1;for(;;){int32_t n=lion_fread(a,x,sizeof(x));if(n<0){ok=0;break;}if(!n)break;if(lion_fwrite(b,x,(uint32_t)n)<0){ok=0;break;}if((uint32_t)n<sizeof(x))break;}lion_close(a);lion_close(b);return ok?0:-1;}
int main(int argc,char**argv){if(argc!=3){puts("usage: cp <source> <destination>");return 1;}if(cp(argv[1],argv[2])<0){puts("cp: copy failed");return 1;}return 0;}
