#include <stdint.h>
#include "gui.h"
#include "debug.h"
#include "framebuffer.h"
#include "keyboard.h"
#include "mouse.h"
#include "memory.h"
#include "vfs.h"

#define FONT_W 5u
#define FONT_H 7u
#define FONT_SCALE 2u
#define CHAR_W 12u
#define CHAR_H 18u
#define TOPBAR_H 52u
#define TASKBAR_H 64u
#define TITLE_H 42u
#define START_W 104u
#define START_X 16u
#define START_H 430u
#define WIN_TERMINAL 1u
#define WIN_ABOUT 2u
#define WIN_SETTINGS 3u
#define WIN_MAX 3u

#define UI_BG 0x07101Cu
#define UI_TOP 0x101A2Au
#define UI_SURFACE 0x111D30u
#define UI_SURFACE2 0x182942u
#define UI_BORDER 0x2C4568u
#define UI_TEXT 0xEEF4FCu
#define UI_DIM 0x91A7C4u
#define UI_BLUE 0x2F80EDu
#define UI_BLUE2 0x5BB7FFu
#define UI_GREEN 0x55D187u
#define UI_YELLOW 0xF2C14Eu
#define UI_RED 0xD95368u
#define UI_WHITE 0xFFFFFFu
#define UI_INPUT 0x08111Eu

struct ui_window {
    uint8_t id;
    uint8_t visible;
    uint8_t minimized;
    uint8_t focused;
    uint32_t x, y, w, h;
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
static char term_lines[24][121];
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
        case '-': rows[3]=0x1F; break;
        case '_': rows[6]=0x1F; break;
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
static uint32_t px(void){uint32_t w=framebuffer_width();uint32_t x=mouse_x();return w?(x*(w-1u))/79u:0u;}
static uint32_t py(void){uint32_t h=framebuffer_height();uint32_t y=mouse_y();return h?(y*(h-1u))/24u:0u;}
static struct ui_window*window_by_id(uint8_t id){return id>=1u&&id<=WIN_MAX?&windows[id-1u]:0;}
static void focus(uint8_t id){for(uint32_t i=0;i<WIN_MAX;++i)windows[i].focused=(windows[i].id==id&&windows[i].visible&&!windows[i].minimized)?1u:0u;}
static void show(uint8_t id){struct ui_window*w=window_by_id(id);if(!w)return;w->visible=1u;w->minimized=0u;focus(id);start_open=0u;}
static void hide(uint8_t id){struct ui_window*w=window_by_id(id);if(!w)return;w->visible=0u;w->minimized=0u;w->focused=0u;drag_active=0u;if(id==WIN_TERMINAL)terminal_focus=0u;}
static void minimize(uint8_t id){struct ui_window*w=window_by_id(id);if(!w)return;w->minimized=1u;w->focused=0u;drag_active=0u;if(id==WIN_TERMINAL)terminal_focus=0u;}

static void term_clear(void){term_line_count=0u;term_len=0u;term_input[0]=0;}
static void term_line(const char*s){if(term_line_count<24u){uint32_t i=0;while(s[i]&&i<120u){term_lines[term_line_count][i]=s[i];++i;}term_lines[term_line_count][i]=0;term_line_count++;return;}for(uint32_t r=1;r<24u;++r)for(uint32_t c=0;c<121u;++c)term_lines[r-1u][c]=term_lines[r][c];uint32_t i=0;while(s[i]&&i<120u){term_lines[23][i]=s[i];++i;}term_lines[23][i]=0;}
static int eq(const char*a,const char*b){while(*a&&*a==*b){++a;++b;}return *a==*b;}
static void terminal_command(void){term_input[term_len]=0;if(!term_len)return;if(eq(term_input,"help"))term_line("help  ls  pwd  mem  uname  version  about  clear  exit");else if(eq(term_input,"ls")){uint32_t n=vfs_count();if(!n)term_line("(no files)");else for(uint32_t i=0;i<n&&term_line_count<23u;++i){const char*nme=vfs_name(i);if(nme)term_line(nme);}}else if(eq(term_input,"pwd"))term_line("/");else if(eq(term_input,"mem"))term_line("memory manager online");else if(eq(term_input,"uname"))term_line("LionOS 0.8 x86 i386");else if(eq(term_input,"version"))term_line("LionOS version 0.8");else if(eq(term_input,"about"))term_line("Experimental 32-bit OS with SMP, VFS and GUI.");else if(eq(term_input,"clear"))term_clear();else if(eq(term_input,"exit")){hide(WIN_TERMINAL);return;}else term_line("Unknown command. Type help.");term_len=0u;term_input[0]=0;}
static void terminal_init(void){term_clear();term_line("LionOS Terminal");term_line("Graphical terminal ready.");term_line("Type help for commands. ESC closes this window.");}
static void window_chrome(const struct ui_window*w,const char*title){fill(w->x,w->y,w->w,w->h,UI_SURFACE);border(w->x,w->y,w->w,w->h,w->focused?UI_BLUE2:UI_BORDER);fill(w->x,w->y,w->w,TITLE_H,w->focused?UI_SURFACE2:UI_TOP);text_line(title,w->x+18u,w->y+12u,UI_TEXT,w->focused?UI_SURFACE2:UI_TOP);fill(w->x+w->w-96u,w->y+12u,20u,18u,UI_SURFACE);fill(w->x+w->w-66u,w->y+12u,20u,18u,UI_YELLOW);fill(w->x+w->w-36u,w->y+12u,20u,18u,UI_RED);}
static void draw_terminal_window(const struct ui_window*w){window_chrome(w,"LionOS Terminal");uint32_t x=w->x+20u,y=w->y+TITLE_H+16u;for(uint32_t i=0;i<term_line_count;++i)text_line(term_lines[i],x,y+i*20u,i+1u==term_line_count?UI_TEXT:UI_DIM,UI_SURFACE);fill(x,y+20u*20u, w->w-40u,24u,UI_INPUT);text_line("lion> ",x,y+20u*20u,UI_BLUE2,UI_INPUT);text_line(term_input,x+6u*CHAR_W,y+20u*20u,UI_TEXT,UI_INPUT);}
static void draw_about_window(const struct ui_window*w){window_chrome(w,"About LionOS");text_line("LionOS",w->x+32u,w->y+82u,UI_YELLOW,UI_SURFACE);text_line("32-bit x86 experimental operating system",w->x+32u,w->y+124u,UI_TEXT,UI_SURFACE);text_line("SMP / paging / processes / VFS / ELF32",w->x+32u,w->y+164u,UI_DIM,UI_SURFACE);text_line("Framebuffer desktop / mouse / keyboard",w->x+32u,w->y+204u,UI_BLUE2,UI_SURFACE);text_line("ESC closes the focused window",w->x+32u,w->y+254u,UI_DIM,UI_SURFACE);}
static void draw_settings_window(const struct ui_window*w){window_chrome(w,"Settings");text_line("Personalization",w->x+28u,w->y+80u,UI_TEXT,UI_SURFACE);fill(w->x+28u,w->y+118u,250u,48u,UI_BLUE);border(w->x+28u,w->y+118u,250u,48u,UI_BORDER);text_line("Blue accent",w->x+44u,w->y+132u,UI_TEXT,UI_BLUE);text_line("Display",w->x+340u,w->y+80u,UI_TEXT,UI_SURFACE);text_line("Framebuffer desktop",w->x+340u,w->y+120u,UI_DIM,UI_SURFACE);}
static void draw_cursor(uint32_t x,uint32_t y){fill(x,y,3u,22u,UI_WHITE);fill(x,y,12u,3u,UI_WHITE);fill(x+5u,y+16u,7u,3u,UI_WHITE);fill(x+7u,y+12u,4u,4u,UI_WHITE);}
static void draw_taskbar(void){uint32_t h=framebuffer_height(),w=framebuffer_width(),y=h-TASKBAR_H;fill(0u,y,w,TASKBAR_H,UI_TOP);fill(0u,y,w,1u,UI_BORDER);fill(14u,y+10u,START_W,42u,UI_BLUE);border(14u,y+10u,START_W,42u,UI_BORDER);text_line("START",30u,y+22u,UI_TEXT,UI_BLUE);fill(136u,y+10u,170u,42u,terminal_focus?UI_SURFACE2:UI_SURFACE);border(136u,y+10u,170u,42u,UI_BORDER);text_line("TERMINAL",150u,y+22u,UI_TEXT,terminal_focus?UI_SURFACE2:UI_SURFACE);fill(322u,y+10u,140u,42u,UI_SURFACE);border(322u,y+10u,140u,42u,UI_BORDER);text_line("ABOUT",336u,y+22u,UI_TEXT,UI_SURFACE);fill(476u,y+10u,160u,42u,UI_SURFACE);border(476u,y+10u,160u,42u,UI_BORDER);text_line("SETTINGS",490u,y+22u,UI_TEXT,UI_SURFACE);text_line("LionOS",w>760u?w-120u:650u,y+22u,UI_DIM,UI_TOP);}
static void draw_start_menu(void){if(!start_open)return;uint32_t h=framebuffer_height();uint32_t y=h-TASKBAR_H-START_H;fill(18u,y,430u,START_H,UI_SURFACE);border(18u,y,430u,START_H,UI_BORDER);fill(18u,y,430u,72u,UI_SURFACE2);text_line("LIONOS",42u,y+22u,UI_YELLOW,UI_SURFACE2);text_line("Pinned",42u,y+92u,UI_DIM,UI_SURFACE);fill(42u,y+126u,330u,48u,UI_SURFACE2);border(42u,y+126u,330u,48u,UI_BORDER);text_line("TERMINAL",58u,y+140u,UI_TEXT,UI_SURFACE2);fill(42u,y+186u,330u,48u,UI_SURFACE2);border(42u,y+186u,330u,48u,UI_BORDER);text_line("ABOUT",58u,y+200u,UI_TEXT,UI_SURFACE2);fill(42u,y+246u,330u,48u,UI_SURFACE2);border(42u,y+246u,330u,48u,UI_BORDER);text_line("SETTINGS",58u,y+260u,UI_TEXT,UI_SURFACE2);fill(42u,y+START_H-62u,150u,40u,UI_RED);border(42u,y+START_H-62u,150u,40u,UI_BORDER);text_line("POWER",58u,y+START_H-50u,UI_TEXT,UI_RED);}
static void draw_desktop(void){uint32_t w=framebuffer_width(),h=framebuffer_height();fill(0u,0u,w,h,UI_BG);fill(0u,0u,w,TOPBAR_H,UI_TOP);text_line("LionOS",24u,18u,UI_YELLOW,UI_TOP);text_line("Desktop",132u,18u,UI_TEXT,UI_TOP);text_line("READY",w>900u?w-110u:700u,18u,UI_GREEN,UI_TOP);fill(40u,96u,72u,56u,UI_SURFACE2);border(40u,96u,72u,56u,UI_BORDER);text_line("PC",60u,112u,UI_BLUE2,UI_SURFACE2);text_line("This PC",40u,164u,UI_TEXT,UI_BG);fill(150u,96u,72u,56u,UI_SURFACE2);border(150u,96u,72u,56u,UI_BORDER);text_line(">_",172u,112u,UI_GREEN,UI_SURFACE2);text_line("Terminal",142u,164u,UI_TEXT,UI_BG);fill(260u,96u,72u,56u,UI_SURFACE2);border(260u,96u,72u,56u,UI_BORDER);text_line("i",286u,112u,UI_YELLOW,UI_SURFACE2);text_line("About",262u,164u,UI_TEXT,UI_BG);fill(390u,100u,720u,250u,UI_SURFACE);border(390u,100u,720u,250u,UI_BORDER);text_line("Welcome to LionOS",430u,132u,UI_TEXT,UI_SURFACE);text_line("A framebuffer desktop inspired by modern operating systems",430u,174u,UI_DIM,UI_SURFACE);text_line("Windows + keyboard + mouse + event-driven GUI",430u,200u,UI_DIM,UI_SURFACE);fill(430u,244u,210u,48u,0x102846u);text_line("SYSTEM ONLINE",446u,258u,UI_GREEN,0x102846u);fill(662u,244u,210u,48u,UI_SURFACE2);text_line("SMP ONLINE",678u,258u,UI_BLUE2,UI_SURFACE2);fill(894u,244u,180u,48u,0x28200Fu);text_line("GUI READY",910u,258u,UI_YELLOW,0x28200Fu);draw_taskbar();draw_start_menu();}
static void draw_windows(void){for(uint32_t i=0;i<WIN_MAX;++i)if(windows[i].visible&&!windows[i].minimized){if(windows[i].id==WIN_TERMINAL)draw_terminal_window(&windows[i]);else if(windows[i].id==WIN_ABOUT)draw_about_window(&windows[i]);else draw_settings_window(&windows[i]);}draw_cursor(mouse_px_x,mouse_px_y);}
static void render_all(void){draw_desktop();draw_windows();}

static void init_windows(void){windows[0]=(struct ui_window){WIN_TERMINAL,0u,0u,0u,360u,150u,1120u,650u};windows[1]=(struct ui_window){WIN_ABOUT,0u,0u,0u,560u,210u,760u,460u};windows[2]=(struct ui_window){WIN_SETTINGS,0u,0u,0u,520u,190u,900u,520u};terminal_init();gui_active=1u;start_open=0u;drag_active=0u;terminal_focus=0u;}
static void close_gui(void){gui_active=0u;mouse_set_cursor_visible(1u);mouse_show();debug_write("LIONOS:GUI-EXIT\n");}
static void handle_window_click(struct ui_window*w){uint32_t x=mouse_px_x,y=mouse_px_y;if(y<w->y+TITLE_H&&x>=w->x&&x<w->x+w->w){if(x>=w->x+w->w-52u&&x<w->x+w->w-22u){minimize(w->id);return;}if(x>=w->x+w->w-22u){hide(w->id);return;}drag_active=1u;drag_id=w->id;drag_dx=(int)x-(int)w->x;drag_dy=(int)y-(int)w->y;focus(w->id);return;}focus(w->id);}
static void handle_click(void){uint32_t x=mouse_px_x,y=mouse_px_y,h=framebuffer_height();if(y>=h-TASKBAR_H){if(x>=14u&&x<14u+START_W){start_open=!start_open;return;}if(x>=136u&&x<306u){show(WIN_TERMINAL);terminal_focus=1u;return;}if(x>=322u&&x<462u){show(WIN_ABOUT);return;}if(x>=476u&&x<636u){show(WIN_SETTINGS);return;}}
    if(start_open){uint32_t sy=h-TASKBAR_H-START_H;if(x>=42u&&x<372u&&y>=sy+126u&&y<sy+174u){show(WIN_TERMINAL);terminal_focus=1u;terminal_init();return;}if(x>=42u&&x<372u&&y>=sy+186u&&y<sy+234u){show(WIN_ABOUT);return;}if(x>=42u&&x<372u&&y>=sy+246u&&y<sy+294u){show(WIN_SETTINGS);return;}if(x>=42u&&x<192u&&y>=sy+START_H-62u&&y<sy+START_H-22u){close_gui();return;}start_open=0u;}
    for(int i=(int)WIN_MAX-1;i>=0;--i){struct ui_window*w=&windows[i];if(w->visible&&!w->minimized&&x>=w->x&&x<w->x+w->w&&y>=w->y&&y<w->y+w->h){handle_window_click(w);return;}}
    if(x>=40u&&x<120u&&y>=92u&&y<180u){show(WIN_TERMINAL);terminal_focus=1u;terminal_init();return;}if(x>=150u&&x<250u&&y>=92u&&y<180u){show(WIN_ABOUT);return;}}
static void handle_move(void){if(!drag_active)return;struct ui_window*w=window_by_id(drag_id);if(!w||!w->visible){drag_active=0u;return;}int nx=(int)mouse_px_x-drag_dx,ny=(int)mouse_px_y-drag_dy;if(nx<8)nx=8;if(ny<(int)TOPBAR_H)ny=(int)TOPBAR_H;if(nx+(int)w->w>(int)framebuffer_width()-8)nx=(int)framebuffer_width()-(int)w->w-8;if(ny+(int)w->h>(int)framebuffer_height()-(int)TASKBAR_H)ny=(int)framebuffer_height()-(int)TASKBAR_H-(int)w->h;w->x=(uint32_t)nx;w->y=(uint32_t)ny;}
static void handle_key(int key){if(key==27){if(terminal_focus&&window_by_id(WIN_TERMINAL)&&window_by_id(WIN_TERMINAL)->visible){hide(WIN_TERMINAL);return;}for(uint32_t i=0;i<WIN_MAX;++i)if(windows[i].focused){hide(windows[i].id);return;}close_gui();return;}if(terminal_focus){if(key=='\n'||key==13){terminal_command();return;}if(key=='\b'||key==127){if(term_len){--term_len;term_input[term_len]=0;}return;}if(key>=32&&key<127&&term_len<120u){term_input[term_len++]=(char)key;term_input[term_len]=0;}return;}if(key=='a'||key=='A')show(WIN_ABOUT);else if(key=='t'||key=='T'){show(WIN_TERMINAL);terminal_focus=1u;}}

void gui_start(void){debug_write("LIONOS:GUI-ENTER\n");if(!framebuffer_available()){debug_write("LIONOS:GUI-NO-FRAMEBUFFER\n");return;}mouse_set_cursor_visible(0u);while(keyboard_available())(void)keyboard_getchar();init_windows();mouse_px_x=px();mouse_px_y=py();previous_buttons=mouse_buttons();render_all();}
void gui_step(void){if(!gui_active)return;keyboard_poll();mouse_poll();mouse_px_x=px();mouse_px_y=py();uint32_t buttons=mouse_buttons();if((buttons&1u)&&!(previous_buttons&1u))handle_click();if(!(buttons&1u)&&(previous_buttons&1u))drag_active=0u;handle_move();while(keyboard_available())handle_key(keyboard_getchar());previous_buttons=buttons;render_all();}
int gui_is_active(void){return gui_active!=0u;}
void gui_run(void){gui_start();}
