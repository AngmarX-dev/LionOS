#ifndef LIONOS_PIPE_H
#define LIONOS_PIPE_H
#include <stdint.h>
#define LIONOS_PIPE_MAX 16u
#define LIONOS_PIPE_BUFFER 1024u
#define PIPE_BLOCKED (-2)
struct lion_pipe_pair { uint32_t read_end; uint32_t write_end; };
void pipe_init(void);
int32_t pipe_create(uint32_t owner_pid,uint32_t *read_end,uint32_t *write_end);
int32_t pipe_read(uint32_t handle,void *data,uint32_t capacity);
int32_t pipe_write(uint32_t handle,const void *data,uint32_t length);
int32_t pipe_close(uint32_t handle);
#endif
