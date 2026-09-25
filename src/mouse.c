#include <stdint.h>
#include "io.h"
#include "mouse.h"
#include "xhci.h"

#define PS2_STATUS 0x64u
#define PS2_COMMAND 0x64u
#define PS2_DATA 0x60u
#define PS2_AUX_DISABLE 0xA7u
#define PS2_AUX_ENABLE 0xA8u
#define PS2_READ_CONFIG 0x20u
#define PS2_WRITE_CONFIG 0x60u
#define PS2_WRITE_AUX 0xD4u
#define MOUSE_SET_DEFAULTS 0xF6u
#define MOUSE_ENABLE_STREAMING 0xF4u
#define MOUSE_ACK 0xFAu
#define MOUSE_PACKET_SIZE 3u

static uint32_t ps2_initialized;
static uint32_t usb_initialized;
static uint32_t usb_status;
static uint32_t usb_retry_frames;
static uint32_t usb_retry_attempts;
static uint32_t cursor_x_pos=512u,cursor_y_pos=384u;
static uint32_t cursor_width=1024u,cursor_height=768u;
static uint8_t packet[MOUSE_PACKET_SIZE];
static uint32_t packet_index;
static uint8_t current_buttons;

static int wait_input_clear(void){
    for(uint32_t i=0;i<100000u;++i){
        if((inb(PS2_STATUS)&2u)==0u)return 0;
        __asm__ volatile("pause");
    }
    return -1;
}
static int wait_output(void){
    for(uint32_t i=0;i<100000u;++i){
        if(inb(PS2_STATUS)&1u)return 0;
        __asm__ volatile("pause");
    }
    return -1;
}
static void flush_output(void){
    for(uint32_t i=0;i<32u&&(inb(PS2_STATUS)&1u);++i)(void)inb(PS2_DATA);
}
static int controller_command(uint8_t command){
    if(wait_input_clear()<0)return -1;
    outb(PS2_COMMAND,command);
    return 0;
}
static int mouse_command(uint8_t command){
    flush_output();
    if(controller_command(PS2_WRITE_AUX)<0)return -1;
    if(wait_input_clear()<0)return -1;
    outb(PS2_DATA,command);
    if(wait_output()<0)return -1;
    if(inb(PS2_DATA)!=MOUSE_ACK){flush_output();return -1;}
    flush_output();
    return 0;
}
static void cursor_move(int32_t dx,int32_t dy){
    int32_t nx=(int32_t)cursor_x_pos+dx,ny=(int32_t)cursor_y_pos+dy;
    if(nx<0)nx=0;
    if(ny<0)ny=0;
    if(nx>=(int32_t)cursor_width)nx=(int32_t)cursor_width-1;
    if(ny>=(int32_t)cursor_height)ny=(int32_t)cursor_height-1;
    cursor_x_pos=(uint32_t)nx;cursor_y_pos=(uint32_t)ny;
}
static void handle_ps2_packet(void){
    uint8_t status=packet[0];
    if(!(status&8u))return;
    current_buttons=status&7u;
    if(status&0xC0u)return;
    cursor_move((int32_t)(int8_t)packet[1]*2,-(int32_t)(int8_t)packet[2]*2);
}

int mouse_init(void){
    ps2_initialized=0u;
    usb_initialized=0u;
    usb_status=0u;
    usb_retry_frames=0u;
    usb_retry_attempts=0u;
    packet_index=0u;
    current_buttons=0u;
    cursor_x_pos=cursor_width/2u;
    cursor_y_pos=cursor_height/2u;

    (void)controller_command(PS2_AUX_DISABLE);
    flush_output();
    if(controller_command(PS2_READ_CONFIG)<0)return -1;
    if(wait_output()<0)return -1;
    uint8_t config=inb(PS2_DATA);
    config&=(uint8_t)~2u;
    if(controller_command(PS2_WRITE_CONFIG)<0)return -1;
    if(wait_input_clear()<0)return -1;
    outb(PS2_DATA,config);
    if(controller_command(PS2_AUX_ENABLE)<0)return -1;
    flush_output();
    if(mouse_command(MOUSE_SET_DEFAULTS)<0)return -1;
    if(mouse_command(MOUSE_ENABLE_STREAMING)<0)return -1;
    ps2_initialized=1u;
    return 0;
}

int mouse_usb_init(void){
    if(usb_initialized)return 0;
    usb_status=3u; /* RETRYING */
    if(xhci_mouse_init()!=0){
        usb_status=0u; /* UNAVAILABLE */
        return -1;
    }
    usb_initialized=1u;
    usb_status=1u; /* READY */
    return 0;
}

const char *mouse_usb_status_text(void){
    if(usb_status==2u)return "ACTIVE";
    if(usb_status==1u)return "READY";
    if(usb_status==3u)return "RETRYING";
    return "UNAVAILABLE";
}

void mouse_poll(void){
    if(ps2_initialized){
        while(inb(PS2_STATUS)&1u){
            uint8_t status=inb(PS2_STATUS);
            if(!(status&0x20u))break;
            uint8_t value=inb(PS2_DATA);
            if(packet_index==0u&&(value&8u)==0u)continue;
            packet[packet_index++]=value;
            if(packet_index==MOUSE_PACKET_SIZE){
                packet_index=0u;
                handle_ps2_packet();
            }
        }
    }
    if(usb_initialized){
        int32_t dx=0,dy=0;
        uint8_t buttons=0;
        int result=xhci_mouse_poll(&dx,&dy,&buttons);
        if(result==1){
            usb_status=2u; /* ACTIVE: at least one HID report arrived */
            current_buttons=buttons;
            cursor_move(dx*2,dy*2);
        }
    }else if(usb_retry_attempts<6u){
        usb_status=3u; /* RETRYING */
        if(++usb_retry_frames>=300u){
            usb_retry_frames=0u;
            ++usb_retry_attempts;
            (void)mouse_usb_init();
        }
    }
}

void mouse_set_bounds(uint32_t width,uint32_t height){
    if(width)cursor_width=width;
    if(height)cursor_height=height;
    cursor_x_pos=cursor_width/2u;
    cursor_y_pos=cursor_height/2u;
}
uint32_t mouse_x(void){return cursor_x_pos;}
uint32_t mouse_y(void){return cursor_y_pos;}
uint8_t mouse_buttons(void){return current_buttons;}
