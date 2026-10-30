#include <stdint.h>
#include "browser.h"
#include "net.h"
#include "framebuffer.h"
#include "keyboard.h"
#include "mouse.h"

#define BW 5u
#define BH 7u
#define BS 2u
#define CW 12u
#define CH 18u
#define URL_MAX 180u
#define PAGE_MAX 8192u

static uint8_t active;
static uint8_t loading;
static uint8_t url_focus;
static char url[URL_MAX + 1u];
static uint32_t url_len;
static char page[PAGE_MAX + 1u];
static uint32_t page_len;
static char status[96];

static const uint8_t letters[26][7]={{14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{15,16,16,16,16,16,15},{30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},{15,16,16,23,17,17,15},{17,17,17,31,17,17,17},{31,4,4,4,4,4,31},{31,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},{17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},{30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},{17,17,17,17,10,10,4},{17,17,17,21,21,27,17},{17,17,10,4,10,17,17},{17,17,10,4,4,4,4},{31,1,2,4,8,16,31}};
static void glyph(char c,uint8_t r[7]){for(uint32_t i=0;i<7;i++)r[i]=0;if(c>='a'&&c<='z')c=(char)(c-'a'+'A');if(c>='A'&&c<='Z'){for(uint32_t i=0;i<7;i++)r[i]=letters[(uint32_t)(c-'A')][i];return;}switch(c){case '0':r[0]=14;r[1]=17;r[2]=19;r[3]=21;r[4]=25;r[5]=17;r[6]=14;break;case '1':r[0]=4;r[1]=12;r[2]=4;r[3]=4;r[4]=4;r[5]=4;r[6]=14;break;case '2':r[0]=14;r[1]=17;r[2]=1;r[3]=2;r[4]=4;r[5]=8;r[6]=31;break;case '3':r[0]=30;r[1]=1;r[2]=1;r[3]=14;r[4]=1;r[5]=1;r[6]=30;break;case '4':r[0]=2;r[1]=6;r[2]=10;r[3]=18;r[4]=31;r[5]=2;r[6]=2;break;case '5':r[0]=31;r[1]=16;r[2]=16;r[3]=30;r[4]=1;r[5]=1;r[6]=30;break;case '6':r[0]=14;r[1]=16;r[2]=16;r[3]=30;r[4]=17;r[5]=17;r[6]=14;break;case '7':r[0]=31;r[1]=1;r[2]=2;r[3]=4;r[4]=8;r[5]=8;r[6]=8;break;case '8':r[0]=14;r[1]=17;r[2]=17;r[3]=14;r[4]=17;r[5]=17;r[6]=14;break;case '9':r[0]=14;r[1]=17;r[2]=17;r[3]=15;r[4]=1;r[5]=1;r[6]=14;break;case ':':r[2]=4;r[4]=4;break;case '.':r[6]=4;break;case '/':r[0]=1;r[1]=2;r[2]=4;r[3]=8;r[4]=16;break;case '-':r[3]=31;break;case '_':r[6]=31;break;case '?':r[0]=14;r[1]=17;r[2]=1;r[3]=2;r[4]=4;r[6]=4;break;case '=':r[2]=31;r[4]=31;break;case '#':r[1]=10;r[2]=31;r[3]=10;r[4]=31;r[5]=10;break;case '!':r[0]=4;r[1]=4;r[2]=4;r[3]=4;r[5]=4;break;case ' ':break;default:r[0]=31;r[2]=21;r[4]=21;r[6]=31;break;}}
static void fill(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c){framebuffer_fill_rect(x,y,w,h,c);}
static void txt(char c,uint32_t x,uint32_t y,uint32_t fg,uint32_t bg){uint8_t r[7];glyph(c,r);fill(x,y,CW,CH,bg);for(uint32_t gy=0;gy<7;gy++)for(uint32_t gx=0;gx<5;gx++)if(r[gy]&(1u<<(4u-gx)))fill(x+gx*BS,y+gy*BS+2u,BS,BS,fg);}
static void line(const char*s,uint32_t x,uint32_t y,uint32_t fg,uint32_t bg,uint32_t max){uint32_t n=0;while(*s&&n<max){txt(*s++,x,y,fg,bg);x+=CW;n++;}}
static uint32_t width(const char*s){uint32_t n=0;while(s[n])n++;return n*CW;}
static void set_status(const char*s){uint32_t i=0;while(s[i]&&i<95u){status[i]=s[i];i++;}status[i]=0;}
static int digit(char c){return c>='0'&&c<='9';}
static int parse_ipv4(const char*s,uint32_t*out){uint32_t a[4]={0,0,0,0};uint32_t n=0,v=0;int have=0;for(uint32_t i=0;;i++){char c=s[i];if(digit(c)){v=v*10u+(uint32_t)(c-'0');if(v>255u)return -1;have=1;continue;}if(c=='.'&&have&&n<3u){a[n++]=v;v=0;have=0;continue;}if(c==0&&have&&n==3u){a[3]=v;*out=(a[0]<<24)|(a[1]<<16)|(a[2]<<8)|a[3];return 0;}return -1;}}
static const char*host_start(void){const char*p=url;if(p[0]=='h'&&p[1]=='t'&&p[2]=='t'&&p[3]=='p'&&p[4]==':'&&p[5]=='/'&&p[6]=='/')p+=7;return p;}
static void load_page(void){
    if(loading)return;
    loading=1u;
    set_status("Loading HTTP page...");
    const char*p=host_start();char host[32];uint32_t i=0;while(p[i]&&p[i]!='/'&&i<31u){host[i]=p[i];i++;}host[i]=0;
    uint32_t ip=0;if(parse_ipv4(host,&ip)!=0){set_status("Only numeric IPv4 HTTP URLs are supported");loading=0u;return;}
    const char*path=p+i;if(!*path)path="/";
    int32_t got=net_http_get(ip,path,page,PAGE_MAX);
    if(got<0){page_len=0;page[0]=0;set_status("HTTP request failed");loading=0u;return;}
    page_len=(uint32_t)got;page[page_len]=0;set_status("HTTP/1.x page loaded");loading=0u;
}
void browser_start(void){active=1u;loading=0;url_focus=1;url_len=0;url[0]=0;page_len=0;page[0]=0;set_status("Enter an HTTP URL, e.g. http://10.0.2.2/");}
void browser_close(void){active=0;url_focus=0;}
int browser_is_active(void){return active!=0;}
void browser_key(int key){if(!active)return;if(key==27){browser_close();return;}if(!url_focus)return;if(key=='\n'||key==13){load_page();return;}if(key=='\b'||key==127){if(url_len){url[--url_len]=0;}return;}if(key>=32&&key<127&&url_len<URL_MAX){url[url_len++]=(char)key;url[url_len]=0;}}
void browser_mouse_click(uint32_t x,uint32_t y){if(!active)return;uint32_t w=framebuffer_width();if(y>=20u&&y<64u){url_focus=1;return;}if(y>=70u&&y<112u&&x>w-130u){load_page();return;}}
void browser_step(void){while(keyboard_available())browser_key(keyboard_getchar());}
static void draw_page(uint32_t x,uint32_t y,uint32_t w,uint32_t h){fill(x,y,w,h,0xF4F0E8u);uint32_t cx=x+18u,cy=y+18u;uint32_t maxcols=w>40u?(w-36u)/CW:1u;uint32_t col=0,row=0;int in_tag=0;char word[128];uint32_t wl=0;for(uint32_t i=0;i<page_len&&row<(h/CH);i++){char c=page[i];if(c=='<'){in_tag=1;continue;}if(in_tag){if(c=='>')in_tag=0;continue;}if(c=='\r')continue;if(c=='\n'){if(wl){word[wl]=0;line(word,cx,cy+row*CH,0x211A10u,0xF4F0E8u,maxcols);wl=0;}row++;col=0;continue;}if(c==' '||c=='\t'){if(wl){word[wl]=0;uint32_t ww=width(word);uint32_t need=ww/CW;if(col+need>maxcols){row++;col=0;}if(row<(h/CH)){line(word,cx+col*CW,cy+row*CH,0x211A10u,0xF4F0E8u,maxcols-col);col+=need+1u;}wl=0;}continue;}if(wl<127u)word[wl++]=c;if(col+wl>=maxcols){word[wl]=0;if(row<(h/CH))line(word,cx+col*CW,cy+row*CH,0x211A10u,0xF4F0E8u,maxcols-col);col=0;wl=0;row++;}}if(wl&&row<(h/CH)){word[wl]=0;line(word,cx+col*CW,cy+row*CH,0x211A10u,0xF4F0E8u,maxcols-col);}}
void browser_render(void){if(!active)return;uint32_t w=framebuffer_width(),h=framebuffer_height();fill(0,0,w,h,0x120D08u);fill(0,0,w,76u,0x241A0Fu);line("LionOS Browser",20,8,0xE0A530u,0x241A0Fu,32);fill(20,28,w>180u?w-180u:100u,34u,0x0E0A06u);line(url,30,36,0xECE2CCu,0x0E0A06u,URL_MAX);fill(w>140u?w-120u:20u,28,100u,34u,0xE0A530u);line("GO",w>140u?w-91u:49u,36,0x211A10u,0xE0A530u,5);line(status,20,78,0xA89980u,0x120D08u,70);draw_page(18,106,w>36u?w-36u:1u,h>124u?h-124u:1u);line("ESC: close browser   HTTP only   IPv4 address required",20,h>24u?h-24u:0u,0xA89980u,0x120D08u,70);}
