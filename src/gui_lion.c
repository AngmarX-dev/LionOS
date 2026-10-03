#include <stdint.h>
#include "gui.h"
#include "debug.h"
#include "framebuffer.h"
#include "keyboard.h"
#include "mouse.h"
#include "memory.h"
#include "vfs.h"
#include "lapic.h"
#include "idt.h"
#include "browser.h"
#include "lion_icons.h"
#include "lion_font.h"
#include "lion_wallpaper.h"

#define FONT_W LION_FONT_W
#define FONT_H LION_FONT_H
#define FONT_SCALE 1u
#define CHAR_W 12u
#define CHAR_H 16u
#define TASKBAR_H 92u
#define TITLE_H 36u
#define WIN_TERMINAL 1u
#define WIN_FILES 2u
#define WIN_ABOUT 3u
#define WIN_SETTINGS 4u
#define WIN_NOTEPAD 5u
#define WIN_MAX 5u

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
static uint8_t scene_dirty;
static const char *last_usb_status;
static uint32_t last_render_tick = 0xFFFFFFFFu;
static uint8_t cursor_overlay;
static uint32_t glass_tick;
static uint32_t glass_blob_x[6], glass_blob_y[6];
static uint8_t glass_blob_ready;
static uint8_t dirty_valid;
static uint32_t dirty_x, dirty_y, dirty_w, dirty_h;

struct desktop_icon {
    uint32_t x, y;
    uint8_t action;
    uint8_t dragging;
    uint8_t moved;
    uint8_t pad;
    const uint32_t *bitmap;
    const char *label;
};

#define DESKTOP_ICON_SIZE 64u
#define DESKTOP_ICON_LABEL_GAP 4u
#define DESKTOP_ICON_LABEL_H 20u
#define DESKTOP_ICON_BLOCK_H (DESKTOP_ICON_SIZE + DESKTOP_ICON_LABEL_GAP + DESKTOP_ICON_LABEL_H)
#define DESKTOP_ICON_COUNT 7u
#define ICON_ACTION_FILES 1u
#define ICON_ACTION_TERMINAL 2u
#define ICON_ACTION_BROWSER 3u
#define ICON_ACTION_SETTINGS 4u
#define ICON_ACTION_ABOUT 5u
#define ICON_ACTION_NOTEPAD 6u

static struct desktop_icon desktop_icons[DESKTOP_ICON_COUNT] = {
    {24u, 24u,  ICON_ACTION_FILES,    0u, 0u, 0u, lion_icon_computer, "THIS PC"},
    {24u, 124u, ICON_ACTION_FILES,    0u, 0u, 0u, lion_icon_home,     "HOME"},
    {24u, 224u, ICON_ACTION_TERMINAL, 0u, 0u, 0u, lion_icon_terminal, "TERMINAL"},
    {24u, 324u, ICON_ACTION_BROWSER,  0u, 0u, 0u, lion_icon_browser,  "BROWSER"},
    {24u, 424u, ICON_ACTION_SETTINGS, 0u, 0u, 0u, lion_icon_tools,    "SETTINGS"},
    {24u, 524u, ICON_ACTION_ABOUT,    0u, 0u, 0u, lion_icon_desktop,  "ABOUT"},
    {24u, 624u, ICON_ACTION_NOTEPAD,    0u, 0u, 0u, lion_icon_documents, "NOTEPAD"}
};

static int desktop_icon_drag = -1;
static int desktop_icon_press_x;
static int desktop_icon_press_y;

static int desktop_icon_at(uint32_t x,uint32_t y);
static void move_desktop_icon(struct desktop_icon *icon,uint32_t x,uint32_t y);
static void activate_desktop_icon(uint8_t action);

static uint32_t label_width(const char*label);

static char term_input[121];
static uint32_t term_len;
static char term_lines[22][121];
static uint32_t term_line_count;

#define NOTEPAD_TEXT_MAX 4095u
static char notepad_text[NOTEPAD_TEXT_MAX+1u];
static uint32_t notepad_len;
static uint32_t notepad_cursor;
static uint8_t notepad_focus;

static void glyph(char c, uint16_t rows[FONT_H]) {
    for (uint32_t i=0u;i< FONT_H;++i) rows[i]=0u;
    uint32_t code=(uint8_t)c;
    if(code<LION_FONT_FIRST||code>=LION_FONT_FIRST+LION_FONT_COUNT) code=(uint32_t)'?';
    for(uint32_t i=0u;i<FONT_H;++i) rows[i]=lion_font[code-LION_FONT_FIRST][i];
}

static uint32_t label_width(const char*label);
static void fill(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c){framebuffer_fill_rect(x,y,w,h,c);}
static void border(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c){
    if(w<2u||h<2u)return;
    fill(x,y,w,1u,c);fill(x,y+h-1u,w,1u,c);fill(x,y,1u,h,c);fill(x+w-1u,y,1u,h,c);
}
static void text(char c,uint32_t x,uint32_t y,uint32_t fg,uint32_t bg){
    (void)bg;
    uint16_t rows[FONT_H];glyph(c,rows);
    for(uint32_t gy=0u;gy<FONT_H;++gy)for(uint32_t gx=0u;gx<FONT_W;++gx)
        if(rows[gy]&(1u<<(FONT_W-1u-gx)))fill(x+gx,y+gy+2u,1u,1u,fg);
}
static void text_line(const char*s,uint32_t x,uint32_t y,uint32_t fg,uint32_t bg){
    while(*s&&x+CHAR_W<framebuffer_width()){text(*s,x,y,fg,bg);x+=CHAR_W;++s;}
}
static int hit(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t px0,uint32_t py0){
    return px0>=x&&px0<x+w&&py0>=y&&py0<y+h;
}
static void shadow(uint32_t x,uint32_t y,uint32_t w,uint32_t h){
    if(!w||!h)return;
    fill(x+5u,y+5u,w,h,0x02050Au);
}
static void widget_button(uint32_t x,uint32_t y,uint32_t w,uint32_t h,
                          uint32_t base,uint32_t accent,const char*label){
    uint32_t bg=base,edge=accent;
    if(hit(x,y,w,h,mouse_px_x,mouse_px_y)){bg=COL_PANEL2;edge=COL_GOLD;}
    if((previous_buttons&1u)&&hit(x,y,w,h,mouse_px_x,mouse_px_y))bg=COL_SKY_MID;
    fill(x,y,w,h,bg);
    border(x,y,w,h,edge);
    if(label&&label[0]){
        uint32_t lw=label_width(label);
        uint32_t tx=x+(w>lw?(w-lw)/2u:6u);
        uint32_t ty=y+(h>CHAR_H?(h-CHAR_H)/2u:0u);
        text_line(label,tx,ty,COL_TEXT,bg);
    }
}
static uint32_t px(void){return mouse_x();}
static uint32_t py(void){return mouse_y();}
static void notepad_init(void);
static void notepad_save(void);

static void dirty_full(void){
    scene_dirty=1u;
    dirty_valid=1u;
    dirty_x=0u; dirty_y=0u;
    dirty_w=framebuffer_width(); dirty_h=framebuffer_height();
}
static void dirty_rect(uint32_t x,uint32_t y,uint32_t w,uint32_t h){
    if(!w||!h)return;
    uint32_t sw=framebuffer_width(),sh=framebuffer_height();
    if(x>=sw||y>=sh)return;
    if(w>sw-x)w=sw-x;
    if(h>sh-y)h=sh-y;
    if(!dirty_valid){
        dirty_x=x;dirty_y=y;dirty_w=w;dirty_h=h;dirty_valid=1u;
    }else{
        uint32_t old_r=dirty_x+dirty_w, old_b=dirty_y+dirty_h;
        uint32_t new_r=x+w, new_b=y+h;
        uint32_t left=dirty_x<x?dirty_x:x, top=dirty_y<y?dirty_y:y;
        uint32_t right=old_r>new_r?old_r:new_r, bottom=old_b>new_b?old_b:new_b;
        dirty_x=left;dirty_y=top;dirty_w=right-left;dirty_h=bottom-top;
    }
    scene_dirty=1u;
}
static struct ui_window*window_by_id(uint8_t id){return id>=1u&&id<=WIN_MAX?&windows[id-1u]:0;}
static void focus(uint8_t id){for(uint32_t i=0;i<WIN_MAX;++i)windows[i].focused=(windows[i].id==id&&windows[i].visible&&!windows[i].minimized)?1u:0u;}
static void show(uint8_t id){
    struct ui_window*w=window_by_id(id); if(!w)return;
    w->visible=1u; w->minimized=0u; focus(id); start_open=0u;
    terminal_focus=0u; notepad_focus=0u;
    if(id==WIN_TERMINAL)terminal_focus=1u;
    if(id==WIN_NOTEPAD){notepad_init();}
}
static void hide(uint8_t id){
    struct ui_window*w=window_by_id(id);if(!w)return;
    if(id==WIN_NOTEPAD)notepad_save();
    w->visible=0u;w->minimized=0u;w->focused=0u;drag_active=0u;
    if(id==WIN_TERMINAL)terminal_focus=0u;
    if(id==WIN_NOTEPAD)notepad_focus=0u;
}
static void minimize(uint8_t id){struct ui_window*w=window_by_id(id);if(!w)return;w->minimized=1u;w->focused=0u;drag_active=0u;if(id==WIN_TERMINAL)terminal_focus=0u;if(id==WIN_NOTEPAD){notepad_save();notepad_focus=0u;}}
static void toggle_max(uint8_t id){struct ui_window*w=window_by_id(id);if(!w)return;uint32_t dh=framebuffer_height()>TASKBAR_H?framebuffer_height()-TASKBAR_H:framebuffer_height();if(!w->maximized){w->old_x=w->x;w->old_y=w->y;w->old_w=w->w;w->old_h=w->h;w->x=0u;w->y=0u;w->w=framebuffer_width();w->h=dh;w->maximized=1u;}else{w->x=w->old_x;w->y=w->old_y;w->w=w->old_w;w->h=w->old_h;w->maximized=0u;}focus(id);}

static void term_clear(void){term_line_count=0u;term_len=0u;term_input[0]=0;}
static void term_line(const char*s){if(term_line_count<22u){uint32_t i=0;while(s[i]&&i<120u){term_lines[term_line_count][i]=s[i];++i;}term_lines[term_line_count][i]=0;term_line_count++;return;}for(uint32_t r=1u;r<22u;++r)for(uint32_t c=0;c<121u;++c)term_lines[r-1u][c]=term_lines[r][c];uint32_t i=0;while(s[i]&&i<120u){term_lines[21][i]=s[i];++i;}term_lines[21][i]=0;}
static int eq(const char*a,const char*b){while(*a&&*a==*b){++a;++b;}return *a==*b;}
static void terminal_command(void){term_input[term_len]=0;if(!term_len)return;if(eq(term_input,"help"))term_line("help  ls  pwd  mem  uname  version  about  clear  exit");else if(eq(term_input,"ls")){uint32_t n=vfs_count();if(!n)term_line("(no files)");else for(uint32_t i=0;i<n&&term_line_count<21u;++i){const char*nme=vfs_name(i);if(nme)term_line(nme);}}else if(eq(term_input,"pwd"))term_line("/");else if(eq(term_input,"mem"))term_line("memory manager online");else if(eq(term_input,"uname"))term_line("LionOS 0.8 x86 i386");else if(eq(term_input,"version"))term_line("LionOS version 0.8");else if(eq(term_input,"about"))term_line("Experimental 32-bit OS with SMP, VFS and GUI.");else if(eq(term_input,"clear"))term_clear();else if(eq(term_input,"exit")){hide(WIN_TERMINAL);return;}else term_line("Unknown command. Type help.");term_len=0u;term_input[0]=0;}
static void terminal_init(void){term_clear();term_line("LionOS Terminal");term_line("Graphical terminal ready.");term_line("Type help for commands. ESC closes this window.");}
static void notepad_init(void){
    notepad_len=0u;
    notepad_cursor=0u;
    notepad_text[0]=0;
    int fd=vfs_open("/notepad.txt",1u);
    if(fd>=0){
        int n=vfs_read(fd,notepad_text,NOTEPAD_TEXT_MAX);
        if(n>0) notepad_len=(uint32_t)n;
        (void)vfs_close(fd);
    }
    notepad_text[notepad_len]=0;
    notepad_cursor=notepad_len;
    notepad_focus=1u;
}
static void notepad_insert(int key){
    if(key=='\b'||key==127){
        if(notepad_cursor){
            for(uint32_t i=notepad_cursor;i<notepad_len;++i) notepad_text[i-1u]=notepad_text[i];
            --notepad_cursor; --notepad_len; notepad_text[notepad_len]=0;
        }
        return;
    }
    if(key<' '&&key!='\n'&&key!=13)return;
    if(key==13)key='\n';
    if(notepad_len>=NOTEPAD_TEXT_MAX)return;
    for(uint32_t i=notepad_len;i>notepad_cursor;--i)notepad_text[i]=notepad_text[i-1u];
    notepad_text[notepad_cursor++]=(char)key;
    ++notepad_len;
    notepad_text[notepad_len]=0;
}
static void notepad_save(void){
    int fd=vfs_open("/notepad.txt",2u);
    if(fd<0)return;
    (void)vfs_write(fd,notepad_text,notepad_len);
    (void)vfs_close(fd);
}

static void glass_panel(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t radius,uint32_t tint,uint8_t alpha,uint8_t focused){
    if(!w||!h)return;
    framebuffer_blend_round_rect(x+7u,y+9u,w,h,radius,COL_GROUND,70u);
    framebuffer_blend_round_rect(x,y,w,h,radius,tint,alpha);
    framebuffer_blend_round_rect(x+1u,y+1u,w>2u?w-2u:1u,h>2u?h-2u:1u,radius>1u?radius-1u:0u,0xBDEBFFu,focused?13u:8u);
    if(h>8u)
        framebuffer_blend_round_rect(x+3u,y+3u,w>6u?w-6u:1u,h/3u,radius>3u?radius-3u:0u,0xFFFFFFu,focused?18u:12u);
    border(x,y,w,h,focused?COL_GOLD:0x56758Du);
}

static void glass_specular(uint32_t x,uint32_t y,uint32_t w,uint32_t h){
    if(!w||!h)return;
    if(mouse_px_x<x||mouse_px_x>=x+w||mouse_px_y<y||mouse_px_y>=y+h)return;
    framebuffer_blend_circle(mouse_px_x,mouse_px_y,28u,0xFFFFFFu,18u);
    framebuffer_blend_circle(mouse_px_x,mouse_px_y,12u,0xD9F6FFu,14u);
}

static void window_chrome(const struct ui_window*w,const char*title){
    uint32_t active=w->focused?COL_PANEL2:COL_PANEL;
    glass_panel(w->x,w->y,w->w,w->h,20u,COL_PANEL,42u,w->focused);
    fill(w->x+1u,w->y+1u,w->w>2u?w->w-2u:1u,TITLE_H,active);
    framebuffer_blend_round_rect(w->x+1u,w->y+1u,w->w>2u?w->w-2u:1u,TITLE_H,18u,0xFFFFFFu,w->focused?14u:8u);
    fill(w->x,w->y+TITLE_H-1u,w->w,1u,w->focused?COL_GOLD_DIM:0x56758Du);
    text_line(title,w->x+14u,w->y+10u,COL_TEXT,active);

    uint32_t bx=w->x+w->w>84u?w->x+w->w-84u:w->x;
    uint32_t by=w->y+8u;
    widget_button(bx,by,22u,20u,COL_PANEL2,COL_GOLD_DIM,"");
    widget_button(bx+28u,by,22u,20u,COL_PANEL2,COL_GOLD_DIM,"");
    widget_button(bx+56u,by,22u,20u,COL_DANGER,COL_DANGER,"");
    fill(bx+6u,by+9u,10u,2u,COL_DIM);
    border(bx+34u,by+5u,10u,10u,COL_OK);
    fill(bx+62u,by+9u,10u,2u,COL_DANGER);
    fill(bx+66u,by+5u,2u,10u,COL_DANGER);
    glass_specular(w->x,w->y,w->w,w->h);
}

static void draw_terminal(const struct ui_window*w){
    window_chrome(w,"Terminal");
    uint32_t x=w->x+16u,y=w->y+TITLE_H+10u;
    uint32_t body_h=w->h>TITLE_H+40u?w->h-TITLE_H-38u:40u;
    framebuffer_blend_round_rect(x,y,w->w-32u,body_h,12u,COL_INPUT,175u);
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
    framebuffer_blend_round_rect(left,top,side,w->h>TITLE_H+26u?w->h-TITLE_H-26u:1u,14u,COL_PANEL2,130u);
    text_line("HOME",left+14u,top+14u,COL_GOLD,COL_PANEL2);
    text_line("NOTES",left+14u,top+52u,COL_DIM,COL_PANEL2);
    text_line("PROJECTS",left+14u,top+90u,COL_DIM,COL_PANEL2);
    framebuffer_blend_round_rect(left+side+1u,top,w->w>side+35u?w->w-side-34u:1u,w->h>TITLE_H+26u?w->h-TITLE_H-26u:1u,14u,COL_PANEL,120u);
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
static void draw_settings(const struct ui_window*w){
    window_chrome(w,"Settings");
    text_line("DISPLAY",w->x+24u,w->y+62u,COL_TEXT,COL_PANEL);
    text_line("MODE",w->x+24u,w->y+96u,COL_DIM,COL_PANEL);
    text_line("AUTOMATIC / MAXIMUM",w->x+190u,w->y+96u,COL_OK,COL_PANEL);
    text_line("RESOLUTION",w->x+24u,w->y+126u,COL_DIM,COL_PANEL);
    draw_resolution(framebuffer_width(),framebuffer_height(),w->x+190u,w->y+126u,COL_GOLD,COL_PANEL);
    text_line("SOURCE",w->x+24u,w->y+158u,COL_DIM,COL_PANEL);
    text_line("GRUB / FIRMWARE FRAMEBUFFER",w->x+190u,w->y+158u,COL_TEXT,COL_PANEL);
    text_line("LionOS does not change display modes at runtime.",w->x+24u,w->y+204u,COL_DIM,COL_PANEL);
    text_line("The bootloader selects the best available mode.",w->x+24u,w->y+230u,COL_DIM,COL_PANEL);
}
static uint32_t label_width(const char*label){uint32_t n=0u;while(label[n])++n;return n*CHAR_W;}
static void draw_notepad(const struct ui_window*w){
    window_chrome(w,"Notepad");
    uint32_t x=w->x+14u,y=w->y+TITLE_H+10u;
    uint32_t body_w=w->w>28u?w->w-28u:1u;
    uint32_t body_h=w->h>TITLE_H+28u?w->h-TITLE_H-28u:1u;
    framebuffer_blend_round_rect(x,y,body_w,body_h,14u,0x050A12u,185u);
    border(x,y,body_w,body_h,COL_GOLD_DIM);

    uint32_t line_y=y+10u;
    uint32_t line_start=0u;
    uint32_t line_no=0u;
    uint32_t visible_lines=body_h>24u?(body_h-20u)/CHAR_H:1u;
    while(line_start<=notepad_len && line_no<visible_lines){
        uint32_t p=line_start;
        while(p<notepad_len && notepad_text[p]!='\n' && p-line_start<((body_w>16u?(body_w-16u):1u)/CHAR_W)) ++p;
        uint32_t end=p;
        char line[121];
        uint32_t n=0u;
        while(line_start<end && n<120u) line[n++]=notepad_text[line_start++];
        line[n]=0;
        text_line(line,x+8u,line_y,COL_TEXT,0x050A12u);
        ++line_no;
        if(p<notepad_len && notepad_text[p]=='\n') ++p;
        line_start=p;
        line_y+=CHAR_H;
    }
    if(notepad_len==0u)
        text_line("Type here...",x+8u,y+10u,COL_DIM,0x050A12u);

    uint32_t status_y=w->y+w->h-30u;
    fill(x,status_y,body_w,20u,COL_PANEL2);
    text_line("NOTEPAD",x+8u,status_y+2u,COL_GOLD,COL_PANEL2);
    text_line("ESC CLOSE   AUTOSAVE",x+100u,status_y+2u,COL_DIM,COL_PANEL2);
}

static void draw_desktop_icon(const struct desktop_icon *icon){
    if(!icon||!icon->bitmap)return;
    uint32_t x=icon->x,y=icon->y;
    uint32_t label_y=y+DESKTOP_ICON_SIZE+DESKTOP_ICON_LABEL_GAP;
    uint32_t hovered=(mouse_px_x>=x&&mouse_px_x<x+DESKTOP_ICON_SIZE&&
                      mouse_px_y>=y&&mouse_px_y<y+DESKTOP_ICON_BLOCK_H);
    if(icon->dragging||hovered){
        fill(x-5u,y-5u,DESKTOP_ICON_SIZE+10u,DESKTOP_ICON_BLOCK_H+10u,COL_PANEL2);
        border(x-5u,y-5u,DESKTOP_ICON_SIZE+10u,DESKTOP_ICON_BLOCK_H+10u,COL_GOLD_DIM);
    }
    if(x+DESKTOP_ICON_SIZE<=framebuffer_width() &&
       y+DESKTOP_ICON_SIZE<=framebuffer_height()-TASKBAR_H){
        framebuffer_blit_rgba32(icon->bitmap,LION_ICON_SIZE,LION_ICON_SIZE,
                                x+8u,y+8u,48u);
    }
    /*
     * Keep every icon caption physically inside its 64px label cell.
     * Long names are shortened instead of painting over the wallpaper.
     */
    const char *label=icon->label;
    switch(icon->action){
        case ICON_ACTION_FILES: label=(icon->label[0]=='T')?"PC":"HOME"; break;
        case ICON_ACTION_TERMINAL: label="TERM"; break;
        case ICON_ACTION_BROWSER: label="BROW"; break;
        case ICON_ACTION_SETTINGS: label="SET"; break;
        case ICON_ACTION_ABOUT: label="ABOUT"; break;
        case ICON_ACTION_NOTEPAD: label="NOTE"; break;
        default: break;
    }
    uint32_t label_w=label_width(label);
    if(label_w>DESKTOP_ICON_SIZE){label_w=DESKTOP_ICON_SIZE;}
    uint32_t label_x=x+(DESKTOP_ICON_SIZE-label_w)/2u;
    text_line(label,label_x,label_y,COL_TEXT,COL_GROUND);
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
    framebuffer_blit_rgb565_cover(lion_wallpaper_rgb565,LION_WALLPAPER_W,LION_WALLPAPER_H);
}
static void draw_task_button(uint32_t x,uint32_t y,uint32_t w,uint32_t c,const char*label,const uint32_t*icon){
    uint32_t bg=hit(x,y,w,42u,mouse_px_x,mouse_px_y)?COL_PANEL2:c;
    uint32_t edge=hit(x,y,w,42u,mouse_px_x,mouse_px_y)?COL_GOLD:COL_GOLD_DIM;
    fill(x,y,w,42u,bg);
    border(x,y,w,42u,edge);
    if(icon)framebuffer_blit_rgba32(icon,LION_ICON_SIZE,LION_ICON_SIZE,x+(w>50u?9u:10u),y+9u,w>50u?24u:22u);
    if(label&&label[0]) text_line(label,x+40u,y+13u,COL_TEXT,bg);
}
static void draw_glass_icon(uint32_t x,uint32_t y,const uint32_t *icon,const char*label){
    framebuffer_blend_round_rect(x,y,52u,52u,16u,0x15283Au,42u);
    framebuffer_blend_round_rect(x+1u,y+1u,50u,26u,14u,0xFFFFFFu,10u);
    border(x,y,52u,52u,COL_GOLD_DIM);
    if(icon)framebuffer_blit_rgba32(icon,LION_ICON_SIZE,LION_ICON_SIZE,x+10u,y+10u,32u);
    if(label)text_line(label,x+7u,y+56u,COL_TEXT,COL_GROUND);
}
static void draw_top_menu(void){
    uint32_t w=framebuffer_width();
    framebuffer_blend_round_rect(10u,6u,w>20u?w-20u:1u,34u,17u,COL_GROUND,54u);
    framebuffer_blend_round_rect(11u,7u,w>22u?w-22u:1u,18u,12u,0xFFFFFFu,12u);
    border(10u,6u,w>20u?w-20u:1u,34u,0x56758Du);
    text_line("LionOS",22u,15u,COL_TEXT,COL_GROUND);
    text_line("File   Edit   View   Window   Help",112u,15u,COL_DIM,COL_GROUND);
    if(w>760u){
        text_line("LIQUID GLASS",w-190u,15u,COL_GOLD,COL_GROUND);
        text_line(mouse_usb_status_text(),w-92u,15u,
                  mouse_usb_status()==1u?COL_OK:COL_GOLD,COL_GROUND);
    }
}
static void draw_notification(void){
    uint32_t w=framebuffer_width();
    if(w<700u)return;
    uint32_t x=w-360u;
    glass_panel(x,50u,330u,92u,22u,0xEAF7FFu,22u,1u);
    text_line("Welcome to LionOS",x+20u,66u,COL_TEXT,COL_PANEL);
    text_line("Liquid glass desktop",x+20u,94u,COL_DIM,COL_PANEL);
    text_line("Drag windows to move.",x+20u,118u,COL_DIM,COL_PANEL);
}
static void draw_taskbar(void){
    uint32_t w=framebuffer_width(),h=framebuffer_height(),y=h-TASKBAR_H;
    framebuffer_blend_rect(0u,y,w,TASKBAR_H,COL_GROUND,22u);
    uint32_t dock_w=w>720u?620u:(w>420u?w-40u:320u);
    uint32_t dock_h=72u;
    uint32_t dx=(w-dock_w)/2u,dy=y+8u;
    glass_panel(dx,dy,dock_w,dock_h,36u,COL_PANEL,38u,1u);
    const uint32_t *icons[6]={
        lion_icon_terminal,lion_icon_documents,lion_icon_browser,
        lion_icon_tools,lion_icon_desktop,lion_icon_documents
    };
    const char *labels[6]={"TERM","FILES","BROW","SET","ABOUT","NOTE"};
    uint32_t step=dock_w>=360u?92u:((dock_w-24u)/6u);
    if(step<52u)step=52u;
    uint32_t start=dx+(dock_w-step*6u)/2u+4u;
    for(uint32_t i=0u;i<6u;++i)
        draw_glass_icon(start+i*step,dy+9u,icons[i],labels[i]);
    draw_top_menu();
    draw_notification();
}

static void draw_start_menu(void){
    if(!start_open) return;
    uint32_t w=framebuffer_width(),h=framebuffer_height();
    uint32_t mw=w>560u?460u:300u;
    uint32_t mh=h>580u?470u:h>480u?410u:350u;
    uint32_t x=12u,y=h-TASKBAR_H-mh-10u;
    glass_panel(x,y,mw,mh,24u,COL_PANEL,55u,1u);
    framebuffer_blend_round_rect(x+1u,y+1u,mw>2u?mw-2u:1u,58u,20u,0xE8F7FFu,14u);
    text_line("LIONOS",x+20u,y+18u,COL_GOLD,COL_PANEL2);
    text_line("APPLICATIONS",x+mw-116u,y+18u,COL_DIM,COL_PANEL2);

    framebuffer_blend_round_rect(x+18u,y+70u,mw-36u,34u,12u,COL_INPUT,90u);
    border(x+18u,y+70u,mw-36u,34u,COL_GOLD_DIM);
    text_line("Search applications...",x+31u,y+79u,COL_DIM,COL_INPUT);

    text_line("PINNED",x+20u,y+120u,COL_DIM,COL_PANEL);
    static const char *items[]={"TERMINAL","FILES","ABOUT","SETTINGS","BROWSER","NOTEPAD"};
    for(uint32_t i=0u;i<6u;++i){
        uint32_t by=y+136u+i*42u;
        int hov=hit(x+18u,by,mw-36u,38u,mouse_px_x,mouse_px_y);
        uint32_t bg=hov?COL_SKY_MID:COL_PANEL2;
        framebuffer_blend_round_rect(x+18u,by,mw-36u,38u,12u,bg,hov?120u:72u);
        border(x+18u,by,mw-36u,38u,hov?COL_GOLD:COL_GOLD_DIM);
        const char *name=items[i];
        text_line(name,x+30u,by+10u,COL_TEXT,bg);
        if(hov) text_line(">",x+mw-48u,by+10u,COL_GOLD,bg);
    }
    if(mh>400u){
        uint32_t py0=y+mh-52u;
        widget_button(x+18u,py0,140u,34u,COL_DANGER,COL_GOLD_DIM,"POWER");
        text_line("ESC",x+mw-58u,py0+9u,COL_DIM,COL_PANEL);
    }
}

static void draw_cursor(uint32_t x,uint32_t y){
    /*
     * Classic desktop pointer: black 1-pixel outline/shadow with a
     * white arrow fill, visually similar to mainstream desktop cursors.
     */
    static const uint16_t arrow[] = {
        0x8000u,0xC000u,0xE000u,0xF000u,0xF800u,
        0xFC00u,0xFE00u,0xFF00u,0xFF80u,0xFFC0u,
        0xFFE0u,0xFFF0u,0xFFF8u,0xFFF0u,0xF3E0u,
        0xE1C0u,0xC080u,0x8000u
    };
    const uint32_t h=sizeof(arrow)/sizeof(arrow[0]);
    for(uint32_t row=0u;row<h;++row){
        for(uint32_t col=0u;col<16u;++col){
            if(arrow[row]&(0x8000u>>col))
                fill(x+col+1u,y+row+1u,1u,1u,COL_GROUND);
        }
    }
    for(uint32_t row=1u;row<h-1u;++row){
        for(uint32_t col=1u;col<15u;++col){
            if(arrow[row]&(0x8000u>>col))
                fill(x+col,y+row,1u,1u,COL_TEXT);
        }
    }
}
static void glass_blob_position(uint32_t i,uint32_t tick,uint32_t *x,uint32_t *y){
    uint32_t w=framebuffer_width(),h=framebuffer_height();
    static const uint32_t phase_x[6]={0u,211u,487u,733u,971u,1249u};
    static const uint32_t phase_y[6]={0u,173u,331u,557u,719u,881u};
    uint32_t sx=w>0u?w:1u,sy=h>TASKBAR_H?h-TASKBAR_H:1u;
    *x=(phase_x[i%6u]+tick*(2u+i%3u)*3u)%sx;
    *y=40u+((phase_y[i%6u]+tick*(1u+i%4u)*2u)%(sy>80u?sy-60u:sy));
}
static void update_glass_background(void){
    uint32_t now=interrupt_timer_ticks();
    if(!glass_blob_ready){
        glass_tick=now;
        for(uint32_t i=0u;i<6u;++i)
            glass_blob_position(i,glass_tick,&glass_blob_x[i],&glass_blob_y[i]);
        glass_blob_ready=1u;
        return;
    }
    if(now==glass_tick)return;
    glass_tick=now;
    for(uint32_t i=0u;i<6u;++i){
        uint32_t ox=glass_blob_x[i],oy=glass_blob_y[i],nx,ny;
        glass_blob_position(i,glass_tick,&nx,&ny);
        uint32_t left=(ox<nx?ox:nx)>90u?(ox<nx?ox:nx)-90u:0u;
        uint32_t top=(oy<ny?oy:ny)>90u?(oy<ny?oy:ny)-90u:0u;
        uint32_t right=(ox>nx?ox:nx)+90u, bottom=(oy>ny?oy:ny)+90u;
        if(right>framebuffer_width())right=framebuffer_width();
        if(bottom>framebuffer_height()-TASKBAR_H)bottom=framebuffer_height()-TASKBAR_H;
        if(right>left&&bottom>top)
            dirty_rect(left,top,right-left,bottom-top);
        glass_blob_x[i]=nx; glass_blob_y[i]=ny;
    }
}
static void draw_desktop_background(void){
    draw_wallpaper();
    static const uint32_t tint[6]={0x6BD7FFu,0xB66BFFu,0xFF7E6Bu,0x58E6B0u,0xFFD166u,0x7D8CFFu};
    static const uint8_t alpha[6]={22u,20u,18u,18u,16u,18u};
    for(uint32_t i=0u;i<6u;++i)
        framebuffer_blend_circle(glass_blob_x[i],glass_blob_y[i],90u,tint[i],alpha[i]);
    framebuffer_blend_rect(0u,0u,framebuffer_width(),42u,COL_GROUND,18u);
}
static void draw_desktop_icons(void){
    for(uint32_t i=0u;i<DESKTOP_ICON_COUNT;++i)
        draw_desktop_icon(&desktop_icons[i]);
}

static void draw_window(const struct ui_window*w){if(!w->visible||w->minimized)return;switch(w->id){case WIN_TERMINAL:draw_terminal(w);break;case WIN_FILES:draw_files(w);break;case WIN_ABOUT:draw_about(w);break;case WIN_NOTEPAD:draw_notepad(w);break;default:draw_settings(w);break;}}
static void draw_windows(void){for(uint32_t i=0;i<WIN_MAX;++i)if(windows[i].visible&&!windows[i].minimized&&!windows[i].focused)draw_window(&windows[i]);for(uint32_t i=0;i<WIN_MAX;++i)if(windows[i].visible&&!windows[i].minimized&&windows[i].focused)draw_window(&windows[i]);}
static void render_all(void){
    uint32_t now=interrupt_timer_ticks();
    uint32_t sw=framebuffer_width(), sh=framebuffer_height();
    uint64_t dirty_pixels=dirty_valid?(uint64_t)dirty_w*dirty_h:(uint64_t)sw*sh;
    uint64_t screen_pixels=(uint64_t)sw*sh;
    uint32_t min_ticks=(dirty_pixels*2u<screen_pixels)?1u:2u;
    if(last_render_tick!=0xFFFFFFFFu && (uint32_t)(now-last_render_tick)<min_ticks)return;

    if(browser_is_active()){
        framebuffer_cursor_hide();
        browser_render();
        framebuffer_present();
        last_render_tick=now;
        dirty_valid=0u;
        scene_dirty=0u;
        return;
    }
    if(!scene_dirty||!dirty_valid)return;

    uint32_t x=dirty_x,y=dirty_y,w=dirty_w,h=dirty_h;
    framebuffer_cursor_hide();
    framebuffer_restore_wallpaper_rect(x,y,w,h);
    framebuffer_set_clip(x,y,w,h);
    draw_desktop_background();
    draw_desktop_icons();
    draw_windows();
    draw_taskbar();
    draw_start_menu();
    if(!cursor_overlay)draw_cursor(mouse_px_x,mouse_px_y);
    framebuffer_clear_clip();
    framebuffer_present_rect(x,y,w,h);
    scene_dirty=0u;
    dirty_valid=0u;
    last_render_tick=now;
}

static void init_windows(void){
    uint32_t sw=framebuffer_width(),sh=framebuffer_height()>TASKBAR_H?framebuffer_height()-TASKBAR_H:framebuffer_height();
    windows[0]=(struct ui_window){WIN_TERMINAL,0u,0u,0u,0u,(sw*18u)/100u,(sh*13u)/100u,(sw*62u)/100u,(sh*68u)/100u,0u,0u,0u,0u};
    windows[1]=(struct ui_window){WIN_FILES,0u,0u,0u,0u,(sw*25u)/100u,(sh*17u)/100u,(sw*50u)/100u,(sh*58u)/100u,0u,0u,0u,0u};
    windows[2]=(struct ui_window){WIN_ABOUT,0u,0u,0u,0u,(sw*34u)/100u,(sh*20u)/100u,(sw*36u)/100u,(sh*48u)/100u,0u,0u,0u,0u};
    windows[3]=(struct ui_window){WIN_SETTINGS,0u,0u,0u,0u,(sw*41u)/100u,(sh*18u)/100u,(sw*34u)/100u,(sh*52u)/100u,0u,0u,0u,0u};
    windows[4]=(struct ui_window){WIN_NOTEPAD,0u,0u,0u,0u,(sw*22u)/100u,(sh*12u)/100u,(sw*56u)/100u,(sh*64u)/100u,0u,0u,0u,0u};
    terminal_init();gui_active=1u;start_open=0u;drag_active=0u;
    desktop_icon_drag=-1;
    for(uint32_t i=0u;i<DESKTOP_ICON_COUNT;++i){
        desktop_icons[i].dragging=0u;
        desktop_icons[i].moved=0u;
    }
    terminal_focus=0u;
    dirty_full();
    last_render_tick=0xFFFFFFFFu;
}
static void close_gui(void){gui_active=0u;dirty_full();debug_write("LIONOS:GUI-EXIT\\n");}

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
    dirty_full();
    uint32_t x=mouse_px_x,y=mouse_px_y,h=framebuffer_height(),sw=framebuffer_width();
    if(browser_is_active()){browser_mouse_click(x,y);return;}
    if(y>=h-TASKBAR_H){
        uint32_t dock_w=sw>720u?620u:(sw>420u?sw-40u:320u);
        uint32_t dock_x=(sw-dock_w)/2u;
        uint32_t step=dock_w>=360u?92u:((dock_w-24u)/6u);
        if(step<52u)step=52u;
        uint32_t dock_y=h-TASKBAR_H+8u;
        for(uint32_t i=0u;i<6u;++i){
            uint32_t ix=dock_x+(dock_w-step*6u)/2u+4u+i*step;
            if(x>=ix&&x<ix+52u&&y>=dock_y+9u&&y<dock_y+61u){
                switch(i){
                    case 0u: show(WIN_TERMINAL); terminal_init(); return;
                    case 1u: show(WIN_FILES); return;
                    case 2u: browser_start(); return;
                    case 3u: show(WIN_SETTINGS); return;
                    case 4u: show(WIN_ABOUT); return;
                    case 5u: show(WIN_NOTEPAD); return;
                    default: break;
                }
            }
        }
        if(x<170u&&y>=h-TASKBAR_H){start_open=!start_open;return;}
    }
    if(start_open){
        uint32_t mw=framebuffer_width()>520u?420u:300u;
        uint32_t mh=framebuffer_height()>560u?440u:framebuffer_height()>460u?400u:340u;
        uint32_t sx=12u,sy=h-TASKBAR_H-mh-8u;
        if(x>=sx+18u&&x<sx+mw-18u&&y>=sy+122u&&y<sy+160u){show(WIN_TERMINAL);terminal_init();return;}
        if(x>=sx+18u&&x<sx+mw-18u&&y>=sy+164u&&y<sy+202u){show(WIN_FILES);return;}
        if(x>=sx+18u&&x<sx+mw-18u&&y>=sy+206u&&y<sy+244u){show(WIN_ABOUT);return;}
        if(x>=sx+18u&&x<sx+mw-18u&&y>=sy+248u&&y<sy+286u){show(WIN_SETTINGS);return;}
        if(x>=sx+18u&&x<sx+mw-18u&&y>=sy+290u&&y<sy+328u){browser_start();return;}
        if(x>=sx+18u&&x<sx+mw-18u&&y>=sy+332u&&y<sy+370u){show(WIN_NOTEPAD);return;}
        if(mh>330u&&x>=sx+18u&&x<sx+158u&&y>=sy+mh-52u&&y<sy+mh-18u){close_gui();return;}
        start_open=0u;
    }
    for(int i=(int)WIN_MAX-1;i>=0;--i){
        struct ui_window*w=&windows[i];
        if(w->visible&&!w->minimized&&x>=w->x&&x<w->x+w->w&&y>=w->y&&y<w->y+w->h){handle_window_click(w);return;}
    }

    {
        int icon=desktop_icon_at(x,y);
        if(icon>=0){
            desktop_icon_drag=icon;
            desktop_icons[icon].dragging=1u;
            desktop_icons[icon].moved=0u;
            desktop_icon_press_x=(int)x-(int)desktop_icons[icon].x;
            desktop_icon_press_y=(int)y-(int)desktop_icons[icon].y;
            return;
        }
    }

}

static int desktop_icon_at(uint32_t x,uint32_t y){
    uint32_t bottom=framebuffer_height()>TASKBAR_H?framebuffer_height()-TASKBAR_H:0u;
    for(int i=(int)DESKTOP_ICON_COUNT-1;i>=0;--i){
        const struct desktop_icon *icon=&desktop_icons[i];
        if(x>=icon->x&&x<icon->x+DESKTOP_ICON_SIZE&&
           y>=icon->y&&y<icon->y+DESKTOP_ICON_BLOCK_H&&
           y+DESKTOP_ICON_BLOCK_H<=bottom) return i;
    }
    return -1;
}

static void move_desktop_icon(struct desktop_icon *icon,uint32_t x,uint32_t y){
    if(!icon)return;
    uint32_t max_x=framebuffer_width()>DESKTOP_ICON_SIZE
        ? framebuffer_width()-DESKTOP_ICON_SIZE : 0u;
    uint32_t max_y=framebuffer_height()>TASKBAR_H+DESKTOP_ICON_BLOCK_H
        ? framebuffer_height()-TASKBAR_H-DESKTOP_ICON_BLOCK_H : 0u;
    int nx=(int)x-desktop_icon_press_x;
    int ny=(int)y-desktop_icon_press_y;
    if(nx<0)nx=0;
    if(ny<0)ny=0;
    if(nx>(int)max_x)nx=(int)max_x;
    if(ny>(int)max_y)ny=(int)max_y;
    if((uint32_t)nx!=icon->x || (uint32_t)ny!=icon->y)
        icon->moved=1u;
    icon->x=(uint32_t)nx;
    icon->y=(uint32_t)ny;
}

static void activate_desktop_icon(uint8_t action){
    switch(action){
        case ICON_ACTION_FILES: show(WIN_FILES); break;
        case ICON_ACTION_TERMINAL: show(WIN_TERMINAL); terminal_init(); break;
        case ICON_ACTION_BROWSER: browser_start(); break;
        case ICON_ACTION_SETTINGS: show(WIN_SETTINGS); break;
        case ICON_ACTION_ABOUT: show(WIN_ABOUT); break;
        case ICON_ACTION_NOTEPAD: show(WIN_NOTEPAD); break;
        default: break;
    }
}

static void handle_move(void){
    if(desktop_icon_drag>=0){
        struct desktop_icon *icon=&desktop_icons[desktop_icon_drag];
        uint32_t old_x=icon->x,old_y=icon->y;
        move_desktop_icon(icon,mouse_px_x,mouse_px_y);
        if(icon->x!=old_x||icon->y!=old_y){
            uint32_t left=old_x<icon->x?old_x:icon->x;
            uint32_t top=old_y<icon->y?old_y:icon->y;
            uint32_t right_old=old_x+DESKTOP_ICON_SIZE+8u;
            uint32_t right_new=icon->x+DESKTOP_ICON_SIZE+8u;
            uint32_t bottom_old=old_y+DESKTOP_ICON_BLOCK_H+8u;
            uint32_t bottom_new=icon->y+DESKTOP_ICON_BLOCK_H+8u;
            uint32_t right=right_old>right_new?right_old:right_new;
            uint32_t bottom=bottom_old>bottom_new?bottom_old:bottom_new;
            dirty_rect(left>5u?left-5u:0u,top>5u?top-5u:0u,
                       right-left+10u,bottom-top+10u);
        }
        return;
    }

    if(!drag_active)return;
    struct ui_window*w=window_by_id(drag_id);
    if(!w||!w->visible){drag_active=0u;return;}
    uint32_t old_x=w->x, old_y=w->y;
    int nx=(int)mouse_px_x-drag_dx,ny=(int)mouse_px_y-drag_dy;
    int max_y=(int)framebuffer_height()-(int)TASKBAR_H-(int)TITLE_H;
    if(nx<4)nx=4;
    if(ny<0)ny=0;
    if(nx+(int)w->w>(int)framebuffer_width()-4)nx=(int)framebuffer_width()-(int)w->w-4;
    if(ny>max_y)ny=max_y;
    if(w->x!=(uint32_t)nx||w->y!=(uint32_t)ny){
        uint32_t new_x=(uint32_t)nx,new_y=(uint32_t)ny;
        uint32_t left=old_x<new_x?old_x:new_x;
        uint32_t top=old_y<new_y?old_y:new_y;
        uint32_t right_old=old_x+w->w,right_new=new_x+w->w;
        uint32_t bottom_old=old_y+w->h,bottom_new=new_y+w->h;
        uint32_t right=right_old>right_new?right_old:right_new;
        uint32_t bottom=bottom_old>bottom_new?bottom_old:bottom_new;
        dirty_rect(left,top,right-left+4u,bottom-top+4u);
    }
    w->x=(uint32_t)nx;
    w->y=(uint32_t)ny;
}

static void handle_key(int key){
    if(browser_is_active()){browser_key(key);return;}
    if(key==27){
        if(terminal_focus&&window_by_id(WIN_TERMINAL)&&window_by_id(WIN_TERMINAL)->visible){hide(WIN_TERMINAL);return;}
        for(uint32_t i=0;i<WIN_MAX;++i)if(windows[i].focused){hide(windows[i].id);return;}
        close_gui();return;
    }
    if(notepad_focus){
        notepad_insert(key);
        dirty_full();
        return;
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
    cursor_overlay=(uint8_t)framebuffer_cursor_overlay_supported();
    init_windows();mouse_px_x=px();mouse_px_y=py();previous_buttons=mouse_buttons();render_all();
    if(cursor_overlay)framebuffer_cursor_move(mouse_px_x,mouse_px_y);
}
void gui_step(void){
    if(!gui_active)return;
    update_glass_background();
    const char *usb_status=mouse_usb_status_text();
    if(usb_status!=last_usb_status){
        last_usb_status=usb_status;
        dirty_full();
    }
    uint32_t old_x=mouse_px_x,old_y=mouse_px_y;
    mouse_px_x=px();mouse_px_y=py();
    uint32_t buttons=mouse_buttons();
    uint8_t cursor_moved=(mouse_px_x!=old_x||mouse_px_y!=old_y)?1u:0u;
    if(cursor_moved){
        uint32_t left=(old_x<mouse_px_x?old_x:mouse_px_x)>44u?(old_x<mouse_px_x?old_x:mouse_px_x)-44u:0u;
        uint32_t top=(old_y<mouse_px_y?old_y:mouse_px_y)>44u?(old_y<mouse_px_y?old_y:mouse_px_y)-44u:0u;
        uint32_t right=(old_x>mouse_px_x?old_x:mouse_px_x)+52u;
        uint32_t bottom=(old_y>mouse_px_y?old_y:mouse_px_y)+52u;
        if(right>framebuffer_width())right=framebuffer_width();
        if(bottom>framebuffer_height())bottom=framebuffer_height();
        if(right>left&&bottom>top)dirty_rect(left,top,right-left,bottom-top);
        if(!cursor_overlay)dirty_full();
    }
    if(buttons!=previous_buttons)dirty_full();
    if((buttons&1u)&&!(previous_buttons&1u))handle_click();
    if(!(buttons&1u)&&(previous_buttons&1u)){
        if(desktop_icon_drag>=0){
            int icon=desktop_icon_drag;
            desktop_icons[icon].dragging=0u;
            if(!desktop_icons[icon].moved)
                activate_desktop_icon(desktop_icons[icon].action);
            desktop_icon_drag=-1;
        }
        drag_active=0u;
        dirty_full();
    }
    handle_move();
    if(browser_is_active()){browser_step();previous_buttons=buttons;dirty_full();render_all();return;}
    while(keyboard_available()){dirty_full();handle_key(keyboard_getchar());}
    previous_buttons=buttons;
    render_all();
    if(cursor_overlay&&cursor_moved)framebuffer_cursor_move(mouse_px_x,mouse_px_y);
}
int gui_is_active(void){return gui_active!=0u;}
void gui_desktop_run(void){
    gui_start();
    if(!gui_active)return;
    uint32_t last_frame=interrupt_timer_ticks();
    for(;;){
        /*
         * Use the existing PIT 100 Hz interrupt as the desktop/input clock.
         * It is already the kernel scheduler clock and does not depend on
         * LAPIC timer calibration, which can vary across physical machines.
         */
        while(gui_active && interrupt_timer_ticks()==last_frame)
            __asm__ volatile("sti; hlt");
        if(!gui_active)break;
        last_frame=interrupt_timer_ticks();
        gui_step();
    }
}
