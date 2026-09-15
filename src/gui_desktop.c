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

#define TOPBAR_H 54u
#define TASKBAR_H 64u
#define START_W 104u
#define START_PANEL_W 430u
#define START_PANEL_H 500u

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
    uint32_t x,y,w,h;
};

static struct ui_window windows[WIN_MAX];
static uint8_t gui_active;
static uint8_t start_open;
static uint8_t drag_active;
static uint8_t drag_id;
static int drag_dx,drag_dy;
static uint32_t mouse_px_x,mouse_px_y,previous_buttons;

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

static void glyph(char c,uint8_t rows[FONT_H]){
    for(uint32_t i=0;i<FONT_H;++i)rows[i]=0u;
    if(c>='a'&&c<='z')c=(char)(c-'a'+'A');
    if(c>='A'&&c<='Z'){
        for(uint32_t i=0;i<FONT_H;++i)rows[i]=letters[(uint32_t)(c-'A')][i];
        return;
    }
    switch(c){
        case '0':rows[0]=0x0E;rows[1]=0x11;rows[2]=0x13;rows[3]=0x15;rows[4]=0x19;rows[5]=0x11;rows[6]=0x0E;break;
        case '1':rows[0]=0x04;rows[1]=0x0C;rows[2]=0x04;rows[3]=0x04;rows[4]=0x04;rows[5]=0x04;rows[6]=0x0E;break;
        case '2':rows[0]=0x0E;rows[1]=0x11;rows[2]=0x01;rows[3]=0x02;rows[4]=0x04;rows[5]=0x08;rows[6]=0x1F;break;
        case '3':rows[0]=0x1E;rows[1]=0x01;rows[2]=0x01;rows[3]=0x0E;rows[4]=0x01;rows[5]=0x01;rows[6]=0x1E;break;
        case '4':rows[0]=0x02;rows[1]=0x06;rows[2]=0x0A;rows[3]=0x12;rows[4]=0x1F;rows[5]=0x02;rows[6]=0x02;break;
        case '5':rows[0]=0x1F;rows[1]=0x10;rows[2]=0x10;rows[3]=0x1E;rows[4]=0x01;rows[5]=0x01;rows[6]=0x1E;break;
        case '6':rows[0]=0x0E;rows[1]=0x10;rows[2]=0x10;rows[3]=0x1E;rows[4]=0x11;rows[5]=0x11;rows[6]=0x0E;break;
        case '7':rows[0]=0x1F;rows[1]=0x01;rows[2]=0x02;rows[3]=0x04;rows[4]=0x08;rows[5]=0x08;rows[6]=0x08;break;
        case '8':rows[0]=0x0E;rows[1]=0x11;rows[2]=0x11;rows[3]=0x0E;rows[4]=0x11;rows[5]=0x11;rows[6]=0x0E;break;
        case '9':rows[0]=0x0E;rows[1]=0x11;rows[2]=0x11;rows[3]=0x0F;rows[4]=0x01;rows[5]=0x01;rows[6]=0x0E;break;
        case ':':rows[2]=0x04;rows[4]=0x04;break;
        case '.':rows[6]=0x04;break;
        case ',':rows[5]=0x04;rows[6]=0x08;break;
        case '-':rows[3]=0x1F;break;
        case '_':rows[6]=0x1F;break;
        case '>':rows[1]=0x10;rows[2]=0x08;rows[3]=0x04;rows[4]=0x08;rows[5]=0x10;break;
        case '<':rows[1]=0x01;rows[2]=0x02;rows[3]=0x04;rows[4]=0x02;rows[5]=0x01;break;
        case '/':rows[0]=0x01;rows[1]=0x02;rows[2]=0x04;rows[3]=0x08;rows[4]=0x10;break;
        case '!':rows[0]=0x04;rows[1]=0x04;rows[2]=0x04;rows[3]=0x04;rows[5]=0x04;break;
        case '?':rows[0]=0x0E;rows[1]=0x11;rows[2]=0x01;rows[3]=0x02;rows[4]=0x04;rows[6]=0x04;break;
        case '=':rows[2]=0x1F;rows[4]=0x1F;break;
        case '|':rows[0]=0x04;rows[1]=0x04;rows[2]=0x04;rows[3]=0x04;rows[4]=0x04;rows[5]=0x04;rows[6]=0x04;break;
        case ' ':break;
        default:rows[0]=0x1F;rows[2]=0x15;rows[4]=0x15;rows[6]=0x1F;break;
    }
}

static void ui_fill(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c){framebuffer_fill_rect(x,y,w,h,c);}
static void ui_border(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c){if(w<2u||h<2u)return;ui_fill(x,y,w,1u,c);ui_fill(x,y+h-1u,w,1u,c);ui_fill(x,y,1u,h,c);ui_fill(x+w-1u,y,1u,h,c);}
static void ui_char(char c,uint32_t x,uint32_t y,uint32_t fg,uint32_t bg){uint8_t rows[FONT_H];glyph(c,rows);ui_fill(x,y,CHAR_W,CHAR_H,bg);for(uint32_t gy=0;gy<FONT_H;++gy)for(uint32_t gx=0;gx<FONT_W;++gx)if(rows[gy]&(1u<<(FONT_W-1u-gx)))ui_fill(x+gx*FONT_SCALE,y+gy*FONT_SCALE+2u,FONT_SCALE,FONT_SCALE,fg);}
static void ui_text(const char*s,uint32_t x,uint32_t y,uint32_t fg){while(*s&&x+CHAR_W<framebuffer_width()){ui_char(*s,x,y,fg,0u);x+=CHAR_W;++s;}}
static void ui_text_bg(const char*s,uint32_t x,uint32_t y,uint32_t fg,uint32_t bg){while(*s&&x+CHAR_W<framebuffer_width()){ui_char(*s,x,y,fg,bg);x+=CHAR_W;++s;}}
static uint32_t mx(void){uint32_t w=framebuffer_width();uint32_t x=mouse_x();return w?(x*(w-1u))/79u:0u;}
static uint32_t my(void){uint32_t h=framebuffer_height();uint32_t y=mouse_y();return h?(y*(h-1u))/24u:0u;}
static struct ui_window*win(uint8_t id){return id>=1u&&id<=WIN_MAX?&windows[id-1u]:0;}
static void focus_window(uint8_t id){for(uint32_t i=0;i<WIN_MAX;++i)windows[i].focused=(windows[i].id==id&&windows[i].visible&&!windows[i].minimized)?1u:0u;}
static void show_window(uint8_t id){struct ui_window*w=win(id);if(!w)return;w->visible=1u;w->minimized=0u;focus_window(id);start_open=0u;}
static void hide_window(uint8_t id){struct ui_window*w=win(id);if(!w)return;w->visible=0u;w->minimized=0u;w->focused=0u;drag_active=0u;if(id==WIN_TERMINAL)term_len=0u;}
static void min_window(uint8_t id){struct ui_window*w=win(id);if(!w)return;w->minimized=1u;w->focused=0u;drag_active=0u;}
static void term_clear(void){term_line_count=0u;term_len=0u;term_input[0]=0;}
static void term_line(const char*s){if(term_line_count<24u){uint32_t i=0;while(s[i]&&i<120u){term_lines[term_line_count][i]=s[i];++i;}term_lines[term_line_count][i]=0;term_line_count++;return;}for(uint32_t r=1;r<24u;++r)for(uint32_t c=0;c<121u;++c)term_lines[r-1u][c]=term_lines[r][c];uint32_t i=0;while(s[i]&&i<120u){term_lines[23][i]=s[i];++i;}term_lines[23][i]=0;}
static int text_eq(const char*a,const char*b){while(*a&&*a==*b){++a;++b;}return *a==*b;}
static void term_command(void){term_input[term_len]=0;if(!term_len)return;if(text_eq(term_input,"help"))term_line("help  ls  pwd  mem  uname  version  about  clear  exit");else if(text_eq(term_input,"ls")){uint32_t n=vfs_count();if(!n)term_line("(no files)");else for(uint32_t i=0;i<n&&term_line_count<23u;++i){const char*nme=vfs_name(i);if(nme)term_line(nme);}}else if(text_eq(term_input,"pwd"))term_line("/");else if(text_eq(term_input,"mem"))term_line("memory manager online");else if(text_eq(term_input,"uname"))term_line("LionOS 0.8 x86 i386");else if(text_eq(term_input,"version"))term_line("LionOS version 0.8");else if(text_eq(term_input,"about"))term_line("Experimental 32-bit OS with SMP, VFS and GUI.");else if(text_eq(term_input,"clear"))term_clear();else if(text_eq(term_input,"exit")){hide_window(WIN_TERMINAL);terminal_focus=0u;}else term_line("Unknown command. Type help.");term_len=0u;term_input[0]=0;}
static void terminal_init(void){term_clear();term_line("LionOS Terminal");term_line("Graphical terminal ready.");term_line("Type help for commands. ESC closes this window.");}
static void draw_window(const struct ui_window*w,const char*title){uint32_t bg=UI_SURFACE;ui_fill(w->x,w->y,w->w,w->h,bg);ui_border(w->x,w->y,w->w,w->h,w->focused?UI_BLUE2:UI_BORDER);ui_fill(w->x,w->y,w->w,44u,w->focused?UI_SURFACE2:UI_SURFACE);ui_fill(w->x+w->w-102u,w->y+12u,20u,20u,UI_SURFACE);ui_fill(w->x+w->w-72u,w->y+12u,20u,20u,UI_YELLOW);ui_fill(w->x+w->w-42u,w->y+12u,20u,20u,UI_RED);ui_text(title,w->x+18u,w->y+12u,UI_TEXT);}
static void draw_terminal_window(const struct ui_window*w){draw_window(w,"LionOS Terminal");uint32_t x=w->x+24u,y=w->y+64u;for(uint32_t i=0;i<term_line_count&&i<23u;++i)ui_text_bg(term_lines[i],x,y+i*CHAR_H,UI_DIM,UI_INPUT);char prompt[121];uint32_t p=0;const char*pre="lion> ";while(pre[p]&&p<110u){prompt[p]=pre[p];++p;}for(uint32_t i=0;i<term_len&&p<119u;++i)prompt[p++]=term_input[i];prompt[p]=0;ui_fill(x,y+23u*CHAR_H,w->w-48u,CHAR_H+4u,UI_INPUT);ui_text_bg(prompt,x,y+23u*CHAR_H,UI_TEXT,UI_INPUT);}
static void draw_about_window(const struct ui_window*w){draw_window(w,"About LionOS");ui_text("LionOS",w->x+36u,w->y+76u,UI_YELLOW);ui_text("32-BIT X86 EXPERIMENTAL OPERATING SYSTEM",w->x+36u,w->y+110u,UI_TEXT);ui_text("SMP  MEMORY  PROCESSES  VFS  ELF32",w->x+36u,w->y+144u,UI_DIM);ui_text("FRAMEBUFFER  MOUSE  KEYBOARD  GUI",w->x+36u,w->y+178u,UI_BLUE2);ui_text("ESC closes this window",w->x+36u,w->y+220u,UI_DIM);}
static void draw_settings_window(const struct ui_window*w){draw_window(w,"Settings");ui_text("Personalization",w->x+32u,w->y+72u,UI_TEXT);ui_fill(w->x+32u,w->y+108u,280u,48u,UI_BLUE);ui_text("BLUE ACCENT",w->x+48u,w->y+123u,UI_TEXT);ui_fill(w->x+32u,w->y+170u,280u,48u,UI_SURFACE2);ui_text("DARK MODE",w->x+48u,w->y+185u,UI_TEXT);ui_text("Display",w->x+360u,w->y+72u,UI_TEXT);ui_text("Framebuffer desktop",w->x+360u,w->y+108u,UI_DIM);}
static void draw_taskbar(void){uint32_t w=framebuffer_width(),h=framebuffer_height(),y=h-TASKBAR_H;ui_fill(0u,y,w,TASKBAR_H,0x0A0F18u);ui_fill(0u,y,w,1u,UI_BORDER);ui_fill(12u,y+10u,START_W,44u,start_open?UI_BLUE2:UI_BLUE);ui_border(12u,y+10u,START_W,44u,UI_BORDER);ui_text("START",30u,y+23u,UI_TEXT);ui_fill(130u,y+10u,170u,44u,terminal_focus?UI_SURFACE2:UI_SURFACE);ui_border(130u,y+10u,170u,44u,UI_BORDER);ui_text("TERMINAL",148u,y+23u,UI_TEXT);ui_fill(310u,y+10u,140u,44u,UI_SURFACE);ui_border(310u,y+10u,140u,44u,UI_BORDER);ui_text("ABOUT",332u,y+23u,UI_TEXT);ui_fill(460u,y+10u,160u,44u,UI_SURFACE);ui_border(460u,y+10u,160u,44u,UI_BORDER);ui_text("SETTINGS",480u,y+23u,UI_TEXT);ui_text("LionOS",w>760u?w-150u:640u,y+23u,UI_DIM);}
static void draw_start_menu(void){if(!start_open)return;uint32_t h=framebuffer_height(),y=h-TASKBAR_H-START_PANEL_H;ui_fill(18u,y,START_PANEL_W,START_PANEL_H,UI_SURFACE);ui_border(18u,y,START_PANEL_W,START_PANEL_H,UI_BORDER);ui_fill(18u,y,START_PANEL_W,72u,UI_SURFACE2);ui_text("LIONOS",42u,y+22u,UI_YELLOW);ui_text("PINNED",42u,y+92u,UI_DIM);ui_fill(42u,y+118u,320u,52u,UI_SURFACE2);ui_text("TERMINAL",60u,y+134u,UI_TEXT);ui_fill(42u,y+182u,320u,52u,UI_SURFACE2);ui_text("ABOUT",60u,y+198u,UI_TEXT);ui_fill(42u,y+246u,320u,52u,UI_SURFACE2);ui_text("SETTINGS",60u,y+262u,UI_TEXT);ui_fill(18u,y+START_PANEL_H-70u,START_PANEL_W,70u,0x0D1625u);ui_fill(42u,y+START_PANEL_H-58u,140u,40u,UI_RED);ui_text("POWER",62u,y+START_PANEL_H-45u,UI_TEXT);}
static void draw_desktop(void){uint32_t w=framebuffer_width(),h=framebuffer_height();ui_fill(0u,0u,w,h,UI_BG);ui_fill(0u,0u,w,TOPBAR_H,UI_TOP);ui_text("LionOS",24u,18u,UI_YELLOW);ui_text("Desktop",126u,18u,UI_TEXT);ui_text("Ready",w>900u?w-130u:680u,18u,UI_GREEN);ui_fill(44u,100u,68u,56u,UI_SURFACE2);ui_border(44u,100u,68u,56u,UI_BORDER);ui_text("PC",65u,120u,UI_BLUE2);ui_text("This PC",38u,166u,UI_TEXT);ui_fill(156u,100u,68u,56u,UI_SURFACE2);ui_border(156u,100u,68u,56u,UI_BORDER);ui_text(">_",177u,120u,UI_GREEN);ui_text("Terminal",150u,166u,UI_TEXT);ui_fill(270u,100u,68u,56u,UI_SURFACE2);ui_border(270u,100u,68u,56u,UI_BORDER);ui_text("i",293u,120u,UI_YELLOW);ui_text("About",282u,166u,UI_TEXT);ui_fill(470u,100u,720u,280u,UI_SURFACE);ui_border(470u,100u,720u,280u,UI_BORDER);ui_text("Welcome to LionOS",510u,136u,UI_TEXT);ui_text("A Windows-style graphical desktop for the",510u,174u,UI_DIM);ui_text("LionOS framebuffer, mouse and keyboard.",510u,198u,UI_DIM);ui_fill(510u,244u,220u,54u,0x102846u);ui_text("SYSTEM ONLINE",528u,261u,UI_GREEN);ui_fill(750u,244u,220u,54u,0x171F31u);ui_text("SMP ONLINE",768u,261u,UI_BLUE2);ui_fill(990u,244u,160u,54u,0x281F10u);ui_text("GUI READY",1008u,261u,UI_YELLOW);draw_taskbar();draw_start_menu();}
static void draw_cursor(void){uint32_t x=mouse_px_x,y=mouse_px_y;ui_fill(x,y,3u,22u,UI_WHITE);ui_fill(x,y,12u,3u,UI_WHITE);ui_fill(x+5u,y+16u,7u,3u,UI_WHITE);ui_fill(x+7u,y+12u,4u,4u,UI_WHITE);}
static void render_all(void){draw_desktop();for(uint32_t i=0;i<WIN_MAX;++i){if(!windows[i].visible||windows[i].minimized)continue;if(windows[i].id==WIN_TERMINAL)draw_terminal_window(&windows[i]);else if(windows[i].id==WIN_ABOUT)draw_about_window(&windows[i]);else draw_settings_window(&windows[i]);}draw_taskbar();draw_cursor();}
static void init_windows(void){windows[0]=(struct ui_window){WIN_TERMINAL,0u,0u,0u,360u,150u,1120u,650u};windows[1]=(struct ui_window){WIN_ABOUT,0u,0u,0u,560u,210u,760u,460u};windows[2]=(struct ui_window){WIN_SETTINGS,0u,0u,0u,520u,190u,900u,520u};terminal_init();start_open=0u;drag_active=0u;terminal_focus=0u;}
static void gui_leave(void){gui_active=0u;mouse_set_cursor_visible(1u);mouse_show();debug_write("LIONOS:GUI-EXIT\n");}
static void handle_window_click(struct ui_window*w){uint32_t mx=mouse_px_x,my=mouse_px_y;if(my<w->y+44u&&mx>=w->x&&mx<w->x+w->w){if(mx>=w->x+w->w-52u&&mx<w->x+w->w-22u){min_window(w->id);terminal_focus=(w->id==WIN_TERMINAL)?0u:terminal_focus;return;}if(mx>=w->x+w->w-22u){hide_window(w->id);terminal_focus=(w->id==WIN_TERMINAL)?0u:terminal_focus;return;}drag_active=1u;drag_id=w->id;drag_dx=(int)mx-(int)w->x;drag_dy=(int)my-(int)w->y;focus_window(w->id);return;}focus_window(w->id);}
static void handle_click(void){uint32_t x=mouse_px_x,y=mouse_px_y,h=framebuffer_height();if(y>=h-TASKBAR_H){if(x>=12u&&x<12u+START_W){start_open=!start_open;return;}if(x>=130u&&x<300u){show_window(WIN_TERMINAL);terminal_focus=1u;return;}if(x>=310u&&x<450u){show_window(WIN_ABOUT);return;}if(x>=460u&&x<620u){show_window(WIN_SETTINGS);return;}}if(start_open){uint32_t sy=h-TASKBAR_H-START_PANEL_H;if(x>=42u&&x<362u&&y>=sy+118u&&y<sy+170u){show_window(WIN_TERMINAL);terminal_focus=1u;terminal_init();return;}if(x>=42u&&x<362u&&y>=sy+182u&&y<sy+234u){show_window(WIN_ABOUT);return;}if(x>=42u&&x<362u&&y>=sy+246u&&y<sy+298u){show_window(WIN_SETTINGS);return;}if(x>=42u&&x<182u&&y>=sy+START_PANEL_H-58u&&y<sy+START_PANEL_H-18u){gui_leave();return;}start_open=0u;}
    struct ui_window*w=0;for(int i=(int)WIN_MAX-1;i>=0;--i){struct ui_window*t=&windows[i];if(t->visible&&!t->minimized&&x>=t->x&&x<t->x+t->w&&y>=t->y&&y<t->y+t->h){w=t;break;}}if(w){handle_window_click(w);return;}if(x>=38u&&x<140u&&y>=94u&&y<182u){show_window(WIN_TERMINAL);terminal_focus=1u;terminal_init();return;}if(x>=150u&&x<250u&&y>=94u&&y<182u){show_window(WIN_ABOUT);return;}}
static void handle_move(void){if(!drag_active)return;struct ui_window*w=win(drag_id);if(!w||!w->visible){drag_active=0u;return;}int nx=(int)mouse_px_x-drag_dx,ny=(int)mouse_px_y-drag_dy;if(nx<8)nx=8;if(ny<(int)TOPBAR_H)ny=TOPBAR_H;if(nx+(int)w->w>(int)framebuffer_width()-8)nx=(int)framebuffer_width()-(int)w->w-8;if(ny+(int)w->h>(int)framebuffer_height()-(int)TASKBAR_H)ny=(int)framebuffer_height()-(int)TASKBAR_H-(int)w->h;w->x=(uint32_t)nx;w->y=(uint32_t)ny;}
static void handle_key(int key){if(key==27){if(terminal_focus&&win(WIN_TERMINAL)&&win(WIN_TERMINAL)->visible){hide_window(WIN_TERMINAL);terminal_focus=0u;return;}for(uint32_t i=0;i<WIN_MAX;++i)if(windows[i].focused){hide_window(windows[i].id);return;}gui_leave();return;}if(terminal_focus){if(key=='\n'||key==13){term_command();return;}if(key=='\b'||key==127){if(term_len){--term_len;term_input[term_len]=0;}return;}if(key>=32&&key<127&&term_len<120u){term_input[term_len++]=(char)key;term_input[term_len]=0;}return;}if(key=='a'||key=='A'){show_window(WIN_ABOUT);return;}if(key=='t'||key=='T'){show_window(WIN_TERMINAL);terminal_focus=1u;return;}}
void gui_start(void){debug_write("LIONOS:GUI-ENTER\n");if(!framebuffer_available()){debug_write("LIONOS:GUI-NO-FRAMEBUFFER\n");return;}mouse_set_cursor_visible(0u);while(keyboard_available())(void)keyboard_getchar();init_windows();mouse_px_x=mx();mouse_px_y=my();ui_active=1u;previous_buttons=mouse_buttons();render_all();}
void gui_step(void){if(!ui_active)return;keyboard_poll();mouse_poll();mouse_px_x=mx();mouse_px_y=my();uint32_t buttons=mouse_buttons();if((buttons&1u)&&!(previous_buttons&1u))handle_click();if(!(buttons&1u)&&(previous_buttons&1u))drag_active=0u;handle_move();while(keyboard_available())handle_key(keyboard_getchar());previous_buttons=buttons;render_all();}
int gui_is_active(void){return ui_active!=0u;}
void gui_run(void){gui_start();}
