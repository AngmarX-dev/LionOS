#include <stdint.h>
#include "memory.h"
#include "paging.h"
#include "process.h"
#include "uapi.h"
#include "signal.h"
#include "tss.h"
#include "spinlock.h"
#include "cpu.h"
#include "debug.h"
#include "pipe.h"
#include "vfs.h"
#include "tty.h"
#include "usercopy.h"
#include "smp.h"
#include "lapic.h"
#include "runqueue.h"
#if PROCESS_PRIORITY_LEVELS != LIONOS_RUNQUEUE_PRIORITY_LEVELS
#error "process and runqueue priority levels must match"
#endif
_Static_assert(LIONOS_PROCESS_MAX >= LIONOS_MAX_CPUS,
               "AP lifecycle probe requires one non-bootstrap slot per CPU");
extern void lionos_kernel_idle(void);

#define PROCESS_WAIT_ANY 0u
#define PROCESS_WAIT_BLOCKED (-2)
#define PROCESS_SYSCALL_ERR 0xFFFFFFFFu
#define PAGE_SIZE 4096u
#define USER_CODE_SEL 0x2Bu
#define USER_DATA_SEL 0x33u

static struct process processes[LIONOS_PROCESS_MAX];
static struct runqueue runqueues[LIONOS_MAX_CPUS];
static struct runqueue_node runqueue_nodes[LIONOS_PROCESS_MAX];
static volatile uint32_t runqueue_fault;
static volatile uint32_t runqueue_fault_reported;
static volatile uint32_t process_scheduler_ready;
static volatile uint32_t scheduler_ap_probe_expected;
static volatile uint32_t scheduler_ap_probe_seen;
static volatile uint32_t scheduler_ap_probe_fault;
static volatile uint32_t scheduler_ap_probe_reported;
static volatile uint32_t scheduler_ap_probe_reaped;
static volatile uint32_t scheduler_ap_probe_expected_count;
static struct spinlock scheduler_ap_probe_wait_locks[LIONOS_MAX_CPUS];
static uint32_t scheduler_ap_probe_wait_token;
static uint32_t scheduler_ap_probe_timeout_token;
static volatile uint32_t scheduler_ap_probe_timeout_seen;
static volatile uint32_t smp_ap_schedule_enter_seen;
static volatile uint32_t smp_ap_schedule_lock_contention_seen;

static void process_kernel_thread_return_trap(void){
    process_exit_current(0xFFFFFFFFu);
    for(;;)__asm__ volatile("sti; hlt");
}

static void process_scheduler_ap_probe(uint32_t expected_cpu){
    if(expected_cpu==0u||expected_cpu>=LIONOS_MAX_CPUS){
        __atomic_store_n(&scheduler_ap_probe_fault,1u,__ATOMIC_RELEASE);
        process_exit_current(1u);
        for(;;)__asm__ volatile("sti; hlt");
    }
    /*
     * Each AP owns its probe wait lock. Waking every AP at once must not make
     * all probe tasks contend on one unfair test-only spinlock with interrupts
     * disabled; that can starve some CPUs and hide scheduler progress.
     */
    struct spinlock *probe_wait_lock=&scheduler_ap_probe_wait_locks[expected_cpu];
    uint32_t wait_flags=spinlock_irqsave_acquire(probe_wait_lock);
    int32_t wait_result=process_sleep_on(
        (uintptr_t)&scheduler_ap_probe_wait_token,
        probe_wait_lock,wait_flags);
    if(wait_result!=PROCESS_WAIT_BLOCKED){
        __atomic_store_n(&scheduler_ap_probe_fault,1u,__ATOMIC_RELEASE);
        process_exit_current(1u);
        for(;;)__asm__ volatile("sti; hlt");
    }

    /*
     * The BSP wakes this task after it has entered WAITING. STI followed by
     * HLT is an atomic interruptible handoff on x86: the AP remains asleep
     * until its owner-local reschedule IPI selects this task again.
     */
    __asm__ volatile("sti; hlt" ::: "memory");
    /*
     * Exercise the AP-local timer timeout path after the remote wake. The
     * scheduler arms the deadline when this task first switches out in
     * WAITING; only that AP's Local APIC timer may expire it.
     */
    uint32_t timeout_flags=spinlock_irqsave_acquire(probe_wait_lock);
    int32_t timeout_wait=process_sleep_on_timeout(
        (uintptr_t)&scheduler_ap_probe_timeout_token,
        probe_wait_lock,timeout_flags,2u);
    if(timeout_wait!=PROCESS_WAIT_BLOCKED){
        __atomic_store_n(&scheduler_ap_probe_fault,1u,__ATOMIC_RELEASE);
        process_exit_current(2u);
        for(;;)__asm__ volatile("sti; hlt");
    }
    __asm__ volatile("sti; hlt" ::: "memory");
    int32_t timeout_result=process_consume_timeout();
    int32_t timeout_consumed_twice=process_consume_timeout();

    uint32_t actual_cpu=cpu_current_index();
    if(actual_cpu!=expected_cpu||actual_cpu==0u||
       actual_cpu>=LIONOS_MAX_CPUS||timeout_result!=1||
       timeout_consumed_twice!=0){
        __atomic_store_n(&scheduler_ap_probe_fault,1u,__ATOMIC_RELEASE);
    }else{
        uint32_t bit=1u<<actual_cpu;
        __atomic_fetch_or(&scheduler_ap_probe_timeout_seen,bit,__ATOMIC_ACQ_REL);
        __atomic_fetch_or(&scheduler_ap_probe_seen,bit,__ATOMIC_ACQ_REL);
    }
    process_exit_current(0u);
    for(;;)__asm__ volatile("sti; hlt");
}

/* Defined after the run-queue validator so reporting can validate every CPU
   queue while process_lock protects process states and membership metadata. */
static void process_scheduler_ap_probe_report(void);
static struct process *current_by_cpu[LIONOS_MAX_CPUS];
static struct process scheduler_idle_processes[LIONOS_MAX_CPUS];
static volatile uint32_t scheduler_idle_entered_mask;
static volatile uint32_t scheduler_idle_reported;
static uint32_t scheduler_idle_ready_mask;
static uint32_t scheduler_idle_expected_mask;
struct scheduler_cpu_state{uint32_t cursor;uint32_t schedule_count;uint32_t steal_count;uint32_t timer_ticks;};
static struct scheduler_cpu_state scheduler_cpu[LIONOS_MAX_CPUS];
static uint32_t next_pid;
static uint32_t next_tid;
static struct spinlock process_lock;
static volatile uint32_t reschedule_requested[LIONOS_MAX_CPUS];
static volatile uint32_t reschedule_ipi_pending[LIONOS_MAX_CPUS];
static volatile uint32_t reschedule_blocked[LIONOS_MAX_CPUS];
static uint32_t fpu_enabled;

static void reap_process(struct process *p);
static struct process *find_pid(uint32_t pid);
static int process_group_orphaned_locked(uint32_t pgid,uint32_t sid);
static int process_group_has_stopped_locked(uint32_t pgid,uint32_t sid);
static void process_orphan_group_notify_locked(uint32_t pgid,uint32_t sid);
static void process_notify_orphan_transition_locked(uint32_t pgid,uint32_t sid,int was_orphaned);
struct process_group_snapshot {
    uint32_t pgid;
    uint32_t sid;
    uint8_t was_orphaned;
};
static uint32_t process_snapshot_child_groups_locked(uint32_t parent_pid,
    struct process_group_snapshot *groups,uint32_t capacity);
static void process_notify_child_group_transitions_locked(
    const struct process_group_snapshot *groups,uint32_t count);
static int process_has_interrupting_signal_locked(const struct process *p);
static void process_clear_waitpid_locked(struct process *p);
static void process_diag_status33(const char *kind,uint32_t pid,
                                  uint32_t parent_pid,uint32_t value);
static uint8_t fpu_initial_state[512] __attribute__((aligned(16)));

static uint32_t quantum_for_priority(uint32_t priority){
    static const uint32_t q[PROCESS_PRIORITY_LEVELS]={2u,4u,8u,12u};
    return q[priority<PROCESS_PRIORITY_LEVELS?priority:PROCESS_PRIORITY_LEVELS-1u];
}
static void fpu_save(struct process *p){if(!fpu_enabled||!p)return;__asm__ volatile("fxsave %0":"=m"(p->fpu_state)::"memory");p->fpu_valid=1u;}
static void fpu_restore(struct process *p){if(!fpu_enabled||!p)return;if(p->fpu_valid)__asm__ volatile("fxrstor %0"::"m"(p->fpu_state):"memory");else __asm__ volatile("fxrstor %0"::"m"(fpu_initial_state):"memory");}

static struct process *current_local(void) { uint32_t cpu = cpu_current_index(); return cpu < LIONOS_MAX_CPUS ? current_by_cpu[cpu] : 0; }
static void set_current_local(struct process *p) { uint32_t cpu = cpu_current_index(); if (cpu < LIONOS_MAX_CPUS) current_by_cpu[cpu] = p; }
static void process_request_blocked_reschedule(uint32_t cpu){
    if(cpu>=LIONOS_MAX_CPUS)cpu=0u;
    __atomic_store_n(&reschedule_blocked[cpu],1u,__ATOMIC_RELEASE);
    __atomic_store_n(&reschedule_requested[cpu],1u,__ATOMIC_RELEASE);
}
/*
 * A CPU-local run queue can receive work while its owner is halted.  Do not
 * rely on the next periodic timer interrupt to make that CPU runnable: send
 * the scheduler IPI after queue publication and after releasing process_lock.
 */
static void process_kick_cpu(uint32_t cpu){
    if(cpu>=LIONOS_MAX_CPUS)return;
    uint32_t apic_id=0xFFFFFFFFu;
    if(cpu==cpu_current_index()){
        apic_id=lapic_id();
    }else{
        const struct cpu_info *target=cpu_get(cpu);
        if(!target||__atomic_load_n(&target->online,__ATOMIC_ACQUIRE)==0u)
            return;
        apic_id=target->apic_id;
    }
    if(apic_id==0xFFFFFFFFu)return;
    __atomic_store_n(&reschedule_requested[cpu],1u,__ATOMIC_RELEASE);
    lapic_send_ipi(apic_id,LIONOS_LAPIC_RESCHEDULE_VECTOR);
}
static struct process *find_free_slot(void) { for (uint32_t i=0;i<LIONOS_PROCESS_MAX;++i) if (processes[i].state==PROCESS_UNUSED) return &processes[i]; return 0; }
static uint32_t process_index(const struct process *p) { return (uint32_t)(p-processes); }
static int process_slot_index(const struct process *p,uint32_t *index){
    uintptr_t base=(uintptr_t)&processes[0],address=(uintptr_t)p;
    uintptr_t span=sizeof(processes);
    if(!p||!index||address<base||address-base>=span||
       (address-base)%sizeof(processes[0])!=0u)return -1;
    *index=(uint32_t)((address-base)/sizeof(processes[0]));
    return 0;
}

/* process_lock is always acquired before a per-CPU run-queue lock. */
static int process_transition_state_locked(struct process *p,uint32_t state){
    if(!p)return -1;
    uint32_t index;
    if(process_slot_index(p,&index)!=0)return -1;
    uint32_t old_state=p->state;
    if(old_state==state)return 0;
    if(old_state==PROCESS_READY){
        uint32_t owner=runqueue_nodes[index].queue_cpu;
        if(owner>=LIONOS_MAX_CPUS){__atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);return -1;}
        spinlock_acquire(&runqueues[owner].lock);
        int result=runqueue_dequeue_locked(&runqueues[owner],runqueue_nodes,
                                           LIONOS_PROCESS_MAX,index);
        spinlock_release(&runqueues[owner].lock);
        if(result!=0){__atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);return -1;}
    }
    if(state==PROCESS_READY){
        uint32_t online=smp_online_count();
        if(online==0u||online>LIONOS_MAX_CPUS)online=1u;
        uint32_t owner=p->cpu_owner;
        if(owner>=online){
            uint32_t current=cpu_current_index();
            owner=current<online?current:0u;
            p->cpu_owner=owner;
        }
        spinlock_acquire(&runqueues[owner].lock);
        int result=runqueue_enqueue_locked(&runqueues[owner],runqueue_nodes,
                                           LIONOS_PROCESS_MAX,index,p->priority);
        spinlock_release(&runqueues[owner].lock);
        if(result!=0){__atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);return -1;}
    }
    p->state=state;
    return 0;
}
static int process_runqueue_validate_locked(uint32_t cpu){
    if(cpu>=LIONOS_MAX_CPUS||
       __atomic_load_n(&runqueue_fault,__ATOMIC_ACQUIRE)!=0u)return -1;
    spinlock_acquire(&runqueues[cpu].lock);
    int valid=runqueue_validate_locked(&runqueues[cpu],runqueue_nodes,
                                       LIONOS_PROCESS_MAX,0);
    uint32_t queue_total=runqueues[cpu].total;
    spinlock_release(&runqueues[cpu].lock);
    if(valid!=0)return -1;

    uint32_t ready_total=0u;
    for(uint32_t i=0u;i<LIONOS_PROCESS_MAX;++i){
        const struct process *p=&processes[i];
        const struct runqueue_node *node=&runqueue_nodes[i];
        if(p->state==PROCESS_READY){
            if(node->queued!=1u||node->queue_cpu>=LIONOS_MAX_CPUS||
               node->queue_cpu!=p->cpu_owner||node->priority!=p->priority)
                return -1;
            if(node->queue_cpu==cpu)++ready_total;
        }else if(node->queued!=0u||
                 node->next!=LIONOS_RUNQUEUE_INVALID_INDEX||
                 node->previous!=LIONOS_RUNQUEUE_INVALID_INDEX||
                 node->queue_cpu!=LIONOS_RUNQUEUE_INVALID_INDEX){
            return -1;
        }
    }
    return queue_total==ready_total?0:-1;
}

static volatile uint32_t scheduler_ap_probe_diag_reported;

static void process_scheduler_ap_probe_diag_hex(uint32_t value){
    static const char digits[]="0123456789ABCDEF";
    char text[9];
    for(uint32_t i=0u;i<8u;++i)
        text[i]=digits[(value>>(28u-i*4u))&0xFu];
    text[8]='\0';
    debug_write(text);
}

static void process_scheduler_ap_probe_report(void){
    if(cpu_current_index()!=0u)return;
    uint32_t expected=__atomic_load_n(&scheduler_ap_probe_expected,__ATOMIC_ACQUIRE);
    if(!expected)return;

    /*
     * Do not wake a probe merely because it published WAITING: first prove
     * that every target AP has switched to its own idle context. This makes
     * the test exercise a real remote wake from HLT, not a READY/RUNNING race.
     */
    uint32_t all_aps_idle=1u;
    if(spinlock_try_acquire(&process_lock)){
        uint32_t online=smp_online_count();
        if(online>LIONOS_MAX_CPUS)online=LIONOS_MAX_CPUS;
        for(uint32_t cpu=1u;cpu<online;++cpu){
            if(!(expected&(1u<<cpu)))continue;
            struct process *probe=0;
            for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
                struct process *candidate=&processes[i];
                if(candidate->state!=PROCESS_UNUSED&&candidate->is_thread&&
                   candidate->thread_group_pid==0u&&
                   candidate->entry==(uint32_t)(uintptr_t)&process_scheduler_ap_probe&&
                   candidate->cpu_owner==cpu){
                    probe=candidate;
                    break;
                }
            }
            if(!probe||probe->state!=PROCESS_WAITING||
               probe->wait_channel!=(uint32_t)(uintptr_t)&scheduler_ap_probe_wait_token||
               current_by_cpu[cpu]!=&scheduler_idle_processes[cpu]){
                all_aps_idle=0u;
                break;
            }
        }
        spinlock_release(&process_lock);
    }else{
        all_aps_idle=0u;
    }
    if(all_aps_idle)
        process_wakeup((uintptr_t)&scheduler_ap_probe_wait_token);

    /*
     * If every AP has fallen back to idle but the probes are not all waiting,
     * capture one coherent scheduler snapshot. This is diagnostic-only: it
     * neither changes task state nor relaxes the 16-CPU acceptance checks.
     */
    uint32_t idle_mask=__atomic_load_n(&scheduler_idle_entered_mask,
                                       __ATOMIC_ACQUIRE);
    if(!all_aps_idle&&scheduler_idle_expected_mask!=0u&&
       (idle_mask&scheduler_idle_expected_mask)==scheduler_idle_expected_mask){
        uint32_t expected_diag=0u;
        if(__atomic_compare_exchange_n(&scheduler_ap_probe_diag_reported,
                &expected_diag,1u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
            uint32_t snapshot[LIONOS_MAX_CPUS][8];
            uint32_t snapshot_count=0u;
            if(spinlock_try_acquire(&process_lock)){
                uint32_t online=smp_online_count();
                if(online>LIONOS_MAX_CPUS)online=LIONOS_MAX_CPUS;
                for(uint32_t cpu=1u;cpu<online;++cpu){
                    struct process *probe=0;
                    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
                        struct process *candidate=&processes[i];
                        if(candidate->state!=PROCESS_UNUSED&&candidate->is_thread&&
                           candidate->thread_group_pid==0u&&
                           candidate->entry==(uint32_t)(uintptr_t)&process_scheduler_ap_probe&&
                           candidate->cpu_owner==cpu){
                            probe=candidate;
                            break;
                        }
                    }
                    uint32_t queue_total=0u;
                    uint32_t queue_locked=spinlock_try_acquire(&runqueues[cpu].lock);
                    if(queue_locked){
                        queue_total=runqueues[cpu].total;
                        spinlock_release(&runqueues[cpu].lock);
                    }
                    snapshot[snapshot_count][0]=cpu;
                    snapshot[snapshot_count][1]=probe?probe->state:0xFFFFFFFFu;
                    snapshot[snapshot_count][2]=probe?probe->cpu_owner:0xFFFFFFFFu;
                    snapshot[snapshot_count][3]=probe?runqueue_nodes[process_index(probe)].queued:0xFFFFFFFFu;
                    snapshot[snapshot_count][4]=queue_locked?queue_total:0xFFFFFFFFu;
                    snapshot[snapshot_count][5]=__atomic_load_n(&scheduler_cpu[cpu].timer_ticks,__ATOMIC_ACQUIRE);
                    snapshot[snapshot_count][6]=scheduler_cpu[cpu].schedule_count;
                    snapshot[snapshot_count][7]=__atomic_load_n(&reschedule_requested[cpu],__ATOMIC_ACQUIRE);
                    ++snapshot_count;
                }
                spinlock_release(&process_lock);
                debug_write("LIONOS:SMP-AP-RUNQUEUE-DIAG-BEGIN\n");
                for(uint32_t i=0u;i<snapshot_count;++i){
                    debug_write("CPU=");process_scheduler_ap_probe_diag_hex(snapshot[i][0]);
                    debug_write(" STATE=");process_scheduler_ap_probe_diag_hex(snapshot[i][1]);
                    debug_write(" OWNER=");process_scheduler_ap_probe_diag_hex(snapshot[i][2]);
                    debug_write(" QUEUED=");process_scheduler_ap_probe_diag_hex(snapshot[i][3]);
                    debug_write(" QTOTAL=");process_scheduler_ap_probe_diag_hex(snapshot[i][4]);
                    debug_write(" TICKS=");process_scheduler_ap_probe_diag_hex(snapshot[i][5]);
                    debug_write(" SCHEDULES=");process_scheduler_ap_probe_diag_hex(snapshot[i][6]);
                    debug_write(" RESCHED=");process_scheduler_ap_probe_diag_hex(snapshot[i][7]);
                    debug_write("\n");
                }
                debug_write("LIONOS:SMP-AP-RUNQUEUE-DIAG-END\n");
            }else{
                __atomic_store_n(&scheduler_ap_probe_diag_reported,0u,__ATOMIC_RELEASE);
            }
        }
    }

    uint32_t fault=__atomic_load_n(&scheduler_ap_probe_fault,__ATOMIC_ACQUIRE);
    uint32_t seen=__atomic_load_n(&scheduler_ap_probe_seen,__ATOMIC_ACQUIRE);
    uint32_t timeout_seen=__atomic_load_n(&scheduler_ap_probe_timeout_seen,
                                           __ATOMIC_ACQUIRE);
    uint32_t expected_count=__atomic_load_n(&scheduler_ap_probe_expected_count,
                                             __ATOMIC_ACQUIRE);
    uint32_t reaped=__atomic_load_n(&scheduler_ap_probe_reaped,__ATOMIC_ACQUIRE);
    if(!fault&&((seen&expected)!=expected||
                (timeout_seen&expected)!=expected||
                reaped!=expected_count))return;

    if(!fault){
        if(!spinlock_try_acquire(&process_lock))return;
        uint32_t online=smp_online_count();
        if(online>LIONOS_MAX_CPUS)online=LIONOS_MAX_CPUS;
        for(uint32_t cpu=0u;cpu<online;++cpu){
            if(process_runqueue_validate_locked(cpu)!=0){
                fault=1u;
                __atomic_store_n(&scheduler_ap_probe_fault,1u,__ATOMIC_RELEASE);
                break;
            }
        }
        spinlock_release(&process_lock);
    }

    uint32_t reported=0u;
    if(!__atomic_compare_exchange_n(&scheduler_ap_probe_reported,&reported,1u,0,
            __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return;
    if(!fault){
        debug_write("LIONOS:SMP-AP-RUNQUEUE-WAKEUP-OK\n");
        debug_write("LIONOS:SMP-AP-RUNQUEUE-TIMEOUT-OK\n");
    }
    debug_write(fault?"LIONOS:SMP-AP-RUNQUEUE-LIFECYCLE-FAIL\n":
                      "LIONOS:SMP-AP-RUNQUEUE-LIFECYCLE-OK\n");
}

static int process_resolve_cow_locked(struct process *process,uint32_t page){
    if(!process||!process->page_directory||page>=0xC0000000u||
       (page&(PAGE_SIZE-1u)))return -1;

    struct process *owner=process;
    if(process->is_thread){
        owner=find_pid(process->thread_group_pid);
        if(!owner||owner->is_thread||
           owner->page_directory!=process->page_directory)return -1;
    }

    uint32_t page_slot=LIONOS_PROCESS_MAX_USER_PAGES;
    int thread_stack=process->is_thread&&process->thread_stack_va==page;
    if(!thread_stack){
        for(uint32_t i=0u;i<owner->user_page_count&&
            i<LIONOS_PROCESS_MAX_USER_PAGES;++i){
            if(owner->user_page_vas[i]==page){
                page_slot=i;
                break;
            }
        }
        if(page_slot==LIONOS_PROCESS_MAX_USER_PAGES)return -1;
    }

    if(paging_resolve_cow(process->page_directory,page)!=0)return -1;
    uint32_t physical=0u,flags=0u;
    if(paging_get_user_page(process->page_directory,page,&physical,&flags)!=0||
       !(flags&0x2u))return -1;

    /* reap_process() releases these physical-page references. Keep its
       ownership metadata synchronized with the PTE after every COW split. */
    if(thread_stack)process->thread_stack_page=physical;
    else owner->user_pages[page_slot]=physical;
    return 0;
}

int process_resolve_cow(uint32_t virtual_address){
    uint32_t irq_flags=spinlock_irqsave_acquire(&process_lock);
    struct process *current=current_local();
    int result=process_resolve_cow_locked(current,
        virtual_address&~(PAGE_SIZE-1u));
    spinlock_irqrestore_release(&process_lock,irq_flags);
    return result;
}

static int write_user_u32(struct process *p,uint32_t ptr,uint32_t value) {
    uint32_t physical,flags;
    if(!p||ptr<0x10000000u||ptr>0xBFFFFFFCu||(ptr&3u))return -1;
    uint32_t page=ptr&~(PAGE_SIZE-1u);
    if(paging_get_user_page(p->page_directory,page,&physical,&flags)!=0)
        return -1;
    if(!(flags&0x2u)){
        if(!(flags&0x200u)||process_resolve_cow_locked(p,page)!=0)return -1;
        if(paging_get_user_page(p->page_directory,page,&physical,&flags)!=0||
           !(flags&0x2u))return -1;
    }
    *(uint32_t *)(uintptr_t)(physical+(ptr&(PAGE_SIZE-1u)))=value;
    return 0;
}
static void clear_process(struct process *p) { if(p){uintptr_t base=(uintptr_t)&processes[0],addr=(uintptr_t)p;uintptr_t span=sizeof(processes);if(addr>=base&&addr-base<span&&(addr-base)%sizeof(processes[0])==0u)runqueue_node_init(&runqueue_nodes[(addr-base)/sizeof(processes[0])]);} p->pid=0;p->state=PROCESS_UNUSED;p->parent_pid=0;p->adopted_by_init=0u;p->pgid=0;p->sid=0;p->exit_code=0;p->wait_pid=0;p->wait_status_ptr=0;p->wait_options=0;p->stop_event_pending=0;p->stop_signal=0;p->continued_event_pending=0;p->wait_channel=0;p->wait_kind=0;__atomic_store_n(&p->wait_frame_saved,0,__ATOMIC_RELEASE);p->wait_timeout_active=0;p->wait_timeout_ticks=0;p->wait_deadline=0;p->wait_cpu=0;p->wait_timeout_result=0; p->reap_pending=0;p->deferred_kernel_stack=0;p->page_directory=0;p->entry=0;p->user_stack=0; p->kernel_stack_top=0;__atomic_store_n(&p->saved_frame,0,__ATOMIC_RELEASE);p->user_code_page=0;p->user_stack_page=0;p->user_page_count=0;p->pending_signals=0;p->signal_active=0u;p->signal_active_restorer=0u;for(uint32_t i=0u;i<32u;++i){p->signal_handlers[i]=LIONOS_SIG_DFL;p->signal_restorers[i]=0u;}for(uint32_t i=0u;i<PROCESS_CONTEXT_WORDS;++i)p->signal_saved_frame[i]=0u;p->cpu_owner=0;p->priority=PROCESS_PRIORITY_DEFAULT;p->base_priority=PROCESS_PRIORITY_DEFAULT;p->time_slice=0;p->ticks_used=0;p->fpu_valid=0;p->tid=0;p->thread_group_pid=0;p->is_thread=0;p->owns_address_space=0;p->thread_stack_page=0;p->thread_stack_va=0;p->join_tid=0;p->join_status_ptr=0;p->pipe_read_mask=0;p->pipe_write_mask=0;p->fd_closing=0; for(uint32_t i=0;i<PROCESS_FD_MAX;++i){p->fd_used[i]=0;p->fd_object[i]=0;p->fd_backend[i]=0;p->fd_flags[i]=0;p->fd_offset[i]=0;p->fd_path[i][0]=0;} for(uint32_t i=0;i<LIONOS_PROCESS_MAX_USER_PAGES;++i){p->user_pages[i]=0;p->user_page_vas[i]=0;} for(uint32_t i=0;i<PROCESS_FD_MAX;++i){p->fd_used[i]=0;p->fd_backend[i]=0;p->fd_flags[i]=0;p->fd_offset[i]=0;p->fd_path[i][0]=0;} }
static void init_interrupt_frame(struct process *p) { uint32_t *f=(uint32_t *)(uintptr_t)(p->kernel_stack_top-PROCESS_CONTEXT_WORDS*4u); for(uint32_t i=0;i<PROCESS_CONTEXT_WORDS;++i) f[i]=0; f[0]=USER_DATA_SEL;f[1]=USER_DATA_SEL;f[2]=USER_DATA_SEL;f[3]=USER_DATA_SEL; f[14]=p->entry;f[15]=USER_CODE_SEL;f[16]=0x202u;f[17]=p->user_stack;f[18]=USER_DATA_SEL; __atomic_store_n(&p->saved_frame,(uint32_t)(uintptr_t)f,__ATOMIC_RELEASE); }
static void init_bootstrap_frame(void) { struct process *b=&processes[0]; void *s=page_alloc(); if(!s){b->kernel_stack_top=0;__atomic_store_n(&b->saved_frame,0,__ATOMIC_RELEASE);return;} b->kernel_stack_top=(uint32_t)(uintptr_t)s+4096u;b->page_directory=paging_kernel_directory(); b->entry=(uint32_t)(uintptr_t)&process_schedule;b->user_stack=b->kernel_stack_top; uint32_t *f=(uint32_t *)(uintptr_t)(b->kernel_stack_top-PROCESS_CONTEXT_WORDS*4u); for(uint32_t i=0;i<PROCESS_CONTEXT_WORDS;++i) f[i]=0; f[0]=0x10u;f[1]=0x10u;f[2]=0x10u;f[3]=0x10u;f[14]=(uint32_t)(uintptr_t)&lionos_kernel_idle; f[15]=0x08u;f[16]=0x202u;f[17]=b->kernel_stack_top;f[18]=0x10u; __atomic_store_n(&b->saved_frame,(uint32_t)(uintptr_t)f,__ATOMIC_RELEASE); }
static int scheduler_idle_context_init(uint32_t cpu){
    if(cpu==0u||cpu>=LIONOS_MAX_CPUS)return -1;
    void *stack=page_alloc();
    if(!stack)return -1;
    struct process *idle=&scheduler_idle_processes[cpu];
    clear_process(idle);
    idle->pid=0u;
    idle->state=PROCESS_RUNNING;
    idle->cpu_owner=cpu;
    idle->page_directory=paging_kernel_directory();
    idle->kernel_stack_top=(uint32_t)(uintptr_t)stack+PAGE_SIZE;
    idle->entry=(uint32_t)(uintptr_t)&scheduler_idle;
    idle->user_stack=idle->kernel_stack_top;
    idle->priority=PROCESS_PRIORITY_LEVELS-1u;
    idle->base_priority=idle->priority;
    idle->time_slice=0xFFFFFFFFu;
    uint32_t *frame=(uint32_t *)(uintptr_t)
        (idle->kernel_stack_top-PROCESS_CONTEXT_WORDS*sizeof(uint32_t));
    for(uint32_t i=0u;i<PROCESS_CONTEXT_WORDS;++i)frame[i]=0u;
    frame[0]=0x10u;frame[1]=0x10u;frame[2]=0x10u;frame[3]=0x10u;
    frame[14]=(uint32_t)(uintptr_t)&scheduler_idle;
    frame[15]=0x08u;
    frame[16]=0x202u;
    frame[17]=idle->kernel_stack_top;
    frame[18]=0x10u;
    __atomic_store_n(&idle->saved_frame,(uint32_t)(uintptr_t)frame,__ATOMIC_RELEASE);
    scheduler_idle_ready_mask|=1u<<cpu;
    return 0;
}

static int scheduler_is_idle_context(const struct process *process){
    if(!process)return 0;
    for(uint32_t cpu=1u;cpu<LIONOS_MAX_CPUS;++cpu)
        if(process==&scheduler_idle_processes[cpu])return 1;
    return 0;
}

static void scheduler_note_idle_entry(uint32_t cpu){
    uint32_t bit=1u<<cpu;
    uint32_t entered=__atomic_fetch_or(&scheduler_idle_entered_mask,bit,
        __ATOMIC_ACQ_REL)|bit;
    if(!scheduler_idle_expected_mask||
       (entered&scheduler_idle_expected_mask)!=scheduler_idle_expected_mask)
        return;
    uint32_t expected=0u;
    if(__atomic_compare_exchange_n(&scheduler_idle_reported,&expected,1u,0,
            __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
        debug_write("LIONOS:SMP-AP-IDLE-ALL-ENTERED\n");
}

void process_init(void) {
    __atomic_store_n(&process_scheduler_ready,0u,__ATOMIC_RELEASE);
    fpu_enabled=(cpu_fpu_sse_init()==0u)?1u:0u;
    if(fpu_enabled){__asm__ volatile("fninit; fxsave %0":"=m"(fpu_initial_state)::"memory");}
    spinlock_init(&process_lock);
    for(uint32_t i=0u;i<LIONOS_MAX_CPUS;++i)
        spinlock_init(&scheduler_ap_probe_wait_locks[i]);
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    for(uint32_t i=0u;i<LIONOS_PROCESS_MAX;++i){runqueue_node_init(&runqueue_nodes[i]);clear_process(&processes[i]);}
    scheduler_idle_ready_mask=0u;
    scheduler_idle_expected_mask=0u;
    __atomic_store_n(&scheduler_idle_entered_mask,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&scheduler_idle_reported,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&runqueue_fault,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&runqueue_fault_reported,0u,__ATOMIC_RELAXED);
    for(uint32_t i=0u;i<LIONOS_MAX_CPUS;++i){
        runqueue_init(&runqueues[i],i);
        current_by_cpu[i]=0;
        scheduler_cpu[i].cursor=0u;
        scheduler_cpu[i].schedule_count=0u;
        scheduler_cpu[i].steal_count=0u;
        scheduler_cpu[i].timer_ticks=0u;
        reschedule_requested[i]=0u;
        reschedule_ipi_pending[i]=0u;
        reschedule_blocked[i]=0u;
        clear_process(&scheduler_idle_processes[i]);
    }
    next_pid=2u;next_tid=1u;
    processes[0].pid=1u;processes[0].pgid=1u;processes[0].sid=1u;
    processes[0].thread_group_pid=1u;processes[0].state=PROCESS_RUNNING;
    init_bootstrap_frame();
    set_current_local(&processes[0]);
    uint32_t online=smp_online_count();
    if(online>LIONOS_MAX_CPUS)online=LIONOS_MAX_CPUS;
    scheduler_idle_expected_mask=online>1u?((1u<<online)-2u):0u;
    for(uint32_t cpu=1u;cpu<online;++cpu){
        if(scheduler_idle_context_init(cpu)!=0)
            debug_write("LIONOS:SMP-AP-IDLE-CONTEXT-FAIL\n");
    }
    if(scheduler_idle_expected_mask&&
       ((scheduler_idle_ready_mask&scheduler_idle_expected_mask)==
        scheduler_idle_expected_mask))
        debug_write("LIONOS:SMP-AP-IDLE-CONTEXTS-READY\n");
    spinlock_irqrestore_release(&process_lock,flags);
    __atomic_store_n(&process_scheduler_ready,1u,__ATOMIC_RELEASE);
    uint32_t probe_mask=0u,probe_count=0u;
    for(uint32_t cpu=1u;cpu<online;++cpu){
        probe_mask|=1u<<cpu;
        ++probe_count;
    }
    __atomic_store_n(&scheduler_ap_probe_expected,probe_mask,__ATOMIC_RELEASE);
    __atomic_store_n(&scheduler_ap_probe_expected_count,probe_count,__ATOMIC_RELEASE);
    __atomic_store_n(&scheduler_ap_probe_seen,0u,__ATOMIC_RELEASE);
    __atomic_store_n(&scheduler_ap_probe_timeout_seen,0u,__ATOMIC_RELEASE);
    __atomic_store_n(&scheduler_ap_probe_fault,0u,__ATOMIC_RELEASE);
    __atomic_store_n(&scheduler_ap_probe_reported,0u,__ATOMIC_RELEASE);
    __atomic_store_n(&scheduler_ap_probe_reaped,0u,__ATOMIC_RELEASE);
    for(uint32_t cpu=1u;cpu<online;++cpu){
        if(!process_create_kernel_thread_on_cpu(
                (uint32_t)(uintptr_t)&process_scheduler_ap_probe,
                cpu,cpu))
            __atomic_store_n(&scheduler_ap_probe_fault,1u,__ATOMIC_RELEASE);
    }
    process_scheduler_ap_probe_report();
}
struct process *process_current(void){return current_local();}
uint32_t process_current_tid(void){struct process*p=current_local();return p?p->tid:0u;}
struct process *process_at(uint32_t i){return i<LIONOS_PROCESS_MAX?&processes[i]:0;}
uint32_t process_current_pid(void){struct process*p=current_local();return p?p->pid:0;}

int32_t process_get_pgid(uint32_t pid){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *target=0;
    if(pid==0u){
        struct process *current=current_local();
        if(current&&current->is_thread)
            target=find_pid(current->thread_group_pid);
        else
            target=current;
    }else{
        target=find_pid(pid);
    }
    int32_t result=target&&target->state!=PROCESS_ZOMBIE?(int32_t)target->pgid:-1;
    spinlock_irqrestore_release(&process_lock,flags);
    return result;
}

int32_t process_tty_set_foreground(uint32_t sid,uint32_t pgid){
    if(!sid||!pgid)return -1;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    int found=0;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        const struct process *member=&processes[i];
        if(member->state!=PROCESS_UNUSED&&member->state!=PROCESS_ZOMBIE&&
           !member->is_thread&&member->sid==sid&&member->pgid==pgid){
            found=1;
            break;
        }
    }
    int32_t previous=found?tty_get_foreground_pgid(sid):-1;
    int32_t result=found?tty_set_foreground_pgid(sid,pgid):-1;
    spinlock_irqrestore_release(&process_lock,flags);
    if(result==0&&previous!=(int32_t)pgid)tty_wake_readers();
    return result;
}

static void process_release_tty_if_session_empty(uint32_t sid){
    if(!sid)return;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    int live=0;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        const struct process *member=&processes[i];
        if(member->state!=PROCESS_UNUSED&&member->state!=PROCESS_ZOMBIE&&
           !member->is_thread&&member->sid==sid){
            live=1;
            break;
        }
    }
    spinlock_irqrestore_release(&process_lock,flags);
    if(!live)(void)tty_release_session(sid);
}

int32_t process_get_sid(uint32_t pid){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *target=0;
    if(pid==0u){
        struct process *current=current_local();
        if(current&&current->is_thread)
            target=find_pid(current->thread_group_pid);
        else
            target=current;
    }else{
        target=find_pid(pid);
    }
    int32_t result=target&&target->state!=PROCESS_ZOMBIE?(int32_t)target->sid:-1;
    spinlock_irqrestore_release(&process_lock,flags);
    return result;
}

static void process_debug_write_hex(uint32_t value){
    static const char digits[]="0123456789ABCDEF";
    char out[9];
    for(uint32_t i=0u;i<8u;++i)
        out[i]=digits[(value>>(28u-i*4u))&0xFu];
    out[8]='\0';
    debug_write(out);
}

int32_t process_create_session(void){
    debug_write("LIONOS:JOBCTL:SETSID-ENTER\n");
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *current=current_local();
    if(!current){
        debug_write("LIONOS:JOBCTL:SETSID-REJECT-NO-CURRENT\n");
        spinlock_irqrestore_release(&process_lock,flags);
        return -1;
    }
    if(current==&processes[0]){
        debug_write("LIONOS:JOBCTL:SETSID-REJECT-INIT\n");
        spinlock_irqrestore_release(&process_lock,flags);
        return -1;
    }
    if(current->is_thread){
        debug_write("LIONOS:JOBCTL:SETSID-REJECT-THREAD\n");
        spinlock_irqrestore_release(&process_lock,flags);
        return -1;
    }
    if(current->pid==current->pgid){
        debug_write("LIONOS:JOBCTL:SETSID-REJECT-LEADER\n");
        spinlock_irqrestore_release(&process_lock,flags);
        return -1;
    }
    debug_write("LIONOS:JOBCTL:SETSID-IDENTITY PID=0x");
    process_debug_write_hex(current->pid);
    debug_write(" PGID=0x");
    process_debug_write_hex(current->pgid);
    debug_write(" SID=0x");
    process_debug_write_hex(current->sid);
    debug_write("\n");
    uint32_t old_pgid=current->pgid;
    uint32_t old_sid=current->sid;
    int old_orphaned=process_group_orphaned_locked(old_pgid,old_sid);
    struct process_group_snapshot child_groups[LIONOS_PROCESS_MAX];
    uint32_t child_group_count=process_snapshot_child_groups_locked(
        current->pid,child_groups,LIONOS_PROCESS_MAX);
    uint32_t group_pid=current->pid;
    current->sid=group_pid;
    current->pgid=group_pid;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *member=&processes[i];
        if(member->state!=PROCESS_UNUSED&&member->is_thread&&
           member->thread_group_pid==group_pid){
            member->sid=group_pid;
            member->pgid=group_pid;
        }
    }
    process_notify_orphan_transition_locked(old_pgid,old_sid,old_orphaned);
    debug_write("LIONOS:JOBCTL:SETSID-OLD-GROUP-DONE\n");
    debug_write("LIONOS:JOBCTL:SETSID-CHILD-GROUPS-ENTER\n");
    process_notify_child_group_transitions_locked(child_groups,child_group_count);
    debug_write("LIONOS:JOBCTL:SETSID-CHILD-GROUPS-DONE\n");
    debug_write("LIONOS:JOBCTL:SETSID-DONE\n");
    spinlock_irqrestore_release(&process_lock,flags);
    debug_write("LIONOS:JOBCTL:SETSID-UNLOCKED\n");
    tty_wake_readers();
    debug_write("LIONOS:JOBCTL:SETSID-TTY-WAKE-DONE\n");
    return (int32_t)group_pid;
}

int32_t process_set_pgid(uint32_t pid,uint32_t pgid){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *current=current_local();
    if(!current||current==&processes[0]||current->is_thread){
        spinlock_irqrestore_release(&process_lock,flags);return -1;
    }
    uint32_t target_pid=pid?pid:current->pid;
    struct process *target=find_pid(target_pid);
    if(!target||target->is_thread||target->state==PROCESS_ZOMBIE||
       target->sid!=current->sid||target->sid==target->pid||
       (target!=current&&target->parent_pid!=current->pid)){
        spinlock_irqrestore_release(&process_lock,flags);return -1;
    }
    uint32_t new_pgid=pgid?pgid:target->pid;
    if(new_pgid!=target->pid&&new_pgid!=current->pgid){
        struct process *leader=find_pid(new_pgid);
        if(!leader||leader->is_thread||leader->state==PROCESS_ZOMBIE||
           leader->sid!=target->sid||leader->pid!=leader->pgid||
           leader->parent_pid!=current->pid){
            spinlock_irqrestore_release(&process_lock,flags);return -1;
        }
    }
    uint32_t old_pgid=target->pgid;
    uint32_t old_sid=target->sid;
    uint32_t new_sid=target->sid;
    int old_orphaned=process_group_orphaned_locked(old_pgid,old_sid);
    int new_orphaned=(new_pgid==old_pgid)?old_orphaned:
        process_group_orphaned_locked(new_pgid,new_sid);
    struct process_group_snapshot child_groups[LIONOS_PROCESS_MAX];
    uint32_t child_group_count=process_snapshot_child_groups_locked(
        target->pid,child_groups,LIONOS_PROCESS_MAX);
    uint32_t group=target->thread_group_pid?target->thread_group_pid:target->pid;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *member=&processes[i];
        if(member->state!=PROCESS_UNUSED&&member->thread_group_pid==group)
            member->pgid=new_pgid;
    }
    if(new_pgid!=old_pgid){
        process_notify_orphan_transition_locked(old_pgid,old_sid,old_orphaned);
        process_notify_orphan_transition_locked(new_pgid,new_sid,new_orphaned);
        process_notify_child_group_transitions_locked(child_groups,child_group_count);
    }
    spinlock_irqrestore_release(&process_lock,flags);
    if(new_pgid!=old_pgid)tty_wake_readers();
    return 0;
}
struct process *process_pipe_owner(void){
    struct process *current=current_local();
    if(!current)return 0;
    if(!current->is_thread)return current->pid?current:0;
    uint32_t group=current->thread_group_pid;
    if(!group)return 0;
    for(uint32_t i=0;i<LIONOS_PROCESS_MAX;++i)
        if(processes[i].state!=PROCESS_UNUSED&&!processes[i].is_thread&&
           processes[i].pid==group)return &processes[i];
    return 0;
}
struct process *process_fd_owner(void){return process_pipe_owner();}
const char *process_state_name(uint32_t s){switch(s){case PROCESS_READY:return "READY";case PROCESS_RUNNING:return "RUNNING";case PROCESS_ZOMBIE:return "ZOMBIE";case PROCESS_WAITING:return "WAITING";case PROCESS_STOPPED:return "STOPPED";default:return "UNUSED";}}

static struct process *process_create_ex_vas_locked(uint32_t entry,uint32_t stack,uint32_t pd,const uint32_t *pages,const uint32_t *vas,uint32_t count){ struct process*p=find_free_slot(); if(!p||!pd||!pages||!vas||!count||count>LIONOS_PROCESS_MAX_USER_PAGES)return 0; void *ks=page_alloc();if(!ks)return 0; p->pid=next_pid++;if(next_pid==0)next_pid=2;p->tid=next_tid++;if(next_tid==0)next_tid=1;p->thread_group_pid=p->pid;p->is_thread=0;p->owns_address_space=1;p->thread_stack_page=0;p->thread_stack_va=0;p->join_tid=0;p->join_status_ptr=0;p->state=PROCESS_STOPPED; struct process *parent=current_local();p->parent_pid=parent?parent->pid:1;p->adopted_by_init=0u;p->pgid=p->pid;p->sid=p->pid; p->exit_code=0;p->wait_pid=0;p->wait_status_ptr=0;p->wait_options=0;p->stop_event_pending=0;p->stop_signal=0;p->wait_channel=0;p->wait_kind=0;__atomic_store_n(&p->wait_frame_saved,0,__ATOMIC_RELEASE);p->reap_pending=0;p->deferred_kernel_stack=0; p->page_directory=pd;p->entry=entry;p->user_stack=stack;p->kernel_stack_top=(uint32_t)(uintptr_t)ks+4096u; __atomic_store_n(&p->saved_frame,0,__ATOMIC_RELEASE);p->user_page_count=count;p->user_code_page=pages[0];p->user_stack_page=pages[count-1u];p->pending_signals=0;p->signal_active=0u;p->signal_active_restorer=0u;for(uint32_t i=0u;i<32u;++i){p->signal_handlers[i]=LIONOS_SIG_DFL;p->signal_restorers[i]=0u;}for(uint32_t i=0u;i<PROCESS_CONTEXT_WORDS;++i)p->signal_saved_frame[i]=0u;p->capabilities=PROCESS_CAP_USER_DEFAULT;p->cpu_owner=cpu_current_index();p->priority=PROCESS_PRIORITY_DEFAULT;p->base_priority=PROCESS_PRIORITY_DEFAULT;p->time_slice=quantum_for_priority(PROCESS_PRIORITY_DEFAULT);p->ticks_used=0;p->fpu_valid=0;p->pipe_read_mask=0;p->pipe_write_mask=0; for(uint32_t i=0;i<count;++i){p->user_pages[i]=pages[i];p->user_page_vas[i]=vas[i];} for(uint32_t i=count;i<LIONOS_PROCESS_MAX_USER_PAGES;++i){p->user_pages[i]=0;p->user_page_vas[i]=0;} init_interrupt_frame(p);return p;}
struct process *process_create_ex_vas(uint32_t entry,uint32_t stack,uint32_t pd,const uint32_t *pages,const uint32_t *vas,uint32_t count){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *p=process_create_ex_vas_locked(entry,stack,pd,pages,vas,count);
    spinlock_irqrestore_release(&process_lock,flags);
    if(!p)return 0;
    if(vfs_stdio_init_process(p)!=0){
        flags=spinlock_irqsave_acquire(&process_lock);
        if(p->state==PROCESS_STOPPED){
            if(p->kernel_stack_top)
                page_free((void *)(uintptr_t)(p->kernel_stack_top-4096u));
            clear_process(p);
        }
        spinlock_irqrestore_release(&process_lock,flags);
        /* The caller still owns the user pages and address space on failure. */
        return 0;
    }
    flags=spinlock_irqsave_acquire(&process_lock);
    if(p->state==PROCESS_STOPPED&&!p->stop_event_pending&&
       process_transition_state_locked(p,PROCESS_READY)!=0){
        spinlock_irqrestore_release(&process_lock,flags);
        vfs_close_all_process(p);
        flags=spinlock_irqsave_acquire(&process_lock);
        if(p->state==PROCESS_STOPPED){
            if(p->kernel_stack_top)
                page_free((void *)(uintptr_t)(p->kernel_stack_top-4096u));
            clear_process(p);
        }
        spinlock_irqrestore_release(&process_lock,flags);
        /* Preserve the caller-owned address space and user pages on failure. */
        return 0;
    }
    spinlock_irqrestore_release(&process_lock,flags);
    return p;
}
struct process *process_create_ex(uint32_t entry,uint32_t stack,uint32_t pd,const uint32_t *pages,uint32_t count){ uint32_t vas[LIONOS_PROCESS_MAX_USER_PAGES];for(uint32_t i=0;i<count&&i<LIONOS_PROCESS_MAX_USER_PAGES;++i)vas[i]=i*4096u;return process_create_ex_vas(entry,stack,pd,pages,vas,count); }
struct process *process_create(uint32_t entry,uint32_t stack,uint32_t pd,uint32_t code,uint32_t user_stack_page){ uint32_t p[2]={code,user_stack_page},v[2]={0x10000000u,0x10001000u};return process_create_ex_vas(entry,stack,pd,p,v,2u); }

int process_exec_replace_current(uint32_t entry,uint32_t stack,uint32_t pd,const uint32_t *pages,const uint32_t *vas,uint32_t count){ uint32_t flags=spinlock_irqsave_acquire(&process_lock);struct process*c=current_local(); if(!c||c==&processes[0]||c->is_thread||!pd||!pages||!vas||!count||count>LIONOS_PROCESS_MAX_USER_PAGES){spinlock_irqrestore_release(&process_lock,flags);return -1;} void*new_ks=page_alloc();if(!new_ks){spinlock_irqrestore_release(&process_lock,flags);return -1;} for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){struct process*t=&processes[i];if(t!=c&&t->state!=PROCESS_UNUSED&&t->is_thread&&t->thread_group_pid==c->thread_group_pid){(void)process_transition_state_locked(t,PROCESS_ZOMBIE);reap_process(t);}} uint32_t old_pd=c->page_directory,old_ks=c->kernel_stack_top,old_pages[LIONOS_PROCESS_MAX_USER_PAGES],old_count=c->user_page_count; for(uint32_t i=0;i<old_count;++i)old_pages[i]=c->user_pages[i]; c->page_directory=pd;c->entry=entry;c->user_stack=stack;c->kernel_stack_top=(uint32_t)(uintptr_t)new_ks+4096u;__atomic_store_n(&c->saved_frame,0,__ATOMIC_RELEASE); c->user_page_count=count;c->user_code_page=pages[0];c->user_stack_page=pages[count-1u];c->exit_code=0;c->wait_pid=0;c->wait_status_ptr=0;c->wait_options=0;__atomic_store_n(&c->wait_frame_saved,0u,__ATOMIC_RELEASE);c->stop_event_pending=0;c->stop_signal=0;c->continued_event_pending=0;c->reap_pending=0;c->pending_signals=0;c->signal_active=0u;c->signal_active_restorer=0u;for(uint32_t i=0u;i<32u;++i){if(c->signal_handlers[i]!=LIONOS_SIG_IGN)c->signal_handlers[i]=LIONOS_SIG_DFL;c->signal_restorers[i]=0u;}for(uint32_t i=0u;i<PROCESS_CONTEXT_WORDS;++i)c->signal_saved_frame[i]=0u;c->deferred_kernel_stack=old_ks; for(uint32_t i=0;i<count;++i){c->user_pages[i]=pages[i];c->user_page_vas[i]=vas[i];} for(uint32_t i=count;i<LIONOS_PROCESS_MAX_USER_PAGES;++i){c->user_pages[i]=0;c->user_page_vas[i]=0;} paging_switch_address_space(pd);tss_set_kernel_stack(c->kernel_stack_top);init_interrupt_frame(c); for(uint32_t i=0;i<old_count;++i)if(old_pages[i])page_free((void *)(uintptr_t)old_pages[i]); if(old_pd&&old_pd!=pd)paging_destroy_address_space(old_pd); spinlock_irqrestore_release(&process_lock,flags);return 0; }

uint32_t process_fork_current(uint32_t *parent_frame){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process*parent=current_local();
    struct process*parent_owner=process_pipe_owner();
    if(!parent||parent==&processes[0]||!parent_owner||!parent_frame||!parent->user_page_count){debug_write("LIONOS:FORK-FAIL-CONTEXT\n");spinlock_irqrestore_release(&process_lock,flags);return PROCESS_SYSCALL_ERR;}
    struct process*slot=find_free_slot();
    if(!slot){debug_write("LIONOS:FORK-FAIL-NO-SLOT\n");spinlock_irqrestore_release(&process_lock,flags);return PROCESS_SYSCALL_ERR;}
    uint32_t pd=paging_create_address_space();
    if(!pd){debug_write("LIONOS:FORK-FAIL-NO-ADDRESS-SPACE\n");spinlock_irqrestore_release(&process_lock,flags);return PROCESS_SYSCALL_ERR;}

    uint32_t pages[LIONOS_PROCESS_MAX_USER_PAGES]={0};
    uint32_t vas[LIONOS_PROCESS_MAX_USER_PAGES]={0};
    uint32_t old_flags[LIONOS_PROCESS_MAX_USER_PAGES]={0};
    uint32_t count=parent->user_page_count,retained=0,cowed=0;

    for(uint32_t i=0;i<count;++i){
        if(paging_get_user_page(parent->page_directory,parent->user_page_vas[i],&pages[i],&old_flags[i])!=0){debug_write("LIONOS:FORK-FAIL-PAGE-LOOKUP\n");goto fail;}
        vas[i]=parent->user_page_vas[i];
        uint32_t child_flags=old_flags[i];
        if(old_flags[i]&0x2u)child_flags=(old_flags[i]&~0x2u)|0x200u;
        if(paging_map_user_page_in(pd,vas[i],pages[i],child_flags)!=0){debug_write("LIONOS:FORK-FAIL-PAGE-MAP\n");goto fail;}
        page_retain((void*)(uintptr_t)pages[i]);++retained;
    }
    for(uint32_t i=0;i<count;++i){
        if(!(old_flags[i]&0x2u))continue;
        if(paging_set_user_page_flags(parent->page_directory,vas[i],(old_flags[i]&~0x2u)|0x200u)!=0){debug_write("LIONOS:FORK-FAIL-PARENT-COW\n");goto restore;}
        ++cowed;
    }

    /* fork() duplicates the caller's architectural FPU/SSE state. FXSAVE is
       non-destructive, so the parent continues with identical live registers. */
    fpu_save(parent);
    slot=process_create_ex_vas_locked(parent->entry,parent->user_stack,pd,pages,vas,count);
    if(!slot){debug_write("LIONOS:FORK-FAIL-PROCESS-CREATE\n");goto restore;}
    if(fpu_enabled&&parent->fpu_valid){
        for(uint32_t i=0u;i<sizeof(parent->fpu_state);++i)
            slot->fpu_state[i]=parent->fpu_state[i];
        slot->fpu_valid=1u;
    }
    slot->parent_pid=parent_owner->pid;slot->pgid=parent_owner->pgid;slot->sid=parent->sid;
    slot->tid=next_tid++;if(next_tid==0)next_tid=1;slot->thread_group_pid=slot->pid;slot->is_thread=0;slot->owns_address_space=1;
    slot->cpu_owner=cpu_current_index();
    slot->capabilities=parent->capabilities;
    for(uint32_t i=0u;i<32u;++i){slot->signal_handlers[i]=parent->signal_handlers[i];slot->signal_restorers[i]=parent->signal_restorers[i];}
    slot->signal_active=parent->signal_active;
    slot->signal_active_restorer=parent->signal_active_restorer;
    for(uint32_t i=0u;i<PROCESS_CONTEXT_WORDS;++i)slot->signal_saved_frame[i]=parent->signal_saved_frame[i];
    slot->priority=parent->priority;
    slot->base_priority=parent->base_priority;
    slot->time_slice=quantum_for_priority(slot->priority);
    slot->ticks_used=0;

    /* File descriptions are shared by fork; pipe ownership is cloned separately. */
    {
        uint32_t*cf=(uint32_t *)(uintptr_t)__atomic_load_n(&slot->saved_frame,__ATOMIC_ACQUIRE);
        for(uint32_t i=0;i<PROCESS_CONTEXT_WORDS;++i)cf[i]=parent_frame[i];
        cf[11]=0;
    }
    {
        uint32_t result=slot->pid;
        slot->state=PROCESS_STOPPED;
        spinlock_irqrestore_release(&process_lock,flags);
        uint32_t cleanup_flags;
        if(vfs_fork_process(parent_owner,slot)!=0){
            debug_write("LIONOS:FORK-FAIL-VFS-CLONE\n");
            cleanup_flags=spinlock_irqsave_acquire(&process_lock);
            if(slot->state==PROCESS_STOPPED)(void)process_transition_state_locked(slot,PROCESS_ZOMBIE);
            spinlock_irqrestore_release(&process_lock,cleanup_flags);
            (void)process_reap_pid(result);
            return PROCESS_SYSCALL_ERR;
        }
        if(pipe_fork(parent_owner,slot)!=0){
            debug_write("LIONOS:FORK-FAIL-PIPE-CLONE\n");
            vfs_close_all_process(slot);
            slot->pipe_read_mask=0u;
            slot->pipe_write_mask=0u;
            cleanup_flags=spinlock_irqsave_acquire(&process_lock);
            if(slot->state==PROCESS_STOPPED)(void)process_transition_state_locked(slot,PROCESS_ZOMBIE);
            spinlock_irqrestore_release(&process_lock,cleanup_flags);
            (void)process_reap_pid(result);
            return PROCESS_SYSCALL_ERR;
        }
        cleanup_flags=spinlock_irqsave_acquire(&process_lock);
        if(slot->state==PROCESS_STOPPED&&!slot->stop_event_pending&&
           process_transition_state_locked(slot,PROCESS_READY)!=0){
            (void)process_transition_state_locked(slot,PROCESS_ZOMBIE);
            spinlock_irqrestore_release(&process_lock,cleanup_flags);
            pipe_close_all(slot);
            vfs_close_all_process(slot);
            (void)process_reap_pid(result);
            return PROCESS_SYSCALL_ERR;
        }
        spinlock_irqrestore_release(&process_lock,cleanup_flags);
        return result;
    }
restore:
    for(uint32_t i=0;i<cowed;++i)paging_set_user_page_flags(parent->page_directory,vas[i],old_flags[i]);
fail:
    for(uint32_t i=0;i<retained;++i)page_free((void*)(uintptr_t)pages[i]);
    paging_destroy_address_space(pd);
    spinlock_irqrestore_release(&process_lock,flags);
    return PROCESS_SYSCALL_ERR;
}

static int is_child_of(const struct process*c,uint32_t parent_pid,uint32_t pid){return c->state!=PROCESS_UNUSED&&c->parent_pid==parent_pid&&(pid==PROCESS_WAIT_ANY||c->pid==pid);}
static struct process*find_zombie_child(uint32_t parent_pid,uint32_t pid){for(uint32_t i=1;i<LIONOS_PROCESS_MAX;++i)if(is_child_of(&processes[i],parent_pid,pid)&&processes[i].state==PROCESS_ZOMBIE&&!processes[i].reap_pending)return &processes[i];return 0;}
static int has_child(uint32_t parent_pid,uint32_t pid){for(uint32_t i=1;i<LIONOS_PROCESS_MAX;++i){struct process *child=&processes[i];if(is_child_of(child,parent_pid,pid)&&!(child->state==PROCESS_ZOMBIE&&child->reap_pending))return 1;}return 0;}
static void reap_process(struct process*p){
    if(!p||p->state!=PROCESS_ZOMBIE)return;
    if(p->is_thread){
        if(p->thread_stack_page){
            if(paging_unmap_user_page_in(p->page_directory,p->thread_stack_va,
                                         p->thread_stack_page)!=0)return;
            page_free((void *)(uintptr_t)p->thread_stack_page);
        }
        if(p->kernel_stack_top)
            page_free((void *)(uintptr_t)(p->kernel_stack_top-4096u));
        if(p->pid==0u&&p->thread_group_pid==0u&&
           p->entry==(uint32_t)(uintptr_t)&process_scheduler_ap_probe)
            __atomic_fetch_add(&scheduler_ap_probe_reaped,1u,__ATOMIC_ACQ_REL);
        clear_process(p);
        return;
    }
    for(uint32_t i=0;i<p->user_page_count;++i)
        if(p->user_pages[i])page_free((void *)(uintptr_t)p->user_pages[i]);
    if(p->page_directory&&p->page_directory!=paging_kernel_directory())
        paging_destroy_address_space(p->page_directory);
    if(p->kernel_stack_top)
        page_free((void *)(uintptr_t)(p->kernel_stack_top-4096u));
    clear_process(p);
}
int process_get_exit_code(uint32_t pid,uint32_t*code){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process*p=0;
    for(uint32_t i=0;i<LIONOS_PROCESS_MAX;++i)if(processes[i].state!=PROCESS_UNUSED&&processes[i].pid==pid){p=&processes[i];break;}
    if(!p||p->state!=PROCESS_ZOMBIE){spinlock_irqrestore_release(&process_lock,flags);return -1;}
    if(code)*code=p->exit_code;
    spinlock_irqrestore_release(&process_lock,flags);
    return 0;
}
int process_reap_pid(uint32_t pid){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process*p=0;
    for(uint32_t i=0;i<LIONOS_PROCESS_MAX;++i)if(processes[i].state!=PROCESS_UNUSED&&processes[i].pid==pid){p=&processes[i];break;}
    if(!p||p->state!=PROCESS_ZOMBIE){spinlock_irqrestore_release(&process_lock,flags);return -1;}
    reap_process(p);
    spinlock_irqrestore_release(&process_lock,flags);
    return 0;
}

static struct process *find_stopped_child(uint32_t parent_pid,uint32_t pid){
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *child=&processes[i];
        if(is_child_of(child,parent_pid,pid)&&child->state==PROCESS_STOPPED&&
           child->stop_event_pending)return child;
    }
    return 0;
}

static struct process *find_continued_child(uint32_t parent_pid,uint32_t pid){
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *child=&processes[i];
        if(is_child_of(child,parent_pid,pid)&&child->continued_event_pending)
            return child;
    }
    return 0;
}

static int process_has_interrupting_signal_locked(const struct process *p){
    if(!p||p->signal_active)return 0;
    const uint32_t hup_bit=1u<<(LIONOS_SIG_HUP-1u),int_bit=1u<<(LIONOS_SIG_INT-1u);
    if((p->pending_signals&hup_bit)&&p->signal_handlers[LIONOS_SIG_HUP-1u]!=LIONOS_SIG_IGN)return 1;
    if((p->pending_signals&int_bit)&&p->signal_handlers[LIONOS_SIG_INT-1u]!=LIONOS_SIG_IGN)return 1;
    return 0;
}

/* Complete waitpid interruption after a contended scheduler tick has saved
   the blocked task's frame. The signal may have arrived before that frame
   became available, so every successful scheduler lock acquisition retries
   pending waitpid wakeups under process_lock. */
static void process_wake_interrupted_waitpid_locked(void){
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *p=&processes[i];
        if(p->state!=PROCESS_WAITING||p->wait_kind!=3u||
           !__atomic_load_n(&p->wait_frame_saved,__ATOMIC_ACQUIRE)||
           !process_has_interrupting_signal_locked(p))
            continue;
        uint32_t frame_address=
            __atomic_load_n(&p->saved_frame,__ATOMIC_ACQUIRE);
        if(!frame_address)continue;
        uint32_t *saved=(uint32_t *)(uintptr_t)frame_address;
        process_diag_status33("WAITPID-INTERRUPTED-SCAN",p->pid,
                              p->wait_pid,p->pending_signals);
        saved[11]=(uint32_t)LIONOS_EINTR;
        process_clear_waitpid_locked(p);
        (void)process_transition_state_locked(p,PROCESS_READY);
        uint32_t cpu=p->cpu_owner<LIONOS_MAX_CPUS?p->cpu_owner:0u;
        __atomic_store_n(&reschedule_requested[cpu],1u,__ATOMIC_RELEASE);
    }
}
static void process_clear_waitpid_locked(struct process *p){
    if(!p)return;
    p->wait_pid=0u;p->wait_status_ptr=0u;p->wait_options=0u;
    p->wait_channel=0u;p->wait_kind=0u;__atomic_store_n(&p->wait_frame_saved,0u,__ATOMIC_RELEASE);
}

int32_t process_waitpid_options(uint32_t pid,uint32_t status_ptr,uint32_t options){
    if(options&~LIONOS_WAIT_OPTIONS_SUPPORTED)return -1;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *caller=current_local();
    if(!caller||caller==&processes[0]){
        spinlock_irqrestore_release(&process_lock,flags);
        return -1;
    }
    struct process *zombie=find_zombie_child(caller->pid,pid);
    if(zombie){
        process_diag_status33("WAITPID-REAP-STATUS",zombie->pid,
                              caller->pid,zombie->exit_code);
        if(status_ptr&&write_user_u32(caller,status_ptr,zombie->exit_code)!=0){
            process_diag_status33("WAITPID-STATUS-WRITE-FAIL",zombie->pid,
                                  caller->pid,status_ptr);
            spinlock_irqrestore_release(&process_lock,flags);
            return -1;
        }
        int32_t result=(int32_t)zombie->pid;
        reap_process(zombie);
        spinlock_irqrestore_release(&process_lock,flags);
        return result;
    }
    if(options&LIONOS_WAIT_UNTRACED){
        struct process *stopped=find_stopped_child(caller->pid,pid);
        if(stopped){
            uint32_t status=LIONOS_WAIT_STATUS_STOPPED(stopped->stop_signal);
            if(status_ptr&&write_user_u32(caller,status_ptr,status)!=0){
                spinlock_irqrestore_release(&process_lock,flags);
                return -1;
            }
            stopped->stop_event_pending=0u;
            int32_t result=(int32_t)stopped->pid;
            spinlock_irqrestore_release(&process_lock,flags);
            return result;
        }
    }
    if(options&LIONOS_WAIT_CONTINUED){
        struct process *continued=find_continued_child(caller->pid,pid);
        if(continued){
            if(status_ptr&&write_user_u32(caller,status_ptr,LIONOS_WAIT_STATUS_CONTINUED)!=0){
                spinlock_irqrestore_release(&process_lock,flags);
                return -1;
            }
            continued->continued_event_pending=0u;
            int32_t result=(int32_t)continued->pid;
            spinlock_irqrestore_release(&process_lock,flags);
            return result;
        }
    }
    if(!has_child(caller->pid,pid)){
        process_diag_status33("WAITPID-NO-CHILD",caller->pid,pid,
                              caller->pending_signals);
        struct process *requested=find_pid(pid);
        if(requested)
            process_diag_status33("WAITPID-TARGET",requested->pid,
                                  requested->parent_pid,requested->state);
        else
            process_diag_status33("WAITPID-TARGET-MISSING",pid,caller->pid,
                                  cpu_current_index());
        spinlock_irqrestore_release(&process_lock,flags);
        return -1;
    }
    if(options&LIONOS_WAIT_NOHANG){
        spinlock_irqrestore_release(&process_lock,flags);
        return 0;
    }
    if(process_has_interrupting_signal_locked(caller)){
        process_diag_status33("WAITPID-INTERRUPTED",caller->pid,pid,
                              caller->pending_signals);
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_EINTR;
    }
    if(options&LIONOS_WAIT_UNTRACED)
        debug_write("LIONOS:JOBCTL:WAITPID-STOP-BLOCK\n");
    process_diag_status33("WAITPID-BLOCK",caller->pid,pid,
                          cpu_current_index());
    caller->wait_pid=pid;
    caller->wait_status_ptr=status_ptr;
    caller->wait_options=options;
    caller->wait_channel=0u;
    caller->wait_kind=3u;
    __atomic_store_n(&caller->wait_frame_saved,0u,__ATOMIC_RELEASE);
    debug_write("LIONOS:WAITPID-EINTR-BLOCK\n");
    (void)process_transition_state_locked(caller,PROCESS_WAITING);
    process_request_blocked_reschedule(cpu_current_index());
    spinlock_irqrestore_release(&process_lock,flags);
    return PROCESS_WAIT_BLOCKED;
}

int32_t process_waitpid(uint32_t pid,uint32_t status_ptr){
    if(!status_ptr)return -1;
    return process_waitpid_options(pid,status_ptr,0u);
}
static int32_t process_sleep_on_kind(uintptr_t channel, struct spinlock *held_lock,
                                      uint32_t irq_flags, uint32_t kind,
                                      uint32_t timeout_ticks){
    if(!channel||!held_lock||timeout_ticks>0x7FFFFFFFu){
        if(held_lock)spinlock_irqrestore_release(held_lock,irq_flags);
        return -1;
    }
    struct process *c=current_local();
    if(!c||c==&processes[0]||c->state==PROCESS_UNUSED){
        spinlock_irqrestore_release(held_lock,irq_flags);
        return -1;
    }
    /* Keep the object lock until PROCESS_WAITING is visible under process_lock.
       This closes the check-to-sleep lost-wakeup window. */
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    uint32_t cpu=cpu_current_index();
    if(cpu>=LIONOS_MAX_CPUS)cpu=0u;
    c->wait_channel=(uint32_t)channel;
    c->wait_kind=kind;
    c->wait_timeout_active=((kind==1u||kind==2u)&&timeout_ticks!=0u)?2u:0u;
    c->wait_timeout_result=0u;
    c->wait_timeout_ticks=timeout_ticks;
    c->wait_cpu=cpu;
    c->wait_deadline=0u;
    (void)process_transition_state_locked(c,PROCESS_WAITING);
    process_request_blocked_reschedule(cpu);
    spinlock_irqrestore_release(&process_lock,flags);
    spinlock_irqrestore_release(held_lock,irq_flags);
    return PROCESS_WAIT_BLOCKED;
}

int32_t process_sleep_on(uintptr_t channel, struct spinlock *held_lock, uint32_t irq_flags){
    return process_sleep_on_kind(channel,held_lock,irq_flags,0u,0u);
}

int32_t process_sleep_on_timeout(uintptr_t channel, struct spinlock *held_lock,
                                 uint32_t irq_flags, uint32_t timeout_ticks){
    return process_sleep_on_kind(channel,held_lock,irq_flags,2u,timeout_ticks);
}

int32_t process_consume_timeout(void){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *current=current_local();
    int32_t result=current&&current->wait_timeout_result?1:0;
    if(current)current->wait_timeout_result=0u;
    spinlock_irqrestore_release(&process_lock,flags);
    return result;
}

int32_t process_futex_sleep_on(uintptr_t channel, struct spinlock *held_lock, uint32_t irq_flags){
    return process_sleep_on_kind(channel,held_lock,irq_flags,1u,0u);
}

int32_t process_futex_sleep_on_timeout(uintptr_t channel, struct spinlock *held_lock,
                                       uint32_t irq_flags, uint32_t timeout_ticks){
    return process_sleep_on_kind(channel,held_lock,irq_flags,1u,timeout_ticks);
}

void process_futex_tick(uint32_t cpu){
    if(cpu>=LIONOS_MAX_CPUS||
       __atomic_load_n(&process_scheduler_ready,__ATOMIC_ACQUIRE)==0u)return;
    /*
     * Account for every local timer interrupt even when process_lock is
     * contended. Dropping the timeout scan is safe; dropping elapsed AP-local
     * ticks can indefinitely postpone a timeout on a repeatedly contended CPU.
     */
    uint32_t now=__atomic_add_fetch(&scheduler_cpu[cpu].timer_ticks,1u,
                                    __ATOMIC_RELAXED);
    if(!spinlock_try_acquire(&process_lock))return;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *p=&processes[i];
        if(p->state!=PROCESS_WAITING||(p->wait_kind!=1u&&p->wait_kind!=2u)||
           p->wait_timeout_active!=1u||p->wait_cpu!=cpu||
           (int32_t)(now-p->wait_deadline)<0)continue;
        uint32_t kind=p->wait_kind;
        p->wait_channel=0u;
        p->wait_kind=0u;
        p->wait_timeout_active=0u;
        (void)process_transition_state_locked(p,PROCESS_READY);
        if(kind==2u)p->wait_timeout_result=1u;
        if(kind==1u){
            uint32_t *frame=(uint32_t *)(uintptr_t)__atomic_load_n(&p->saved_frame,__ATOMIC_ACQUIRE);
            if(frame)frame[11]=2u;
        }
    }
    spinlock_release(&process_lock);
}

void process_wakeup(uintptr_t channel){
    if(!channel)return;
    uint32_t wake_mask=0u;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *p=&processes[i];
        if(p->state==PROCESS_WAITING&&(p->wait_kind==0u||p->wait_kind==2u)&&
           p->wait_channel==(uint32_t)channel){
            if(process_transition_state_locked(p,PROCESS_READY)==0){
                p->wait_channel=0u;
                p->wait_timeout_active=0u;
                p->wait_timeout_result=0u;
                p->wait_kind=0u;
                uint32_t owner=p->cpu_owner<LIONOS_MAX_CPUS?p->cpu_owner:0u;
                wake_mask|=1u<<owner;
            }
        }
    }
    spinlock_irqrestore_release(&process_lock,flags);

    /* Kick remote owners only after READY is published and process_lock is released. */
    for(uint32_t cpu=0u;cpu<LIONOS_MAX_CPUS;++cpu)
        if(wake_mask&(1u<<cpu))process_kick_cpu(cpu);
}

uint32_t process_futex_wakeup(uintptr_t channel,uint32_t maximum){
    if(!channel||!maximum)return 0u;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock),woken=0u;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX&&woken<maximum;++i){
        struct process *p=&processes[i];
        if(p->state!=PROCESS_WAITING||p->wait_kind!=1u||
           p->wait_channel!=(uint32_t)channel)continue;
        p->wait_channel=0u;
        p->wait_kind=0u;
        p->wait_timeout_active=0u;
        (void)process_transition_state_locked(p,PROCESS_READY);
        uint32_t *frame=(uint32_t *)(uintptr_t)__atomic_load_n(&p->saved_frame,__ATOMIC_ACQUIRE);
        if(frame)frame[11]=0u;
        ++woken;
    }
    spinlock_irqrestore_release(&process_lock,flags);
    return woken;
}

static void wake_waiting_parent(struct process *child){
    if(!child||child->state!=PROCESS_ZOMBIE)return;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *parent=&processes[i];
        if(parent->state!=PROCESS_WAITING||parent->wait_kind!=3u||
           parent->pid!=child->parent_pid||
           (parent->wait_pid!=PROCESS_WAIT_ANY&&parent->wait_pid!=child->pid))
            continue;

        int status_ok=!parent->wait_status_ptr||
            write_user_u32(parent,parent->wait_status_ptr,child->exit_code)==0;
        uint32_t *frame=(uint32_t *)(uintptr_t)__atomic_load_n(&parent->saved_frame,__ATOMIC_ACQUIRE);
        process_diag_status33("WAITPID-WAKE",parent->pid,child->pid,
                              status_ok?1u:0u);
        if(frame){
            frame[11]=status_ok?child->pid:PROCESS_SYSCALL_ERR;
            process_diag_status33("WAITPID-WAKE-FRAME",parent->pid,
                                  frame[11],parent->cpu_owner);
        }else{
            process_diag_status33("WAITPID-WAKE-NO-FRAME",parent->pid,
                                  child->pid,parent->cpu_owner);
        }

        parent->wait_pid=0u;
        parent->wait_status_ptr=0u;
        parent->wait_options=0u;
        parent->wait_channel=0u;
        parent->wait_kind=0u;
        __atomic_store_n(&parent->wait_frame_saved,0u,__ATOMIC_RELEASE);
        (void)process_transition_state_locked(parent,PROCESS_READY);

        /* Keep the zombie available for a retry when status delivery failed. */
        if(status_ok)child->reap_pending=1u;
        else debug_write("LIONOS:WAITPID-ASYNC-STATUS-WRITE-FAIL\n");
        return;
    }
}static void wake_waiting_parent_stop(struct process *child){
    if(!child||!child->stop_event_pending)return;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *parent=&processes[i];
        if(parent->state!=PROCESS_WAITING||parent->wait_kind!=3u||parent->pid!=child->parent_pid||
           !(parent->wait_options&LIONOS_WAIT_UNTRACED)||
           (parent->wait_pid!=PROCESS_WAIT_ANY&&parent->wait_pid!=child->pid))
            continue;
        debug_write("LIONOS:JOBCTL:WAITPID-STOP-WAKE\n");
        uint32_t status=LIONOS_WAIT_STATUS_STOPPED(child->stop_signal);
        int status_ok=!parent->wait_status_ptr||
            write_user_u32(parent,parent->wait_status_ptr,status)==0;
        uint32_t *frame=(uint32_t *)(uintptr_t)__atomic_load_n(&parent->saved_frame,__ATOMIC_ACQUIRE);
        if(frame)frame[11]=status_ok?child->pid:PROCESS_SYSCALL_ERR;
        if(status_ok)child->stop_event_pending=0u;
        parent->wait_pid=0u;
        parent->wait_status_ptr=0u;
        parent->wait_options=0u;
        parent->wait_channel=0u;
        parent->wait_kind=0u;
        __atomic_store_n(&parent->wait_frame_saved,0u,__ATOMIC_RELEASE);
        (void)process_transition_state_locked(parent,PROCESS_READY);
        debug_write("LIONOS:JOBCTL:WAITPID-STOP-READY\n");
        return;
    }
}


static void wake_waiting_parent_continued(struct process *child){
    if(!child||!child->continued_event_pending)return;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *parent=&processes[i];
        if(parent->state!=PROCESS_WAITING||parent->wait_kind!=3u||
           parent->pid!=child->parent_pid||
           !(parent->wait_options&LIONOS_WAIT_CONTINUED)||
           (parent->wait_pid!=PROCESS_WAIT_ANY&&parent->wait_pid!=child->pid))
            continue;
        int status_ok=!parent->wait_status_ptr||
            write_user_u32(parent,parent->wait_status_ptr,LIONOS_WAIT_STATUS_CONTINUED)==0;
        uint32_t *frame=(uint32_t *)(uintptr_t)__atomic_load_n(&parent->saved_frame,__ATOMIC_ACQUIRE);
        if(frame)frame[11]=status_ok?child->pid:PROCESS_SYSCALL_ERR;
        if(status_ok)child->continued_event_pending=0u;
        parent->wait_pid=0u;parent->wait_status_ptr=0u;parent->wait_options=0u;
        parent->wait_channel=0u;parent->wait_kind=0u;__atomic_store_n(&parent->wait_frame_saved,0u,__ATOMIC_RELEASE);(void)process_transition_state_locked(parent,PROCESS_READY);
        return;
    }
}

static void wake_pending_waitpid_event(struct process *parent){
    if(!parent||parent->state!=PROCESS_WAITING||parent->wait_kind!=3u)
        return;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *child=&processes[i];
        if(child->state==PROCESS_UNUSED||child->reap_pending||
           child->parent_pid!=parent->pid||
           (parent->wait_pid!=PROCESS_WAIT_ANY&&parent->wait_pid!=child->pid))
            continue;
        if(child->state==PROCESS_ZOMBIE){
            wake_waiting_parent(child);
            return;
        }
        if((parent->wait_options&LIONOS_WAIT_UNTRACED)&&
           child->state==PROCESS_STOPPED&&child->stop_event_pending){
            wake_waiting_parent_stop(child);
            return;
        }
        if((parent->wait_options&LIONOS_WAIT_CONTINUED)&&child->continued_event_pending){
            wake_waiting_parent_continued(child);
            return;
        }
    }
}

static void stop_process_locked(struct process *process,uint32_t signal){
    if(!process||process->state==PROCESS_UNUSED||process->state==PROCESS_ZOMBIE)return;
    uint32_t bit=1u<<(signal-1u);
    int was_stopped=process->state==PROCESS_STOPPED&&
        (process->is_thread?(process->pending_signals&bit)!=0u:process->stop_signal!=0u);
    process->pending_signals|=bit;
    if(!process->is_thread&&!was_stopped){
        process->stop_event_pending=1u;
        process->stop_signal=signal;
    }
    if(process->state==PROCESS_WAITING&&(process->wait_kind==1u||process->wait_kind==2u)){
        uint32_t *frame=(uint32_t *)(uintptr_t)__atomic_load_n(&process->saved_frame,__ATOMIC_ACQUIRE);
        if(frame)frame[11]=LIONOS_FUTEX_INTERRUPTED;
        process->wait_channel=0u;process->wait_kind=0u;
        process->wait_timeout_active=0u;process->wait_timeout_result=0u;
    }
    if(!process->is_thread&&!was_stopped)
        process_diag_status33("PROCESS-STOP",process->pid,
                              process->parent_pid,signal);
    (void)process_transition_state_locked(process,PROCESS_STOPPED);
    uint32_t cpu=process->cpu_owner<LIONOS_MAX_CPUS?process->cpu_owner:0u;
    if(current_by_cpu[cpu]==process)
        process_request_blocked_reschedule(cpu);
    else
        __atomic_store_n(&reschedule_requested[cpu],1u,__ATOMIC_RELEASE);
    if(!process->is_thread){
        for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
            struct process *thread=&processes[i];
            if(thread->state==PROCESS_UNUSED||thread->state==PROCESS_ZOMBIE||
               !thread->is_thread||thread->thread_group_pid!=process->pid)continue;
            thread->pending_signals|=bit;
            if(thread->state==PROCESS_WAITING&&(thread->wait_kind==1u||thread->wait_kind==2u)){
                uint32_t *frame=(uint32_t *)(uintptr_t)__atomic_load_n(&thread->saved_frame,__ATOMIC_ACQUIRE);
                if(frame)frame[11]=LIONOS_FUTEX_INTERRUPTED;
                thread->wait_channel=0u;thread->wait_kind=0u;
                thread->wait_timeout_active=0u;thread->wait_timeout_result=0u;
            }
            (void)process_transition_state_locked(thread,PROCESS_STOPPED);
            cpu=thread->cpu_owner<LIONOS_MAX_CPUS?thread->cpu_owner:0u;
            if(current_by_cpu[cpu]==thread)
                process_request_blocked_reschedule(cpu);
            else
                __atomic_store_n(&reschedule_requested[cpu],1u,__ATOMIC_RELEASE);
        }
        if(!was_stopped)wake_waiting_parent_stop(process);
    }
}

static void continue_process_locked(struct process *process){
    if(!process||process->state==PROCESS_UNUSED||process->state==PROCESS_ZOMBIE)return;
    int was_stopped=process->state==PROCESS_STOPPED;
    const uint32_t stop_mask=(1u<<(LIONOS_SIG_STOP-1u))|
        (1u<<(LIONOS_SIG_TTIN-1u))|(1u<<(LIONOS_SIG_TTOU-1u));
    process->pending_signals&=~stop_mask;
    if(!process->is_thread){
        process->stop_event_pending=0u;
        process->stop_signal=0u;
        if(was_stopped)process->continued_event_pending=1u;
    }
    uint32_t resume_waitpid=(was_stopped&&process->wait_kind==3u);
    if(was_stopped)if(process_transition_state_locked(process,resume_waitpid?PROCESS_WAITING:PROCESS_READY)!=0)return;
    if(!process->is_thread){
        for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
            struct process *thread=&processes[i];
            if(thread->state==PROCESS_UNUSED||thread->state==PROCESS_ZOMBIE||
               !thread->is_thread||thread->thread_group_pid!=process->pid)continue;
            thread->pending_signals&=~stop_mask;
            if(thread->state==PROCESS_STOPPED)(void)process_transition_state_locked(thread,PROCESS_READY);
            uint32_t cpu=thread->cpu_owner<LIONOS_MAX_CPUS?thread->cpu_owner:0u;
            __atomic_store_n(&reschedule_requested[cpu],1u,__ATOMIC_RELEASE);
        }
    }
    if(was_stopped&&!process->is_thread)wake_waiting_parent_continued(process);
    if(resume_waitpid)wake_pending_waitpid_event(process);
}

static int process_group_orphaned_locked(uint32_t pgid,uint32_t sid){
    int found=0;
    if(!pgid||!sid)return 0;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *member=&processes[i];
        if(member->state==PROCESS_UNUSED||member->state==PROCESS_ZOMBIE||
           member->is_thread||member->pgid!=pgid||member->sid!=sid)continue;
        found=1;
        struct process *parent=find_pid(member->parent_pid);
        if(parent&&parent->state!=PROCESS_UNUSED&&parent->state!=PROCESS_ZOMBIE&&
           parent->sid==sid&&parent->pgid!=pgid)return 0;
    }
    return found;
}

static int process_group_has_stopped_locked(uint32_t pgid,uint32_t sid){
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        const struct process *member=&processes[i];
        if(member->state==PROCESS_STOPPED&&!member->is_thread&&
           member->pgid==pgid&&member->sid==sid)return 1;
    }
    return 0;
}

static uint32_t process_snapshot_child_groups_locked(uint32_t parent_pid,
    struct process_group_snapshot *groups,uint32_t capacity){
    uint32_t count=0u;
    if(!parent_pid||!groups||!capacity)return 0u;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        const struct process *child=&processes[i];
        if(child->state==PROCESS_UNUSED||child->state==PROCESS_ZOMBIE||
           child->is_thread||child->parent_pid!=parent_pid)continue;
        uint32_t j=0u;
        while(j<count&&(groups[j].pgid!=child->pgid||groups[j].sid!=child->sid))++j;
        if(j<count)continue;
        if(count>=capacity)break;
        groups[count].pgid=child->pgid;
        groups[count].sid=child->sid;
        groups[count].was_orphaned=(uint8_t)
            process_group_orphaned_locked(child->pgid,child->sid);
        ++count;
    }
    return count;
}

static void process_notify_child_group_transitions_locked(
    const struct process_group_snapshot *groups,uint32_t count){
    if(!groups)return;
    for(uint32_t i=0u;i<count;++i)
        process_notify_orphan_transition_locked(groups[i].pgid,groups[i].sid,
                                                 groups[i].was_orphaned);
}

static void process_notify_orphan_transition_locked(uint32_t pgid,uint32_t sid,int was_orphaned){
    if(!was_orphaned&&process_group_orphaned_locked(pgid,sid)&&
       process_group_has_stopped_locked(pgid,sid))
        process_orphan_group_notify_locked(pgid,sid);
}

static void process_orphan_group_notify_locked(uint32_t pgid,uint32_t sid){
    debug_write("LIONOS:JOBCTL:ORPHAN-NOTIFY\n");
    const uint32_t hup_bit=1u<<(LIONOS_SIG_HUP-1u);
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *member=&processes[i];
        if(member->state==PROCESS_UNUSED||member->state==PROCESS_ZOMBIE||
           member->is_thread||member->pgid!=pgid||member->sid!=sid)continue;
        member->pending_signals|=hup_bit;
        process_diag_status33("JOBCTL-HUP-MEMBER",member->pid,
                              member->parent_pid,member->state);
        process_diag_status33("JOBCTL-HUP-PGID",member->pid,
                              member->pgid,member->sid);
        if(member->state==PROCESS_WAITING&&member->wait_kind==3u&&
           __atomic_load_n(&member->wait_frame_saved,__ATOMIC_ACQUIRE)&&
           process_has_interrupting_signal_locked(member)){
            uint32_t frame_address=
                __atomic_load_n(&member->saved_frame,__ATOMIC_ACQUIRE);
            uint32_t *saved=(uint32_t *)(uintptr_t)frame_address;
            if(saved){saved[11]=(uint32_t)LIONOS_EINTR;process_clear_waitpid_locked(member);(void)process_transition_state_locked(member,PROCESS_READY);uint32_t cpu=member->cpu_owner<LIONOS_MAX_CPUS?member->cpu_owner:0u;__atomic_store_n(&reschedule_requested[cpu],1u,__ATOMIC_RELEASE);}
        }
        for(uint32_t j=1u;j<LIONOS_PROCESS_MAX;++j){
            struct process *thread=&processes[j];
            if(thread->state!=PROCESS_UNUSED&&thread->state!=PROCESS_ZOMBIE&&
               thread->is_thread&&thread->thread_group_pid==member->pid)
                thread->pending_signals|=hup_bit;
        }
    }
    debug_write("LIONOS:JOBCTL:ORPHAN-HUP-SET-DONE\n");
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *member=&processes[i];
        if(member->state!=PROCESS_UNUSED&&member->state!=PROCESS_ZOMBIE&&
           !member->is_thread&&member->pgid==pgid&&member->sid==sid){
            debug_write("LIONOS:JOBCTL:ORPHAN-CONTINUE-ENTER\n");
            continue_process_locked(member);
            debug_write("LIONOS:JOBCTL:ORPHAN-CONTINUE-DONE\n");
        }
    }
    debug_write("LIONOS:JOBCTL:ORPHAN-NOTIFY-DONE\n");
}

/* Called with process_lock held. Reparenting is immediate, as required when
   a parent exits; init (PID 1) is the adopter and auto-reaps its children. */
static void process_reparent_children_locked(struct process *exiting){
    uint32_t pgids[LIONOS_PROCESS_MAX],sids[LIONOS_PROCESS_MAX];
    uint8_t was_orphaned[LIONOS_PROCESS_MAX];
    uint32_t group_count=0u;
    if(!exiting||!exiting->pid)return;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *child=&processes[i];
        if(child->state==PROCESS_UNUSED||child->parent_pid!=exiting->pid||
           child->is_thread)continue;
        if(child->state!=PROCESS_ZOMBIE){
            uint32_t g=0u;
            while(g<group_count&&(pgids[g]!=child->pgid||sids[g]!=child->sid))++g;
            if(g==group_count&&group_count<LIONOS_PROCESS_MAX){
                pgids[g]=child->pgid;sids[g]=child->sid;
                was_orphaned[g]=(uint8_t)process_group_orphaned_locked(child->pgid,child->sid);
                ++group_count;
            }
        }
        process_diag_status33("PROCESS-REPARENT",child->pid,
                              child->parent_pid,child->state);
        child->parent_pid=1u;
        child->adopted_by_init=1u;
        if(child->state==PROCESS_ZOMBIE)child->reap_pending=1u;
    }
    (void)process_transition_state_locked(exiting,PROCESS_ZOMBIE);
    if(exiting->adopted_by_init)exiting->reap_pending=1u;
    for(uint32_t g=0u;g<group_count;++g){
        if(!was_orphaned[g]&&process_group_orphaned_locked(pgids[g],sids[g])&&
           process_group_has_stopped_locked(pgids[g],sids[g]))
            process_orphan_group_notify_locked(pgids[g],sids[g]);
    }
}

static void process_diag_hex32(uint32_t value){
    static const char digits[]="0123456789ABCDEF";
    for(uint32_t shift=28u;;shift-=4u){
        debug_putc(digits[(value>>shift)&0xFu]);
        if(shift==0u)break;
    }
}

static void process_diag_status33(const char *kind,uint32_t pid,
                                  uint32_t parent_pid,uint32_t value){
    debug_write("LIONOS:");
    debug_write(kind);
    debug_write(" pid=0x");
    process_diag_hex32(pid);
    debug_write(" parent=0x");
    process_diag_hex32(parent_pid);
    debug_write(" value=0x");
    process_diag_hex32(value);
    debug_write("\n");
}

void process_exit_current(uint32_t code){
    /*
     * Exit may be reached from more than one return/exception path. Once a
     * process is a zombie its exit status and parent relationship are final.
     * Check before closing resources, then recheck after cleanup under the
     * lock before publishing the terminal state.
     */
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *c=current_local();
    if(!c||c==&processes[0]||c->state==PROCESS_UNUSED||
       c->state==PROCESS_ZOMBIE){
        if(c&&c->state==PROCESS_ZOMBIE){
            process_diag_status33("PROCESS-EXIT-DUPLICATE",c->pid,
                                  c->parent_pid,code);
            process_diag_status33("PROCESS-EXIT-PRESERVED",c->pid,
                                  c->parent_pid,c->exit_code);
        }
        spinlock_irqrestore_release(&process_lock,flags);
        return;
    }
    spinlock_irqrestore_release(&process_lock,flags);

    int is_hup_exit=(code==(128u+LIONOS_SIG_HUP));
    if(is_hup_exit)debug_write("LIONOS:SIGHUP:EXIT-ENTER\n");
    if(!c->is_thread){
        pipe_close_all(c);
        vfs_close_all_process(c);
    }
    if(is_hup_exit)debug_write("LIONOS:SIGHUP:FD-CLOSE-DONE\n");

    flags=spinlock_irqsave_acquire(&process_lock);
    c=current_local();
    if(!c||c==&processes[0]||c->state==PROCESS_UNUSED||
       c->state==PROCESS_ZOMBIE){
        if(c&&c->state==PROCESS_ZOMBIE){
            process_diag_status33("PROCESS-EXIT-RACE-DUPLICATE",c->pid,
                                  c->parent_pid,code);
            process_diag_status33("PROCESS-EXIT-RACE-PRESERVED",c->pid,
                                  c->parent_pid,c->exit_code);
        }
        spinlock_irqrestore_release(&process_lock,flags);
        return;
    }
    if(is_hup_exit)debug_write("LIONOS:SIGHUP:LOCK-ACQUIRED\n");
    if(c->is_thread){
        process_diag_status33("THREAD-EXIT",c->tid,c->thread_group_pid,code);
        c->exit_code=code;
        if(c->thread_group_pid==0u)c->reap_pending=1u;
        (void)process_transition_state_locked(c,PROCESS_ZOMBIE);
        for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
            struct process*w=&processes[i];
            if(w->state!=PROCESS_WAITING||w->is_thread||
               w->thread_group_pid!=c->thread_group_pid||w->join_tid!=c->tid)
                continue;
            uint32_t*f=(uint32_t *)(uintptr_t)__atomic_load_n(&w->saved_frame,__ATOMIC_ACQUIRE);
            int status_ok=w->join_status_ptr&&
                write_user_u32(w,w->join_status_ptr,code)==0;
            process_diag_status33("THREAD-JOIN-WAKE",w->pid,c->tid,
                                  status_ok?code:PROCESS_SYSCALL_ERR);
            if(f)f[11]=status_ok?c->tid:PROCESS_SYSCALL_ERR;
            w->join_tid=0u;
            w->join_status_ptr=0u;
            (void)process_transition_state_locked(w,PROCESS_READY);
            c->reap_pending=1u;
            break;
        }
        spinlock_irqrestore_release(&process_lock,flags);
        uint32_t exit_cpu=cpu_current_index();
        process_request_blocked_reschedule(exit_cpu);
        process_kick_cpu(exit_cpu);
        return;
    }

    /* A process owns its user threads. Retire all non-current siblings before
       the address space can be reaped by waitpid. Threads share cpu_owner, so
       none of these siblings can be executing concurrently on another CPU. */
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process*t=&processes[i];
        if(t!=c&&t->state!=PROCESS_UNUSED&&t->is_thread&&
           t->thread_group_pid==c->thread_group_pid){
            (void)process_transition_state_locked(t,PROCESS_ZOMBIE);
            reap_process(t);
        }
    }
    uint32_t exiting_sid=c->sid;
    uint32_t diagnostic_pid=c->pid;
    uint32_t diagnostic_parent_pid=c->parent_pid;
    c->exit_code=code;
    process_reparent_children_locked(c);
    if(is_hup_exit)debug_write("LIONOS:SIGHUP:REPARENT-DONE\n");
    (void)process_transition_state_locked(c,PROCESS_ZOMBIE);
    if(is_hup_exit)debug_write("LIONOS:SIGHUP:ZOMBIE-SET\n");
    wake_waiting_parent(c);
    if(is_hup_exit)debug_write("LIONOS:SIGHUP:PARENT-WAKE-DONE\n");
    spinlock_irqrestore_release(&process_lock,flags);
    process_request_reschedule();
    if(is_hup_exit)debug_write("LIONOS:SIGHUP:UNLOCKED\n");
    process_diag_status33("PROCESS-EXIT-STATUS",diagnostic_pid,
                          diagnostic_parent_pid,code);
    process_release_tty_if_session_empty(exiting_sid);
    if(is_hup_exit)debug_write("LIONOS:SIGHUP:TTY-RELEASE-DONE\n");
}
static struct process*find_pid(uint32_t pid){for(uint32_t i=0;i<LIONOS_PROCESS_MAX;++i)if(processes[i].state!=PROCESS_UNUSED&&processes[i].pid==pid)return &processes[i];return 0;}
int process_is_descendant_or_child(uint32_t pid,uint32_t ancestor_pid){uint32_t flags=spinlock_irqsave_acquire(&process_lock);struct process*p=find_pid(pid);if(!p||!ancestor_pid){spinlock_irqrestore_release(&process_lock,flags);return 0;}if(p->parent_pid==ancestor_pid){spinlock_irqrestore_release(&process_lock,flags);return 1;}for(uint32_t d=0;d<LIONOS_PROCESS_MAX&&p->parent_pid;++d){p=find_pid(p->parent_pid);if(!p)break;if(p->pid==ancestor_pid){spinlock_irqrestore_release(&process_lock,flags);return 1;}}spinlock_irqrestore_release(&process_lock,flags);return 0;}
static int process_is_descendant_locked(const struct process *candidate,uint32_t ancestor_pid){
    if(!candidate||!ancestor_pid)return 0;
    uint32_t parent_pid=candidate->parent_pid;
    for(uint32_t depth=0u;depth<LIONOS_PROCESS_MAX&&parent_pid;++depth){
        if(parent_pid==ancestor_pid)return 1;
        struct process *parent=find_pid(parent_pid);
        if(!parent)break;
        parent_pid=parent->parent_pid;
    }
    return 0;
}

int32_t process_tty_signal_foreground(uint32_t sid,uint32_t pgid,uint32_t signal){
    if(!sid||!pgid||signal!=LIONOS_SIG_INT)return LIONOS_SIGNAL_ERR;
    uint32_t bit=1u<<(signal-1u);
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    if(tty_get_foreground_pgid(sid)!=(int32_t)pgid){
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_SIGNAL_ERR;
    }
    uint32_t members=0u;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *member=&processes[i];
        if(member->state==PROCESS_UNUSED||member->is_thread||
           member->state==PROCESS_ZOMBIE||member->pgid!=pgid)continue;
        if(member->sid!=sid){
            spinlock_irqrestore_release(&process_lock,flags);
            return LIONOS_SIGNAL_ERR;
        }
        ++members;
        for(uint32_t j=1u;j<LIONOS_PROCESS_MAX;++j){
            struct process *thread=&processes[j];
            if(thread->state==PROCESS_UNUSED||!thread->is_thread||
               thread->thread_group_pid!=member->pid||thread->state==PROCESS_ZOMBIE)continue;
            if(thread->sid!=sid){
                spinlock_irqrestore_release(&process_lock,flags);
                return LIONOS_SIGNAL_ERR;
            }
        }
    }
    if(!members){
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_SIGNAL_ERR;
    }
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *member=&processes[i];
        if(member->state==PROCESS_UNUSED||member->is_thread||
           member->state==PROCESS_ZOMBIE||member->pgid!=pgid)continue;
        member->pending_signals|=bit;
        if(member->state==PROCESS_WAITING&&member->wait_kind==3u&&
           __atomic_load_n(&member->wait_frame_saved,__ATOMIC_ACQUIRE)&&
           process_has_interrupting_signal_locked(member)){
            uint32_t frame_address=
                __atomic_load_n(&member->saved_frame,__ATOMIC_ACQUIRE);
            uint32_t *saved=(uint32_t *)(uintptr_t)frame_address;
            if(saved){saved[11]=(uint32_t)LIONOS_EINTR;process_clear_waitpid_locked(member);(void)process_transition_state_locked(member,PROCESS_READY);uint32_t cpu=member->cpu_owner<LIONOS_MAX_CPUS?member->cpu_owner:0u;__atomic_store_n(&reschedule_requested[cpu],1u,__ATOMIC_RELEASE);}
        }
        for(uint32_t j=1u;j<LIONOS_PROCESS_MAX;++j){
            struct process *thread=&processes[j];
            if(thread->state!=PROCESS_UNUSED&&thread->is_thread&&
               thread->thread_group_pid==member->pid&&thread->state!=PROCESS_ZOMBIE)
                thread->pending_signals|=bit;
        }
    }
    spinlock_irqrestore_release(&process_lock,flags);
    return LIONOS_SIGNAL_OK;
}

int32_t process_tty_check_foreground(uint32_t sid,uint32_t caller_pid,uint32_t signal){
    if(!sid||!caller_pid||
       (signal!=LIONOS_SIG_TTIN&&signal!=LIONOS_SIG_TTOU))
        return LIONOS_SIGNAL_ERR;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *caller=find_pid(caller_pid);
    if(caller&&caller->is_thread)
        caller=find_pid(caller->thread_group_pid);
    if(!caller||caller==&processes[0]||caller->state==PROCESS_ZOMBIE||caller->sid!=sid||
       !caller->pgid){
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_SIGNAL_ERR;
    }
    int32_t foreground=tty_get_foreground_pgid(sid);
    if(foreground<=0){
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_SIGNAL_ERR;
    }
    if(caller->pgid==(uint32_t)foreground){
        spinlock_irqrestore_release(&process_lock,flags);
        return 0;
    }
    debug_write("LIONOS:JOBCTL:BACKGROUND-CHECK\n");
    /* POSIX job-control signals are not generated for an orphaned group. */
    if(process_group_orphaned_locked(caller->pgid,sid)){
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_SIGNAL_ERR;
    }
    uint32_t members=0u;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *member=&processes[i];
        if(member->state==PROCESS_UNUSED||member->state==PROCESS_ZOMBIE||
           member->is_thread||member->pgid!=caller->pgid)
            continue;
        if(member->sid!=sid){
            spinlock_irqrestore_release(&process_lock,flags);
            return LIONOS_SIGNAL_ERR;
        }
        ++members;
        for(uint32_t j=1u;j<LIONOS_PROCESS_MAX;++j){
            struct process *thread=&processes[j];
            if(thread->state==PROCESS_UNUSED||thread->state==PROCESS_ZOMBIE||
               !thread->is_thread||thread->thread_group_pid!=member->pid)
                continue;
            if(thread->sid!=sid){
                spinlock_irqrestore_release(&process_lock,flags);
                return LIONOS_SIGNAL_ERR;
            }
        }
    }
    if(!members){
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_SIGNAL_ERR;
    }
    debug_write("LIONOS:JOBCTL:BACKGROUND-STOP-REQUEST\n");
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *member=&processes[i];
        if(member->state==PROCESS_UNUSED||member->state==PROCESS_ZOMBIE||
           member->is_thread||member->pgid!=caller->pgid)continue;
        stop_process_locked(member,signal);
    }
    debug_write("LIONOS:JOBCTL:BACKGROUND-STOP-DONE\n");
    spinlock_irqrestore_release(&process_lock,flags);
    return 1;
}

int32_t process_signal_group(uint32_t pgid,uint32_t signal,uint32_t sender_pid){
    uint32_t targets[LIONOS_PROCESS_MAX];
    uint32_t count=0u;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *sender=sender_pid?find_pid(sender_pid):current_local();
    if(sender&&sender->is_thread)sender=find_pid(sender->thread_group_pid);
    if(!sender||sender==&processes[0]||!pgid||signal==0u||signal>LIONOS_SIG_MAX){
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_SIGNAL_ERR;
    }
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *member=&processes[i];
        if(member->state==PROCESS_UNUSED||member->is_thread||
           member->state==PROCESS_ZOMBIE||member->pgid!=pgid)continue;
        if(member==sender||!process_is_descendant_locked(member,sender->pid)){
            spinlock_irqrestore_release(&process_lock,flags);
            return LIONOS_SIGNAL_ERR;
        }
        if(signal==LIONOS_SIG_KILL||signal==LIONOS_SIG_TERM){
            for(uint32_t j=1u;j<LIONOS_PROCESS_MAX;++j){
                struct process *thread=&processes[j];
                if(thread->state!=PROCESS_UNUSED&&thread->is_thread&&
                   thread->thread_group_pid==member->pid&&thread->state!=PROCESS_ZOMBIE){
                    spinlock_irqrestore_release(&process_lock,flags);
                    return LIONOS_SIGNAL_ERR;
                }
            }
        }
        if(count<LIONOS_PROCESS_MAX)targets[count++]=member->pid;
    }
    spinlock_irqrestore_release(&process_lock,flags);
    if(!count)return LIONOS_SIGNAL_ERR;
    uint32_t delivered=0u;
    for(uint32_t i=0u;i<count;++i)
        if(process_signal(targets[i],signal)==LIONOS_SIGNAL_OK)++delivered;
    return delivered?LIONOS_SIGNAL_OK:LIONOS_SIGNAL_ERR;
}

int32_t process_signal(uint32_t pid,uint32_t signal){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process*p=find_pid(pid);
    if(!p||p==&processes[0]||p->state==PROCESS_ZOMBIE||signal==0||signal>LIONOS_SIG_MAX){
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_SIGNAL_ERR;
    }
    if(signal==LIONOS_SIG_KILL||signal==LIONOS_SIG_TERM){
        if(!p->is_thread){
            for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
                struct process *thread=&processes[i];
                if(thread->state!=PROCESS_UNUSED&&thread->is_thread&&
                   thread->thread_group_pid==p->pid&&thread->state!=PROCESS_ZOMBIE){
                    spinlock_irqrestore_release(&process_lock,flags);
                    return LIONOS_SIGNAL_ERR;
                }
            }
        }
        p->wait_channel=0u;
        p->wait_kind=0u;
        p->wait_timeout_active=0u;
        p->wait_timeout_result=0u;
        __atomic_store_n(&p->wait_frame_saved,0u,__ATOMIC_RELEASE);
        if(p->is_thread){
            p->pending_signals|=(1u<<(signal-1u));
            p->exit_code=128u+signal;
            (void)process_transition_state_locked(p,PROCESS_ZOMBIE);
            uint32_t target_cpu=p->cpu_owner<LIONOS_MAX_CPUS?p->cpu_owner:0u;
            if(current_by_cpu[target_cpu]==p)
                process_request_blocked_reschedule(target_cpu);
            spinlock_irqrestore_release(&process_lock,flags);
            return LIONOS_SIGNAL_OK;
        }
        p->pending_signals|=(1u<<(signal-1u));
        p->exit_code=128u+signal;
        (void)process_transition_state_locked(p,PROCESS_STOPPED);
        uint32_t target_cpu=p->cpu_owner<LIONOS_MAX_CPUS?p->cpu_owner:0u;
        if(current_by_cpu[target_cpu]==p)
            process_request_blocked_reschedule(target_cpu);
        uint32_t exiting_sid=p->sid;
        spinlock_irqrestore_release(&process_lock,flags);
        pipe_close_all(p);
        vfs_close_all_process(p);
        flags=spinlock_irqsave_acquire(&process_lock);
        if(p->state==PROCESS_STOPPED){
            process_reparent_children_locked(p);
            wake_waiting_parent(p);
        }
        spinlock_irqrestore_release(&process_lock,flags);
        process_release_tty_if_session_empty(exiting_sid);
        return LIONOS_SIGNAL_OK;
    }
    if(signal==LIONOS_SIG_HUP||signal==LIONOS_SIG_INT){
        p->pending_signals|=(1u<<(signal-1u));
        if(p->state==PROCESS_WAITING&&p->wait_kind==3u&&
           __atomic_load_n(&p->wait_frame_saved,__ATOMIC_ACQUIRE)&&
           process_has_interrupting_signal_locked(p)){
            uint32_t frame_address=
                __atomic_load_n(&p->saved_frame,__ATOMIC_ACQUIRE);
            uint32_t *saved=(uint32_t *)(uintptr_t)frame_address;
            if(saved){debug_write("LIONOS:WAITPID-EINTR-REMOTE-WAKE\n");saved[11]=(uint32_t)LIONOS_EINTR;process_clear_waitpid_locked(p);(void)process_transition_state_locked(p,PROCESS_READY);uint32_t cpu=p->cpu_owner<LIONOS_MAX_CPUS?p->cpu_owner:0u;__atomic_store_n(&reschedule_requested[cpu],1u,__ATOMIC_RELEASE);}
        }
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_SIGNAL_OK;
    }
    if(signal==LIONOS_SIG_STOP||signal==LIONOS_SIG_TTIN||signal==LIONOS_SIG_TTOU){
        stop_process_locked(p,signal);
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_SIGNAL_OK;
    }
    if(signal==LIONOS_SIG_CONT){
        continue_process_locked(p);
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_SIGNAL_OK;
    }
    spinlock_irqrestore_release(&process_lock,flags);
    return LIONOS_SIGNAL_ERR;
}
int32_t process_signal_set_action(uint32_t signal,const struct lion_sigaction *action,
                                  struct lion_sigaction *old_action){
    if(signal!=LIONOS_SIG_HUP&&signal!=LIONOS_SIG_INT)
        return LIONOS_SIGNAL_ERR;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *current=current_local();
    if(!current||current==&processes[0]||current->is_thread||
       current->state==PROCESS_UNUSED||current->state==PROCESS_ZOMBIE){
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_SIGNAL_ERR;
    }
    uint32_t index=signal-1u;
    if(old_action){
        old_action->handler=current->signal_handlers[index];
        old_action->restorer=current->signal_restorers[index];
    }
    if(!action){
        spinlock_irqrestore_release(&process_lock,flags);
        return LIONOS_SIGNAL_OK;
    }
    if(action->handler>LIONOS_SIG_IGN){
        if(!action->restorer||
           !paging_user_page_executable(current->page_directory,action->handler)||
           !paging_user_page_executable(current->page_directory,action->restorer)){
            spinlock_irqrestore_release(&process_lock,flags);
            return LIONOS_SIGNAL_ERR;
        }
        current->signal_handlers[index]=action->handler;
        current->signal_restorers[index]=action->restorer;
    }else{
        current->signal_handlers[index]=action->handler;
        current->signal_restorers[index]=0u;
    }
    spinlock_irqrestore_release(&process_lock,flags);
    return LIONOS_SIGNAL_OK;
}

int process_deliver_pending_signal(uint32_t *frame){
    const uint32_t hup_bit=1u<<(LIONOS_SIG_HUP-1u);
    const uint32_t int_bit=1u<<(LIONOS_SIG_INT-1u);
    uint32_t signal=0u,handler=0u,restorer=0u,stack=0u;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *current=current_local();
    if(!current||current==&processes[0]||current->is_thread||
       current->state==PROCESS_UNUSED||current->state==PROCESS_ZOMBIE||
       current->signal_active){
        spinlock_irqrestore_release(&process_lock,flags);
        return 0;
    }
    if(current->pending_signals&hup_bit)signal=LIONOS_SIG_HUP;
    else if(current->pending_signals&int_bit)signal=LIONOS_SIG_INT;
    if(signal==0u){
        spinlock_irqrestore_release(&process_lock,flags);
        return 0;
    }
    handler=current->signal_handlers[signal-1u];
    restorer=current->signal_restorers[signal-1u];
    if(handler!=LIONOS_SIG_IGN&&current->state==PROCESS_WAITING&&current->wait_kind==3u&&frame){
        debug_write("LIONOS:WAITPID-EINTR-CURRENT-WAKE\n");
        frame[11]=(uint32_t)LIONOS_EINTR;process_clear_waitpid_locked(current);current->state=PROCESS_RUNNING;
    }
    if(current->wait_kind==0u&&handler>LIONOS_SIG_IGN&&signal==LIONOS_SIG_INT)
        debug_write("LIONOS:SIGINT-HANDLER-PENDING-DELIVERY\n");
    current->pending_signals&=~(1u<<(signal-1u));
    if(handler==LIONOS_SIG_IGN){
        spinlock_irqrestore_release(&process_lock,flags);
        return 0;
    }
    if(handler>LIONOS_SIG_IGN&&frame&&(frame[15]&3u)==3u&&
       frame[17]>=0x10000008u&&frame[17]<=0xC0000000u){
        stack=((frame[17]-8u)&~0xFu)-4u;
        if(stack>=0x10000000u&&stack<=0xBFFFFFF8u){
            current->signal_active=1u;
            current->signal_active_restorer=restorer;
            for(uint32_t i=0u;i<PROCESS_CONTEXT_WORDS;++i)
                current->signal_saved_frame[i]=frame[i];
            spinlock_irqrestore_release(&process_lock,flags);
            uint32_t user_frame[2]={restorer,signal};
            if(usercopy_copy_to(stack,user_frame,sizeof(user_frame))==0){
                frame[14]=handler;
                frame[17]=stack;
                debug_write(signal==LIONOS_SIG_HUP
                    ?"LIONOS:SIGHUP:HANDLER-ENTER\n"
                    :"LIONOS:SIGINT:HANDLER-ENTER\n");
                return 1;
            }
            flags=spinlock_irqsave_acquire(&process_lock);
            current=current_local();
            if(current){current->signal_active=0u;current->signal_active_restorer=0u;}
            spinlock_irqrestore_release(&process_lock,flags);
        }else{
            spinlock_irqrestore_release(&process_lock,flags);
        }
    }else{
        spinlock_irqrestore_release(&process_lock,flags);
    }
    if(signal==LIONOS_SIG_HUP)debug_write("LIONOS:SIGHUP:DELIVER\n");
    else debug_write("LIONOS:SIGINT:DELIVER\n");
    process_exit_current(128u+signal);
    return 1;
}

int process_signal_return(uint32_t *frame){
    if(!frame)return 0;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *current=current_local();
    if(!current||current==&processes[0]||current->is_thread||
       current->state==PROCESS_UNUSED||current->state==PROCESS_ZOMBIE||
       !current->signal_active||
       !current->signal_active_restorer||
       current->signal_active_restorer>0xFFFFFFF8u||
       frame[14]!=current->signal_active_restorer+7u){
        spinlock_irqrestore_release(&process_lock,flags);
        return 0;
    }
    for(uint32_t i=0u;i<PROCESS_CONTEXT_WORDS;++i)
        frame[i]=current->signal_saved_frame[i];
    current->signal_active=0u;
    current->signal_active_restorer=0u;
    for(uint32_t i=0u;i<PROCESS_CONTEXT_WORDS;++i)
        current->signal_saved_frame[i]=0u;
    spinlock_irqrestore_release(&process_lock,flags);
    debug_write("LIONOS:SIGNAL-HANDLER-RETURN\n");
    return 1;
}
uint32_t process_signal_pending(uint32_t pid){uint32_t flags=spinlock_irqsave_acquire(&process_lock);struct process*p=find_pid(pid);uint32_t r=p?p->pending_signals:0u;spinlock_irqrestore_release(&process_lock,flags);return r;}
int32_t process_get_state(uint32_t pid){uint32_t flags=spinlock_irqsave_acquire(&process_lock);struct process*p=find_pid(pid);int32_t r=p?(int32_t)p->state:-1;spinlock_irqrestore_release(&process_lock,flags);return r;}
int process_has_capability(const struct process *p,uint32_t capability){return p&&(p->capabilities&capability)==capability;}
uint32_t process_capabilities(const struct process *p){return p?p->capabilities:0u;}
int process_set_capabilities(struct process *p,uint32_t capabilities){uint32_t flags=spinlock_irqsave_acquire(&process_lock);if(!p||p==&processes[0]){spinlock_irqrestore_release(&process_lock,flags);return -1;}p->capabilities=capabilities&~PROCESS_CAP_ADMIN;spinlock_irqrestore_release(&process_lock,flags);return 0;}

int process_set_current(struct process*p){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    uint32_t cpu=cpu_current_index();
    struct process *old=current_local();
    uint32_t slot;
    if(process_slot_index(p,&slot)!=0||p->state==PROCESS_UNUSED||
       p->state==PROCESS_STOPPED||!p->page_directory||cpu>=LIONOS_MAX_CPUS||
       (p!=old&&p->state==PROCESS_RUNNING)||
       (p->state==PROCESS_READY&&p->cpu_owner!=cpu)){
        spinlock_irqrestore_release(&process_lock,flags);return -1;
    }
    int old_requeued=0;
    if(old&&old!=p&&old!=&processes[0]&&!scheduler_is_idle_context(old)&&
       old->state==PROCESS_RUNNING){
        if(process_transition_state_locked(old,PROCESS_READY)!=0){
            spinlock_irqrestore_release(&process_lock,flags);return -1;
        }
        old_requeued=1;
    }
    if(p!=old&&process_transition_state_locked(p,PROCESS_RUNNING)!=0){
        if(old_requeued&&process_transition_state_locked(old,PROCESS_RUNNING)!=0)
            __atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);
        spinlock_irqrestore_release(&process_lock,flags);return -1;
    }
    set_current_local(p);
    p->cpu_owner=cpu;
    paging_switch_address_space(p->page_directory);
    tss_set_kernel_stack(p->kernel_stack_top);
    spinlock_irqrestore_release(&process_lock,flags);
    return 0;
}
uint32_t process_count(void){uint32_t flags=spinlock_irqsave_acquire(&process_lock),n=0;for(uint32_t i=0;i<LIONOS_PROCESS_MAX;++i)if(processes[i].state!=PROCESS_UNUSED)++n;spinlock_irqrestore_release(&process_lock,flags);return n;}
void process_set_saved_frame(struct process*p,uint32_t*f){uint32_t flags=spinlock_irqsave_acquire(&process_lock);if(p)__atomic_store_n(&p->saved_frame,(uint32_t)(uintptr_t)f,__ATOMIC_RELEASE);spinlock_irqrestore_release(&process_lock,flags);}
uint32_t*process_saved_frame(struct process*p){return p?(uint32_t *)(uintptr_t)__atomic_load_n(&p->saved_frame,__ATOMIC_ACQUIRE):0;}
uint32_t process_kernel_stack_top(struct process*p){return p?p->kernel_stack_top:0;}

static uint32_t thread_stack_va_for_slot(uint32_t slot){return PROCESS_THREAD_STACK_BASE-(slot*PROCESS_THREAD_STACK_STRIDE)-0x1000u;}
struct process *process_create_kernel_thread_on_cpu(uint32_t entry,uint32_t arg,uint32_t cpu){
    if(!entry)return 0;
    uint32_t online=smp_online_count();
    if(online==0u||online>LIONOS_MAX_CPUS)online=1u;
    if(cpu>=online||cpu>=LIONOS_MAX_CPUS)return 0;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process*p=find_free_slot();if(!p){spinlock_irqrestore_release(&process_lock,flags);return 0;}
    void*ks=page_alloc();if(!ks){spinlock_irqrestore_release(&process_lock,flags);return 0;}
    clear_process(p);p->pid=0;p->tid=next_tid++;if(next_tid==0)next_tid=1;p->thread_group_pid=0;p->is_thread=1;p->owns_address_space=0;p->state=PROCESS_STOPPED;p->cpu_owner=cpu;p->page_directory=paging_kernel_directory();p->entry=entry;p->kernel_stack_top=(uint32_t)(uintptr_t)ks+4096u;
    /*
     * isr_common restores a same-ring kernel context with IRETD.  IRETD
     * consumes EIP/CS/EFLAGS but does not load ESP/SS, so after the restore
     * the live stack begins at frame[17].  Seed that stack as a normal i386
     * call frame: return address followed by the entry argument.  The
     * three-word padding makes ESP 12 mod 16 at function entry, matching
     * the i386 SysV ABI while keeping both words inside this stack page.
     */
    uint32_t *f=(uint32_t *)(uintptr_t)
        (p->kernel_stack_top-(PROCESS_CONTEXT_WORDS+3u)*sizeof(uint32_t));
    for(uint32_t i=0u;i<PROCESS_CONTEXT_WORDS;++i)f[i]=0u;
    f[14]=entry;
    f[15]=0x08u;
    f[16]=0x202u;
    f[17]=(uint32_t)(uintptr_t)&process_kernel_thread_return_trap;
    f[18]=arg;
    __atomic_store_n(&p->saved_frame,(uint32_t)(uintptr_t)f,__ATOMIC_RELEASE);
    if(process_transition_state_locked(p,PROCESS_READY)!=0){
        (void)process_transition_state_locked(p,PROCESS_ZOMBIE);
        reap_process(p);
        spinlock_irqrestore_release(&process_lock,flags);
        return 0;
    }
    spinlock_irqrestore_release(&process_lock,flags);
    process_kick_cpu(cpu);
    return p;
}
struct process *process_create_kernel_thread(uint32_t entry,uint32_t arg){
    return process_create_kernel_thread_on_cpu(entry,arg,cpu_current_index());
}
uint32_t process_create_user_thread(uint32_t entry){
    if(!entry)return PROCESS_SYSCALL_ERR;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);struct process*parent=current_local();
    if(!parent||parent==&processes[0]||
       !paging_user_page_executable(parent->page_directory,entry)){
        spinlock_irqrestore_release(&process_lock,flags);return PROCESS_SYSCALL_ERR;
    }
    struct process*p=find_free_slot();if(!p){spinlock_irqrestore_release(&process_lock,flags);return PROCESS_SYSCALL_ERR;}
    void*kp=page_alloc();void*ks=page_alloc();if(!kp||!ks){if(kp)page_free(kp);if(ks)page_free(ks);spinlock_irqrestore_release(&process_lock,flags);return PROCESS_SYSCALL_ERR;}
    uint32_t slot=process_index(p),va=thread_stack_va_for_slot(slot);
    if(paging_map_user_page_in(parent->page_directory,va,(uint32_t)(uintptr_t)kp,3u)!=0){page_free(kp);page_free(ks);spinlock_irqrestore_release(&process_lock,flags);return PROCESS_SYSCALL_ERR;}
    for(uint32_t i=0;i<4096u;++i)((uint8_t*)kp)[i]=0;
    uint32_t top=va+4096u;uint32_t*sp=(uint32_t*)(uintptr_t)top;--sp;*sp=0u;
    clear_process(p);p->pid=0;p->tid=next_tid++;if(next_tid==0)next_tid=1;p->thread_group_pid=parent->thread_group_pid;p->pgid=parent->pgid;p->sid=parent->sid;p->is_thread=1;p->owns_address_space=0;(void)process_transition_state_locked(p,PROCESS_STOPPED);p->cpu_owner=cpu_current_index();p->page_directory=parent->page_directory;p->entry=entry;p->user_stack=(uint32_t)(uintptr_t)sp;p->kernel_stack_top=(uint32_t)(uintptr_t)ks+4096u;p->thread_stack_page=(uint32_t)(uintptr_t)kp;p->thread_stack_va=va;p->capabilities=parent->capabilities;p->priority=parent->priority;p->base_priority=parent->base_priority;p->time_slice=quantum_for_priority(p->priority);p->fpu_valid=0;
    uint32_t*f=(uint32_t*)(uintptr_t)(p->kernel_stack_top-PROCESS_CONTEXT_WORDS*4u);for(uint32_t i=0;i<PROCESS_CONTEXT_WORDS;++i)f[i]=0;f[14]=entry;f[15]=USER_CODE_SEL;f[16]=0x202u;f[17]=p->user_stack;f[18]=USER_DATA_SEL;__atomic_store_n(&p->saved_frame,(uint32_t)(uintptr_t)f,__ATOMIC_RELEASE);
    if(process_transition_state_locked(p,PROCESS_READY)!=0){
        (void)process_transition_state_locked(p,PROCESS_ZOMBIE);
        reap_process(p);
        spinlock_irqrestore_release(&process_lock,flags);
        return PROCESS_SYSCALL_ERR;
    }
    spinlock_irqrestore_release(&process_lock,flags);return p->tid;
}
void process_exit_thread(uint32_t code){process_exit_current(code);}
int32_t process_join_thread(uint32_t tid,uint32_t status_ptr){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process*c=current_local();
    if(!c||c->is_thread||!tid||!status_ptr){
        process_diag_status33("THREAD-JOIN-INVALID",tid,status_ptr,
                              cpu_current_index());
        spinlock_irqrestore_release(&process_lock,flags);
        return -1;
    }
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process*p=&processes[i];
        if(!p->is_thread||p->thread_group_pid!=c->thread_group_pid||
           p->tid!=tid)continue;
        if(p->state==PROCESS_ZOMBIE){
            if(p->reap_pending){
                process_diag_status33("THREAD-JOIN-REAP-PENDING",tid,c->pid,
                                      p->exit_code);
                spinlock_irqrestore_release(&process_lock,flags);
                return -1;
            }
            if(write_user_u32(c,status_ptr,p->exit_code)!=0){
                process_diag_status33("THREAD-JOIN-STATUS-WRITE-FAIL",tid,
                                      c->pid,status_ptr);
                spinlock_irqrestore_release(&process_lock,flags);
                return -1;
            }
            process_diag_status33("THREAD-JOIN-REAP",tid,c->pid,p->exit_code);
            reap_process(p);
            spinlock_irqrestore_release(&process_lock,flags);
            return (int32_t)tid;
        }
        process_diag_status33("THREAD-JOIN-BLOCK",tid,c->pid,p->state);
        c->join_tid=tid;
        c->join_status_ptr=status_ptr;
        (void)process_transition_state_locked(c,PROCESS_WAITING);
        process_request_blocked_reschedule(cpu_current_index());
        spinlock_irqrestore_release(&process_lock,flags);
        return PROCESS_WAIT_BLOCKED;
    }
    process_diag_status33("THREAD-JOIN-NOT-FOUND",tid,c->pid,
                          c->thread_group_pid);
    spinlock_irqrestore_release(&process_lock,flags);
    return -1;
}

void process_request_reschedule(void){
    uint32_t cpu=cpu_current_index();
    if(cpu>=LIONOS_MAX_CPUS)cpu=0u;
    __atomic_store_n(&reschedule_requested[cpu],1u,__ATOMIC_RELEASE);
}

int process_set_priority(struct process*p,uint32_t priority){
    uint32_t index;
    if(priority>=PROCESS_PRIORITY_LEVELS||
       process_slot_index(p,&index)!=0)return -1;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    if(p->state==PROCESS_READY){
        uint32_t owner=runqueue_nodes[index].queue_cpu;
        if(owner>=LIONOS_MAX_CPUS){
            __atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);
            spinlock_irqrestore_release(&process_lock,flags);return -1;
        }
        spinlock_acquire(&runqueues[owner].lock);
        int queued=runqueue_requeue_priority_locked(&runqueues[owner],
            runqueue_nodes,LIONOS_PROCESS_MAX,index,priority);
        spinlock_release(&runqueues[owner].lock);
        if(queued!=0){__atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);spinlock_irqrestore_release(&process_lock,flags);return -1;}
    }
    p->priority=priority;p->base_priority=priority;p->time_slice=quantum_for_priority(priority);p->ticks_used=0;
    spinlock_irqrestore_release(&process_lock,flags);
    return 0;
}
uint32_t process_scheduler_ticks(uint32_t cpu){return cpu<LIONOS_MAX_CPUS?scheduler_cpu[cpu].schedule_count:0u;}
uint32_t process_scheduler_steals(uint32_t cpu){return cpu<LIONOS_MAX_CPUS?scheduler_cpu[cpu].steal_count:0u;}

uint32_t *process_schedule(uint32_t *frame){
    process_scheduler_ap_probe_report();
    if(__atomic_load_n(&process_scheduler_ready,__ATOMIC_ACQUIRE)==0u)
        return frame;
    if(__atomic_load_n(&runqueue_fault,__ATOMIC_ACQUIRE)!=0u){
        uint32_t expected=0u;
        if(__atomic_compare_exchange_n(&runqueue_fault_reported,&expected,1u,0,
                __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
            debug_write("LIONOS:RUNQUEUE-FAULT\n");
    }
    /* Do not let a timer interrupt spin behind another CPU. Per-CPU\n     * run-queue locks are in place, but process lifetime and state are still\n     * serialized by process_lock; dropping a contended tick is preferable\n     * to stalling the CPU in an IRQ-disabled spin loop. */
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    uint32_t cpu=cpu_current_index();
    if(cpu>=LIONOS_MAX_CPUS)cpu=0u;
    if(cpu!=0u){uint32_t bit=1u<<cpu;uint32_t old=__atomic_fetch_or(&smp_ap_schedule_enter_seen,bit,__ATOMIC_ACQ_REL);if(!(old&bit)){debug_write("LIONOS:SMP-AP-SCHEDULE-ENTER cpu=");process_debug_write_hex(cpu);debug_write("\n");}}
    struct process *contended_prev=current_local();
    if(!spinlock_try_acquire(&process_lock)){
        if(cpu!=0u){uint32_t bit=1u<<cpu;uint32_t old=__atomic_fetch_or(&smp_ap_schedule_lock_contention_seen,bit,__ATOMIC_ACQ_REL);if(!(old&bit)){debug_write("LIONOS:SMP-AP-SCHEDULE-LOCK-CONTENDED cpu=");process_debug_write_hex(cpu);debug_write("\n");}}
        /* An explicit reschedule must not be lost when another CPU briefly
         * owns process_lock. Queue one local APIC retry; the IPI is delivered
         * after this interrupt returns, without spinning in IRQ context. */
        if(__atomic_load_n(&reschedule_requested[cpu],__ATOMIC_ACQUIRE)!=0u){
            uint32_t expected=0u;
            uint32_t apic=lapic_id();
            if(apic!=0xFFFFFFFFu&&
               __atomic_compare_exchange_n(&reschedule_ipi_pending[cpu],
                   &expected,1u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
                lapic_send_ipi(apic,LIONOS_LAPIC_RESCHEDULE_VECTOR);
        }
        /*
         * A contended timer tick may be dropped for a runnable task, but a
         * blocked/stopped/zombie task must never be returned to user mode.
         * Switch it to the CPU-local bootstrap/idle context without touching
         * the contended global process lock.
         */
        if(contended_prev&&
           __atomic_load_n(&reschedule_blocked[cpu],__ATOMIC_ACQUIRE)!=0u){
            __atomic_store_n(&contended_prev->saved_frame,
                             (uint32_t)(uintptr_t)frame,__ATOMIC_RELEASE);
            if(frame)
                __atomic_store_n(&contended_prev->wait_frame_saved,1u,
                                 __ATOMIC_RELEASE);
            if(cpu==0u){
                struct process *bootstrap=&processes[0];
                fpu_save(contended_prev);
                fpu_restore(bootstrap);
                set_current_local(bootstrap);
                bootstrap->cpu_owner=0u;
                bootstrap->state=PROCESS_RUNNING;
                paging_switch_address_space(bootstrap->page_directory);
                tss_set_kernel_stack(bootstrap->kernel_stack_top);
                uint32_t *bootstrap_frame=process_saved_frame(bootstrap);
                if(bootstrap_frame){
                    __asm__ volatile("pushl %0; popfl" : : "r"(flags) : "memory","cc");
                    return bootstrap_frame;
                }
                debug_write("LIONOS:SCHEDULER-BOOTSTRAP-FRAME-MISSING\n");
                for(;;)__asm__ volatile("cli; hlt");
            }
            struct process *idle=
                (scheduler_idle_ready_mask&(1u<<cpu))?
                &scheduler_idle_processes[cpu]:0;
            if(idle&&__atomic_load_n(&idle->saved_frame,__ATOMIC_ACQUIRE)){
                fpu_save(contended_prev);
                fpu_restore(idle);
                idle->state=PROCESS_RUNNING;
                set_current_local(idle);
                paging_switch_address_space(idle->page_directory);
                tss_set_kernel_stack(idle->kernel_stack_top);
                scheduler_note_idle_entry(cpu);
                uint32_t *idle_frame=process_saved_frame(idle);
                __asm__ volatile("pushl %0; popfl" : : "r"(flags) : "memory","cc");
                return idle_frame;
            }
            debug_write("LIONOS:SCHEDULER-CONTENDED-NO-IDLE\n");
            for(;;)__asm__ volatile("cli; hlt");
        }
        __asm__ volatile("pushl %0; popfl" : : "r"(flags) : "memory","cc");
        return frame;
    }
    __atomic_store_n(&reschedule_blocked[cpu],0u,__ATOMIC_RELEASE);
    __atomic_store_n(&reschedule_ipi_pending[cpu],0u,__ATOMIC_RELEASE);
    struct scheduler_cpu_state*sc=&scheduler_cpu[cpu];
    ++sc->schedule_count;
    struct process*prev=current_local();
    int prev_is_idle=scheduler_is_idle_context(prev);
    if(prev){
        __atomic_store_n(&prev->saved_frame,(uint32_t)(uintptr_t)frame,
                         __ATOMIC_RELEASE);
        if(prev->state==PROCESS_WAITING&&prev->wait_kind==3u&&frame)
            __atomic_store_n(&prev->wait_frame_saved,1u,__ATOMIC_RELEASE);
        process_wake_interrupted_waitpid_locked();
        /* Reap adopted or explicitly deferred zombies only after no CPU
         * references them as its current execution context. */
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *zombie=&processes[i];
        if(zombie==prev||zombie->state!=PROCESS_ZOMBIE||
           (!zombie->adopted_by_init&&!zombie->reap_pending))continue;
        int current_on_cpu=0;
        for(uint32_t c=0u;c<LIONOS_MAX_CPUS;++c)
            if(current_by_cpu[c]==zombie){current_on_cpu=1;break;}
        if(!current_on_cpu)reap_process(zombie);
    }

        if(prev->state==PROCESS_WAITING&&(prev->wait_kind==1u||prev->wait_kind==2u)&&
           prev->wait_timeout_active==2u&&prev->wait_cpu==cpu){
            prev->wait_deadline=sc->timer_ticks+prev->wait_timeout_ticks;
            prev->wait_timeout_active=1u;
        }
    }

    if(prev && prev->state==PROCESS_RUNNING && !prev_is_idle){
        if(__atomic_exchange_n(&reschedule_requested[cpu],0u,__ATOMIC_ACQ_REL)==0u){
            ++prev->ticks_used;
            if(prev->ticks_used < prev->time_slice){
                spinlock_irqrestore_release(&process_lock,flags);
                return frame;
            }
            prev->ticks_used=0u;
            if(prev->priority+1u<PROCESS_PRIORITY_LEVELS) ++prev->priority;
        } else {
            prev->ticks_used=0u;
        }
    }
    if(prev&&prev->deferred_kernel_stack){page_free((void *)(uintptr_t)(prev->deferred_kernel_stack-4096u));prev->deferred_kernel_stack=0;}

    struct process*next=0;
    uint32_t picked=LIONOS_RUNQUEUE_INVALID_INDEX;
    uint32_t prev_index=LIONOS_RUNQUEUE_INVALID_INDEX;
    int prev_enqueued=0;
    int queue_lock_acquired=spinlock_try_acquire(&runqueues[cpu].lock);
    int pick_result=-1;
    if(queue_lock_acquired){
        if(prev&&prev!=&processes[0]&&prev->state==PROCESS_RUNNING&&!prev_is_idle){
            prev_index=process_index(prev);
            if(prev_index>=LIONOS_PROCESS_MAX||
               runqueue_enqueue_locked(&runqueues[cpu],runqueue_nodes,
                   LIONOS_PROCESS_MAX,prev_index,prev->priority)!=0){
                spinlock_release(&runqueues[cpu].lock);
                __atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);
                spinlock_irqrestore_release(&process_lock,flags);
                return frame;
            }
            prev->state=PROCESS_READY;
            prev_enqueued=1;
        }
        pick_result=runqueue_pick_next_locked(&runqueues[cpu],runqueue_nodes,
                                               LIONOS_PROCESS_MAX,&picked);
        if(pick_result==0&&picked<LIONOS_PROCESS_MAX){
            next=&processes[picked];
            sc->cursor=picked;
            next->state=PROCESS_RUNNING;
        }else if(prev_enqueued){
            /* Roll back the outgoing task before returning its live frame. */
            if(runqueue_dequeue_locked(&runqueues[cpu],runqueue_nodes,
                                       LIONOS_PROCESS_MAX,prev_index)!=0){
                spinlock_release(&runqueues[cpu].lock);
                __atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);
                spinlock_irqrestore_release(&process_lock,flags);
                for(;;)__asm__ volatile("cli; hlt");
            }
            prev->state=PROCESS_RUNNING;
            prev_enqueued=0;
        }
        spinlock_release(&runqueues[cpu].lock);
        if(pick_result<0||(pick_result==0&&picked>=LIONOS_PROCESS_MAX)||
           process_runqueue_validate_locked(cpu)!=0)
            __atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);
    }else{
        /*
         * Do not return a blocked task's frame merely because a local queue
         * lock is contended. Leave state unchanged and retry next tick; the
         * common idle fallback below handles blocked/stopped/zombie contexts.
         */
        __atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);
        __atomic_store_n(&reschedule_requested[cpu],1u,__ATOMIC_RELEASE);
    }
    /*
     * APs now have dedicated idle contexts, but cross-CPU work stealing remains
     * disabled. Keep tasks pinned until run-queue membership, migration
     * lifetime, and cross-CPU reaping have explicit synchronization and tests.
     */
    if(next){
        /* Preserve the outgoing CPU's FPU/SSE state on every real switch,
           including tasks that blocked, stopped, or exited before scheduling. */
        if(prev)fpu_save(prev);
        fpu_restore(next);
        set_current_local(next);
        next->cpu_owner=cpu;
        next->state=PROCESS_RUNNING;
        /* Scheduler hot path: never emit debug I/O here. Port 0xE9 is a\n         * QEMU debug console and is far more expensive than the context switch. */
        paging_switch_address_space(next->page_directory);
        tss_set_kernel_stack(next->kernel_stack_top);

        uint32_t*n=process_saved_frame(next);
        spinlock_irqrestore_release(&process_lock,flags);
        return n;
    }
    if(prev&&prev->state!=PROCESS_RUNNING){
        if(cpu==0){
            fpu_save(prev);
            struct process*b=&processes[0];
            /* Restore the bootstrap task's architectural FPU/SSE context too. */
            fpu_restore(b);
            set_current_local(b);
            b->cpu_owner=0;
            b->state=PROCESS_RUNNING;
            paging_switch_address_space(b->page_directory);
            tss_set_kernel_stack(b->kernel_stack_top);

            uint32_t*n=process_saved_frame(b);
            spinlock_irqrestore_release(&process_lock,flags);
            return n;
        }
        struct process *idle=(cpu<LIONOS_MAX_CPUS&&
            (scheduler_idle_ready_mask&(1u<<cpu)))?
            &scheduler_idle_processes[cpu]:0;
        if(idle){
            if(prev)fpu_save(prev);

            fpu_restore(idle);
            idle->state=PROCESS_RUNNING;
            set_current_local(idle);
            paging_switch_address_space(idle->page_directory);
            tss_set_kernel_stack(idle->kernel_stack_top);
            scheduler_note_idle_entry(cpu);
            uint32_t *idle_frame=process_saved_frame(idle);
            spinlock_irqrestore_release(&process_lock,flags);
            return idle_frame?idle_frame:frame;
        }
        set_current_local(0);

        spinlock_irqrestore_release(&process_lock,flags);
        debug_write("LIONOS:SCHEDULER-BLOCKED-AP-NO-IDLE\n");
        for(;;)__asm__ volatile("cli; hlt");
    }
    if(!prev&&cpu!=0u){
        struct process *idle=(cpu<LIONOS_MAX_CPUS&&
            (scheduler_idle_ready_mask&(1u<<cpu)))?
            &scheduler_idle_processes[cpu]:0;
        if(idle){
            fpu_restore(idle);
            idle->state=PROCESS_RUNNING;
            set_current_local(idle);
            paging_switch_address_space(idle->page_directory);
            tss_set_kernel_stack(idle->kernel_stack_top);
            scheduler_note_idle_entry(cpu);
            uint32_t *idle_frame=process_saved_frame(idle);
            spinlock_irqrestore_release(&process_lock,flags);
            return idle_frame?idle_frame:frame;
        }
    }
    if(prev_is_idle&&prev->state==PROCESS_RUNNING){
        spinlock_irqrestore_release(&process_lock,flags);
        return frame;
    }
    if(!prev&&cpu==0){
        set_current_local(&processes[0]);
        processes[0].cpu_owner=0;
        processes[0].state=PROCESS_RUNNING;
        paging_switch_address_space(processes[0].page_directory);
        tss_set_kernel_stack(processes[0].kernel_stack_top);
    }
    spinlock_irqrestore_release(&process_lock,flags);
    return frame;
}
void scheduler_idle(void){for(;;)__asm__ volatile("sti; hlt");}
