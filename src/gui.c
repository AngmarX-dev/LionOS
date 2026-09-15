#include <stdint.h>
#include "debug.h"
#include "framebuffer.h"
#include "console.h"
#include "gui.h"
#include "keyboard.h"
#include "mouse.h"

#define GRID_W 80u
#define GRID_H 48u
#define CELL_W 12u
#define CELL_H 16u
#define GRID_X 32u
#define GRID_Y 12u

#define C_BG 0x07101Cu
#define C_TOP 0x0D1B2Cu
#define C_SIDE 0x0A1625u
#define C_CARD 0x102238u
#define C_CARD2 0x142A45u
#define C_BORDER 0x284D73u
#define C_ACCENT 0xF2C14Eu
#define C_ACCENT2 0x56B4FFu
#define C_TEXT 0xEEF4FCu
#define C_DIM 0x8FA7C2u
#define C_GOOD 0x55D187u
#define C_DANGER 0x5B2A34u
#define C_CURSOR 0xFFFFFFu

#define GUI_EVENT_QUEUE 32u
#define GUI_WINDOW_MAX 3u
#define WIN_DESKTOP 0u
#define WIN_TERMINAL 1u
#define WIN_ABOUT 2u

#define GUI_EVENT_MOUSE_MOVE 1u
#define GUI_EVENT_MOUSE_PRESS 2u
#define GUI_EVENT_MOUSE_RELEASE 3u
#define GUI_EVENT_KEY 4u

#define TASKBAR_Y 44u
#define TASKBAR_H 4u
#define TITLE_H 2u

struct gui_event { uint8_t type; uint8_t button; int key; uint32_t x; uint32_t y; };
struct gui_window { uint8_t id, visible, minimized, modal, focused; uint32_t x,y,w,h; };

static struct gui_event event_queue[GUI_EVENT_QUEUE];
static uint32_t event_read,event_write;
static struct gui_window windows[GUI_WINDOW_MAX];
static uint32_t focused_window,dirty;
static uint8_t drag_active,drag_window;
static int drag_offset_x,drag_offset_y;

static const uint8_t font[26][7]={
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
{0x11,0x11,0x0A,0x04,0x04,0x04,0x04},{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}};

static void invalidate(void){dirty=1u;}

static void draw_char(char c,uint32_t x,uint32_t y,uint32_t color){
    if(c>='a'&&c<='z')c=(char)(c-'a'+'A');
    if(c<'A'||c>'Z')return;
    const uint8_t*r=font[(uint32_t)(c-'A')];
    for(uint32_t gy=0;gy<7u;++gy)
        for(uint32_t gx=0;gx<5u;++gx)
            if(r[gy]&(1u<<(4u-gx)))framebuffer_fill_rect(x+gx*2u,y+gy*2u,2u,2u,color);
}

static void draw_text(const char*t,uint32_t col,uint32_t row,uint32_t color){
    uint32_t x=GRID_X+col*CELL_W,y=GRID_Y+row*CELL_H;
    while(*t&&col<GRID_W){if(*t!=' ')draw_char(*t,x,y+1u,color);x+=CELL_W;++col;++t;}
}

static void panel(uint32_t col,uint32_t row,uint32_t width,uint32_t height,uint32_t fill){
    uint32_t x=GRID_X+col*CELL_W,y=GRID_Y+row*CELL_H;
    framebuffer_fill_rect(x,y,width*CELL_W,height*CELL_H,C_BORDER);
    framebuffer_fill_rect(x+2u,y+2u,width*CELL_W-4u,height*CELL_H-4u,fill);
}

static void button(uint32_t col,uint32_t row,uint32_t width,uint32_t height,uint32_t fill,const char*label){
    uint32_t x=GRID_X+col*CELL_W,y=GRID_Y+row*CELL_H;
    framebuffer_fill_rect(x,y,width*CELL_W,height*CELL_H,C_BORDER);
    framebuffer_fill_rect(x+2u,y+2u,width*CELL_W-4u,height*CELL_H-4u,fill);
    uint32_t len=0u;while(label[len])++len;
    draw_text(label,col+(len<width?(width-len)/2u:0u),row+1u,C_TEXT);
}

static void draw_cursor(uint32_t mx,uint32_t my){
    uint32_t x=mx*CELL_W+4u,y=my*CELL_H+4u;
    framebuffer_fill_rect(x,y,3u,18u,C_CURSOR);
    framebuffer_fill_rect(x,y,10u,3u,C_CURSOR);
    framebuffer_fill_rect(x+4u,y+14u,6u,3u,C_CURSOR);
}

static void queue_event(uint8_t type,uint8_t button_value,int key,uint32_t x,uint32_t y){
    uint32_t next=(event_write+1u)%GUI_EVENT_QUEUE;
    if(next==event_read)return;
    event_queue[event_write].type=type;
    event_queue[event_write].button=button_value;
    event_queue[event_write].key=key;
    event_queue[event_write].x=x;
    event_queue[event_write].y=y;
    event_write=next;
}

static int next_event(struct gui_event*event){
    if(event_read==event_write)return 0;
    *event=event_queue[event_read];
    event_read=(event_read+1u)%GUI_EVENT_QUEUE;
    return 1;
}

static struct gui_window*window_by_id(uint8_t id){
    for(uint32_t i=0;i<GUI_WINDOW_MAX;++i)if(windows[i].id==id)return &windows[i];
    return 0;
}

static void wm_raise(uint8_t id){
    if(id==WIN_DESKTOP)return;
    int found=-1;
    for(uint32_t i=1u;i<GUI_WINDOW_MAX;++i)if(windows[i].id==id)found=(int)i;
    if(found<0||found==(int)GUI_WINDOW_MAX-1)return;
    struct gui_window tmp=windows[found];
    for(uint32_t i=(uint32_t)found;i+1u<GUI_WINDOW_MAX;++i)windows[i]=windows[i+1u];
    windows[GUI_WINDOW_MAX-1u]=tmp;
}

static void wm_focus(uint8_t id){
    wm_raise(id);
    for(uint32_t i=0;i<GUI_WINDOW_MAX;++i)windows[i].focused=(windows[i].id==id)?1u:0u;
    focused_window=id;
    invalidate();
}

static void wm_focus_next(void){
    for(int i=(int)GUI_WINDOW_MAX-1;i>=1;--i)
        if(windows[i].visible&&!windows[i].minimized){wm_focus(windows[i].id);return;}
    wm_focus(WIN_DESKTOP);
}

static struct gui_window*wm_top_at(uint32_t x,uint32_t y){
    for(int i=(int)GUI_WINDOW_MAX-1;i>=0;--i){
        struct gui_window*w=&windows[i];
        if(!w->visible||w->minimized)continue;
        if(x>=GRID_X+w->x*CELL_W&&x<GRID_X+(w->x+w->w)*CELL_W&&
           y>=GRID_Y+w->y*CELL_H&&y<GRID_Y+(w->y+w->h)*CELL_H)return w;
    }
    return 0;
}

static void wm_show(uint8_t id){
    struct gui_window*w=window_by_id(id);if(!w)return;
    w->visible=1u;w->minimized=0u;w->modal=(id==WIN_ABOUT)?1u:0u;wm_focus(id);
}

static void wm_hide(uint8_t id){
    struct gui_window*w=window_by_id(id);if(!w)return;
    w->visible=0u;w->minimized=0u;w->modal=0u;drag_active=0u;
    if(focused_window==id)wm_focus_next();else invalidate();
}

static void wm_minimize(uint8_t id){
    struct gui_window*w=window_by_id(id);if(!w||id==WIN_DESKTOP||!w->visible)return;
    w->minimized=1u;w->focused=0u;drag_active=0u;wm_focus_next();
}

static void wm_init(void){
    event_read=event_write=0u;drag_active=0u;focused_window=WIN_DESKTOP;
    windows[0]=(struct gui_window){WIN_DESKTOP,1u,0u,0u,1u,0u,GRID_W,GRID_H};
    windows[1]=(struct gui_window){WIN_TERMINAL,0u,0u,0u,0u,16u,8u,52u,24u};
    windows[2]=(struct gui_window){WIN_ABOUT,0u,0u,1u,0u,9u,6u,60u,24u};
    windows[0].focused=1u;dirty=1u;
}

static void draw_window_shell(const struct gui_window*w,const char*title,uint32_t fill){
    uint32_t x=GRID_X+w->x*CELL_W,y=GRID_Y+w->y*CELL_H;
    uint32_t width=w->w*CELL_W,height=w->h*CELL_H;
    framebuffer_fill_rect(x,y,width,height,C_BORDER);
    framebuffer_fill_rect(x+3u,y+3u,width-6u,height-6u,fill);
    framebuffer_fill_rect(x+3u,y+3u,width-6u,28u,w->focused?C_CARD2:C_CARD);
    draw_text(title,w->x+2u,w->y+1u,C_TEXT);
    framebuffer_fill_rect(x+width-54u,y+7u,12u,12u,0xF4A261u);
    framebuffer_fill_rect(x+width-36u,y+7u,12u,12u,C_GOOD);
    framebuffer_fill_rect(x+width-18u,y+7u,12u,12u,C_DANGER);
}

static void draw_terminal_window(void){
    struct gui_window*w=window_by_id(WIN_TERMINAL);if(!w||!w->visible||w->minimized)return;
    draw_window_shell(w,"TERMINAL",C_TOP);
    draw_text("LIONOS TERMINAL WINDOW",w->x+3u,w->y+3u,C_ACCENT);
    draw_text("DRAG TITLE BAR TO MOVE",w->x+3u,w->y+6u,C_TEXT);
    draw_text("EVENTS ROUTED THROUGH WM",w->x+3u,w->y+8u,C_DIM);
    draw_text("T OR ENTER OPEN REAL SHELL",w->x+3u,w->y+11u,C_ACCENT2);
    draw_text("ESC CLOSE",w->x+3u,w->y+14u,C_DIM);
}

static void draw_about_window(void){
    struct gui_window*w=window_by_id(WIN_ABOUT);if(!w||!w->visible||w->minimized)return;
    draw_window_shell(w,"ABOUT LIONOS",C_TOP);
    draw_text("THIRTY TWO BIT EXPERIMENTAL OS",w->x+4u,w->y+5u,C_TEXT);
    draw_text("MULTIBOOT GRUB BOOT",w->x+4u,w->y+8u,C_DIM);
    draw_text("SMP MEMORY PROCESS VFS",w->x+4u,w->y+10u,C_DIM);
    draw_text("MOUSE FRAMEBUFFER GUI",w->x+4u,w->y+12u,C_DIM);
    draw_text("WINDOWS EVENTS FOCUS",w->x+4u,w->y+14u,C_GOOD);
    draw_text("ESC OR CLOSE BUTTON",w->x+4u,w->y+17u,C_ACCENT2);
}

static void draw_taskbar(void){
    uint32_t width=framebuffer_width(),height=framebuffer_height();
    uint32_t y=height-(TASKBAR_H*CELL_H);
    framebuffer_fill_rect(0u,y,width,TASKBAR_H*CELL_H,C_TOP);
    framebuffer_fill_rect(0u,y,width,2u,C_BORDER);
    button(1u,44u,10u,3u,focused_window==WIN_DESKTOP?C_CARD2:C_CARD,"LIONOS");
    struct gui_window*t=window_by_id(WIN_TERMINAL);
    struct gui_window*a=window_by_id(WIN_ABOUT);
    if(t&&t->visible)button(13u,44u,15u,3u,(t->minimized||focused_window!=WIN_TERMINAL)?C_CARD:C_CARD2,"TERMINAL");
    if(a&&a->visible)button(30u,44u,13u,3u,(a->minimized||focused_window!=WIN_ABOUT)?C_CARD:C_CARD2,"ABOUT");
    draw_text("TASKBAR",48u,45u,C_DIM);
}

static void draw_desktop(uint32_t mx,uint32_t my){
    uint32_t width=framebuffer_width(),height=framebuffer_height();
    framebuffer_clear(C_BG);
    framebuffer_fill_rect(0u,0u,width,52u,C_TOP);
    framebuffer_fill_rect(0u,52u,220u,height-(TASKBAR_H*CELL_H)-52u,C_SIDE);
    framebuffer_fill_rect(220u,52u,2u,height-(TASKBAR_H*CELL_H)-52u,C_BORDER);
    draw_text("LIONOS",2u,1u,C_ACCENT);
    draw_text("PHASE DESKTOP",64u,1u,C_DIM);
    panel(2u,5u,16u,28u,C_SIDE);
    draw_text("DESKTOP",4u,7u,C_DIM);
    button(4u,9u,12u,3u,C_CARD2,"HOME");
    button(4u,14u,12u,3u,focused_window==WIN_TERMINAL?C_CARD2:C_CARD,"TERMINAL");
    button(4u,19u,12u,3u,focused_window==WIN_ABOUT?C_CARD2:C_CARD,"ABOUT");
    button(4u,24u,12u,3u,C_DANGER,"EXIT");
    draw_text("LIONOS",5u,29u,C_ACCENT);
    draw_text("WELCOME BACK",20u,6u,C_TEXT);
    draw_text("WINDOWS EVENTS COMPOSITOR",20u,8u,C_DIM);
    panel(20u,11u,24u,13u,C_CARD);
    draw_text("SYSTEM STATUS",24u,13u,C_TEXT);
    draw_text("GRAPHICS ONLINE",22u,16u,C_GOOD);
    draw_text("SMP STORAGE ONLINE",22u,18u,C_DIM);
    draw_text("EVENT SYSTEM READY",22u,20u,C_ACCENT2);
    panel(46u,11u,25u,13u,C_CARD);
    draw_text("QUICK ACTIONS",49u,13u,C_TEXT);
    button(49u,16u,18u,3u,C_CARD2,"TERMINAL");
    button(49u,20u,18u,3u,C_CARD2,"ABOUT");
    panel(20u,26u,51u,9u,C_CARD);
    draw_text("LIONOS GUI",23u,28u,C_ACCENT);
    draw_text("EVENT QUEUE",23u,31u,C_TEXT);
    draw_text("WINDOW MANAGER",39u,28u,C_DIM);
    draw_text("TASKBAR MINIMIZE",39u,31u,C_GOOD);
    draw_text("DRAGGABLE WINDOWS",39u,33u,C_DIM);
    draw_text("READY",2u,42u,C_GOOD);
    draw_text("Q ESC CLOSE",19u,42u,C_DIM);
    draw_text("A ABOUT",38u,42u,C_DIM);
    draw_taskbar();
    draw_cursor(mx,my);
}

static void render(uint32_t mx,uint32_t my){
    draw_desktop(mx,my);
    for(uint32_t i=1u;i<GUI_WINDOW_MAX;++i){
        if(!windows[i].visible||windows[i].minimized)continue;
        if(windows[i].id==WIN_TERMINAL)draw_terminal_window();
        else if(windows[i].id==WIN_ABOUT)draw_about_window();
    }
    draw_taskbar();
    draw_cursor(mx,my);
}

static void return_shell(void){
    console_use_framebuffer();
    mouse_set_cursor_visible(1u);
    mouse_show();
    debug_write("LIONOS:GUI-TERMINAL\n");
    debug_write("LIONOS:GUI-EXIT\n");
}

static void poll_input(uint32_t mx,uint32_t my,uint32_t previous_buttons){
    keyboard_poll();mouse_poll();
    uint32_t nx=mouse_x(),ny=(mouse_y()*GRID_H)/25u;
    if(nx>=GRID_W)nx=GRID_W-1u;if(ny>=GRID_H)ny=GRID_H-1u;
    uint32_t buttons=mouse_buttons();
    if(nx!=mx||ny!=my)queue_event(GUI_EVENT_MOUSE_MOVE,0u,0,nx,ny);
    if((buttons&1u)&&!(previous_buttons&1u))queue_event(GUI_EVENT_MOUSE_PRESS,1u,0,nx,ny);
    if(!(buttons&1u)&&(previous_buttons&1u))queue_event(GUI_EVENT_MOUSE_RELEASE,1u,0,nx,ny);
    while(keyboard_available())queue_event(GUI_EVENT_KEY,0u,keyboard_getchar(),nx,ny);
}

static int handle_taskbar_click(uint32_t x,uint32_t y){
    if(y<TASKBAR_Y)return 0;
    if(x>=1u&&x<11u){wm_focus(WIN_DESKTOP);return 1;}
    if(x>=13u&&x<28u){struct gui_window*t=window_by_id(WIN_TERMINAL);if(t&&t->visible){if(t->minimized||focused_window!=WIN_TERMINAL)wm_show(WIN_TERMINAL);else wm_minimize(WIN_TERMINAL);}else wm_show(WIN_TERMINAL);return 1;}
    if(x>=30u&&x<43u){struct gui_window*a=window_by_id(WIN_ABOUT);if(a&&a->visible){if(a->minimized||focused_window!=WIN_ABOUT)wm_show(WIN_ABOUT);else wm_minimize(WIN_ABOUT);}else wm_show(WIN_ABOUT);return 1;}
    return 0;
}

static void handle_mouse_press(uint32_t x,uint32_t y){
    if(handle_taskbar_click(x,y))return;
    uint32_t px=GRID_X+x*CELL_W,py=GRID_Y+y*CELL_H;
    struct gui_window*top=wm_top_at(px,py);
    struct gui_window*about=window_by_id(WIN_ABOUT);
    if(about&&about->visible&&!about->minimized&&(!top||top->id!=WIN_ABOUT)){wm_hide(WIN_ABOUT);debug_write("LIONOS:GUI-ABOUT-CLOSE-OUTSIDE\n");return;}
    if(top){
        wm_focus(top->id);
        if(top->id!=WIN_DESKTOP){
            uint32_t relx=px-(GRID_X+top->x*CELL_W),rely=py-(GRID_Y+top->y*CELL_H);
            if(rely<TITLE_H*CELL_H){
                if(relx>=top->w*CELL_W-18u){debug_write(top->id==WIN_ABOUT?"LIONOS:GUI-ABOUT-CLOSE-MOUSE\n":"LIONOS:GUI-TERMINAL-CLOSE-MOUSE\n");wm_hide(top->id);return;}
                if(relx>=top->w*CELL_W-54u&&relx<top->w*CELL_W-36u){wm_minimize(top->id);return;}
                drag_active=1u;drag_window=top->id;drag_offset_x=(int)x-(int)top->x;drag_offset_y=(int)y-(int)top->y;
            }
            return;
        }
    }
    if(x>=4u&&x<16u&&y>=14u&&y<17u){wm_show(WIN_TERMINAL);debug_write("LIONOS:GUI-TERMINAL-OPEN\n");return;}
    if(x>=4u&&x<16u&&y>=19u&&y<22u){wm_show(WIN_ABOUT);debug_write("LIONOS:GUI-ABOUT-OPEN\n");return;}
    if(x>=4u&&x<16u&&y>=24u&&y<27u){return_shell();return;}
    if(x>=49u&&x<67u&&y>=16u&&y<19u){wm_show(WIN_TERMINAL);debug_write("LIONOS:GUI-TERMINAL-OPEN\n");return;}
    if(x>=49u&&x<67u&&y>=20u&&y<23u){wm_show(WIN_ABOUT);debug_write("LIONOS:GUI-ABOUT-OPEN\n");return;}
}

static void handle_mouse_move(uint32_t x,uint32_t y){
    if(!drag_active)return;
    struct gui_window*w=window_by_id(drag_window);if(!w||!w->visible){drag_active=0u;return;}
    int nx=(int)x-drag_offset_x,ny=(int)y-drag_offset_y;
    if(nx<19)nx=19;if(ny<1)ny=1;
    if(nx>(int)GRID_W-(int)w->w)nx=(int)GRID_W-(int)w->w;
    if(ny>(int)TASKBAR_Y-(int)w->h)ny=(int)TASKBAR_Y-(int)w->h;
    w->x=(uint32_t)nx;w->y=(uint32_t)ny;invalidate();
}

static void handle_key(int key){
    struct gui_window*top=window_by_id((uint8_t)focused_window);
    if(key==27||key=='q'||key=='Q'){if(top&&top->id!=WIN_DESKTOP)wm_hide(top->id);else return_shell();return;}
    if(key=='a'||key=='A'){struct gui_window*a=window_by_id(WIN_ABOUT);if(a&&a->visible&&!a->minimized)wm_hide(WIN_ABOUT);else wm_show(WIN_ABOUT);debug_write("LIONOS:GUI-ABOUT-KEY\n");return;}
    if(key=='m'||key=='M'){if(top&&top->id!=WIN_DESKTOP)wm_minimize(top->id);return;}
    if((key=='t'||key=='T'||key==13)&&focused_window==WIN_TERMINAL){return_shell();return;}
}

void gui_run(void){
    debug_write("LIONOS:GUI-ENTER\n");
    if(!framebuffer_available()){debug_write("LIONOS:GUI-NO-FRAMEBUFFER\n");return;}
    mouse_set_cursor_visible(0u);
    while(keyboard_available())(void)keyboard_getchar();
    wm_init();
    uint32_t mx=mouse_x(),my=(mouse_y()*GRID_H)/25u;
    if(mx>=GRID_W)mx=GRID_W-1u;if(my>=GRID_H)my=GRID_H-1u;
    uint32_t previous_buttons=mouse_buttons();
    render(mx,my);dirty=0u;
    for(;;){
        poll_input(mx,my,previous_buttons);previous_buttons=mouse_buttons();
        struct gui_event event;
        while(next_event(&event)){
            if(event.type==GUI_EVENT_MOUSE_MOVE){mx=event.x;my=event.y;handle_mouse_move(mx,my);}
            else if(event.type==GUI_EVENT_MOUSE_PRESS)handle_mouse_press(event.x,event.y);
            else if(event.type==GUI_EVENT_MOUSE_RELEASE)drag_active=0u;
            else if(event.type==GUI_EVENT_KEY)handle_key(event.key);
            invalidate();
        }
        if(dirty){render(mx,my);dirty=0u;}
        __asm__ volatile("pause");
    }
}
