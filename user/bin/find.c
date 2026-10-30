#include "user_libc.h"
int main(int argc,char**argv){(void)argc;(void)argv;char n[64];uint32_t i=0;while(lion_getfile(i,n,sizeof(n))>=0){puts(n);++i;}return 0;}
