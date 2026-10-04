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

static const char keymap[128] = {
    [0x01] = 27,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x10] = 'q', [0x11] = 'w', [0x12] = 'e',
    [0x13] = 'r', [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']', [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
    [0x28] = '\'', [0x29] = '`', [0x2B] = '\\', [0x2C] = 'z', [0x2D] = 'x',
    [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b', [0x31] = 'n', [0x32] = 'm',
    [0x33] = ',', [0x34] = '.', [0x35] = '/', [0x39] = ' '
};

static const char shiftmap[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+', [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E',
    [0x13] = 'R', [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I',
    [0x18] = 'O', [0x19] = 'P', [0x1A] = '{', [0x1B] = '}', [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
    [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L', [0x27] = ':',
    [0x28] = '"', [0x29] = '~', [0x2B] = '|', [0x2C] = 'Z', [0x2D] = 'X',
    [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B', [0x31] = 'N', [0x32] = 'M',
    [0x33] = '<', [0x34] = '>', [0x35] = '?', [0x39] = ' '
};

static void push_char(uint8_t c) {
    uint32_t next = (write_index + 1u) % KEYBOARD_BUFFER_SIZE;
    if (next == read_index) {
        ++dropped_chars;
        return;
    }
    buffer[write_index] = c;
    write_index = next;
}

static void keyboard_wait_write(void){
    for(uint32_t i=0u;i<100000u;++i){
        if(!(inb(PS2_STATUS)&0x02u)) return;
        io_wait();
    }
}

static int keyboard_wait_read(void){
    for(uint32_t i=0u;i<100000u;++i){
        if(inb(PS2_STATUS)&0x01u) return 0;
        io_wait();
    }
    return -1;
}

static void keyboard_flush_output(void){
    for(uint32_t i=0u;i<64u;++i){
        if(!(inb(PS2_STATUS)&0x01u)) break;
        (void)inb(PS2_DATA);
    }
}

static int keyboard_controller_config(void){
    keyboard_wait_write();
    outb(PS2_STATUS,0x20u);
    if(keyboard_wait_read()!=0) return -1;
    uint8_t cfg=inb(PS2_DATA);
    /* Enable IRQ1, keep the controller's system flag set, enable the
       keyboard clock, and force i8042 translation to scan-code Set 1.
       Laptop firmware commonly leaves translation disabled; LionOS's
       keymap intentionally consumes Set-1 codes. */
    cfg|=0x01u|0x04u|0x40u;
    cfg&=(uint8_t)~0x10u;
    keyboard_wait_write();
    outb(PS2_STATUS,0x60u);
    keyboard_wait_write();
    outb(PS2_DATA,cfg);
    return 0;
}

static int keyboard_device_command_once(uint8_t command){
    keyboard_wait_write();
    outb(PS2_DATA,command);
    for(uint32_t i=0u;i<200000u;++i){
        uint8_t st=inb(PS2_STATUS);
        if(st&0x01u){
            uint8_t v=inb(PS2_DATA);
            /* Never consume an AUX byte while waiting for keyboard ACK. */
            if(st&0x20u) continue;
            if(v==0xFAu) return 0;   /* ACK */
            if(v==0xFEu) return -2;  /* RESEND */
            /* 0xAA can be emitted by a keyboard reset self-test; do not
               confuse it with the command's ACK. */
        }
        io_wait();
    }
    return -1;
}

static int keyboard_device_command(uint8_t command){
    for(uint32_t retry=0u;retry<3u;++retry){
        int rc=keyboard_device_command_once(command);
        if(rc==0) return 0;
        if(rc!=-2) return rc;
    }
    return -2;
}

static int keyboard_set_scancode_set2(void){
    /* PS/2 Set Scancode Set command: F0, then Set 2. */
    int rc=keyboard_device_command(0xF0u);
    if(rc!=0) return rc;
    return keyboard_device_command(0x02u);
}

void keyboard_init(void) {
    read_index = 0;
    write_index = 0;
    shift_down = 0;
    ctrl_down = 0;
    alt_down = 0;
    caps_lock = 0;
    extended_prefix = 0;
    dropped_chars = 0;
    scancode_count = 0u;
    last_scancode = 0u;
    input_seen = 0u;

    /*
     * Fully re-arm the i8042 keyboard port. Some laptop firmware leaves the
     * keyboard clock/scanning disabled after boot, and some machines retain
     * stale bytes in the controller output FIFO. Keep AUX disabled here;
     * mouse_init() enables it after the keyboard is ready.
     */
    keyboard_wait_write();
    outb(PS2_STATUS,0xADu); /* disable keyboard */
    keyboard_wait_write();
    outb(PS2_STATUS,0xA7u); /* disable auxiliary */
    keyboard_flush_output();

    int cfg_rc=keyboard_controller_config();
    if(cfg_rc!=0)
        debug_write("LIONOS:KEYBOARD-CFG-FAIL\n");

    keyboard_wait_write();
    outb(PS2_STATUS,0xAEu); /* enable keyboard */
    keyboard_flush_output();

    /*
     * Make the physical device generate Set-2 codes explicitly. The i8042
     * translation bit is also enabled above, but programming the keyboard
     * itself removes another source of laptop/firmware-dependent behavior.
     */
    int set_rc=keyboard_set_scancode_set2();
    int scan_rc=-1;
    if(set_rc==0)
        scan_rc=keyboard_device_command(0xF4u); /* enable scanning */

    keyboard_flush_output();
    if(set_rc==0 && scan_rc==0)
        debug_write("LIONOS:KEYBOARD-READY\n");
    else
        debug_write("LIONOS:KEYBOARD-NO-ACK\n");

    uint8_t mask = inb(0x21);
    mask &= (uint8_t)~(1u << 1);
    outb(0x21, mask);
}

void keyboard_handle_scancode(uint8_t scancode) {
    ++scancode_count;
    last_scancode=scancode;
    if(!input_seen){
        input_seen=1u;
        debug_write("LIONOS:KEYBOARD-INPUT\n");
    }

    if(scancode==0xE0u){
        extended_prefix=1u;
        return;
    }
    if(extended_prefix){
        extended_prefix=0u;
        return;
    }

    if(scancode==0x2Au||scancode==0x36u){shift_down=1u;return;}
    if(scancode==0xAAu||scancode==0xB6u){shift_down=0u;return;}
    if(scancode==0x1Du){ctrl_down=1u;return;}
    if(scancode==0x9Du){ctrl_down=0u;return;}
    if(scancode==0x38u){alt_down=1u;return;}
    if(scancode==0xB8u){alt_down=0u;return;}
    if(scancode==0x3Au){caps_lock^=1u;return;}

    if(scancode&0x80u)return;

    if(scancode==0x0Eu){
        push_char('\b');
        return;
    }
    if(scancode==0x0Fu){
        push_char('\t');
        return;
    }

    if(scancode<128u){
        char base=keymap[scancode];
        char shifted=shiftmap[scancode];
        char ch;

        if((base>='a'&&base<='z')&&(caps_lock^shift_down))
            ch=(char)(base-'a'+'A');
        else
            ch=shift_down&&shifted?shifted:base;

        /*
         * Preserve terminal control characters. A foreground-console layer
         * can later map these to signals; LionOS does not yet have a
         * foreground-job abstraction, so the driver does not guess an owner.
         */
        if(ctrl_down){
            if(base>='a'&&base<='z'){
                ch=(char)(base-'a'+1);
            }else if(scancode==0x2Cu){
                ch=0x1Cu; /* Ctrl-\ */
            }else if(scancode==0x0Cu){
                ch=0x1Fu;
            }else{
                ch=0;
            }
        }

        if(ch)push_char((uint8_t)ch);
    }
}

void keyboard_irq_handler(void){
    /*
     * IRQ 1 is the interrupt-driven producer for the ring buffer. Consume
     * keyboard bytes already waiting in the i8042 output buffer, but never
     * steal AUX (mouse) bytes belonging to IRQ 12.
     */
    for(uint32_t n=0u;n<64u;++n){
        uint8_t status=inb(PS2_STATUS);
        if(!(status&0x01u))break;
        if(status&0x20u){
            /* The i8042 output buffer is shared by keyboard and mouse. */
            mouse_irq_handler();
            continue;
        }
        keyboard_handle_scancode(inb(PS2_DATA));
    }
}

void keyboard_poll(void){
    /*
     * Compatibility entry point for early diagnostics. Normal desktop input
     * is interrupt-driven through keyboard_irq_handler().
     */
    keyboard_irq_handler();
}


int keyboard_getchar(void) {
    if (read_index == write_index) return -1;
    uint8_t c = buffer[read_index];
    read_index = (read_index + 1u) % KEYBOARD_BUFFER_SIZE;
    return (int)c;
}

uint32_t keyboard_available(void) {
    if (write_index >= read_index) return write_index - read_index;
    return KEYBOARD_BUFFER_SIZE - read_index + write_index;
}
