#ifndef LIONOS_TTY_H
#define LIONOS_TTY_H

#include <stdint.h>

/* One firmware-console TTY is exposed by this initial software core. */
#define TTY_INPUT_QUEUE_CAPACITY 512u
#define TTY_EDIT_LINE_CAPACITY 128u

#define TTY_INPUT_NONE 0
#define TTY_INPUT_INTERRUPT 1
#define TTY_INPUT_LINE_READY 2
#define TTY_INPUT_DATA_READY 3
#define TTY_READ_EMPTY (-2)
#define TTY_READ_BLOCKED (-3)
#define TTY_ERROR (-1)

#define TTY_ATTR_ECHO 0x01u
#define TTY_ATTR_ISIG 0x02u
#define TTY_ATTR_ICANON 0x04u
#define TTY_ATTR_TOSTOP 0x08u
#define TTY_ATTR_SUPPORTED (TTY_ATTR_ECHO | TTY_ATTR_ISIG | TTY_ATTR_ICANON | TTY_ATTR_TOSTOP)
#define TTY_VMIN_MAX 255u
#define TTY_VTIME_MAX 255u

void tty_init(void);

/* A session leader may claim the console TTY once for its session. */
int32_t tty_claim_session(uint32_t sid, uint32_t leader_pid);
int32_t tty_release_session(uint32_t sid);
uint32_t tty_session_owner(void);
int32_t tty_get_foreground_pgid(uint32_t sid);
int32_t tty_set_foreground_pgid(uint32_t sid, uint32_t pgid);

/*
 * Feed translated characters from an input backend. This API performs
 * canonical editing or byte-oriented input, echo, and foreground Ctrl-C
 * signal routing; it does not own PS/2/xHCI devices.
 */
int32_t tty_input_byte(uint8_t byte);

/* Nonblocking read retained for the explicit TTY syscall. */
int32_t tty_read(uint32_t sid, uint32_t caller_pid,
                 uint8_t *destination, uint32_t capacity);
/* VFS stdin read: sleeps atomically until input arrives or the session ends. */
int32_t tty_read_blocking(uint32_t sid, uint32_t caller_pid,
                          uint8_t *destination, uint32_t capacity);
int32_t tty_write(uint32_t sid, uint32_t caller_pid,
                  const uint8_t *source, uint32_t length);
int32_t tty_get_attributes(uint32_t sid, uint32_t *flags, uint32_t *vmin, uint32_t *vtime);
int32_t tty_set_attributes(uint32_t sid, uint32_t flags, uint32_t vmin, uint32_t vtime);
/* Flush queued input echo from process context, never from a keyboard IRQ. */
void tty_flush_echo(uint32_t sid);
/* Re-check blocked readers after the foreground process group changes. */
void tty_wake_readers(void);

#endif
