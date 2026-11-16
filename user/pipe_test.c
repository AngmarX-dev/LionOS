#include <stdint.h>
#include "user_api.h"
#include "user_libc.h"

int main(void) {
    uint32_t read_end = 0u;
    uint32_t write_end = 0u;
    static const char message[] = "pipe fork inheritance";
    char buffer[sizeof(message)];
    uint32_t child;
    int32_t status = -1;

    if (lion_pipe_create(&read_end, &write_end) != 0) return 1;
    child = lion_fork();
    if (child == (uint32_t)-1) return 2;

    if (child == 0u) {
        if (lion_pipe_close(read_end) != 0) lion_exit_code(3u);
        if (lion_pipe_write(write_end, message, sizeof(message)) !=
            (int32_t)sizeof(message)) lion_exit_code(4u);
        /* Process exit must release this inherited writer automatically. */
        lion_exit_code(0u);
    }

    if (lion_pipe_close(write_end) != 0) return 5;
    int32_t read_result = lion_pipe_read(read_end, buffer, sizeof(buffer));
    if (read_result != (int32_t)sizeof(message)) {
        int32_t child_status = -1;
        (void)lion_pipe_close(read_end);
        if (lion_waitpid(child, &child_status) == (int32_t)child &&
            child_status >= 0 && child_status <= 20)
            return 40 + child_status;
        if (read_result == 0) return 60;
        if (read_result == -1) return 61;
        if (read_result == -2) return 62;
        return 63;
    }
    if (memcmp(buffer, message, sizeof(message)) != 0) return 7;
    if (lion_pipe_read(read_end, buffer, sizeof(buffer)) != 0) return 8;
    if (lion_pipe_close(read_end) != 0) return 9;
    if (lion_waitpid(child, &status) != (int32_t)child || status != 0)
        return 10;

    puts("PIPE TEST: PASS");
    return 0;
}
