#include <stdint.h>
#include "gui.h"
#include "debug.h"
#include "framebuffer.h"
#include "keyboard.h"
#include "mouse.h"
#include "memory.h"
#include "vfs.h"
#include "lapic.h"

#define FONT_W 5u
#define FONT_H 7u
#define FONT_SCALE 2u
#define CHAR_W 12u
#define CHAR_H 18u
#define TASKBAR_H 54u
#define TITLE_H 30u
#define WIN_TERMINAL 1u
#define WIN_FILES 2u
#define WIN_ABOUT 3u
#define WIN_SETTINGS 4u
#define WIN_MAX 4u

/* Warm, restrained palette matching the supplied LionOS visual reference. */
#define COL_SKY_TOP 0x241708u
#define COL_SKY_MID 0x6B3F18u
#define COL_SUN 0xF2B736u
#define COL_GROUND 0x0E0A06u
#define COL_PANEL 0x1C140Bu
#define COL_PANEL2 0x241A0Fu
#define COL_TEXT 0xECE2CCu
#define COL_DIM 0xA89980u
#define COL_GOLD 0xE0A530u
#define COL_GOLD_DIM 0x8A6A26u
#define COL_DANGER 0xC1502Eu
#define COL_OK 0x8FAE5Cu
#define COL_INPUT 0x0E0A06u

struct ui_window {
    uint8_t id;
    uint8_t visible;
    uint8_t minimized;
    uint8_t focused;
    uint8_t maximized;
    uint32_t x, y, w, h;
    uint32_t old_x, old_y, old_w, old_h;
};

static struct ui_window windows[WIN_MAX];
static uint8_t gui_active;
static uint8_t start_open;
static uint8_t terminal_focus;
static uint8_t drag_active;
static uint8_t drag_id;
static int drag_dx, drag_dy;
static uint32_t mouse_px_x, mouse_px_y, previous_buttons;

static char term_input[121];
static uint32_t term_len;
static char term_lines[22][121];
static uint32_t term_line_count;

static const uint8_t letters[26][7] = {
{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
{0x0F,0x10,0x10,0x10,0x10,0x10,0x0F},{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
{0x0F,0x10,0x10,0x17,0x11,0x11,0x0F},{0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
{0x1F,0x04,0x04,0x04,0x04,0x04,0x1F},{0x1F,0x02,0x02,0x02,0x12,0x12,0x0C},
{0x11,0x12,0x14,0x18,0x14,0x12,0x11},{0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
{0x11,0x1B,0x15,0x15,0x11,0x11,0x11},{0x11,0x19,0x15,0x13,0x11,0x11,0x11},
{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},{0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
{0x11,0x11,0x11,0x11,0x11,0x11,0x0E},{0x11,0x11,0x11,0x11,0x0A,0x0A,0x04},
{0x11,0x11,0x11,0x15,0x15,0x1B,0x11},{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
{0x11,0x11,0x0A,0x04,0x04,0x04,0x04},{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}
};

static void glyph(char c, uint8_t rows[FONT_H]) {
    for (uint32_t i = 0; i < FONT_H; ++i) rows[i] = 0u;
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') {
        for (uint32_t i = 0; i < FONT_H; ++i) rows[i] = letters[(uint32_t)(c - 'A')][i];
        return;
    }
    switch (c) {
        case '0': rows[0]=0x0E;rows[1]=0x11;rows[2]=0x13;rows[3]=0x15;rows[4]=0x19;rows[5]=0x11;rows[6]=0x0E; break;
        case '1': rows[0]=0x04;rows[1]=0x0C;rows[2]=0x04;rows[3]=0x04;rows[4]=0x04;rows[5]=0x04;rows[6]=0x0E; break;
        case '2': rows[0]=0x0E;rows[1]=0x11;rows[2]=0x01;rows[3]=0x02;rows[4]=0x04;rows[5]=0x08;rows[6]=0x1F; break;
        case '3': rows[0]=0x1E;rows[1]=0x01;rows[2]=0x01;rows[3]=0x0E;rows[4]=0x01;rows[5]=0x01;rows[6]=0x1E; break;
        case '4': rows[0]=0x02;rows[1]=0x06;rows[2]=0x0A;rows[3]=0x12;rows[4]=0x1F;rows[5]=0x02;rows[6]=0x02; break;
        case '5': rows[0]=0x1F;rows[1]=0x10;rows[2]=0x10;rows[3]=0x1E;rows[4]=0x01;rows[5]=0x01;rows[6]=0x1E; break;
        case '6': rows[0]=0x0E;rows[1]=0x10;rows[2]=0x10;rows[3]=0x1E;rows[4]=0x11;rows[5]=0x11;rows[6]=0x0E; break;
        case '7': rows[0]=0x1F;rows[1]=0x01;rows[2]=0x02;rows[3]=0x04;rows[4]=0x08;rows[5]=0x08;rows[6]=0x08; break;
        case '8': rows[0]=0x0E;rows[1]=0x11;rows[2]=0x11;rows[3]=0x0E;rows[4]=0x11;rows[5]=0x11;rows[6]=0x0E; break;
        case '9': rows[0]=0x0E;rows[1]=0x11;rows[2]=0x11;rows[3]=0x0F;rows[4]=0x01;rows[5]=0x01;rows[6]=0x0E; break;
        case ':': rows[2]=0x04;rows[4]=0x04; break;
        case '.': rows[6]=0x04; break;
        case ',': rows[5]=0x04;rows[6]=0x08; break;
        case '-': rows[3]=0x1F; break;
        case '_': rows[6]=0x1F; break;
        case '+': rows[2]=0x04;rows[3]=0x1F;rows[4]=0x04; break;
        case '>': rows[1]=0x10;rows[2]=0x08;rows[3]=0x04;rows[4]=0x08;rows[5]=0x10; break;
        case '<': rows[1]=0x01;rows[2]=0x02;rows[3]=0x04;rows[4]=0x02;rows[5]=0x01; break;
        case '/': rows[0]=0x01;rows[1]=0x02;rows[2]=0x04;rows[3]=0x08;rows[4]=0x10; break;
        case '!': rows[0]=0x04;rows[1]=0x04;rows[2]=0x04;rows[3]=0x04;rows[5]=0x04; break;
        case '?': rows[0]=0x0E;rows[1]=0x11;rows[2]=0x01;rows[3]=0x02;rows[4]=0x04;rows[6]=0x04; break;
        case '=': rows[2]=0x1F;rows[4]=0x1F; break;
        case '|': rows[0]=0x04;rows[1]=0x04;rows[2]=0x04;rows[3]=0x04;rows[4]=0x04;rows[5]=0x04;rows[6]=0x04; break;
        case ' ': break;
        default: rows[0]=0x1F;rows[2]=0x15;rows[4]=0x15;rows[6]=0x1F; break;
    }
}

static void fill(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c){framebuffer_fill_rect(x,y,w,h,c);}
static void border(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c){if(w<2u||h<2u)return;fill(x,y,w,1u,c);fill(x,y+h-1u,w,1u,c);fill(x,y,1u,h,c);fill(x+w-1u,y,1u,h,c);}
static void text(char c,uint32_t x,uint32_t y,uint32_t fg,uint32_t bg){uint8_t rows[FONT_H];glyph(c,rows);fill(x,y,CHAR_W,CHAR_H,bg);for(uint32_t gy=0;gy<FONT_H;++gy)for(uint32_t gx=0;gx<FONT_W;++gx)if(rows[gy]&(1u<<(FONT_W-1u-gx)))fill(x+gx*FONT_SCALE,y+gy*FONT_SCALE+2u,FONT_SCALE,FONT_SCALE,fg);}
static void text_line(const char*s,uint32_t x,uint32_t y,uint32_t fg,uint32_t bg){while(*s&&x+CHAR_W<framebuffer_width()){text(*s,x,y,fg,bg);x+=CHAR_W;++s;}}
static uint32_t px(void){return mouse_x();}
static uint32_t py(void){return mouse_y();}
static struct ui_window*window_by_id(uint8_t id){return id>=1u&&id<=WIN_MAX?&windows[id-1u]:0;}
static void focus(uint8_t id){for(uint32_t i=0;i<WIN_MAX;++i)windows[i].focused=(windows[i].id==id&&windows[i].visible&&!windows[i].minimized)?1u:0u;}
static void show(uint8_t id){struct ui_window*w=window_by_id(id);if(!w)return;w->visible=1u;w->minimized=0u;focus(id);start_open=0u;if(id==WIN_TERMINAL)terminal_focus=1u;}
static void hide(uint8_t id){struct ui_window*w=window_by_id(id);if(!w)return;w->visible=0u;w->minimized=0u;w->focused=0u;drag_active=0u;if(id==WIN_TERMINAL)terminal_focus=0u;}
static void minimize(uint8_t id){struct ui_window*w=window_by_id(id);if(!w)return;w->minimized=1u;w->focused=0u;drag_active=0u;if(id==WIN_TERMINAL)terminal_focus=0u;}
static void toggle_max(uint8_t id){struct ui_window*w=window_by_id(id);if(!w)return;uint32_t dh=framebuffer_height()>TASKBAR_H?framebuffer_height()-TASKBAR_H:framebuffer_height();if(!w->maximized){w->old_x=w->x;w->old_y=w->y;w->old_w=w->w;w->old_h=w->h;w->x=0u;w->y=0u;w->w=framebuffer_width();w->h=dh;w->maximized=1u;}else{w->x=w->old_x;w->y=w->old_y;w->w=w->old_w;w->h=w->old_h;w->maximized=0u;}focus(id);}

static void term_clear(void){term_line_count=0u;term_len=0u;term_input[0]=0;}
static void term_line(const char*s){if(term_line_count<22u){uint32_t i=0;while(s[i]&&i<120u){term_lines[term_line_count][i]=s[i];++i;}term_lines[term_line_count][i]=0;term_line_count++;return;}for(uint32_t r=1u;r<22u;++r)for(uint32_t c=0;c<121u;++c)term_lines[r-1u][c]=term_lines[r][c];uint32_t i=0;while(s[i]&&i<120u){term_lines[21][i]=s[i];++i;}term_lines[21][i]=0;}
static int eq(const char*a,const char*b){while(*a&&*a==*b){++a;++b;}return *a==*b;}
static void terminal_command(void){term_input[term_len]=0;if(!term_len)return;if(eq(term_input,"help"))term_line("help  ls  pwd  mem  uname  version  about  clear  exit");else if(eq(term_input,"ls")){uint32_t n=vfs_count();if(!n)term_line("(no files)");else for(uint32_t i=0;i<n&&term_line_count<21u;++i){const char*nme=vfs_name(i);if(nme)term_line(nme);}}else if(eq(term_input,"pwd"))term_line("/");else if(eq(term_input,"mem"))term_line("memory manager online");else if(eq(term_input,"uname"))term_line("LionOS 0.8 x86 i386");else if(eq(term_input,"version"))term_line("LionOS version 0.8");else if(eq(term_input,"about"))term_line("Experimental 32-bit OS with SMP, VFS and GUI.");else if(eq(term_input,"clear"))term_clear();else if(eq(term_input,"exit")){hide(WIN_TERMINAL);return;}else term_line("Unknown command. Type help.");term_len=0u;term_input[0]=0;}
static void terminal_init(void){term_clear();term_line("LionOS Terminal");term_line("Graphical terminal ready.");term_line("Type help for commands. ESC closes this window.");}

static void window_chrome(const struct ui_window*w,const char*title){
    uint32_t active=w->focused?COL_PANEL2:COL_PANEL;
    fill(w->x,w->y,w->w,w->h,COL_PANEL);
    border(w->x,w->y,w->w,w->h,w->focused?COL_GOLD:COL_GOLD_DIM);
    fill(w->x,w->y,w->w,TITLE_H,active);
    text_line(title,w->x+12u,w->y+6u,COL_TEXT,active);
    fill(w->x+w->w-74u,w->y+7u,18u,16u,COL_PANEL2);
    fill(w->x+w->w-50u,w->y+7u,18u,16u,COL_PANEL2);
    fill(w->x+w->w-26u,w->y+7u,18u,16u,COL_PANEL2);
    border(w->x+w->w-74u,w->y+7u,18u,16u,COL_GOLD_DIM);
    border(w->x+w->w-50u,w->y+7u,18u,16u,COL_GOLD_DIM);
    border(w->x+w->w-26u,w->y+7u,18u,16u,COL_GOLD_DIM);
    fill(w->x+w->w-69u,w->y+14u,8u,2u,COL_DIM);
    border(w->x+w->w-46u,w->y+11u,8u,8u,COL_OK);
    fill(w->x+w->w-21u,w->y+14u,8u,2u,COL_DANGER);
    fill(w->x+w->w-18u,w->y+11u,2u,8u,COL_DANGER);
}

static void draw_terminal(const struct ui_window*w){
    window_chrome(w,"Terminal");
    uint32_t x=w->x+16u,y=w->y+TITLE_H+10u;
    uint32_t body_h=w->h>TITLE_H+40u?w->h-TITLE_H-38u:40u;
    fill(x,y,w->w-32u,body_h,COL_INPUT);
    uint32_t max_lines=(body_h/CHAR_H);if(max_lines>22u)max_lines=22u;
    uint32_t start=term_line_count>max_lines?term_line_count-max_lines:0u;
    uint32_t row=0u;
    for(uint32_t i=start;i<term_line_count;++i){text_line(term_lines[i],x+6u,y+6u+row*CHAR_H,row+1u==max_lines?COL_TEXT:COL_DIM,COL_INPUT);++row;}
    uint32_t py0=y+body_h-CHAR_H-6u;
    fill(x+4u,py0,w->w-40u,CHAR_H+4u,COL_PANEL2);
    text_line("pride@lionos",x+10u,py0+4u,COL_GOLD,COL_PANEL2);
    text_line(":/ $",x+10u+11u*CHAR_W,py0+4u,COL_DIM,COL_PANEL2);
    text_line(term_input,x+10u+16u*CHAR_W,py0+4u,COL_TEXT,COL_PANEL2);
}

static void draw_files(const struct ui_window*w){
    window_chrome(w,"Files");
    uint32_t left=w->x+16u, top=w->y+TITLE_H+10u, side=128u;
    fill(left,top,side,w->h-TITLE_H-26u,COL_PANEL2);
    text_line("HOME",left+14u,top+14u,COL_GOLD,COL_PANEL2);
    text_line("NOTES",left+14u,top+52u,COL_DIM,COL_PANEL2);
    text_line("PROJECTS",left+14u,top+90u,COL_DIM,COL_PANEL2);
    fill(left+side+1u,top,w->w-side-34u,w->h-TITLE_H-26u,COL_PANEL);
    text_line("/home/pride",left+side+18u,top+14u,COL_DIM,COL_PANEL);
    uint32_t gx=left+side+18u, gy=top+48u;
    uint32_t n=vfs_count();
    if(n==0u){text_line("Folder is empty",gx,gy,COL_DIM,COL_PANEL);return;}
    for(uint32_t i=0;i<n && i<12u;++i){const char*nme=vfs_name(i);if(!nme)continue;uint32_t col=i%4u,row=i/4u;uint32_t bx=gx+col*100u,by=gy+row*70u;fill(bx,by,46u,40u,COL_PANEL2);border(bx,by,46u,40u,COL_GOLD_DIM);text_line("FILE",bx+7u,by+12u,COL_GOLD,COL_PANEL2);text_line(nme,bx,by+46u,COL_TEXT,COL_PANEL);}
}

static void draw_about(const struct ui_window*w){
    window_chrome(w,"About LionOS");
    text_line("LionOS",w->x+24u,w->y+64u,COL_GOLD,w->focused?COL_PANEL2:COL_PANEL);
    text_line("Experimental 32-bit operating system",w->x+24u,w->y+104u,COL_TEXT,COL_PANEL);
    text_line("SMP / paging / processes / VFS",w->x+24u,w->y+142u,COL_DIM,COL_PANEL);
    text_line("Framebuffer desktop / keyboard / mouse",w->x+24u,w->y+180u,COL_DIM,COL_PANEL);
    text_line("Theme: LionOS sunset",w->x+24u,w->y+218u,COL_GOLD,COL_PANEL);
    fill(w->x+24u,w->y+258u,190u,34u,COL_PANEL2);border(w->x+24u,w->y+258u,190u,34u,COL_GOLD_DIM);text_line("GUI ONLINE",w->x+40u,w->y+266u,COL_OK,COL_PANEL2);
}

static void draw_settings(const struct ui_window*w){
    window_chrome(w,"Settings");
    text_line("Appearance",w->x+24u,w->y+62u,COL_TEXT,COL_PANEL);
    text_line("Accent",w->x+24u,w->y+96u,COL_DIM,COL_PANEL);
    fill(w->x+24u,w->y+124u,34u,34u,COL_GOLD);border(w->x+24u,w->y+124u,34u,34u,COL_TEXT);
    fill(w->x+70u,w->y+124u,34u,34u,COL_DANGER);border(w->x+70u,w->y+124u,34u,34u,COL_GOLD_DIM);
    fill(w->x+116u,w->y+124u,34u,34u,COL_OK);border(w->x+116u,w->y+124u,34u,34u,COL_GOLD_DIM);
    text_line("Sunset desktop",w->x+24u,w->y+184u,COL_GOLD,COL_PANEL);
    text_line("Ultra-wide target: 19:6",w->x+24u,w->y+218u,COL_DIM,COL_PANEL);
}

static void draw_icon(uint32_t x,uint32_t y,const char*name,char symbol,uint32_t accent){
    fill(x,y,54u,44u,COL_PANEL2);border(x,y,54u,44u,COL_GOLD_DIM);text(symbol,x+21u,y+12u,accent,COL_PANEL2);
    text_line(name,x-8u,y+52u,COL_TEXT,COL_GROUND);
}

static void draw_sun(uint32_t cx,uint32_t cy,uint32_t r){for(int dy=-(int)r;dy<=(int)r;++dy){uint32_t ady=(uint32_t)(dy<0?-dy:dy);uint32_t rem=ady>r?0u:r-ady;uint32_t half=(rem*rem)/(r?r:1u);uint32_t dx=0u;while((dx+1u)*(dx+1u)<=half)++dx;fill(cx>=dx?cx-dx:0u,cy+(uint32_t)dy,dx*2u+1u,1u,COL_SUN);}}
static void draw_wallpaper(void){
    uint32_t w=framebuffer_width(),h=framebuffer_height();uint32_t sky_h=(h*63u)/100u;
    const uint32_t sky[8]={0x241708u,0x2C1A0Bu,0x39210Du,0x46280Fu,0x533012u,0x5F3715u,0x673B17u,0x6B3F18u};
    for(uint32_t i=0;i<8u;++i){uint32_t y0=(sky_h*i)/8u;uint32_t y1=(sky_h*(i+1u))/8u;fill(0u,y0,w,y1-y0,sky[i]);}
    fill(0u,sky_h,w,h-sky_h,COL_GROUND);
    draw_sun((w*78u)/100u,(h*18u)/100u,(h>200u?h/22u:12u));
    uint32_t horizon=(h*61u)/100u;fill(0u,horizon,w,(h*8u)/100u,COL_GROUND);
    for(uint32_t i=0;i<96u;++i){uint32_t x=(i*53u+17u)%w;uint32_t base=h-(i%11u)*3u;uint32_t bh=10u+(i%9u)*3u;if(base>bh)fill(x,base-bh,2u,bh,COL_GROUND);}
}

static void draw_task_button(uint32_t x,uint32_t y,uint32_t w,uint32_t c,const char*label){fill(x,y,w,34u,c);border(x,y,w,34u,COL_GOLD_DIM);text_line(label,x+12u,y+8u,COL_TEXT,c);}
static void draw_taskbar(void){
    uint32_t w=framebuffer_width(),h=framebuffer_height(),y=h-TASKBAR_H;
    fill(0u,y,w,TASKBAR_H,COL_PANEL);fill(0u,y,w,1u,COL_GOLD_DIM);
    draw_task_button(12u,y+10u,92u,COL_PANEL2,"LIONOS");
    draw_task_button(112u,y+10u,112u,terminal_focus?COL_PANEL2:COL_PANEL,"TERMINAL");
    draw_task_button(232u,y+10u,86u,COL_PANEL,"FILES");
    draw_task_button(326u,y+10u,82u,COL_PANEL,"ABOUT");
    draw_task_button(416u,y+10u,98u,COL_PANEL,"SETTINGS");
    text_line("WIFI",w>240u?w-214u:12u,y+8u,COL_DIM,COL_PANEL);
    text_line("VOL",w>186u?w-160u:66u,y+8u,COL_DIM,COL_PANEL);
    text_line("19:6",w>118u?w-92u:112u,y+8u,COL_TEXT,COL_PANEL);
}

static void draw_start_menu(void){
    if(!start_open) return;
    uint32_t w=framebuffer_width(),h=framebuffer_height();
    uint32_t mw=w>520u?420u:300u;
    uint32_t mh=h>520u?390u:h>360u?300u:240u;
    uint32_t x=12u,y=h-TASKBAR_H-mh-8u;
    fill(x,y,mw,mh,COL_PANEL);border(x,y,mw,mh,COL_GOLD_DIM);fill(x,y,mw,54u,COL_PANEL2);
    text_line("LIONOS",x+18u,y+18u,COL_GOLD,COL_PANEL2);text_line("Pinned applications",x+18u,y+72u,COL_DIM,COL_PANEL);
    fill(x+18u,y+96u,mw-36u,38u,COL_PANEL2);border(x+18u,y+96u,mw-36u,38u,COL_GOLD_DIM);text_line("TERMINAL",x+32u,y+106u,COL_TEXT,COL_PANEL2);
    fill(x+18u,y+142u,mw-36u,38u,COL_PANEL2);border(x+18u,y+142u,mw-36u,38u,COL_GOLD_DIM);text_line("FILES",x+32u,y+152u,COL_TEXT,COL_PANEL2);
    fill(x+18u,y+188u,mw-36u,38u,COL_PANEL2);border(x+18u,y+188u,mw-36u,38u,COL_GOLD_DIM);text_line("ABOUT",x+32u,y+198u,COL_TEXT,COL_PANEL2);
    fill(x+18u,y+234u,mw-36u,38u,COL_PANEL2);border(x+18u,y+234u,mw-36u,38u,COL_GOLD_DIM);text_line("SETTINGS",x+32u,y+244u,COL_TEXT,COL_PANEL2);
    if(mh>330u){fill(x+18u,y+mh-52u,140u,34u,COL_DANGER);border(x+18u,y+mh-52u,140u,34u,COL_GOLD_DIM);text_line("POWER",x+32u,y+mh-44u,COL_TEXT,COL_DANGER);}
}

static void draw_cursor(uint32_t x,uint32_t y){
    fill(x,y,2u,20u,COL_TEXT);
    fill(x+2u,y+2u,2u,15u,COL_TEXT);
    fill(x+4u,y+4u,2u,12u,COL_TEXT);
    fill(x+6u,y+6u,2u,10u,COL_TEXT);
    fill(x+8u,y+8u,2u,8u,COL_TEXT);
    fill(x+3u,y+14u,4u,2u,COL_GROUND);
    fill(x+5u,y+16u,5u,2u,COL_GROUND);
}
static void draw_desktop_background(void){draw_wallpaper();}
static void draw_desktop_icons(void){uint32_t base_y=18u;draw_icon(18u,base_y,"Terminal",'>',COL_GOLD);draw_icon(18u,base_y+88u,"Files",'#',COL_OK);draw_icon(18u,base_y+176u,"About",'i',COL_GOLD);draw_icon(18u,base_y+264u,"Settings",'+',COL_GOLD);}

static void draw_window(const struct ui_window*w){if(!w->visible||w->minimized)return;switch(w->id){case WIN_TERMINAL:draw_terminal(w);break;case WIN_FILES:draw_files(w);break;case WIN_ABOUT:draw_about(w);break;default:draw_settings(w);break;}}
static void draw_windows(void){for(uint32_t i=0;i<WIN_MAX;++i)if(windows[i].visible&&!windows[i].minimized&&!windows[i].focused)draw_window(&windows[i]);for(uint32_t i=0;i<WIN_MAX;++i)if(windows[i].visible&&!windows[i].minimized&&windows[i].focused)draw_window(&windows[i]);}
static void render_all(void){draw_desktop_background();draw_desktop_icons();draw_windows();draw_taskbar();draw_start_menu();draw_cursor(mouse_px_x,mouse_px_y);framebuffer_present();}

static void init_windows(void){
    uint32_t sw=framebuffer_width(),sh=framebuffer_height()>TASKBAR_H?framebuffer_height()-TASKBAR_H:framebuffer_height();
    windows[0]=(struct ui_window){WIN_TERMINAL,0u,0u,0u,0u,(sw*18u)/100u,(sh*13u)/100u,(sw*62u)/100u,(sh*68u)/100u,0u,0u,0u,0u};
    windows[1]=(struct ui_window){WIN_FILES,0u,0u,0u,0u,(sw*25u)/100u,(sh*17u)/100u,(sw*50u)/100u,(sh*58u)/100u,0u,0u,0u,0u};
    windows[2]=(struct ui_window){WIN_ABOUT,0u,0u,0u,0u,(sw*34u)/100u,(sh*20u)/100u,(sw*36u)/100u,(sh*48u)/100u,0u,0u,0u,0u};
    windows[3]=(struct ui_window){WIN_SETTINGS,0u,0u,0u,0u,(sw*41u)/100u,(sh*18u)/100u,(sw*34u)/100u,(sh*52u)/100u,0u,0u,0u,0u};
    terminal_init();gui_active=1u;start_open=0u;drag_active=0u;terminal_focus=0u;
}
static void close_gui(void){gui_active=0u;debug_write("LIONOS:GUI-EXIT\\n");}

static void handle_window_click(struct ui_window*w){
    uint32_t x=mouse_px_x,y=mouse_px_y;
    focus(w->id);
    if(y<w->y+TITLE_H&&x>=w->x&&x<w->x+w->w){
        if(x>=w->x+w->w-28u){hide(w->id);return;}
        if(x>=w->x+w->w-52u){toggle_max(w->id);return;}
        if(x>=w->x+w->w-76u){minimize(w->id);return;}
        if(!w->maximized){drag_active=1u;drag_id=w->id;drag_dx=(int)x-(int)w->x;drag_dy=(int)y-(int)w->y;}
    }
}

static void handle_click(void){
    uint32_t x=mouse_px_x,y=mouse_px_y,h=framebuffer_height();
    if(y>=h-TASKBAR_H){
        if(x>=12u&&x<104u){start_open=!start_open;return;}
        if(x>=112u&&x<224u){show(WIN_TERMINAL);terminal_init();return;}
        if(x>=232u&&x<318u){show(WIN_FILES);return;}
        if(x>=326u&&x<408u){show(WIN_ABOUT);return;}
        if(x>=416u&&x<514u){show(WIN_SETTINGS);return;}
    }
    if(start_open){
        uint32_t mw=framebuffer_width()>520u?420u:300u;
        uint32_t mh=framebuffer_height()>520u?390u:framebuffer_height()>360u?300u:240u;
        uint32_t sx=12u,sy=h-TASKBAR_H-mh-8u;
        if(x>=sx+18u&&x<sx+mw-18u&&y>=sy+96u&&y<sy+134u){show(WIN_TERMINAL);terminal_init();return;}
        if(x>=sx+18u&&x<sx+mw-18u&&y>=sy+142u&&y<sy+180u){show(WIN_FILES);return;}
        if(x>=sx+18u&&x<sx+mw-18u&&y>=sy+188u&&y<sy+226u){show(WIN_ABOUT);return;}
        if(x>=sx+18u&&x<sx+mw-18u&&y>=sy+234u&&y<sy+272u){show(WIN_SETTINGS);return;}
        if(mh>330u&&x>=sx+18u&&x<sx+158u&&y>=sy+mh-52u&&y<sy+mh-18u){close_gui();return;}
        start_open=0u;
    }
    for(int i=(int)WIN_MAX-1;i>=0;--i){
        struct ui_window*w=&windows[i];
        if(w->visible&&!w->minimized&&x>=w->x&&x<w->x+w->w&&y>=w->y&&y<w->y+w->h){handle_window_click(w);return;}
    }
    if(x>=18u&&x<90u&&y>=18u&&y<62u){show(WIN_TERMINAL);terminal_init();return;}
    if(x>=18u&&x<90u&&y>=106u&&y<150u){show(WIN_FILES);return;}
    if(x>=18u&&x<90u&&y>=194u&&y<238u){show(WIN_ABOUT);return;}
    if(x>=18u&&x<90u&&y>=282u&&y<326u){show(WIN_SETTINGS);return;}
}

static void handle_move(void){
    if(!drag_active)return;
    struct ui_window*w=window_by_id(drag_id);
    if(!w||!w->visible){drag_active=0u;return;}
    int nx=(int)mouse_px_x-drag_dx,ny=(int)mouse_px_y-drag_dy;
    int max_y=(int)framebuffer_height()-(int)TASKBAR_H-(int)TITLE_H;
    if(nx<4)nx=4;
    if(ny<0)ny=0;
    if(nx+(int)w->w>(int)framebuffer_width()-4)nx=(int)framebuffer_width()-(int)w->w-4;
    if(ny>max_y)ny=max_y;
    w->x=(uint32_t)nx;w->y=(uint32_t)ny;
}

static void handle_key(int key){
    if(key==27){
        if(terminal_focus&&window_by_id(WIN_TERMINAL)&&window_by_id(WIN_TERMINAL)->visible){hide(WIN_TERMINAL);return;}
        for(uint32_t i=0;i<WIN_MAX;++i)if(windows[i].focused){hide(windows[i].id);return;}
        close_gui();return;
    }
    if(terminal_focus){
        if(key=='\n'||key==13){terminal_command();return;}
        if(key=='\b'||key==127){if(term_len){--term_len;term_input[term_len]=0;}return;}
        if(key>=32&&key<127&&term_len<120u){term_input[term_len++]=(char)key;term_input[term_len]=0;}
        return;
    }
    if(key=='t'||key=='T'){show(WIN_TERMINAL);terminal_init();}
    else if(key=='f'||key=='F')show(WIN_FILES);
    else if(key=='a'||key=='A')show(WIN_ABOUT);
    else if(key=='s'||key=='S')show(WIN_SETTINGS);
}

void gui_start(void){
    debug_write("LIONOS:GUI-ENTER\\n");
    if(!framebuffer_available()){debug_write("LIONOS:GUI-NO-FRAMEBUFFER\\n");return;}
    mouse_set_bounds(framebuffer_width(),framebuffer_height());
    if(framebuffer_begin_desktop()!=0){debug_write("LIONOS:GUI-NO-DESKTOP-BUFFER\\n");return;}
    while(keyboard_available())(void)keyboard_getchar();
    init_windows();mouse_px_x=px();mouse_px_y=py();previous_buttons=mouse_buttons();render_all();
}
void gui_step(void){
    if(!gui_active)return;
    keyboard_poll();mouse_poll();mouse_px_x=px();mouse_px_y=py();
    uint32_t buttons=mouse_buttons();
    if((buttons&1u)&&!(previous_buttons&1u))handle_click();
    if(!(buttons&1u)&&(previous_buttons&1u))drag_active=0u;
    handle_move();
    while(keyboard_available())handle_key(keyboard_getchar());
    previous_buttons=buttons;render_all();
}
int gui_is_active(void){return gui_active!=0u;}
void gui_desktop_run(void){
    gui_start();
    if(!gui_active)return;
    uint32_t last_frame=lapic_timer_ticks();
    for(;;){
        while(gui_active && lapic_timer_ticks()==last_frame)
            __asm__ volatile("sti; hlt");
        if(!gui_active)break;
        last_frame=lapic_timer_ticks();
        gui_step();
    }
}
