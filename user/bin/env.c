#include "user_libc.h"
int main(void){static const char *n[]={"PATH","HOME","TERM","USER","SHELL"};for(uint32_t i=0;i<sizeof(n)/sizeof(n[0]);++i){const char*v=getenv(n[i]);if(v)printf("%s=%s\n",n[i],v);}return 0;}
