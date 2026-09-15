#define gui_run gui_run_legacy
#include "gui.c"
#undef gui_run
#include "vfs.h"
#include "memory.h"

#define UI_BG 0x0B1220u
#define UI_SURFACE 0x111A2Au
#define UI_SURFACE2 0x17233Au
#define UI_BORDER 0x2A3B58u
#define UI_TEXT 0xEEF4FCu
#define UI_DIM 0x8EA3BFu
#define UI_BLUE 0x2F80EDu
#define UI_BLUE2 0x56B4FFu
#define UI_GREEN 0x55D187u
#define UI_YELLOW 0xF2C14Eu
#define UI_RED 0xD95368u
#define UI_WHITE 0xFFFFFFu
#define TOPBAR_H 56u
#define TASKBAR_H 64u
#define TITLE_H 40u
#define START_W 92u
#define START_PANEL_W 420u
#define START_PANEL_H 520u
#define W_TERMINAL 1u
#define W_ABOUT 2u
#define W_SETTINGS 3u
#define W_MAX 3u

struct desktop_window { uint8_t id,visible,minimized,focused; uint32_t x,y,w,h; };
static struct desktop_window dw[W_MAX];
static uint32_t ui_mouse_x,ui_mouse_y,ui_prev_buttons;
static uint8_t ui_started,ui_active,ui_start_open,ui_dragging,ui_drag_window,ui_terminal_focus;
static int ui_drag_dx,ui_drag_dy;
static char term_input[121];
static uint32_t term_input_len,term_count;
static char term_lines[28][121];

static uint32_t ui_w(void){return framebuffer_width();}
static uint32_t ui_h(void){return framebuffer_height();}
static void ui_fill(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c){framebuffer_fill_rect(x,y,w,h,c);}
static void ui_border(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c){ui_fill(x,y,w,1u,c);ui_fill(x,y+h-1u,w,1u,c);ui_fill(x,y,1u,h,c);ui_fill(x+w-1u,y,1u,h,c);}
static void ui_text(const char*s,uint32_t x,uint32_t y,uint32_t c){while(*s&&x+12u<ui_w()){if(*s!=' ')draw_char(*s,x,y+2u,c);x+=12u;++s;}}
static void ui_button(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t fill,const char*s){ui_fill(x,y,w,h,fill);ui_border(x,y,w,h,UI_BORDER);ui_text(s,x+12u,y+10u,UI_TEXT);}
static struct desktop_window*dw_by_id(uint8_t id){for(uint32_t i=0;i<W_MAX;++i)if(dw[i].id==id)return &dw[i];return 0;}
static struct desktop_window*dw_top_at(uint32_t x,uint32_t y){for(int i=(int)W_MAX-1;i>=0;--i){struct desktop_window*w=&dw[i];if(!w->visible||w->minimized)continue;if(x>=w->x&&x<w->x+w->w&&y>=w->y&&y<w->y+w->h)return w;}return 0;}
static void dw_focus(uint8_t id){for(uint32_t i=0;i<W_MAX;++i)dw[i].focused=(dw[i].id==id&&dw[i].visible&&!dw[i].minimized)?1u:0u;}
static void dw_show(uint8_t id){struct desktop_window*w=dw_by_id(id);if(!w)return;w->visible=1u;w->minimized=0u;dw_focus(id);}
static void dw_hide(uint8_t id){struct desktop_window*w=dw_by_id(id);if(!w)return;w->visible=0u;w->minimized=0u;w->focused=0u;ui_dragging=0u;}
static void dw_min(uint8_t id){struct desktop_window*w=dw_by_id(id);if(!w)return;w->minimized=1u;w->focused=0u;ui_dragging=0u;}

static void term_clear(void){term_count=0u;term_input_len=0u;term_input[0]=0;}
static void term_line(const char*s){uint32_t i=0;if(term_count<28u){while(s[i]&&i<120u){term_lines[term_count][i]=s[i];++i;}term_lines[term_count][i]=0;term_count++;return;}for(uint32_t r=1;r<28u;++r)for(uint32_t c=0;c<121u;++c)term_lines[r-1][c]=term_lines[r][c];while(s[i]&&i<120u){term_lines[27][i]=s[i];++i;}term_lines[27][i]=0;}
static void term_start(void){term_clear();term_line("LionOS Terminal");term_line("Windows-style graphical terminal");term_line("Type help for commands. ESC closes this window.");}
static void term_ls(void){uint32_t n=vfs_count();if(!n){term_line("(no files)");return;}for(uint32_t i=0;i<n&&term_count<27u;++i){const char*name=vfs_name(i);if(name)term_line(name);}}
static int streq(const char*a,const char*b){while(*a&&*a==*b){++a;++b;}return *a==*b;}
static void term_command(void){
    term_input[term_input_len]=0;if(!term_input_len)return;
    if(streq(term_input,"help"))term_line("help  ls  pwd  uname  version  about  mem  clear  exit");
    else if(streq(term_input,"ls"))term_ls();
    else if(streq(term_input,"pwd"))term_line("/");
    else if(streq(term_input,"uname"))term_line("LionOS 0.8 x86 i386");
    else if(streq(term_input,"version"))term_line("LionOS version 0.8");
    else if(streq(term_input,"about"))term_line("Experimental 32-bit OS with SMP, VFS, ELF32 and GUI.");
    else if(streq(term_input,"mem")){char b[48];uint32_t v=memory_free_pages(),p=0;char d[11];if(!v)term_line("free pages: 0");else{while(v&&p<10u){d[p++]=(char)('0'+v%10u);v/=10u;}uint32_t j=0;const char*h="free pages: ";while(h[j]){b[j]=h[j];++j;}while(p)b[j++]=d[--p];b[j]=0;term_line(b);}}
    else if(streq(term_input,"clear"))term_clear();
    else if(streq(term_input,"exit")){dw_hide(W_TERMINAL);ui_terminal_focus=0u;}
    else term_line("Unknown command. Type help.");
    term_input_len=0u;term_input[0]=0;
}

static void draw_terminal(const struct desktop_window*w){
    ui_fill(w->x,w->y,w->w,w->h,UI_SURFACE);ui_border(w->x,w->y,w->w,w->h,UI_BORDER);ui_fill(w->x,w->y,w->w,TITLE_H,UI_SURFACE2);
    ui_fill(w->x+16u,w->y+12u,14u,14u,UI_BLUE2);ui_text("LIONOS TERMINAL",w->x+40u,w->y+10u,UI_TEXT);
    ui_fill(w->x+w->w-126u,w->y+10u,26u,20u,UI_SURFACE);ui_fill(w->x+w->w-92u,w->y+10u,26u,20u,UI_YELLOW);ui_fill(w->x+w->w-58u,w->y+10u,26u,20u,UI_RED);
    uint32_t tx=w->x+20u,ty=w->y+TITLE_H+20u;for(uint32_t i=0;i<term_count&&i<28u;++i)ui_text(term_lines[i],tx,ty+i*18u,i==term_count-1u?UI_TEXT:UI_DIM);
    char prompt[121];uint32_t p=0;prompt[p++]='l';prompt[p++]='i';prompt[p++]='o';prompt[p++]='n';prompt[p++]='>';prompt[p++]=' ';for(uint32_t i=0;i<term_input_len&&p<119u;++i)prompt[p++]=term_input[i];prompt[p]=0;
    ui_fill(tx,ty+28u*18u,1160u,22u,0x0A101Cu);ui_text(prompt,tx,ty+28u*18u,UI_TEXT);
}
static void draw_about(const struct desktop_window*w){ui_fill(w->x,w->y,w->w,w->h,UI_SURFACE);ui_border(w->x,w->y,w->w,w->h,UI_BORDER);ui_fill(w->x,w->y,w->w,TITLE_H,UI_SURFACE2);ui_text("ABOUT LIONOS",w->x+18u,w->y+10u,UI_TEXT);ui_text("LionOS",w->x+40u,w->y+80u,UI_YELLOW);ui_text("32-BIT X86 EXPERIMENTAL OPERATING SYSTEM",w->x+40u,w->y+120u,UI_TEXT);ui_text("SMP  MEMORY  PROCESSES  VFS  ELF32",w->x+40u,w->y+150u,UI_DIM);ui_text("FRAMEBUFFER DESKTOP / MOUSE / KEYBOARD",w->x+40u,w->y+180u,UI_BLUE2);ui_text("ESC closes this window",w->x+40u,w->y+230u,UI_DIM);}
static void draw_settings(const struct desktop_window*w){ui_fill(w->x,w->y,w->w,w->h,UI_SURFACE);ui_border(w->x,w->y,w->w,w->h,UI_BORDER);ui_fill(w->x,w->y,w->w,TITLE_H,UI_SURFACE2);ui_text("SETTINGS",w->x+18u,w->y+10u,UI_TEXT);ui_text("Personalization",w->x+26u,w->y+70u,UI_TEXT);ui_button(w->x+26u,w->y+110u,260u,44u,UI_BLUE,"BLUE ACCENT");ui_button(w->x+26u,w->y+170u,260u,44u,UI_SURFACE2,"DARK MODE");ui_text("Display",w->x+360u,w->y+70u,UI_TEXT);ui_text("Framebuffer desktop",w->x+360u,w->y+110u,UI_DIM);}
static void draw_cursor(uint32_t x,uint32_t y){ui_fill(x,y,3u,22u,UI_WHITE);ui_fill(x,y,12u,3u,UI_WHITE);ui_fill(x+5u,y+16u,7u,3u,UI_WHITE);ui_fill(x+7u,y+12u,4u,4u,UI_WHITE);}
static void draw_taskbar(void){uint32_t h=ui_h(),w=ui_w(),y=h-TASKBAR_H;ui_fill(0u,y,w,TASKBAR_H,0x0A0F18u);ui_fill(0u,y,w,1u,UI_BORDER);ui_button(10u,y+10u,START_W,44u,UI_BLUE,"START");ui_button(118u,y+10u,164u,44u,ui_terminal_focus?UI_SURFACE2:UI_SURFACE,"TERMINAL");ui_button(290u,y+10u,130u,44u,UI_SURFACE,"ABOUT");ui_button(428u,y+10u,150u,44u,UI_SURFACE,"SETTINGS");ui_text("LionOS",w>700u?w-180u:600u,y+19u,UI_DIM);}
static void draw_start_menu(void){if(!ui_start_open)return;uint32_t y=ui_h()-TASKBAR_H-START_PANEL_H;ui_fill(18u,y,START_PANEL_W,START_PANEL_H,UI_SURFACE);ui_border(18u,y,START_PANEL_W,START_PANEL_H,UI_BORDER);ui_fill(18u,y,START_PANEL_W,76u,UI_SURFACE2);ui_text("LIONOS",44u,y+22u,UI_YELLOW);ui_text("PINNED",44u,y+96u,UI_DIM);ui_button(44u,y+126u,320u,54u,UI_SURFACE2,"TERMINAL");ui_button(44u,y+190u,320u,54u,UI_SURFACE2,"ABOUT");ui_button(44u,y+254u,320u,54u,UI_SURFACE2,"SETTINGS");ui_fill(18u,y+START_PANEL_H-70u,START_PANEL_W,70u,0x0D1625u);ui_button(44u,y+START_PANEL_H-58u,140u,40u,UI_RED,"POWER");}
static void draw_desktop_v2(void){uint32_t w=ui_w(),h=ui_h();ui_fill(0u,0u,w,h,UI_BG);ui_fill(0u,0u,w,TOPBAR_H,UI_SURFACE2);ui_text("LionOS",24u,18u,UI_YELLOW);ui_text("Desktop",140u,18u,UI_TEXT);ui_text("Ready",w>900u?w-190u:700u,18u,UI_GREEN);ui_fill(42u,96u,62u,52u,UI_SURFACE2);ui_border(42u,96u,62u,52u,UI_BORDER);ui_text("PC",60u,112u,UI_BLUE2);ui_text("This PC",38u,158u,UI_TEXT);ui_fill(150u,96u,62u,52u,UI_SURFACE2);ui_border(150u,96u,62u,52u,UI_BORDER);ui_text(">_",168u,112u,UI_GREEN);ui_text("Terminal",142u,158u,UI_TEXT);ui_fill(258u,96u,62u,52u,UI_SURFACE2);ui_border(258u,96u,62u,52u,UI_BORDER);ui_text("i",286u,112u,UI_YELLOW);ui_text("About",264u,158u,UI_TEXT);ui_fill(460u,100u,720u,260u,UI_SURFACE);ui_border(460u,100u,720u,260u,UI_BORDER);ui_text("Welcome to LionOS",500u,132u,UI_TEXT);ui_text("A real framebuffer desktop with windows, taskbar,",500u,174u,UI_DIM);ui_text("mouse input and an event-driven terminal.",500u,196u,UI_DIM);ui_fill(500u,238u,230u,54u,0x102846u);ui_text("SYSTEM ONLINE",520u,256u,UI_GREEN);ui_fill(748u,238u,230u,54u,0x171F31u);ui_text("SMP ONLINE",768u,256u,UI_BLUE2);ui_fill(996u,238u,150u,54u,0x281F10u);ui_text("GUI READY",1018u,256u,UI_YELLOW);draw_taskbar();draw_start_menu();draw_cursor(ui_mouse_x,ui_mouse_y);}
static void draw_windows(void){for(uint32_t i=0;i<W_MAX;++i)if(dw[i].visible&&!dw[i].minimized){if(dw[i].id==W_TERMINAL)draw_terminal(&dw[i]);else if(dw[i].id==W_ABOUT)draw_about(&dw[i]);else draw_settings(&dw[i]);}}
static void ui_render(void){draw_desktop_v2();draw_windows();}
static void ui_init_windows(void){dw[0]=(struct desktop_window){W_TERMINAL,0u,0u,0u,520u,170u,1180u,620u};dw[1]=(struct desktop_window){W_ABOUT,0u,0u,0u,620u,210u,760u,460u};dw[2]=(struct desktop_window){W_SETTINGS,0u,0u,0u,560u,190u,920u,520u};term_start();ui_terminal_focus=0u;ui_start_open=0u;ui_dragging=0u;}
static void ui_power_to_shell(void){ui_active=0u;mouse_set_cursor_visible(1u);mouse_show();debug_write("LIONOS:GUI-EXIT\n");}
static void ui_handle_click(uint32_t x,uint32_t y){uint32_t h=ui_h();if(ui_start_open){uint32_t sy=h-TASKBAR_H-START_PANEL_H;if(x>=44u&&x<364u&&y>=sy+126u&&y<sy+180u){ui_start_open=0u;dw_show(W_TERMINAL);ui_terminal_focus=1u;term_start();return;}if(x>=44u&&x<364u&&y>=sy+190u&&y<sy+244u){ui_start_open=0u;dw_show(W_ABOUT);return;}if(x>=44u&&x<364u&&y>=sy+254u&&y<sy+308u){ui_start_open=0u;dw_show(W_SETTINGS);return;}if(x>=44u&&x<184u&&y>=sy+START_PANEL_H-58u&&y<sy+START_PANEL_H-18u){ui_start_open=0u;ui_power_to_shell();return;}ui_start_open=0u;return;}if(y>=h-TASKBAR_H){uint32_t by=y-(h-TASKBAR_H);if(x<102u&&by>=10u){ui_start_open=1u;return;}if(x>=118u&&x<282u&&by>=10u){dw_show(W_TERMINAL);ui_terminal_focus=1u;return;}if(x>=290u&&x<420u&&by>=10u){dw_show(W_ABOUT);return;}if(x>=428u&&x<578u&&by>=10u){dw_show(W_SETTINGS);return;}return;}if(x>=42u&&x<104u&&y>=96u&&y<148u)return;if(x>=150u&&x<212u&&y>=96u&&y<148u){dw_show(W_TERMINAL);ui_terminal_focus=1u;return;}if(x>=258u&&x<320u&&y>=96u&&y<148u){dw_show(W_ABOUT);return;}struct desktop_window*w=dw_top_at(x,y);if(!w)return;dw_focus(w->id);uint32_t relx=x-w->x,rely=y-w->y;if(rely<TITLE_H){if(relx>w->w-76u&&relx<w->w-48u){dw_min(w->id);ui_terminal_focus=0u;return;}if(relx>w->w-42u){dw_hide(w->id);if(w->id==W_TERMINAL)ui_terminal_focus=0u;return;}ui_dragging=1u;ui_drag_window=w->id;ui_drag_dx=(int)x-(int)w->x;ui_drag_dy=(int)y-(int)w->y;return;}if(w->id==W_TERMINAL)ui_terminal_focus=1u;}
static void ui_mouse_move(void){if(!ui_dragging)return;struct desktop_window*w=dw_by_id(ui_drag_window);if(!w)return;int nx=(int)ui_mouse_x-ui_drag_dx,ny=(int)ui_mouse_y-ui_drag_dy;if(nx<20)nx=20;if(ny<(int)TOPBAR_H)ny=TOPBAR_H;if(nx+(int)w->w>(int)ui_w())nx=(int)ui_w()-(int)w->w;if(ny+(int)w->h>(int)ui_h()-(int)TASKBAR_H)ny=(int)ui_h()-(int)TASKBAR_H-(int)w->h;w->x=(uint32_t)nx;w->y=(uint32_t)ny;}
static void ui_handle_key(int c){if(c==27){if(ui_start_open){ui_start_open=0u;return;}if(ui_terminal_focus){dw_hide(W_TERMINAL);ui_terminal_focus=0u;return;}for(uint32_t i=0;i<W_MAX;++i)if(dw[i].focused){dw_hide(dw[i].id);return;}return;}if(c=='t'||c=='T'){dw_show(W_TERMINAL);ui_terminal_focus=1u;return;}if(c=='a'||c=='A'){dw_show(W_ABOUT);return;}if(c=='s'||c=='S'){dw_show(W_SETTINGS);return;}if(ui_terminal_focus){if(c=='\b'){if(term_input_len)term_input[--term_input_len]=0;return;}if(c==13){term_command();return;}if(c>=32&&c<127&&term_input_len<120u){term_input[term_input_len++]=(char)c;term_input[term_input_len]=0;}}
}
int gui_is_active(void){return ui_active!=0u;}
void gui_start(void){ui_started=0u;ui_active=0u;ui_terminal_focus=0u;debug_write("LIONOS:GUI-ENTER\n");if(!framebuffer_available()){debug_write("LIONOS:GUI-NO-FRAMEBUFFER\n");return;}mouse_set_cursor_visible(0u);while(keyboard_available())(void)keyboard_getchar();ui_init_windows();ui_mouse_x=mouse_x();ui_mouse_y=mouse_y();if(ui_mouse_x>=ui_w())ui_mouse_x=ui_w()-1u;if(ui_mouse_y>=ui_h())ui_mouse_y=ui_h()-1u;ui_prev_buttons=mouse_buttons();ui_started=1u;ui_active=1u;ui_render();}
void gui_step(void){if(!ui_started||!ui_active)return;keyboard_poll();mouse_poll();ui_mouse_x=mouse_x();ui_mouse_y=mouse_y();if(ui_mouse_x>=ui_w())ui_mouse_x=ui_w()-1u;if(ui_mouse_y>=ui_h())ui_mouse_y=ui_h()-1u;uint32_t buttons=mouse_buttons();if(buttons!=ui_prev_buttons){if((buttons&1u)&&!(ui_prev_buttons&1u))ui_handle_click(ui_mouse_x,ui_mouse_y);if(!(buttons&1u)&&(ui_prev_buttons&1u))ui_dragging=0u;ui_prev_buttons=buttons;}ui_mouse_move();while(keyboard_available())ui_handle_key(keyboard_getchar());ui_render();}
