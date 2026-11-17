#ifndef LIONOS_TTY_H
#define LIONOS_TTY_H

#include <stdint.h>

/* One firmware-console TTY is exposed by this initial software core. */
#define TTY_INPUT_QUEUE_CAPACITY 512u
#define TTY_EDIT_LINE_CAPACITY 128u

#define TTY_INPUT_NONE 0
#define TTY_INPUT_INTERRUPT 1
#define TTY_INPUT_LINE_READY 2
#define TTY_READ_EMPTY (-2)
#define TTY_READ_BLOCKED (-3)
#define TTY_ERROR (-1)

void tty_init(void);

/* A session leader may claim the console TTY once for its session. */
int32_t tty_claim_session(uint32_t sid, uint32_t leader_pid);
int32_t tty_release_session(uint32_t sid);
uint32_t tty_session_owner(void);
int32_t tty_get_foreground_pgid(uint32_t sid);
int32_t tty_set_foreground_pgid(uint32_t sid, uint32_t pgid);

/*
 * Feed translated characters from an input backend. This API performs
 * canonical editing only; it does not own PS/2/xHCI devices or deliver
 * process signals.
 */
int32_t tty_input_byte(uint8_t byte);

/* Nonblocking read retained for the explicit TTY syscall. */
int32_t tty_read(uint32_t sid, uint8_t *destination, uint32_t capacity);
/* VFS stdin read: sleeps atomically until a line arrives or the session ends. */
int32_t tty_read_blocking(uint32_t sid, uint8_t *destination, uint32_t capacity);
int32_t tty_write(uint32_t sid, const uint8_t *source, uint32_t length);

#endif
