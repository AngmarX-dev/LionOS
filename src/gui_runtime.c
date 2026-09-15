#define gui_run gui_run_legacy
#include "gui.c"
#undef gui_run

static uint8_t gui_started;
static uint8_t gui_active_state;
static uint32_t gui_mouse_x;
static uint32_t gui_mouse_y;
static uint32_t gui_previous_buttons;

int gui_is_active(void){
    return gui_active_state!=0u;
}

void gui_start(void){
    gui_started=0u;
    gui_active_state=0u;
    debug_write("LIONOS:GUI-ENTER\n");
    if(!framebuffer_available()){
        debug_write("LIONOS:GUI-NO-FRAMEBUFFER\n");
        return;
    }
    mouse_set_cursor_visible(0u);
    while(keyboard_available())(void)keyboard_getchar();
    wm_init();
    gui_mouse_x=mouse_x();
    gui_mouse_y=(mouse_y()*GRID_H)/25u;
    if(gui_mouse_x>=GRID_W)gui_mouse_x=GRID_W-1u;
    if(gui_mouse_y>=GRID_H)gui_mouse_y=GRID_H-1u;
    gui_previous_buttons=mouse_buttons();
    render(gui_mouse_x,gui_mouse_y);
    dirty=0u;
    gui_started=1u;
    gui_active_state=1u;
}

void gui_step(void){
    if(!gui_started||!gui_active_state)return;

    poll_input(gui_mouse_x,gui_mouse_y,gui_previous_buttons);
    gui_previous_buttons=mouse_buttons();

    struct gui_event event;
    while(next_event(&event)){
        if(event.type==GUI_EVENT_MOUSE_MOVE){
            gui_mouse_x=event.x;
            gui_mouse_y=event.y;
            handle_mouse_move(gui_mouse_x,gui_mouse_y);
        }else if(event.type==GUI_EVENT_MOUSE_PRESS){
            handle_mouse_press(event.x,event.y);
            if(event.x>=4u&&event.x<16u&&event.y>=24u&&event.y<27u)
                gui_active_state=0u;
        }else if(event.type==GUI_EVENT_MOUSE_RELEASE){
            drag_active=0u;
        }else if(event.type==GUI_EVENT_KEY){
            handle_key(event.key);
            if((event.key==27||event.key=='q'||event.key=='Q')&&focused_window==WIN_DESKTOP)
                gui_active_state=0u;
            if((event.key=='t'||event.key=='T'||event.key==13)&&focused_window==WIN_TERMINAL)
                gui_active_state=0u;
        }
        invalidate();
    }

    if(dirty&&gui_active_state){
        render(gui_mouse_x,gui_mouse_y);
        dirty=0u;
    }
}
