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

#define C_BG       0x07101Cu
#define C_TOP      0x0D1B2Cu
#define C_SIDE     0x0A1625u
#define C_CARD     0x102238u
#define C_CARD2    0x142A45u
#define C_BORDER   0x284D73u
#define C_ACCENT   0xF2C14Eu
#define C_ACCENT2  0x56B4FFu
#define C_TEXT     0xEEF4FCu
#define C_DIM      0x8FA7C2u
#define C_GOOD     0x55D187u
#define C_WARN     0xF4A261u
#define C_DANGER   0x5B2A34u
#define C_CURSOR   0xFFFFFFu

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
    uint32_t x=GRID_X+col*CELL_W,y=GRID_Y+row*CELL_H,w=width*CELL_W,h=height*CELL_H;
    framebuffer_fill_rect(x,y,w,h,C_BORDER);
    framebuffer_fill_rect(x+2u,y+2u,w-4u,h-4u,fill);
}

static void button(uint32_t col,uint32_t row,uint32_t width,uint32_t height,uint32_t fill,const char*label){
    uint32_t x=GRID_X+col*CELL_W,y=GRID_Y+row*CELL_H,w=width*CELL_W,h=height*CELL_H;
    framebuffer_fill_rect(x,y,w,h,C_BORDER);
    framebuffer_fill_rect(x+2u,y+2u,w-4u,h-4u,fill);
    uint32_t len=0u;while(label[len])++len;
    draw_text(label,col+(len<width?(width-len)/2u:0u),row+1u,C_TEXT);
}

static void dot(uint32_t col,uint32_t row,uint32_t color){
    uint32_t x=GRID_X+col*CELL_W,y=GRID_Y+row*CELL_H;
    framebuffer_fill_rect(x+3u,y+5u,6u,6u,color);
}

static void draw_cursor(uint32_t mx,uint32_t my){
    uint32_t x=mx*CELL_W+4u,y=my*CELL_H+4u;
    framebuffer_fill_rect(x,y,3u,18u,C_CURSOR);
    framebuffer_fill_rect(x,y,10u,3u,C_CURSOR);
    framebuffer_fill_rect(x+4u,y+14u,6u,3u,C_CURSOR);
}

static void draw_desktop(uint32_t mx,uint32_t my,const char*status){
    uint32_t width=framebuffer_width(),height=framebuffer_height();
    framebuffer_clear(C_BG);

    framebuffer_fill_rect(0,0,width,52u,C_TOP);
    framebuffer_fill_rect(0,52u,220u,height-100u,C_SIDE);
    framebuffer_fill_rect(0,height-48u,width,48u,C_TOP);
    framebuffer_fill_rect(220u,52u,2u,height-100u,C_BORDER);

    draw_text("LIONOS",2u,1u,C_ACCENT);
    draw_text("PHASE 26",68u,1u,C_DIM);

    panel(2u,5u,16u,28u,C_SIDE);
    draw_text("DESKTOP",4u,7u,C_DIM);
    button(4u,9u,12u,3u,C_CARD2,"HOME");
    button(4u,14u,12u,3u,C_CARD,"TERMINAL");
    button(4u,19u,12u,3u,C_CARD,"ABOUT");
    button(4u,24u,12u,3u,C_DANGER,"EXIT");
    draw_text("LIONOS",5u,29u,C_ACCENT);

    draw_text("WELCOME BACK",20u,6u,C_TEXT);
    draw_text("A SMALL DESKTOP FOR A SMALL KERNEL",20u,8u,C_DIM);

    panel(20u,11u,24u,13u,C_CARD);
    dot(22u,13u,C_GOOD);
    draw_text("SYSTEM STATUS",24u,13u,C_TEXT);
    draw_text("GRAPHICS ONLINE",22u,16u,C_GOOD);
    draw_text("SMP AND STORAGE ONLINE",22u,18u,C_DIM);
    draw_text(status,22u,20u,C_ACCENT2);

    panel(46u,11u,25u,13u,C_CARD);
    draw_text("QUICK ACTIONS",49u,13u,C_TEXT);
    button(49u,16u,18u,3u,C_CARD2,"TERMINAL");
    button(49u,20u,18u,3u,C_CARD2,"ABOUT");

    panel(20u,26u,51u,9u,C_CARD);
    draw_text("LIONOS CORE",23u,28u,C_ACCENT);
    draw_text("32 BIT X86",23u,31u,C_TEXT);
    draw_text("MULTIBOOT2  GRUB  LAPIC  VFS",39u,28u,C_DIM);
    draw_text("MOUSE POLLING  PS2 INPUT  HEAP",39u,31u,C_DIM);
    draw_text("PHASE 26 GRAPHICAL DESKTOP",39u,33u,C_GOOD);

    draw_text("READY",2u,46u,C_GOOD);
    draw_text("Q / ESC  SHELL",19u,46u,C_DIM);
    draw_text("A  ABOUT",46u,46u,C_DIM);
    draw_text("MOUSE  CLICK ACTION",60u,46u,C_DIM);
    draw_cursor(mx,my);
}

static void draw_about(uint32_t mx,uint32_t my){
    uint32_t x=GRID_X+9u*CELL_W,y=GRID_Y+6u*CELL_H,w=60u*CELL_W,h=24u*CELL_H;
    framebuffer_fill_rect(x,y,w,h,C_BORDER);
    framebuffer_fill_rect(x+3u,y+3u,w-6u,h-6u,C_TOP);
    draw_text("ABOUT LIONOS",14u,9u,C_ACCENT);
    draw_text("32 BIT X86 EXPERIMENTAL OS",14u,12u,C_TEXT);
    draw_text("MULTIBOOT2 GRUB BOOT",14u,15u,C_DIM);
    draw_text("SMP MEMORY PROCESS VFS",14u,17u,C_DIM);
    draw_text("MOUSE PS2 AND FRAMEBUFFER UI",14u,19u,C_DIM);
    draw_text("TERMINAL AND DESKTOP TOGETHER",14u,21u,C_GOOD);
    draw_text("PRESS A Q OR ESC TO CLOSE",14u,24u,C_ACCENT2);
    button(62u,7u,4u,2u,C_DANGER,"X");
    draw_cursor(mx,my);
}

static void return_shell(void){
    console_use_framebuffer();
    mouse_set_cursor_visible(1u);
    mouse_show();
    debug_write("LIONOS:GUI-TERMINAL\n");
    debug_write("LIONOS:GUI-EXIT\n");
}

void gui_run(void){
    debug_write("LIONOS:GUI-ENTER\n");
    if(!framebuffer_available()){
        debug_write("LIONOS:GUI-NO-FRAMEBUFFER\n");
        return;
    }

    mouse_set_cursor_visible(0u);
    while(keyboard_available())(void)keyboard_getchar();

    uint32_t about_open=0u;
    uint32_t previous_buttons=mouse_buttons();
    uint32_t mx=mouse_x();
    uint32_t my=(mouse_y()*GRID_H)/25u;
    if(mx>=GRID_W)mx=GRID_W-1u;
    if(my>=GRID_H)my=GRID_H-1u;

    const char*status="SYSTEM READY";
    draw_desktop(mx,my,status);

    for(;;){
        keyboard_poll();
        mouse_poll();

        uint32_t next_mx=mouse_x();
        uint32_t next_my=(mouse_y()*GRID_H)/25u;
        if(next_mx>=GRID_W)next_mx=GRID_W-1u;
        if(next_my>=GRID_H)next_my=GRID_H-1u;

        int redraw=(next_mx!=mx||next_my!=my)&&!about_open;
        mx=next_mx;
        my=next_my;

        uint32_t buttons=mouse_buttons();
        if((buttons&1u)&&!(previous_buttons&1u)){
            if(about_open){
                if(mx>=62u&&mx<66u&&my>=7u&&my<9u){
                    about_open=0u;
                    redraw=1;
                    debug_write("LIONOS:GUI-ABOUT-CLOSE-MOUSE\n");
                }else if(mx<9u||mx>=69u||my<6u||my>=30u){
                    about_open=0u;
                    redraw=1;
                    debug_write("LIONOS:GUI-ABOUT-CLOSE-OUTSIDE\n");
                }
            }else if((mx>=4u&&mx<16u&&my>=14u&&my<17u)||(mx>=49u&&mx<67u&&my>=16u&&my<19u)){
                return_shell();
                return;
            }else if((mx>=4u&&mx<16u&&my>=19u&&my<22u)||(mx>=49u&&mx<67u&&my>=20u&&my<23u)){
                about_open=1u;
                redraw=1;
                debug_write("LIONOS:GUI-ABOUT-OPEN\n");
            }else if(mx>=4u&&mx<16u&&my>=24u&&my<27u){
                return_shell();
                return;
            }
        }
        previous_buttons=buttons;

        while(keyboard_available()){
            int ch=keyboard_getchar();
            if(ch==27||ch=='q'||ch=='Q'){
                if(about_open){
                    about_open=0u;
                    redraw=1;
                    debug_write("LIONOS:GUI-ABOUT-CLOSE-KEY\n");
                }else{
                    return_shell();
                    return;
                }
            }else if(ch=='a'||ch=='A'){
                about_open=about_open?0u:1u;
                redraw=1;
                debug_write("LIONOS:GUI-ABOUT-KEY\n");
            }else if(ch=='t'||ch=='T'){
                return_shell();
                return;
            }
        }

        if(redraw){
            draw_desktop(mx,my,status);
            if(about_open)draw_about(mx,my);
        }

        __asm__ volatile("pause");
    }
}
