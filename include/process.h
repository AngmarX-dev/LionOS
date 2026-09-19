#ifndef LIONOS_PROCESS_H
#define LIONOS_PROCESS_H

#include <stdint.h>

#define LIONOS_PROCESS_MAX 16u
#define LIONOS_PROCESS_MAX_USER_PAGES 128u
#define PROCESS_UNUSED 0u
#define PROCESS_READY 1u
#define PROCESS_RUNNING 2u
#define PROCESS_ZOMBIE 3u
#define PROCESS_WAITING 4u
#define PROCESS_STOPPED 5u
#define PROCESS_CONTEXT_WORDS 19u
#define PROCESS_FD_MAX 32u
#define PROCESS_FD_PATH_MAX 64u
#define PROCESS_CAP_CONSOLE (1u<<0)
#define PROCESS_CAP_FS      (1u<<1)
#define PROCESS_CAP_PROCESS (1u<<2)
#define PROCESS_CAP_IPC     (1u<<3)
#define PROCESS_CAP_NET     (1u<<4)
#define PROCESS_CAP_ADMIN   (1u<<31)
#define PROCESS_CAP_USER_DEFAULT (PROCESS_CAP_CONSOLE|PROCESS_CAP_FS|PROCESS_CAP_PROCESS|PROCESS_CAP_IPC|PROCESS_CAP_NET)

struct process {
    uint32_t pid;
    uint32_t state;
    uint32_t parent_pid;
    uint32_t exit_code;
    uint32_t wait_pid;
    uint32_t wait_status_ptr;
    uint32_t reap_pending;
    uint32_t deferred_kernel_stack;
    uint32_t page_directory;
    uint32_t entry;
    uint32_t user_stack;
    uint32_t kernel_stack_top;
    uint32_t saved_frame;
    uint32_t user_code_page;
    uint32_t user_stack_page;
    uint32_t user_pages[LIONOS_PROCESS_MAX_USER_PAGES];
    uint32_t user_page_vas[LIONOS_PROCESS_MAX_USER_PAGES];
    uint32_t user_page_count;
    uint32_t pending_signals;
    uint32_t capabilities;
    uint8_t fd_used[PROCESS_FD_MAX];
    uint8_t fd_backend[PROCESS_FD_MAX];
    uint32_t fd_flags[PROCESS_FD_MAX];
    uint32_t fd_offset[PROCESS_FD_MAX];
    char fd_path[PROCESS_FD_MAX][PROCESS_FD_PATH_MAX];
};

void process_init(void);
struct process *process_current(void);
struct process *process_at(uint32_t index);
const char *process_state_name(uint32_t state);
uint32_t process_current_pid(void);
struct process *process_create(uint32_t entry, uint32_t user_stack, uint32_t page_directory,
                               uint32_t user_code_page, uint32_t user_stack_page);
struct process *process_create_ex(uint32_t entry, uint32_t user_stack, uint32_t page_directory,
                                  const uint32_t *user_pages, uint32_t user_page_count);
struct process *process_create_ex_vas(uint32_t entry, uint32_t user_stack, uint32_t page_directory,
                                      const uint32_t *user_pages, const uint32_t *user_page_vas,
                                      uint32_t user_page_count);
int process_exec_replace_current(uint32_t entry, uint32_t user_stack, uint32_t page_directory,
                                 const uint32_t *user_pages, const uint32_t *user_page_vas,
                                 uint32_t user_page_count);
int process_set_current(struct process *process);
int process_has_capability(const struct process *process, uint32_t capability);
uint32_t process_capabilities(const struct process *process);
int process_set_capabilities(struct process *process, uint32_t capabilities);
void process_exit_current(uint32_t exit_code);
uint32_t process_fork_current(uint32_t *parent_frame);
int32_t process_waitpid(uint32_t pid, uint32_t status_ptr);
uint32_t process_count(void);
uint32_t *process_schedule(uint32_t *frame);
void process_set_saved_frame(struct process *process, uint32_t *frame);
uint32_t *process_saved_frame(struct process *process);
uint32_t process_kernel_stack_top(struct process *process);
int process_is_descendant_or_child(uint32_t pid, uint32_t ancestor_pid);
void scheduler_idle(void);

#endif
