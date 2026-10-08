#include "debug.h"
#include <stdint.h>
#include "io.h"
#include "keyboard.h"
#include "mouse.h"

#define KEYBOARD_BUFFER_SIZE 128u
#define PS2_STATUS 0x64u
#define PS2_DATA 0x60u

static volatile uint8_t buffer[KEYBOARD_BUFFER_SIZE];
static volatile uint32_t read_index;
static volatile uint32_t write_index;
static volatile uint8_t shift_down;
static volatile uint8_t ctrl_down;
static volatile uint8_t alt_down;
static volatile uint8_t caps_lock;
static volatile uint8_t extended_prefix;
static volatile uint32_t dropped_chars;
static volatile uint32_t scancode_count;
static volatile uint8_t last_scancode;
static volatile uint8_t input_seen;
static volatile uint8_t raw_input_seen;
static volatile uint8_t scancode_set=1u;
static volatile uint8_t set2_break_pending=0u;
static volatile uint8_t ps2_controller_present=0u;
static volatile uint8_t ps2_translation_enabled=1u;

static void keyboard_wait_write(void);
static int keyboard_wait_read(void);
static void keyboard_flush_output(void);

static int keyboard_probe_controller(void){
    uint8_t status=inb(PS2_STATUS);
    return status==0xFFu ? -1 : 0;
}

static const char keymap[128]={
    [0x01]=27,
    [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',
    [0x07]='6',[0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',
    [0x0C]='-',[0x0D]='=',[0x10]='q',[0x11]='w',[0x12]='e',
    [0x13]='r',[0x14]='t',[0x15]='y',[0x16]='u',[0x17]='i',
    [0x18]='o',[0x19]='p',[0x1A]='[',[0x1B]=']',[0x1C]='\n',
    [0x1E]='a',[0x1F]='s',[0x20]='d',[0x21]='f',[0x22]='g',
    [0x23]='h',[0x24]='j',[0x25]='k',[0x26]='l',[0x27]=';',
    [0x28]='\'',[0x29]='`',[0x2B]='\\',[0x2C]='z',[0x2D]='x',
    [0x2E]='c',[0x2F]='v',[0x30]='b',[0x31]='n',[0x32]='m',
    [0x33]=',',[0x34]='.',[0x35]='/',[0x39]=' '
};

static const char shiftmap[128]={
    [0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',
    [0x07]='^',[0x08]='&',[0x09]='*',[0x0A]='(',[0x0B]=')',
    [0x0C]='_',[0x0D]='+',[0x10]='Q',[0x11]='W',[0x12]='E',
    [0x13]='R',[0x14]='T',[0x15]='Y',[0x16]='U',[0x17]='I',
    [0x18]='O',[0x19]='P',[0x1A]='{',[0x1B]='}',[0x1C]='\n',
    [0x1E]='A',[0x1F]='S',[0x20]='D',[0x21]='F',[0x22]='G',
    [0x23]='H',[0x24]='J',[0x25]='K',[0x26]='L',[0x27]=':',
    [0x28]='"',[0x29]='~',[0x2B]='|',[0x2C]='Z',[0x2D]='X',
    [0x2E]='C',[0x2F]='V',[0x30]='B',[0x31]='N',[0x32]='M',
    [0x33]='<',[0x34]='>',[0x35]='?',[0x39]=' '
};

static void push_char(uint8_t c){
    uint32_t next=(write_index+1u)%KEYBOARD_BUFFER_SIZE;
    if(next==read_index){++dropped_chars;return;}
    buffer[write_index]=c;
    write_index=next;
}

static void keyboard_wait_write(void){
    for(uint32_t i=0u;i<100000u;++i){
        if(!(inb(PS2_STATUS)&0x02u))return;
        io_wait();
    }
}

static int keyboard_wait_read(void){
    for(uint32_t i=0u;i<100000u;++i){
        if(inb(PS2_STATUS)&0x01u)return 0;
        io_wait();
    }
    return -1;
}

static void keyboard_flush_output(void){
    for(uint32_t i=0u;i<64u;++i){
        if(!(inb(PS2_STATUS)&0x01u))break;
        (void)inb(PS2_DATA);
    }
}

static int keyboard_controller_config(void){
    keyboard_wait_write();
    outb(PS2_STATUS,0x20u);
    if(keyboard_wait_read()!=0)return -1;
    uint8_t cfg=inb(PS2_DATA);

    cfg|=0x01u|0x04u;
    cfg&=(uint8_t)~0x10u;

    keyboard_wait_write();
    outb(PS2_STATUS,0x60u);
    keyboard_wait_write();
    outb(PS2_DATA,cfg);

    ps2_translation_enabled=(uint8_t)((cfg&0x40u)!=0u);
    return 0;
}

static int keyboard_device_command_once(uint8_t command){
    keyboard_wait_write();
    outb(PS2_DATA,command);

    for(uint32_t i=0u;i<200000u;++i){
        uint8_t st=inb(PS2_STATUS);
        if(st&0x01u){
            uint8_t v=inb(PS2_DATA);
            if(st&0x20u){
                mouse_handle_ps2_byte(v);
                continue;
            }
            if(v==0xFAu)return 0;
            if(v==0xFEu)return -2;
        }
        io_wait();
    }
    return -1;
}

static int keyboard_device_command(uint8_t command){
    for(uint32_t retry=0u;retry<3u;++retry){
        int rc=keyboard_device_command_once(command);
        if(rc==0)return 0;
        if(rc!=-2)return rc;
    }
    return -2;
}

static int keyboard_query_scancode_set(uint8_t *set){
    if(!set)return -1;

    keyboard_wait_write();
    outb(PS2_DATA,0xF0u);
    int ack=0;
    for(uint32_t i=0u;i<200000u;++i){
        uint8_t st=inb(PS2_STATUS);
        if(st&0x01u){
            uint8_t v=inb(PS2_DATA);
            if(st&0x20u){
                mouse_handle_ps2_byte(v);
                continue;
            }
            if(v==0xFAu){ack=1;break;}
            if(v==0xFEu)continue;
        }
        io_wait();
    }
    if(!ack)return -1;

    keyboard_wait_write();
    outb(PS2_DATA,0x00u);
    for(uint32_t i=0u;i<300000u;++i){
        uint8_t st=inb(PS2_STATUS);
        if(st&0x01u){
            uint8_t v=inb(PS2_DATA);
            if(st&0x20u){
                mouse_handle_ps2_byte(v);
                continue;
            }
            if(v==0xFAu||v==0xFEu)continue;
            if(v==0x41u){*set=1u;return 0;}
            if(v==0x43u){*set=2u;return 0;}
            if(v==0x3Fu){*set=3u;return 0;}
        }
        io_wait();
    }
    return -1;
}

static int keyboard_enable_scanning(void){
    return keyboard_device_command(0xF4u);
}

void keyboard_init(void){
    read_index=0u;write_index=0u;
    shift_down=0u;ctrl_down=0u;alt_down=0u;caps_lock=0u;
    extended_prefix=0u;dropped_chars=0u;scancode_count=0u;last_scancode=0u;
    input_seen=0u;raw_input_seen=0u;scancode_set=1u;set2_break_pending=0u;
    ps2_controller_present=0u;ps2_translation_enabled=0u;

    if(keyboard_probe_controller()!=0){
        debug_write("LIONOS:KEYBOARD-PS2-ABSENT\n");
        return;
    }
    ps2_controller_present=1u;

    keyboard_flush_output();

    int cfg_rc=keyboard_controller_config();
    if(cfg_rc!=0)
        debug_write("LIONOS:KEYBOARD-CFG-FAIL\n");

    /*
     * keyboard_controller_config() clears KBDDIS itself, matching Linux's
     * separate port-enable stage without sending a redundant 0xAE command.
     */
    keyboard_flush_output();

    set2_break_pending=0u;
    if(ps2_translation_enabled){
        scancode_set=1u;
    }else{
        uint8_t detected=0u;
        if(keyboard_query_scancode_set(&detected)==0 &&
           (detected==1u||detected==2u)){
            scancode_set=detected;
        }else{
            scancode_set=2u;
        }
    }

    int scan_rc=keyboard_enable_scanning();

    debug_write(scancode_set==2u
        ?"LIONOS:KEYBOARD-SET2\n"
        :"LIONOS:KEYBOARD-SET1\n");
    if(scan_rc==0)debug_write("LIONOS:KEYBOARD-READY\n");
    else debug_write("LIONOS:KEYBOARD-NO-ACK\n");

    uint8_t mask=inb(0x21);
    mask&=(uint8_t)~(1u<<1);
    outb(0x21,mask);
}

static uint8_t set2_make_to_set1(uint8_t code){
    switch(code){
        case 0x1Cu:return 0x1Eu;case 0x32u:return 0x30u;case 0x21u:return 0x2Eu;
        case 0x23u:return 0x20u;case 0x24u:return 0x12u;case 0x2Bu:return 0x21u;
        case 0x34u:return 0x22u;case 0x33u:return 0x23u;case 0x43u:return 0x17u;
        case 0x3Bu:return 0x24u;case 0x42u:return 0x25u;case 0x4Bu:return 0x26u;
        case 0x3Au:return 0x32u;case 0x31u:return 0x31u;case 0x44u:return 0x18u;
        case 0x4Du:return 0x19u;case 0x15u:return 0x10u;case 0x2Du:return 0x13u;
        case 0x1Bu:return 0x1Fu;case 0x2Cu:return 0x14u;case 0x3Cu:return 0x16u;
        case 0x2Au:return 0x2Fu;case 0x1Du:return 0x11u;case 0x22u:return 0x2Du;
        case 0x35u:return 0x15u;case 0x1Au:return 0x2Cu;
        case 0x16u:return 0x02u;case 0x1Eu:return 0x03u;case 0x26u:return 0x04u;
        case 0x25u:return 0x05u;case 0x2Eu:return 0x06u;case 0x36u:return 0x07u;
        case 0x3Du:return 0x08u;case 0x3Eu:return 0x09u;case 0x46u:return 0x0Au;
        case 0x45u:return 0x0Bu;case 0x4Eu:return 0x0Cu;case 0x55u:return 0x0Du;
        case 0x0Eu:return 0x29u;case 0x54u:return 0x1Au;case 0x5Bu:return 0x1Bu;
        case 0x5Du:return 0x2Bu;case 0x4Cu:return 0x27u;case 0x52u:return 0x28u;
        case 0x41u:return 0x33u;case 0x49u:return 0x34u;case 0x4Au:return 0x35u;
        case 0x66u:return 0x0Eu;case 0x0Du:return 0x0Fu;case 0x5Au:return 0x1Cu;
        case 0x29u:return 0x39u;case 0x76u:return 0x01u;
        case 0x12u:return 0x2Au;case 0x59u:return 0x36u;case 0x14u:return 0x1Du;
        case 0x11u:return 0x38u;case 0x58u:return 0x3Au;
        default:return 0u;
    }
}

void keyboard_handle_scancode(uint8_t scancode){
    if(scancode_set==2u){
        if(scancode==0xE0u){extended_prefix=1u;return;}
        if(scancode==0xF0u){set2_break_pending=1u;return;}

        uint8_t make=set2_make_to_set1(scancode);
        if(!make){
            set2_break_pending=0u;extended_prefix=0u;return;
        }

        if(set2_break_pending){
            set2_break_pending=0u;extended_prefix=0u;
            if(make==0x2Au||make==0x36u)shift_down=0u;
            else if(make==0x1Du)ctrl_down=0u;
            else if(make==0x38u)alt_down=0u;
            return;
        }

        if(extended_prefix){extended_prefix=0u;return;}
        scancode=make;
    }

    ++scancode_count;
    last_scancode=scancode;
    if(!raw_input_seen){raw_input_seen=1u;debug_write("LIONOS:KEYBOARD-RAW-INPUT\n");}
    if(!input_seen){input_seen=1u;debug_write("LIONOS:KEYBOARD-INPUT\n");}

    if(scancode==0xE0u){extended_prefix=1u;return;}
    if(extended_prefix){extended_prefix=0u;return;}

    if(scancode==0x2Au||scancode==0x36u){shift_down=1u;return;}
    if(scancode==0xAAu||scancode==0xB6u){shift_down=0u;return;}
    if(scancode==0x1Du){ctrl_down=1u;return;}
    if(scancode==0x9Du){ctrl_down=0u;return;}
    if(scancode==0x38u){alt_down=1u;return;}
    if(scancode==0xB8u){alt_down=0u;return;}
    if(scancode==0x3Au){caps_lock^=1u;return;}
    if(scancode&0x80u)return;

    if(scancode==0x0Eu){push_char('\b');return;}
    if(scancode==0x0Fu){push_char('\t');return;}

    if(scancode<128u){
        char base=keymap[scancode];
        char shifted=shiftmap[scancode];
        char ch;

        if((base>='a'&&base<='z')&&(caps_lock^shift_down))
            ch=(char)(base-'a'+'A');
        else
            ch=shift_down&&shifted?shifted:base;

        if(ctrl_down){
            if(base>='a'&&base<='z')ch=(char)(base-'a'+1);
            else if(scancode==0x2Cu)ch=0x1Cu;
            else if(scancode==0x0Cu)ch=0x1Fu;
            else ch=0;
        }

        if(ch)push_char((uint8_t)ch);
    }
}

static volatile uint8_t usb_prev_keys[6];
static int usb_key_present(const volatile uint8_t *keys,uint8_t code){
    for(uint32_t i=0u;i<6u;++i)if(keys[i]==code)return 1;
    return 0;
}
static char usb_key_char(uint8_t usage,uint8_t shifted){
    static const char normal[]="abcdefghijklmnopqrstuvwxyz1234567890";
    static const char shifted_map[]="ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()";
    if(usage>=0x04u&&usage<=0x1Du)
        return shifted?shifted_map[usage-0x04u]:normal[usage-0x04u];
    if(usage>=0x1Eu&&usage<=0x27u)
        return shifted?shifted_map[usage-0x1Eu+26u]:normal[usage-0x1Eu+26u];
    switch(usage){
        case 0x28u:return 10;case 0x29u:return 27;case 0x2Au:return 8;case 0x2Bu:return 9;
        case 0x2Cu:return 32;case 0x2Du:return shifted?95:45;case 0x2Eu:return shifted?43:61;
        case 0x2Fu:return shifted?123:91;case 0x30u:return shifted?125:93;case 0x31u:return shifted?124:92;
        case 0x33u:return shifted?58:59;case 0x34u:return shifted?34:39;case 0x35u:return shifted?126:96;
        case 0x36u:return shifted?60:44;case 0x37u:return shifted?62:46;case 0x38u:return shifted?63:47;
        default:return 0;
    }
}

void keyboard_handle_usb_report(const uint8_t *report,uint32_t length){
    if(!report||length<8u)return;
    uint8_t modifiers=report[0];
    uint8_t shift=(uint8_t)((modifiers&0x22u)!=0u);
    uint8_t ctrl=(uint8_t)((modifiers&0x11u)!=0u);
    uint8_t alt=(uint8_t)((modifiers&0x44u)!=0u);
    shift_down=shift;ctrl_down=ctrl;alt_down=alt;

    for(uint32_t i=0u;i<6u;++i){
        uint8_t code=report[2u+i];
        if(!code||usb_key_present(usb_prev_keys,code))continue;
        if(code==0x39u){caps_lock^=1u;continue;}
        char ch=usb_key_char(code,(uint8_t)(shift^(caps_lock&&code>=0x04u&&code<=0x1Du)));
        if(ctrl&&ch>='a'&&ch<='z')ch=(char)(ch-'a'+1);
        if(ch){
            push_char((uint8_t)ch);
            if(!input_seen){input_seen=1u;debug_write("LIONOS:KEYBOARD-USB-INPUT\n");}
        }
    }
    for(uint32_t i=0u;i<6u;++i)usb_prev_keys[i]=report[2u+i];
}

/* Linux-style i8042 dispatcher: one status sample owns one output byte. */
void keyboard_ps2_service(void){
    if(!ps2_controller_present)return;
    for(uint32_t n=0u;n<64u;++n){
        uint8_t status=inb(PS2_STATUS);
        if(!(status&0x01u))break;
        uint8_t data=inb(PS2_DATA);
        if(status&0x20u)mouse_handle_ps2_byte(data);
        else keyboard_handle_scancode(data);
    }
}

void keyboard_irq_handler(void){keyboard_ps2_service();}

void keyboard_rearm_after_mouse_init(void){
    if(!ps2_controller_present)return;
    set2_break_pending=0u;
    extended_prefix=0u;
    (void)keyboard_enable_scanning();
}

void keyboard_poll(void){keyboard_ps2_service();}

int keyboard_getchar(void){
    if(read_index==write_index)return -1;
    uint8_t c=buffer[read_index];
    read_index=(read_index+1u)%KEYBOARD_BUFFER_SIZE;
    return (int)c;
}

int keyboard_ps2_available(void){return ps2_controller_present!=0u;}

uint32_t keyboard_available(void){
    if(write_index>=read_index)return write_index-read_index;
    return KEYBOARD_BUFFER_SIZE-read_index+write_index;
}
