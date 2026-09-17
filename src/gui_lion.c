#include <stdint.h>
#include "gui.h"
#include "debug.h"
#include "framebuffer.h"
#include "keyboard.h"
#include "mouse.h"
#include "vfs.h"

#define FW 5u
#define FH 7u
#define FS 2u
#define CW 12u
#define CH 18u
#define TASKBAR_H 46u
#define TITLE_H 30u
#define WIN_MAX 3u
#define WTERM 1u
#define WABOUT 2u
#define WSET 3u

#define C_SKY0 0x241708u
#define C_SKY1 0x3F2410u
#define C_SKY2 0x6B3F18u
#define C_SUN 0xF2B736u
#define C_GROUND 0x0E0A06u
#define C_PANEL 0x1C140Bu
#define C_PANEL2 0x241A0Fu
#define C_BORDER 0x8A6A26u
#define C_GOLD 0xE0A530u
#define C_TEXT 0xECE2CCu
#define C_DIM 0xA89980u
#define C_OK 0x8FAE5Cu
#define C_RED 0xC1502Eu
#define C_INPUT 0x120C07u
#define C_WHITE 0xF6ECD8u

struct win { uint8_t id, visible, minimized, focused, maximized; uint32_t x,y,w,h,px,py,pw,ph; };
static struct win ws[WIN_MAX];
static uint8_t active, menu_open, term_focus, dragging, drag_id;
static int drag_dx, drag_dy;
static uint32_t mx,my,prev_buttons;
static char input[121];
static uint32_t input_len,line_count;
static char lines[24][121];

static const uint8_t letters[26][7]={
{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},{0x0F,0x10,0x10,0x10,0x10,0x10,0x0F},{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},{0x0F,0x10,0x10,0x17,0x11,0x11,0x0F},{0x11,0x11,0x11,0x1F,0x11,0x11,0x11},{0x1F,0x04,0x04,0x04,0x04,0x04,0x1F},{0x1F,0x02,0x02,0x02,0x12,0x12,0x0C},{0x11,0x12,0x14,0x18,0x14,0x12,0x11},{0x10,0x10,0x10,0x10,0x10,0x10,0x1F},{0x11,0x1B,0x15,0x15,0x11,0x11,0x11},{0x11,0x19,0x15,0x13,0x11,0x11,0x11},{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},{0x1F,0x04,0x04,0x04,0x04,0x04,0x04},{0x11,0x11,0x11,0x11,0x11,0x11,0x0E},{0x11,0x11,0x11,0x11,0x0A,0x0A,0x04},{0x11,0x11,0x11,0x15,0x15,0x1B,0x11},{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},{0x11,0x11,0x0A,0x04,0x04,0x04,0x04},{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}};

static void rect(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c){framebuffer_fill_rect(x,y,w,h,c);}
static void box(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c){if(w<2u||h<2u)return;rect(x,y,w,1u,c);rect(x,y+h-1u,w,1u,c);rect(x,y,1u,h,c);rect(x+w-1u,y,1u,h,c);}
static uint32_t minu(uint32_t a,uint32_t b){return a<b?a:b;}
static uint32_t dh(void){uint32_t h=framebuffer_height();return h>TASKBAR_H?h-TASKBAR_H:h;}
static void glyph(char c,uint8_t r[FH]){
    for(uint32_t i=0;i<FH;i++)r[i]=0u;
    if(c>='a'&&c<='z')c=(char)(c-'a'+'A');
    if(c>='A'&&c<='Z'){for(uint32_t i=0;i<FH;i++)r[i]=letters[(uint32_t)(c-'A')][i];return;}
    switch(c){case '0':r[0]=0x0E;r[1]=0x11;r[2]=0x13;r[3]=0x15;r[4]=0x19;r[5]=0x11;r[6]=0x0E;break;case '1':r[0]=4;r[1]=12;r[2]=4;r[3]=4;r[4]=4;r[5]=4;r[6]=14;break;case '2':r[0]=14;r[1]=17;r[2]=1;r[3]=2;r[4]=4;r[5]=8;r[6]=31;break;case '3':r[0]=30;r[1]=1;r[2]=1;r[3]=14;r[4]=1;r[5]=1;r[6]=30;break;case '4':r[0]=2;r[1]=6;r[2]=10;r[3]=18;r[4]=31;r[5]=2;r[6]=2;break;case '5':r[0]=31;r[1]=16;r[2]=16;r[3]=30;r[4]=1;r[5]=1;r[6]=30;break;case '6':r[0]=14;r[1]=16;r[2]=16;r[3]=30;r[4]=17;r[5]=17;r[6]=14;break;case '7':r[0]=31;r[1]=1;r[2]=2;r[3]=4;r[4]=8;r[5]=8;r[6]=8;break;case '8':r[0]=14;r[1]=17;r[2]=17;r[3]=14;r[4]=17;r[5]=17;r[6]=14;break;case '9':r[0]=14;r[1]=17;r[2]=17;r[3]=15;r[4]=1;r[5]=1;r[6]=14;break;case ':':r[2]=4;r[4]=4;break;case '.':r[6]=4;break;case '-':r[3]=31;break;case '_':r[6]=31;break;case '>':r[1]=16;r[2]=8;r[3]=4;r[4]=8;r[5]=16;break;case '<':r[1]=1;r[2]=2;r[3]=4;r[4]=2;r[5]=1;break;case '/':r[0]=1;r[1]=2;r[2]=4;r[3]=8;r[4]=16;break;case '!':r[0]=4;r[1]=4;r[2]=4;r[3]=4;r[5]=4;break;case '?':r[0]=14;r[1]=17;r[2]=1;r[3]=2;r[4]=4;r[6]=4;break;case '=':r[2]=31;r[4]=31;break;case ' ':break;default:r[0]=31;r[2]=21;r[4]=21;r[6]=31;break;}
}
static void putc(char c,uint32_t x,uint32_t y,uint32_t fg,uint32_t bg){uint8_t r[FH];glyph(c,r);rect(x,y,CW,CH,bg);for(uint32_t gy=0;gy<FH;gy++)for(uint32_t gx=0;gx<FW;gx++)if(r[gy]&(1u<<(FW-1u-gx)))rect(x+gx*FS,y+gy*FS+2u,FS,FS,fg);}
static void str(const char*s,uint32_t x,uint32_t y,uint32_t fg,uint32_t bg){while(*s&&x+CW<framebuffer_width()){putc(*s,x,y,fg,bg);x+=CW;s++;}}
static uint32_t mouse_px_x(void){uint32_t w=framebuffer_width();return w?(mouse_x()*(w-1u))/79u:0u;}
static uint32_t mouse_px_y(void){uint32_t h=framebuffer_height();return h?(mouse_y()*(h-1u))/24u:0u;}
static struct win*getwin(uint8_t id){return id>=1u&&id<=WIN_MAX?&ws[id-1u]:0;}
static void focus(uint8_t id){for(uint32_t i=0;i<WIN_MAX;i++)ws[i].focused=(ws[i].id==id&&ws[i].visible&&!ws[i].minimized)?1u:0u;}
static void showwin(uint8_t id){struct win*w=getwin(id);if(!w)return;w->visible=1u;w->minimized=0u;focus(id);menu_open=0u;}
static void hidewin(uint8_t id){struct win*w=getwin(id);if(!w)return;w->visible=0u;w->minimized=0u;w->focused=0u;w->maximized=0u;dragging=0u;if(id==WTERM)term_focus=0u;}
static void minwin(uint8_t id){struct win*w=getwin(id);if(!w)return;w->minimized=1u;w->focused=0u;dragging=0u;if(id==WTERM)term_focus=0u;}
static void maxwin(uint8_t id){struct win*w=getwin(id);if(!w||w->minimized)return;if(!w->maximized){w->px=w->x;w->py=w->y;w->pw=w->w;w->ph=w->h;w->x=8u;w->y=8u;w->w=framebuffer_width()>16u?framebuffer_width()-16u:framebuffer_width();w->h=dh()>16u?dh()-16u:dh();w->maximized=1u;}else{w->x=w->px;w->y=w->py;w->w=w->pw;w->h=w->ph;w->maximized=0u;}}

static void tclear(void){line_count=0u;input_len=0u;input[0]=0;}
static void tline(const char*s){uint32_t i;if(line_count<24u){for(i=0;i<120u&&s[i];i++)lines[line_count][i]=s[i];lines[line_count][i]=0;line_count++;return;}for(uint32_t r=1;r<24u;r++)for(uint32_t c=0;c<121u;c++)lines[r-1u][c]=lines[r][c];for(i=0;i<120u&&s[i];i++)lines[23][i]=s[i];lines[23][i]=0;}
static int same(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
static void tcommand(void){input[input_len]=0;if(!input_len)return;if(same(input,"help"))tline("help  ls  pwd  mem  uname  version  about  clear  exit");else if(same(input,"ls")){uint32_t n=vfs_count();if(!n)tline("(no files)");else for(uint32_t i=0;i<n&&line_count<23u;i++){const char*n=vfs_name(i);if(n)tline(n);}}else if(same(input,"pwd"))tline("/");else if(same(input,"mem"))tline("memory manager online");else if(same(input,"uname"))tline("LionOS 0.8 x86 i386");else if(same(input,"version"))tline("LionOS version 0.8");else if(same(input,"about"))tline("Experimental 32-bit OS with SMP, VFS and GUI.");else if(same(input,"clear"))tclear();else if(same(input,"exit")){hidewin(WTERM);return;}else tline("Unknown command. Type help.");input_len=0u;input[0]=0;}
static void tinit(void){tclear();tline("LionOS Terminal");tline("Graphical terminal ready.");tline("Type help for commands. ESC closes this window.");}

static void titlebar(const struct win*w,const char*title){uint32_t bg=w->focused?C_PANEL2:C_PANEL;rect(w->x,w->y,w->w,w->h,C_PANEL);box(w->x,w->y,w->w,w->h,w->focused?C_GOLD:C_BORDER);rect(w->x,w->y,w->w,TITLE_H,bg);str(title,w->x+12u,w->y+6u,C_TEXT,bg);uint32_t cx=w->x+w->w-66u;rect(cx,w->y+6u,18u,18u,C_PANEL);box(cx,w->y+6u,18u,18u,C_BORDER);putc('-',cx+3u,w->y+6u,C_DIM,C_PANEL);rect(cx+22u,w->y+6u,18u,18u,C_PANEL);box(cx+22u,w->y+6u,18u,18u,C_BORDER);putc('=',cx+25u,w->y+6u,C_GOLD,C_PANEL);rect(cx+44u,w->y+6u,18u,18u,C_PANEL);box(cx+44u,w->y+6u,18u,18u,C_RED);putc('x',cx+47u,w->y+6u,C_RED,C_PANEL);}
static void draw_terminal(const struct win*w){titlebar(w,"Terminal");uint32_t x=w->x+12u,y=w->y+TITLE_H+12u,body_h=w->h-TITLE_H-22u;rect(x,y,w->w-24u,body_h,C_GROUND);for(uint32_t i=0;i<line_count;i++)str(lines[i],x+8u,y+8u+i*20u,i+1u==line_count?C_TEXT:C_DIM,C_GROUND);uint32_t iy=y+body_h-34u;rect(x+6u,iy,w->w-36u,28u,C_INPUT);str("lion@os:$",x+14u,iy+5u,C_GOLD,C_INPUT);str(input,x+14u+9u*CW,iy+5u,C_TEXT,C_INPUT);}
static void draw_about(const struct win*w){titlebar(w,"About LionOS");uint32_t x=w->x+24u,y=w->y+58u;rect(x,y,72u,72u,C_PANEL2);box(x,y,72u,72u,C_BORDER);putc('L',x+28u,y+14u,C_GOLD,C_PANEL2);putc('X',x+28u,y+34u,C_GOLD,C_PANEL2);str("LionOS",x+88u,y+4u,C_GOLD,C_PANEL);str("Experimental 32-bit x86 operating system",x+88u,y+30u,C_TEXT,C_PANEL);str("SMP / paging / processes / VFS / GUI",x+88u,y+56u,C_DIM,C_PANEL);str("Warm amber desktop theme",x,y+104u,C_DIM,C_PANEL);str("ESC closes the focused window",x,y+132u,C_OK,C_PANEL);}
static void draw_settings(const struct win*w){titlebar(w,"Settings");uint32_t x=w->x+24u,y=w->y+56u;str("Appearance",x,y,C_DIM,C_PANEL);str("Accent",x,y+30u,C_TEXT,C_PANEL);rect(x,y+58u,40u,28u,C_GOLD);rect(x+50u,y+58u,40u,28u,C_RED);rect(x+100u,y+58u,40u,28u,C_OK);rect(x+150u,y+58u,40u,28u,0xB77B38u);box(x,y+58u,40u,28u,C_TEXT);str("Warm amber LionOS desktop",x,y+112u,C_TEXT,C_PANEL);str("Framebuffer adapts to the current resolution",x,y+140u,C_DIM,C_PANEL);}
static void sun(uint32_t x,uint32_t y){rect(x-18u,y-6u,36u,12u,C_SUN);rect(x-12u,y-12u,24u,24u,C_SUN);rect(x-24u,y,48u,8u,C_SUN);}
static void wallpaper(void){uint32_t w=framebuffer_width(),h=dh(),a=h/5u,b=(h*42u)/100u,c=(h*62u)/100u;rect(0u,0u,w,a,C_SKY0);rect(0u,a,w,b-a,C_SKY1);rect(0u,b,w,c-b,C_SKY2);rect(0u,c,w,h-c,C_GROUND);sun((w*78u)/100u,(h*23u)/100u);uint32_t r=c>32u?c-32u:0u;rect(0u,r,w,32u,C_GROUND);rect(0u,r+26u,w,20u,C_GROUND);for(uint32_t x=12u;x<w;x+=72u)rect(x,h>42u?h-40u+(x/72u%3u)*2u:0u,2u,12u,C_SKY1);}
static void icon(uint32_t x,uint32_t y,const char*label,char mark){rect(x,y,72u,46u,C_PANEL2);box(x,y,72u,46u,C_BORDER);putc(mark,x+28u,y+6u,C_GOLD,C_PANEL2);str(label,x,y+52u,C_TEXT,C_GROUND);}
static void taskbar(void){uint32_t w=framebuffer_width(),h=framebuffer_height(),y=h-TASKBAR_H;rect(0u,y,w,TASKBAR_H,C_PANEL);rect(0u,y,w,1u,C_BORDER);rect(12u,y+7u,126u,32u,C_PANEL2);box(12u,y+7u,126u,32u,C_BORDER);putc('L',24u,y+13u,C_GOLD,C_PANEL2);str("LIONOS",48u,y+11u,C_GOLD,C_PANEL2);rect(146u,y+7u,132u,32u,term_focus?C_PANEL2:C_PANEL);box(146u,y+7u,132u,32u,C_BORDER);str("TERMINAL",158u,y+11u,C_TEXT,term_focus?C_PANEL2:C_PANEL);rect(284u,y+7u,102u,32u,C_PANEL);box(284u,y+7u,102u,32u,C_BORDER);str("ABOUT",296u,y+11u,C_TEXT,C_PANEL);rect(392u,y+7u,118u,32u,C_PANEL);box(392u,y+7u,118u,32u,C_BORDER);str("SETTINGS",404u,y+11u,C_TEXT,C_PANEL);if(w>620u){str("SMP",w-184u,y+11u,C_OK,C_PANEL);str("GUI",w-134u,y+11u,C_GOLD,C_PANEL);str("19:6",w-82u,y+11u,C_DIM,C_PANEL);}}
static void menu(void){if(!menu_open)return;uint32_t w=framebuffer_width(),h=dh(),mw=minu(320u,w>24u?w-24u:320u),mh=minu(360u,h>16u?h-16u:360u),y=h-mh-8u;rect(12u,y,mw,mh,C_PANEL);box(12u,y,mw,mh,C_BORDER);rect(12u,y,mw,54u,C_PANEL2);str("LIONOS",28u,y+17u,C_GOLD,C_PANEL2);str("Pinned",28u,y+74u,C_DIM,C_PANEL);rect(28u,y+100u,mw-56u,42u,C_PANEL2);box(28u,y+100u,mw-56u,42u,C_BORDER);str("TERMINAL",44u,y+112u,C_TEXT,C_PANEL2);rect(28u,y+150u,mw-56u,42u,C_PANEL2);box(28u,y+150u,mw-56u,42u,C_BORDER);str("ABOUT",44u,y+162u,C_TEXT,C_PANEL2);rect(28u,y+200u,mw-56u,42u,C_PANEL2);box(28u,y+200u,mw-56u,42u,C_BORDER);str("SETTINGS",44u,y+212u,C_TEXT,C_PANEL2);if(mh>310u){rect(28u,y+mh-52u,122u,34u,C_RED);box(28u,y+mh-52u,122u,34u,C_BORDER);str("POWER",42u,y+mh-42u,C_TEXT,C_RED);}}
static void desktop(void){uint32_t w=framebuffer_width(),h=dh();wallpaper();icon(20u,18u,"This PC",'P');icon(20u,94u,"Terminal",'>');icon(20u,170u,"About",'i');icon(20u,246u,"Settings",'=');if(w>760u&&h>340u){uint32_t pw=minu(720u,w-250u),ph=minu(180u,h-150u),x=(w-pw)/2u,y=(h-ph)/2u;rect(x,y,pw,ph,C_PANEL);box(x,y,pw,ph,C_BORDER);str("Welcome to LionOS",x+28u,y+26u,C_TEXT,C_PANEL);str("A warm, minimal framebuffer desktop.",x+28u,y+56u,C_DIM,C_PANEL);str("Keyboard + mouse + terminal + windows",x+28u,y+82u,C_DIM,C_PANEL);rect(x+28u,y+118u,164u,32u,C_PANEL2);str("SYSTEM ONLINE",x+40u,y+126u,C_OK,C_PANEL2);rect(x+206u,y+118u,142u,32u,C_PANEL2);str("SMP ONLINE",x+218u,y+126u,C_OK,C_PANEL2);rect(x+362u,y+118u,132u,32u,C_PANEL2);str("GUI READY",x+374u,y+126u,C_GOLD,C_PANEL2);}taskbar();menu();}
static void cursor(uint32_t x,uint32_t y){rect(x,y,3u,20u,C_WHITE);rect(x,y,11u,3u,C_WHITE);rect(x+5u,y+15u,7u,3u,C_WHITE);rect(x+7u,y+11u,4u,4u,C_WHITE);}
static void windows_draw(void){for(uint32_t i=0;i<WIN_MAX;i++)if(ws[i].visible&&!ws[i].minimized){if(ws[i].id==WTERM)draw_terminal(&ws[i]);else if(ws[i].id==WABOUT)draw_about(&ws[i]);else draw_settings(&ws[i]);}cursor(mx,my);}
static void render(void){desktop();windows_draw();}

static void initwins(void){uint32_t w=framebuffer_width(),h=dh(),tw=w>620u?(w*72u)/100u:500u,th=h>340u?(h*70u)/100u:300u,aw=w>700u?(w*42u)/100u:340u,ah=h>300u?(h*52u)/100u:260u,sw=w>680u?(w*36u)/100u:320u,sh=h>300u?(h*44u)/100u:240u;if(tw<420u)tw=420u;if(th<220u)th=220u;if(aw<300u)aw=300u;if(ah<220u)ah=220u;if(sw<300u)sw=300u;if(sh<220u)sh=220u;if(tw>w-16u)tw=w>32u?w-16u:w;if(th>h-16u)th=h>32u?h-16u:h;ws[0]=(struct win){.id=WTERM,.x=(w>tw?(w-tw)/2u:8u),.y=24u,.w=tw,.h=th};ws[1]=(struct win){.id=WABOUT,.x=(w>aw?(w-aw)/2u:8u),.y=48u,.w=aw,.h=ah};ws[2]=(struct win){.id=WSET,.x=(w>sw?(w-sw)/2u:8u),.y=72u,.w=sw,.h=sh};tinit();active=1u;menu_open=0u;dragging=0u;term_focus=0u;}
static void closegui(void){active=0u;mouse_set_cursor_visible(1u);mouse_show();debug_write("LIONOS:GUI-EXIT\n");}
static void window_click(struct win*w){uint32_t x=mx,y=my;if(y<w->y+TITLE_H&&x>=w->x&&x<w->x+w->w){uint32_t cx=w->x+w->w-66u;if(x>=cx+44u){hidewin(w->id);return;}if(x>=cx+22u){maxwin(w->id);return;}if(x>=cx){minwin(w->id);return;}if(!w->maximized){dragging=1u;drag_id=w->id;drag_dx=(int)x-(int)w->x;drag_dy=(int)y-(int)w->y;}focus(w->id);return;}focus(w->id);}
static void click(void){uint32_t x=mx,y=my,H=framebuffer_height();if(y>=H-TASKBAR_H){if(x>=12u&&x<138u){menu_open=!menu_open;return;}if(x>=146u&&x<278u){showwin(WTERM);term_focus=1u;return;}if(x>=284u&&x<386u){showwin(WABOUT);return;}if(x>=392u&&x<510u){showwin(WSET);return;}}
if(menu_open){uint32_t mh=minu(360u,dh()>16u?dh()-16u:360u),sy=dh()-mh-8u;if(x>=28u&&x<framebuffer_width()&&y>=sy+100u&&y<sy+142u){showwin(WTERM);term_focus=1u;tinit();return;}if(x>=28u&&y>=sy+150u&&y<sy+192u){showwin(WABOUT);return;}if(x>=28u&&y>=sy+200u&&y<sy+242u){showwin(WSET);return;}if(mh>310u&&x>=28u&&x<150u&&y>=sy+mh-52u&&y<sy+mh-18u){closegui();return;}menu_open=0u;}
for(int i=(int)WIN_MAX-1;i>=0;i--){struct win*w=&ws[i];if(w->visible&&!w->minimized&&x>=w->x&&x<w->x+w->w&&y>=w->y&&y<w->y+w->h){window_click(w);return;}}
if(x>=20u&&x<104u&&y>=18u&&y<78u){showwin(WTERM);term_focus=1u;tinit();return;}if(x>=20u&&x<104u&&y>=94u&&y<154u){showwin(WTERM);term_focus=1u;tinit();return;}if(x>=20u&&x<104u&&y>=170u&&y<230u){showwin(WABOUT);return;}if(x>=20u&&x<104u&&y>=246u&&y<306u){showwin(WSET);return;}}
static void move_window(void){if(!dragging)return;struct win*w=getwin(drag_id);if(!w||!w->visible||w->maximized){dragging=0u;return;}int nx=(int)mx-drag_dx,ny=(int)my-drag_dy,maxx=(int)framebuffer_width()-(int)w->w-8,maxy=(int)dh()-(int)w->h-8;if(nx<8)nx=8;if(ny<8)ny=8;if(nx>maxx)nx=maxx;if(ny>maxy)ny=maxy;if(nx<8)nx=8;if(ny<8)ny=8;w->x=(uint32_t)nx;w->y=(uint32_t)ny;}
static void key(int k){if(k==27){if(term_focus&&getwin(WTERM)&&getwin(WTERM)->visible){hidewin(WTERM);return;}for(uint32_t i=0;i<WIN_MAX;i++)if(ws[i].focused){hidewin(ws[i].id);return;}closegui();return;}if(term_focus){if(k=='\n'||k==13){tcommand();return;}if(k=='\b'||k==127){if(input_len){input_len--;input[input_len]=0;}return;}if(k>=32&&k<127&&input_len<120u){input[input_len++]=(char)k;input[input_len]=0;}return;}if(k=='a'||k=='A')showwin(WABOUT);else if(k=='t'||k=='T'){showwin(WTERM);term_focus=1u;}}

void gui_start(void){debug_write("LIONOS:GUI-ENTER\n");if(!framebuffer_available()){debug_write("LIONOS:GUI-NO-FRAMEBUFFER\n");return;}mouse_set_cursor_visible(0u);while(keyboard_available())(void)keyboard_getchar();initwins();mx=mouse_px_x();my=mouse_px_y();prev_buttons=mouse_buttons();render();}
void gui_step(void){if(!active)return;keyboard_poll();mouse_poll();mx=mouse_px_x();my=mouse_px_y();uint32_t b=mouse_buttons();if((b&1u)&&!(prev_buttons&1u))click();if(!(b&1u)&&(prev_buttons&1u))dragging=0u;move_window();while(keyboard_available())key(keyboard_getchar());prev_buttons=b;render();}
int gui_is_active(void){return active!=0u;}
void gui_run(void){gui_start();}
