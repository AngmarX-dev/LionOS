#ifndef LIONOS_PROCESS_H
#define LIONOS_PROCESS_H

#include <stdint.h>
#include "spinlock.h"

#define LIONOS_PROCESS_MAX 16u
#define LIONOS_PROCESS_MAX_USER_PAGES 128u
#define PROCESS_UNUSED 0u
#define PROCESS_READY 1u
#define PROCESS_RUNNING 2u
#define PROCESS_ZOMBIE 3u
#define PROCESS_WAITING 4u
#define PROCESS_WAIT_BLOCKED (-2)
#define PROCESS_STOPPED 5u
#define PROCESS_CONTEXT_WORDS 19u
#define PROCESS_FD_MAX 32u
#define PROCESS_FD_PATH_MAX 64u
#define PROCESS_PRIORITY_LEVELS 4u
#define PROCESS_PRIORITY_DEFAULT 2u
#define PROCESS_CAP_CONSOLE (1u<<0)
#define PROCESS_CAP_FS      (1u<<1)
#define PROCESS_CAP_PROCESS (1u<<2)
#define PROCESS_CAP_IPC     (1u<<3)
#define PROCESS_CAP_NET     (1u<<4)
#define PROCESS_CAP_ADMIN   (1u<<31)
#define PROCESS_MAX_THREADS 32u
#define PROCESS_THREAD_STACK_BASE 0xBFFE0000u
#define PROCESS_THREAD_STACK_STRIDE 0x2000u
#define PROCESS_CAP_USER_DEFAULT (PROCESS_CAP_CONSOLE|PROCESS_CAP_FS|PROCESS_CAP_PROCESS|PROCESS_CAP_IPC|PROCESS_CAP_NET)

struct process {
    uint32_t pid;
    uint32_t state;
    uint32_t parent_pid;
    uint32_t adopted_by_init;
    uint32_t pgid;
    uint32_t sid;
    uint32_t exit_code;
    uint32_t wait_pid;
    uint32_t wait_status_ptr;
    uint32_t wait_options;
    uint32_t stop_event_pending;
    uint32_t stop_signal;
    uint32_t continued_event_pending;
    uint32_t wait_channel;
    uint32_t wait_kind;
    uint32_t wait_frame_saved;
    uint32_t wait_timeout_active;
    uint32_t wait_timeout_ticks;
    uint32_t wait_deadline;
    uint32_t wait_cpu;
    uint32_t wait_timeout_result;
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
    uint32_t signal_handlers[32];
    uint32_t signal_restorers[32];
    uint32_t signal_saved_frame[PROCESS_CONTEXT_WORDS];
    uint32_t signal_active;
    uint32_t signal_active_restorer;
    uint32_t capabilities;
    uint32_t cpu_owner;
    uint32_t migration_count;
    uint32_t migration_eligible;
    uint32_t priority;
    uint32_t base_priority;
    uint32_t time_slice;
    uint32_t ticks_used;
    uint8_t fpu_state[512] __attribute__((aligned(16)));
    uint32_t fpu_valid;
    uint32_t tid;
    uint32_t thread_group_pid;
    uint32_t is_thread;
    uint32_t owns_address_space;
    uint32_t thread_stack_page;
    uint32_t thread_stack_va;
    uint32_t join_tid;
    uint32_t join_status_ptr;
    uint32_t pipe_read_mask;
    uint32_t pipe_write_mask;
    uint32_t fd_closing;
    uint8_t fd_used[PROCESS_FD_MAX];
    uint32_t fd_object[PROCESS_FD_MAX];
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
int32_t process_get_pgid(uint32_t pid);
int32_t process_get_sid(uint32_t pid);
int32_t process_tty_set_foreground(uint32_t sid, uint32_t pgid);
int32_t process_tty_signal_foreground(uint32_t sid, uint32_t pgid, uint32_t signal);
/* Returns 0 for foreground, 1 after queuing SIGTTIN/SIGTTOU, or -1 on invalid state. */
int32_t process_tty_check_foreground(uint32_t sid, uint32_t caller_pid, uint32_t signal);
int32_t process_create_session(void);
int32_t process_set_pgid(uint32_t pid, uint32_t pgid);
struct process *process_pipe_owner(void);
struct process *process_fd_owner(void);
uint32_t process_current_tid(void);
struct process *process_create_kernel_thread(uint32_t entry, uint32_t arg);
struct process *process_create_kernel_thread_on_cpu(uint32_t entry, uint32_t arg, uint32_t cpu);
uint32_t process_create_user_thread(uint32_t entry);
void process_exit_thread(uint32_t exit_code);
int32_t process_join_thread(uint32_t tid, uint32_t status_ptr);
struct process *process_create(uint32_t entry, uint32_t user_stack, uint32_t page_directory,
                               uint32_t user_code_page, uint32_t user_stack_page);
struct process *process_create_ex(uint32_t entry, uint32_t user_stack, uint32_t page_directory,
                                  const uint32_t *user_pages, uint32_t user_page_count);
struct process *process_create_ex_vas(uint32_t entry, uint32_t user_stack, uint32_t page_directory,
                                      const uint32_t *user_pages, const uint32_t *user_page_vas,
                                      uint32_t user_page_count);
/* Return the PID captured before the process becomes READY. */
int process_create_ex_vas_pid(uint32_t entry, uint32_t user_stack, uint32_t page_directory,
                              const uint32_t *user_pages, const uint32_t *user_page_vas,
                              uint32_t user_page_count, uint32_t migration_eligible,
                              uint32_t *pid_out);
int process_exec_replace_current(uint32_t entry, uint32_t user_stack, uint32_t page_directory,
                                 const uint32_t *user_pages, const uint32_t *user_page_vas,
                                 uint32_t user_page_count);
int process_set_current(struct process *process);
int process_has_capability(const struct process *process, uint32_t capability);
uint32_t process_capabilities(const struct process *process);
int process_set_capabilities(struct process *process, uint32_t capabilities);
void process_exit_current(uint32_t exit_code);
/* Resolve a user COW fault and update the owning process's page references. */
int process_resolve_cow(uint32_t virtual_address);
/* Delivers a pending catchable signal at a syscall return boundary. */
int process_deliver_pending_signal(uint32_t *frame);
int process_signal_return(uint32_t *frame);
uint32_t process_fork_current(uint32_t *parent_frame);
int32_t process_waitpid(uint32_t pid, uint32_t status_ptr);
int32_t process_waitpid_options(uint32_t pid, uint32_t status_ptr, uint32_t options);
/* sleep_on is called with held_lock already acquired and interrupts disabled.
 * It atomically publishes PROCESS_WAITING before releasing held_lock. */
int32_t process_sleep_on(uintptr_t channel, struct spinlock *held_lock, uint32_t irq_flags);
int32_t process_sleep_on_timeout(uintptr_t channel, struct spinlock *held_lock,
                                 uint32_t irq_flags, uint32_t timeout_ticks);
int32_t process_consume_timeout(void);
int32_t process_futex_sleep_on(uintptr_t channel, struct spinlock *held_lock, uint32_t irq_flags);
int32_t process_futex_sleep_on_timeout(uintptr_t channel, struct spinlock *held_lock,
                                       uint32_t irq_flags, uint32_t timeout_ticks);
void process_futex_tick(uint32_t cpu);
void process_scheduler_tick(uint32_t cpu);
void    process_wakeup(uintptr_t channel);
uint32_t process_futex_wakeup(uintptr_t channel, uint32_t maximum);
uint32_t process_count(void);
uint32_t *process_schedule(uint32_t *frame);
void process_set_saved_frame(struct process *process, uint32_t *frame);
void process_request_reschedule(void);
int process_set_priority(struct process *process, uint32_t priority);
uint32_t process_scheduler_ticks(uint32_t cpu);
uint32_t process_scheduler_steals(uint32_t cpu);
/* Successful READY transfers into a destination with a running task. */
uint32_t process_scheduler_busy_balances(uint32_t cpu);
/* Test-only scheduler instrumentation for cross-CPU execution exclusion. */
int process_scheduler_execution_guard_enable(void);
/* During guarded tests: zero disables idle steals; UINT32_MAX allows all CPUs. */
int process_scheduler_execution_guard_set_idle_steal_mask(uint32_t cpu_mask);
int process_scheduler_execution_guard_finish(void);
uint32_t *process_saved_frame(struct process *process);
uint32_t process_kernel_stack_top(struct process *process);
int process_is_descendant_or_child(uint32_t pid, uint32_t ancestor_pid);
int process_get_exit_code(uint32_t pid, uint32_t *code);
int process_get_migration_info(uint32_t pid, uint32_t *cpu_owner, uint32_t *migration_count);
int process_set_migration_eligible(uint32_t pid);
int process_set_migration_eligible_batch(const uint32_t *pids, uint32_t count);
void process_request_reschedule_cpu(uint32_t cpu);
/* 0=reaped, 1=still current on a CPU (retry), -1=invalid/not zombie. */
int process_reap_pid(uint32_t pid);
/* 1=captured kernel-thread slot cleared/reused, 0=still present, -1=invalid. */
int process_kernel_thread_reaped(const struct process *thread, uint32_t tid);
void scheduler_idle(void);

#endif
