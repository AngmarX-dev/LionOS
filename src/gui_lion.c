#include <stdint.h>
#include "gui.h"
#include "debug.h"
#include "framebuffer.h"
#include "keyboard.h"
#include "mouse.h"
#include "memory.h"
#include "vfs.h"
#include "lapic.h"
#include "browser.h"
#include "lion_icons.h"
#include "lion_font.h"

#define FONT_W LION_FONT_W
#define FONT_H LION_FONT_H
#define FONT_SCALE 1u
#define CHAR_W 12u
#define CHAR_H 16u
#define TASKBAR_H 54u
#define TITLE_H 30u
#define WIN_TERMINAL 1u
#define WIN_FILES 2u
#define WIN_ABOUT 3u
#define WIN_SETTINGS 4u
#define WIN_MAX 4u

/* Warm, restrained palette matching the supplied LionOS visual reference. */
#define COL_SKY_TOP 0x07111Fu
#define COL_SKY_MID 0x12345Au
#define COL_SUN 0xF2C94Cu
#define COL_GROUND 0x050A12u
#define COL_PANEL 0x0B1422u
#define COL_PANEL2 0x13233Au
#define COL_TEXT 0xF2F5FAu
#define COL_DIM 0x91A4BCu
#define COL_GOLD 0xF2C94Cu
#define COL_GOLD_DIM 0x7D6726u
#define COL_DANGER 0xC94B4Bu
#define COL_OK 0x61C58Au
#define COL_INPUT 0x050A12u

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

struct display_mode { uint32_t width; uint32_t height; };
static const struct display_mode display_modes[] = {
    {640u,480u},{800u,600u},{1024u,768u},{1280u,720u},
    {1366u,768u},{1600u,900u},{1920u,1080u}
};
static uint32_t selected_display_mode;
static char display_status[64];
static uint32_t label_width(const char*label);

static char term_input[121];
static uint32_t term_len;
static char term_lines[22][121];
static uint32_t term_line_count;

static void glyph(char c, uint16_t rows[FONT_H]) {
    for (uint32_t i=0u;i< FONT_H;++i) rows[i]=0u;
    uint32_t code=(uint8_t)c;
    if(code<LION_FONT_FIRST||code>=LION_FONT_FIRST+LION_FONT_COUNT) code=(uint32_t)'?';
    for(uint32_t i=0u;i<FONT_H;++i) rows[i]=lion_font[code-LION_FONT_FIRST][i];
}

static void fill(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c){framebuffer_fill_rect(x,y,w,h,c);}
static void border(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c){if(w<2u||h<2u)return;fill(x,y,w,1u,c);fill(x,y+h-1u,w,1u,c);fill(x,y,1u,h,c);fill(x+w-1u,y,1u,h,c);}
static void text(char c,uint32_t x,uint32_t y,uint32_t fg,uint32_t bg){
    uint16_t rows[FONT_H];glyph(c,rows);fill(x,y,CHAR_W,CHAR_H,bg);
    for(uint32_t gy=0u;gy<FONT_H;++gy)for(uint32_t gx=0u;gx<FONT_W;++gx)
        if(rows[gy]&(1u<<(FONT_W-1u-gx)))fill(x+gx,y+gy+2u,1u,1u,fg);
}
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

static void draw_uint(uint32_t value,uint32_t x,uint32_t y,uint32_t fg,uint32_t bg){
    char b[11];uint32_t n=0u;
    if(value==0u){text_line("0",x,y,fg,bg);return;}
    while(value&&n<10u){b[n++]=(char)('0'+value%10u);value/=10u;}
    while(n){char s[2]={b[--n],0};text_line(s,x,y,fg,bg);x+=CHAR_W;}
}
static void draw_resolution(uint32_t width,uint32_t height,uint32_t x,uint32_t y,uint32_t fg,uint32_t bg){
    draw_uint(width,x,y,fg,bg);x+=label_width("1920");text_line("x",x,y,fg,bg);x+=CHAR_W;draw_uint(height,x,y,fg,bg);
}
static void set_display_status(const char*s){
    uint32_t i=0u;while(s[i]&&i<63u){display_status[i]=s[i];++i;}display_status[i]=0;
}
static void resize_windows_to_display(void){
    uint32_t sw=framebuffer_width(),sh=framebuffer_height()>TASKBAR_H?framebuffer_height()-TASKBAR_H:framebuffer_height();
    windows[0].x=(sw*18u)/100u;windows[0].y=(sh*13u)/100u;windows[0].w=(sw*62u)/100u;windows[0].h=(sh*68u)/100u;
    windows[1].x=(sw*25u)/100u;windows[1].y=(sh*17u)/100u;windows[1].w=(sw*50u)/100u;windows[1].h=(sh*58u)/100u;
    windows[2].x=(sw*34u)/100u;windows[2].y=(sh*20u)/100u;windows[2].w=(sw*36u)/100u;windows[2].h=(sh*48u)/100u;
    windows[3].x=(sw*41u)/100u;windows[3].y=(sh*18u)/100u;windows[3].w=(sw*34u)/100u;windows[3].h=(sh*52u)/100u;
    for(uint32_t i=0u;i<WIN_MAX;++i)if(windows[i].maximized){windows[i].x=0u;windows[i].y=0u;windows[i].w=sw;windows[i].h=sh;}
    mouse_set_bounds(sw,framebuffer_height());
}
static void draw_settings(const struct ui_window*w){
    window_chrome(w,"Settings");
    text_line("Display",w->x+24u,w->y+62u,COL_TEXT,COL_PANEL);
    text_line("Current resolution",w->x+24u,w->y+90u,COL_DIM,COL_PANEL);
    draw_resolution(framebuffer_width(),framebuffer_height(),w->x+190u,w->y+90u,COL_GOLD,COL_PANEL);
    text_line("Choose a display mode",w->x+24u,w->y+122u,COL_DIM,COL_PANEL);
    uint32_t row_y=w->y+148u;
    for(uint32_t i=0u;i<sizeof(display_modes)/sizeof(display_modes[0]);++i){
        uint32_t selected=(i==selected_display_mode);
        uint32_t bg=selected?COL_PANEL2:COL_PANEL;
        fill(w->x+20u,row_y+i*42u,w->w-40u,34u,bg);
        border(w->x+20u,row_y+i*42u,w->w-40u,34u,selected?COL_GOLD:COL_GOLD_DIM);
        draw_resolution(display_modes[i].width,display_modes[i].height,w->x+34u,row_y+8u+i*42u,COL_TEXT,bg);
        if(selected)text_line("ACTIVE",w->x+w->w-90u,row_y+8u+i*42u,COL_OK,bg);
    }
    text_line(display_status,w->x+24u,w->y+w->h-34u,COL_DIM,COL_PANEL);
}

static uint32_t label_width(const char*label);

static void draw_icon(uint32_t x,uint32_t y,const char*name,const uint32_t*icon){
    fill(x,y,66u,66u,COL_PANEL2);
    border(x,y,66u,66u,COL_GOLD_DIM);
    framebuffer_blit_rgba32(icon,LION_ICON_SIZE,LION_ICON_SIZE,x+9u,y+9u,48u);
    text_line(name,x+(66u>label_width(name)?(66u-label_width(name))/2u:0u),y+72u,COL_TEXT,COL_GROUND);
}

static void draw_sun(uint32_t cx,uint32_t cy,uint32_t r){for(int dy=-(int)r;dy<=(int)r;++dy){uint32_t ady=(uint32_t)(dy<0?-dy:dy);uint32_t rem=ady>r?0u:r-ady;uint32_t half=(rem*rem)/(r?r:1u);uint32_t dx=0u;while((dx+1u)*(dx+1u)<=half)++dx;fill(cx>=dx?cx-dx:0u,cy+(uint32_t)dy,dx*2u+1u,1u,COL_SUN);}}
static uint32_t label_width(const char*label);

static void draw_lion_logo(uint32_t cx,uint32_t cy,uint32_t s){
    uint32_t m=s*2u+1u;
    fill(cx-m*3u,cy-m*3u,m*2u,m,COL_GOLD);
    fill(cx+m,cy-m*3u,m*2u,m,COL_GOLD);
    fill(cx-m*4u,cy-m*2u,m*8u,m*6u,COL_GOLD);
    fill(cx-m*3u,cy-m*1u,m*6u,m*5u,COL_PANEL);
    fill(cx-m*2u,cy-m*1u,m,m,COL_GOLD);
    fill(cx+m,cy-m*1u,m,m,COL_GOLD);
    fill(cx-m*2u,cy+m,m*4u,m*2u,COL_GOLD);
    fill(cx-m,cy+m*3u,m*2u,m,COL_GOLD);
    fill(cx-m*3u,cy+m*4u,m,m,COL_GOLD);
    fill(cx+m*2u,cy+m*4u,m,m,COL_GOLD);
}
static void draw_wallpaper(void){
    uint32_t w=framebuffer_width(),h=framebuffer_height();
    uint32_t sky_h=(h*68u)/100u;
    const uint32_t sky[8]={0x07111Fu,0x09182Au,0x0B2037u,0x0E2944u,0x12345Au,0x163D68u,0x1A4671u,0x1E4D78u};
    for(uint32_t i=0;i<8u;++i){
        uint32_t y0=(sky_h*i)/8u,y1=(sky_h*(i+1u))/8u;
        fill(0u,y0,w,y1-y0,sky[i]);
    }
    fill(0u,sky_h,w,h-sky_h,COL_GROUND);
    for(uint32_t i=0;i<72u;++i){
        uint32_t x=(i*97u+31u)%w;
        uint32_t y=(i*53u+19u)%(sky_h>30u?sky_h-20u:10u);
        if((i%5u)!=0u) fill(x,y,2u,2u,COL_TEXT);
    }
    draw_sun((w*78u)/100u,(h*22u)/100u,(h>200u?h/24u:10u));
    uint32_t horizon=(h*62u)/100u;
    fill(0u,horizon,w,h-horizon,COL_GROUND);
    for(uint32_t i=0;i<18u;++i){
        uint32_t x=(i*w)/18u;
        uint32_t peak=70u+(i%6u)*18u;
        uint32_t width=(w/12u)+(i%3u)*28u;
        for(uint32_t r=0u;r<peak;r+=6u){
            uint32_t inset=(r*width)/(peak?peak:1u);
            uint32_t yy=horizon-peak+r;
            if(yy<horizon) fill(x+inset,yy,width>inset*2u?width-inset*2u:2u,6u,0x0A1727u);
        }
    }
    for(uint32_t i=0;i<12u;++i){
        uint32_t x=(i*w)/12u;
        uint32_t peak=42u+(i%4u)*14u;
        for(uint32_t r=0u;r<peak;r+=5u){
            uint32_t inset=(r*70u)/(peak?peak:1u);
            uint32_t yy=horizon-peak+r;
            if(yy<horizon) fill(x+inset,yy,70u>inset*2u?70u-inset*2u:2u,5u,0x10263Bu);
        }
    }
    uint32_t logo_s=h>700u?4u:3u;
    draw_lion_logo(w/2u,(h*31u)/100u,logo_s);
    {
        const char title[]="LionOS";
        uint32_t tw=label_width(title);
        text_line(title,w>tw?(w-tw)/2u:8u,(h*42u)/100u,COL_TEXT,COL_GROUND);
        const char sub[]="Small - Fast - Powerful";
        uint32_t sw=label_width(sub);
        text_line(sub,w>sw?(w-sw)/2u:8u,(h*46u)/100u,COL_GOLD,COL_GROUND);
    }
}
static uint32_t label_width(const char*label){uint32_t n=0u;while(label[n])++n;return n*CHAR_W;}

static void draw_task_button(uint32_t x,uint32_t y,uint32_t w,uint32_t c,const char*label,const uint32_t*icon){
    fill(x,y,w,36u,c);
    framebuffer_blit_rgba32(icon,LION_ICON_SIZE,LION_ICON_SIZE,x+7u,y+7u,22u);
    if(label&&label[0]) text_line(label,x+35u,y+9u,COL_TEXT,c);
}
static void draw_taskbar(void){
    uint32_t w=framebuffer_width(),h=framebuffer_height(),y=h-TASKBAR_H;
    fill(0u,y,w,TASKBAR_H,0x050B13u); fill(0u,y,w,2u,COL_GOLD_DIM);
    draw_task_button(16u,y+8u,164u,COL_PANEL,"LionOS",lion_icon_lionos);
    fill(194u,y+7u,2u,38u,COL_GOLD);
    draw_task_button(218u,y+8u,42u,COL_PANEL,"",lion_icon_documents);
    draw_task_button(270u,y+8u,42u,COL_PANEL,"",lion_icon_terminal);
    draw_task_button(322u,y+8u,42u,COL_PANEL,"",lion_icon_browser);
    draw_task_button(374u,y+8u,42u,COL_PANEL,"",lion_icon_tools);
    draw_task_button(426u,y+8u,42u,COL_PANEL,"",lion_icon_desktop);
    if(w>760u){
        text_line("WiFi",w-170u,y+9u,COL_TEXT,COL_PANEL);
        text_line("VOL",w-116u,y+9u,COL_TEXT,COL_PANEL);
        text_line("10:24 AM",w-86u,y+8u,COL_TEXT,COL_PANEL);
        text_line("May 25, 2025",w-128u,y+27u,COL_DIM,COL_PANEL);
    }
}

static void draw_start_menu(void){
    if(!start_open) return;
    uint32_t w=framebuffer_width(),h=framebuffer_height();
    uint32_t mw=w>520u?420u:300u;
    uint32_t mh=h>520u?390u:h>360u?300u:240u;
    uint32_t x=12u,y=h-TASKBAR_H-mh-8u;
    fill(x,y,mw,mh,COL_PANEL);border(x,y,mw,mh,COL_GOLD_DIM);fill(x,y,mw,54u,COL_PANEL2);
    text_line("LIONOS",x+18u,y+18u,COL_GOLD,COL_PANEL2);text_line("Pinned applications",x+18u,y+72u,COL_DIM,COL_PANEL);
    fill(x+18u,y+96u,mw-36u,38u,COL_PANEL2);border(x+18u,y+96u,mw-36u,38u,COL_GOLD_DIM);text_line("TERMINAL",x+18u+(mw-36u-label_width("TERMINAL"))/2u,y+106u,COL_TEXT,COL_PANEL2);
    fill(x+18u,y+142u,mw-36u,38u,COL_PANEL2);border(x+18u,y+142u,mw-36u,38u,COL_GOLD_DIM);text_line("FILES",x+18u+(mw-36u-label_width("FILES"))/2u,y+152u,COL_TEXT,COL_PANEL2);
    fill(x+18u,y+188u,mw-36u,38u,COL_PANEL2);border(x+18u,y+188u,mw-36u,38u,COL_GOLD_DIM);text_line("ABOUT",x+18u+(mw-36u-label_width("ABOUT"))/2u,y+198u,COL_TEXT,COL_PANEL2);
    fill(x+18u,y+234u,mw-36u,38u,COL_PANEL2);border(x+18u,y+234u,mw-36u,38u,COL_GOLD_DIM);text_line("SETTINGS",x+18u+(mw-36u-label_width("SETTINGS"))/2u,y+244u,COL_TEXT,COL_PANEL2);
    fill(x+18u,y+280u,mw-36u,38u,COL_PANEL2);border(x+18u,y+280u,mw-36u,38u,COL_GOLD_DIM);text_line("BROWSER",x+18u+(mw-36u-label_width("BROWSER"))/2u,y+290u,COL_TEXT,COL_PANEL2);
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
static void draw_desktop_icons(void){
    uint32_t base_y=12u;
    draw_icon(16u,base_y,"This PC",lion_icon_computer);
    draw_icon(16u,base_y+96u,"Home",lion_icon_home);
    draw_icon(16u,base_y+192u,"Terminal",lion_icon_terminal);
    draw_icon(16u,base_y+288u,"Browser",lion_icon_browser);
    draw_icon(16u,base_y+384u,"Settings",lion_icon_tools);
    draw_icon(16u,base_y+480u,"About",lion_icon_desktop);
    draw_icon(16u,base_y+576u,"Trash",lion_icon_trash);
}

static void draw_window(const struct ui_window*w){if(!w->visible||w->minimized)return;switch(w->id){case WIN_TERMINAL:draw_terminal(w);break;case WIN_FILES:draw_files(w);break;case WIN_ABOUT:draw_about(w);break;default:draw_settings(w);break;}}
static void draw_windows(void){for(uint32_t i=0;i<WIN_MAX;++i)if(windows[i].visible&&!windows[i].minimized&&!windows[i].focused)draw_window(&windows[i]);for(uint32_t i=0;i<WIN_MAX;++i)if(windows[i].visible&&!windows[i].minimized&&windows[i].focused)draw_window(&windows[i]);}
static void render_all(void){if(browser_is_active()){browser_render();framebuffer_present();return;}draw_desktop_background();draw_desktop_icons();draw_windows();draw_taskbar();draw_start_menu();draw_cursor(mouse_px_x,mouse_px_y);framebuffer_present();}

static void init_windows(void){
    uint32_t sw=framebuffer_width(),sh=framebuffer_height()>TASKBAR_H?framebuffer_height()-TASKBAR_H:framebuffer_height();
    windows[0]=(struct ui_window){WIN_TERMINAL,0u,0u,0u,0u,(sw*18u)/100u,(sh*13u)/100u,(sw*62u)/100u,(sh*68u)/100u,0u,0u,0u,0u};
    windows[1]=(struct ui_window){WIN_FILES,0u,0u,0u,0u,(sw*25u)/100u,(sh*17u)/100u,(sw*50u)/100u,(sh*58u)/100u,0u,0u,0u,0u};
    windows[2]=(struct ui_window){WIN_ABOUT,0u,0u,0u,0u,(sw*34u)/100u,(sh*20u)/100u,(sw*36u)/100u,(sh*48u)/100u,0u,0u,0u,0u};
    windows[3]=(struct ui_window){WIN_SETTINGS,0u,0u,0u,0u,(sw*41u)/100u,(sh*18u)/100u,(sw*34u)/100u,(sh*52u)/100u,0u,0u,0u,0u};
    terminal_init();gui_active=1u;start_open=0u;drag_active=0u;terminal_focus=0u;
    selected_display_mode=0u;
    for(uint32_t i=0u;i<sizeof(display_modes)/sizeof(display_modes[0]);++i)
        if(display_modes[i].width==framebuffer_width()&&display_modes[i].height==framebuffer_height()){selected_display_mode=i;break;}
    set_display_status("Select a resolution");
}
static void close_gui(void){gui_active=0u;debug_write("LIONOS:GUI-EXIT\\n");}

static void handle_window_click(struct ui_window*w){
    uint32_t x=mouse_px_x,y=mouse_px_y;
    focus(w->id);
    if(w->id==WIN_SETTINGS && y>=w->y+148u && y<w->y+148u+42u*(sizeof(display_modes)/sizeof(display_modes[0])) && x>=w->x+20u && x<w->x+w->w-20u){
        uint32_t idx=(y-(w->y+148u))/42u;
        if(idx<sizeof(display_modes)/sizeof(display_modes[0])){
            selected_display_mode=idx;
            if(framebuffer_set_mode(display_modes[idx].width,display_modes[idx].height)==0){
                resize_windows_to_display();
                set_display_status("Resolution applied");
            }else set_display_status("Mode unavailable");
        }
        return;
    }
    if(y<w->y+TITLE_H&&x>=w->x&&x<w->x+w->w){
        if(x>=w->x+w->w-28u){hide(w->id);return;}
        if(x>=w->x+w->w-52u){toggle_max(w->id);return;}
        if(x>=w->x+w->w-76u){minimize(w->id);return;}
        if(!w->maximized){drag_active=1u;drag_id=w->id;drag_dx=(int)x-(int)w->x;drag_dy=(int)y-(int)w->y;}
    }
}

static void handle_click(void){
    uint32_t x=mouse_px_x,y=mouse_px_y,h=framebuffer_height();
    if(browser_is_active()){browser_mouse_click(x,y);return;}
    if(y>=h-TASKBAR_H){
        if(x>=16u&&x<180u){start_open=!start_open;return;}
        if(x>=218u&&x<260u){show(WIN_TERMINAL);terminal_init();return;}
        if(x>=270u&&x<312u){show(WIN_FILES);return;}
        if(x>=322u&&x<364u){browser_start();return;}
        if(x>=374u&&x<416u){show(WIN_SETTINGS);return;}
        if(x>=426u&&x<468u){show(WIN_ABOUT);return;}
    }
    if(start_open){
        uint32_t mw=framebuffer_width()>520u?420u:300u;
        uint32_t mh=framebuffer_height()>520u?390u:framebuffer_height()>360u?300u:240u;
        uint32_t sx=12u,sy=h-TASKBAR_H-mh-8u;
        if(x>=sx+18u&&x<sx+mw-18u&&y>=sy+96u&&y<sy+134u){show(WIN_TERMINAL);terminal_init();return;}
        if(x>=sx+18u&&x<sx+mw-18u&&y>=sy+142u&&y<sy+180u){show(WIN_FILES);return;}
        if(x>=sx+18u&&x<sx+mw-18u&&y>=sy+188u&&y<sy+226u){show(WIN_ABOUT);return;}
        if(x>=sx+18u&&x<sx+mw-18u&&y>=sy+234u&&y<sy+272u){show(WIN_SETTINGS);return;}
        if(x>=sx+18u&&x<sx+mw-18u&&y>=sy+280u&&y<sy+318u){browser_start();return;}
        if(mh>330u&&x>=sx+18u&&x<sx+158u&&y>=sy+mh-52u&&y<sy+mh-18u){close_gui();return;}
        start_open=0u;
    }
    for(int i=(int)WIN_MAX-1;i>=0;--i){
        struct ui_window*w=&windows[i];
        if(w->visible&&!w->minimized&&x>=w->x&&x<w->x+w->w&&y>=w->y&&y<w->y+w->h){handle_window_click(w);return;}
    }
    if(x>=16u&&x<86u&&y>=12u&&y<82u){show(WIN_FILES);return;}
    if(x>=16u&&x<86u&&y>=108u&&y<178u){show(WIN_FILES);return;}
    if(x>=16u&&x<86u&&y>=204u&&y<274u){show(WIN_TERMINAL);terminal_init();return;}
    if(x>=16u&&x<86u&&y>=300u&&y<370u){browser_start();return;}
    if(x>=16u&&x<86u&&y>=396u&&y<466u){show(WIN_SETTINGS);return;}
    if(x>=16u&&x<86u&&y>=492u&&y<562u){show(WIN_ABOUT);return;}
    if(x>=16u&&x<86u&&y>=588u&&y<658u){return;}
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
    if(browser_is_active()){browser_key(key);return;}
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
    if(browser_is_active()){browser_step();previous_buttons=buttons;render_all();return;}
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
