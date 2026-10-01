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
#define TASKBAR_H 54u
#define TITLE_H 30u
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

static void fill(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c){framebuffer_fill_rect(x,y,w,h,c);}
static void border(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c){if(w<2u||h<2u)return;fill(x,y,w,1u,c);fill(x,y+h-1u,w,1u,c);fill(x,y,1u,h,c);fill(x+w-1u,y,1u,h,c);}
static void text(char c,uint32_t x,uint32_t y,uint32_t fg,uint32_t bg){
    (void)bg;
    uint16_t rows[FONT_H];glyph(c,rows);
    for(uint32_t gy=0u;gy<FONT_H;++gy)for(uint32_t gx=0u;gx<FONT_W;++gx)
        if(rows[gy]&(1u<<(FONT_W-1u-gx)))fill(x+gx,y+gy+2u,1u,1u,fg);
}
static void text_line(const char*s,uint32_t x,uint32_t y,uint32_t fg,uint32_t bg){while(*s&&x+CHAR_W<framebuffer_width()){text(*s,x,y,fg,bg);x+=CHAR_W;++s;}}
static uint32_t px(void){return mouse_x();}
static uint32_t py(void){return mouse_y();}
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
        if(n>0u) notepad_len=(uint32_t)n;
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
    fill(x,y,body_w,body_h,0x050A12u);
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
    static const char *short_names[]={"","FILES","HOME","TERM","BROW","SET","ABOUT","NOTE"};
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
    (void)short_names;
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
    uint32_t mh=h>560u?440u:h>460u?400u:340u;
    uint32_t x=12u,y=h-TASKBAR_H-mh-8u;
    fill(x+5u,y+5u,mw,mh,0x03070Cu);
    fill(x,y,mw,mh,COL_PANEL);border(x,y,mw,mh,COL_GOLD_DIM);fill(x,y,mw,54u,COL_PANEL2);
    text_line("LIONOS",x+18u,y+18u,COL_GOLD,COL_PANEL2);
    fill(x+18u,y+62u,mw-36u,32u,COL_INPUT);border(x+18u,y+62u,mw-36u,32u,COL_GOLD_DIM);
    text_line("Search applications...",x+30u,y+71u,COL_DIM,COL_INPUT);
    text_line("PINNED APPLICATIONS",x+18u,y+108u,COL_DIM,COL_PANEL);
    static const char *items[]={"TERMINAL","FILES","ABOUT","SETTINGS","BROWSER","NOTEPAD"};
    for(uint32_t i=0u;i<6u;++i){
        uint32_t by=y+122u+i*42u;
        fill(x+18u,by,mw-36u,38u,COL_PANEL2);
        border(x+18u,by,mw-36u,38u,COL_GOLD_DIM);
        const char *name=items[i];
        text_line(name,x+18u+(mw-36u-label_width(name))/2u,by+10u,COL_TEXT,COL_PANEL2);
    }
    if(mh>400u){
        uint32_t py=y+mh-52u;
        fill(x+18u,py,140u,34u,COL_DANGER);
        border(x+18u,py,140u,34u,COL_GOLD_DIM);
        text_line("POWER",x+32u,py+8u,COL_TEXT,COL_DANGER);
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
static void draw_desktop_background(void){draw_wallpaper();}
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
    uint32_t x=mouse_px_x,y=mouse_px_y,h=framebuffer_height();
    if(browser_is_active()){browser_mouse_click(x,y);return;}
    if(y>=h-TASKBAR_H){
        if(x>=16u&&x<180u){start_open=!start_open;return;}
        if(x>=218u&&x<260u){show(WIN_TERMINAL);terminal_init();return;}
        if(x>=270u&&x<312u){show(WIN_FILES);return;}
        if(x>=322u&&x<364u){browser_start();return;}
        if(x>=374u&&x<416u){show(WIN_SETTINGS);return;}
        if(x>=426u&&x<468u){show(WIN_ABOUT);return;}
        if(x>=478u&&x<520u){show(WIN_NOTEPAD);return;}
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
    keyboard_poll();mouse_poll();
    const char *usb_status=mouse_usb_status_text();
    if(usb_status!=last_usb_status){
        last_usb_status=usb_status;
        dirty_full();
    }
    uint32_t old_x=mouse_px_x,old_y=mouse_px_y;
    mouse_px_x=px();mouse_px_y=py();
    uint32_t buttons=mouse_buttons();
    uint8_t cursor_moved=(mouse_px_x!=old_x||mouse_px_y!=old_y)?1u:0u;
    if((cursor_moved&&!cursor_overlay)||buttons!=previous_buttons)dirty_full();
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
