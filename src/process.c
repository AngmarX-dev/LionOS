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
#include "scheduler_policy.h"
#if PROCESS_PRIORITY_LEVELS != LIONOS_RUNQUEUE_PRIORITY_LEVELS
#error "process and runqueue priority levels must match"
#endif
#if PROCESS_PRIORITY_LEVELS != LIONOS_SCHEDULER_PRIORITY_LEVELS
#error "process and scheduler policy priority levels must match"
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
static volatile uint32_t scheduler_ap_probe_stop_continue_issued;
static volatile uint32_t scheduler_ap_probe_continue_seen;
static volatile uint32_t smp_ap_schedule_enter_seen;
static volatile uint32_t smp_ap_schedule_lock_contention_seen;

static void process_kernel_thread_return_trap(void){
    process_exit_current(0xFFFFFFFFu);
    for(;;)__asm__ volatile("sti; hlt");
}

static int process_scheduler_ap_probe_stop_current(void);

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
    /*
     * Stop this AP-local kernel task through the normal process-state path.
     * The BSP continues it only after every AP has switched to its idle
     * context, proving remote queue removal and reinsertion.
     */
    if(process_scheduler_ap_probe_stop_current()!=0){
        __atomic_store_n(&scheduler_ap_probe_fault,1u,__ATOMIC_RELEASE);
        process_exit_current(3u);
        for(;;)__asm__ volatile("sti; hlt");
    }
    __asm__ volatile("sti; hlt" ::: "memory");
    actual_cpu=cpu_current_index();
    if(actual_cpu!=expected_cpu||actual_cpu==0u||actual_cpu>=LIONOS_MAX_CPUS){
        __atomic_store_n(&scheduler_ap_probe_fault,1u,__ATOMIC_RELEASE);
    }else{
        __atomic_fetch_or(&scheduler_ap_probe_continue_seen,1u<<actual_cpu,
                          __ATOMIC_ACQ_REL);
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
struct scheduler_cpu_state{uint32_t cursor;uint32_t balance_cursor;uint32_t schedule_count;uint32_t steal_count;uint32_t busy_balance_count;uint32_t timer_ticks;volatile uint32_t pending_schedule_ticks;};
static struct scheduler_cpu_state scheduler_cpu[LIONOS_MAX_CPUS];
static uint32_t next_pid;
static uint32_t next_tid;
static struct spinlock process_lock;
static volatile uint32_t reschedule_requested[LIONOS_MAX_CPUS];
static volatile uint32_t reschedule_ipi_pending[LIONOS_MAX_CPUS];
static volatile uint32_t reschedule_blocked[LIONOS_MAX_CPUS];
static uint32_t fpu_enabled;
/* Test-only proof that one process context is never dispatched on two CPUs. */
static volatile uint32_t scheduler_execution_guard_enabled;
static volatile uint32_t scheduler_execution_guard_idle_steal_mask;
static volatile uint32_t scheduler_execution_guard_fault;
static volatile uint32_t scheduler_execution_cpu[LIONOS_PROCESS_MAX];

static void reap_process(struct process *p);
static uint32_t continue_process_locked(struct process *process);
static int process_scheduler_ap_probe_stop_current(void);
static struct process *find_pid(uint32_t pid);
static int process_group_orphaned_locked(uint32_t pgid,uint32_t sid);
static int process_group_has_stopped_locked(uint32_t pgid,uint32_t sid);
static uint32_t process_orphan_group_notify_locked(uint32_t pgid,uint32_t sid);
static uint32_t process_notify_orphan_transition_locked(uint32_t pgid,uint32_t sid,int was_orphaned);
struct process_group_snapshot {
    uint32_t pgid;
    uint32_t sid;
    uint8_t was_orphaned;
};
static uint32_t process_snapshot_child_groups_locked(uint32_t parent_pid,
    struct process_group_snapshot *groups,uint32_t capacity);
static uint32_t process_notify_child_group_transitions_locked(
    const struct process_group_snapshot *groups,uint32_t count);
static int process_has_interrupting_signal_locked(const struct process *p);
static void process_clear_waitpid_locked(struct process *p);
static void process_diag_status33(const char *kind,uint32_t pid,
                                  uint32_t parent_pid,uint32_t value);
static uint8_t fpu_initial_state[512] __attribute__((aligned(16)));

static void fpu_save(struct process *p){if(!fpu_enabled||!p)return;__asm__ volatile("fxsave %0":"=m"(p->fpu_state)::"memory");p->fpu_valid=1u;}
static void fpu_restore(struct process *p){if(!fpu_enabled||!p)return;if(p->fpu_valid)__asm__ volatile("fxrstor %0"::"m"(p->fpu_state):"memory");else __asm__ volatile("fxrstor %0"::"m"(fpu_initial_state):"memory");}

static struct process *current_cpu_load(uint32_t cpu){
    return cpu<LIONOS_MAX_CPUS?
        __atomic_load_n(&current_by_cpu[cpu],__ATOMIC_ACQUIRE):0;
}
static struct process *current_local(void){
    return current_cpu_load(cpu_current_index());
}
static void set_current_local(struct process *p){
    uint32_t cpu=cpu_current_index();
    if(cpu<LIONOS_MAX_CPUS)
        __atomic_store_n(&current_by_cpu[cpu],p,__ATOMIC_RELEASE);
}
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
static void process_kick_mask(uint32_t mask,uint32_t exclude_cpu){
    for(uint32_t cpu=0u;cpu<LIONOS_MAX_CPUS&&cpu<32u;++cpu)
        if(cpu!=exclude_cpu&&(mask&(1u<<cpu)))process_kick_cpu(cpu);
}
void process_request_reschedule_cpu(uint32_t cpu){process_kick_cpu(cpu);}
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
static void scheduler_execution_guard_leave(struct process *p,uint32_t cpu){
    uint32_t index;
    if(!p||cpu>=LIONOS_MAX_CPUS||
       __atomic_load_n(&scheduler_execution_guard_enabled,__ATOMIC_ACQUIRE)==0u||
       process_slot_index(p,&index)!=0)return;
    uint32_t expected=cpu+1u;
    if(!__atomic_compare_exchange_n(&scheduler_execution_cpu[index],&expected,
            0u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
        __atomic_store_n(&scheduler_execution_guard_fault,1u,__ATOMIC_RELEASE);
}
static void scheduler_execution_guard_enter(struct process *p,uint32_t cpu){
    uint32_t index;
    if(!p||cpu>=LIONOS_MAX_CPUS||
       __atomic_load_n(&scheduler_execution_guard_enabled,__ATOMIC_ACQUIRE)==0u||
       process_slot_index(p,&index)!=0)return;
    uint32_t expected=0u;
    if(!__atomic_compare_exchange_n(&scheduler_execution_cpu[index],&expected,
            cpu+1u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
        __atomic_store_n(&scheduler_execution_guard_fault,1u,__ATOMIC_RELEASE);
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
        /*
         * A READY process must agree with its queue node before unlinking.
         * Do not silently repair stale ownership or priority metadata while
         * transitioning out of READY; that would hide a scheduler invariant
         * violation and make later queue diagnostics harder to trust.
         */
        if(owner>=LIONOS_MAX_CPUS||owner!=p->cpu_owner||
           runqueue_nodes[index].queued!=1u||
           runqueue_nodes[index].priority!=p->priority){
            __atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);
            return -1;
        }
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
        }
        /*
         * Publish the process owner only after the queue accepts the node.
         * A failed enqueue must leave the non-READY process metadata intact;
         * process_lock keeps the temporary queue/process publication window
         * invisible to lifecycle operations.
         */
        spinlock_acquire(&runqueues[owner].lock);
        int result=runqueue_enqueue_locked(&runqueues[owner],runqueue_nodes,
                                           LIONOS_PROCESS_MAX,index,p->priority);
        spinlock_release(&runqueues[owner].lock);
        if(result!=0){__atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);return -1;}
        p->cpu_owner=owner;
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


/*
 * process_lock must be held. Validate every per-CPU queue and its process
 * ownership metadata as one snapshot. Acquiring all queue locks in ascending
 * CPU order prevents transfers from changing membership during the scan.
 */
static int process_runqueues_validate_all_locked(void){
    uint32_t queue_totals[LIONOS_MAX_CPUS];
    uint32_t ready_totals[LIONOS_MAX_CPUS];
    int valid=1;

    if(__atomic_load_n(&runqueue_fault,__ATOMIC_ACQUIRE)!=0u)return -1;
    for(uint32_t cpu=0u;cpu<LIONOS_MAX_CPUS;++cpu)
        spinlock_acquire(&runqueues[cpu].lock);

    for(uint32_t cpu=0u;cpu<LIONOS_MAX_CPUS;++cpu){
        queue_totals[cpu]=runqueues[cpu].total;
        ready_totals[cpu]=0u;
        if(runqueues[cpu].cpu!=cpu||
           runqueue_validate_locked(&runqueues[cpu],runqueue_nodes,
                                    LIONOS_PROCESS_MAX,0)!=0)
            valid=0;
    }

    for(uint32_t i=0u;i<LIONOS_PROCESS_MAX;++i){
        const struct process *p=&processes[i];
        const struct runqueue_node *node=&runqueue_nodes[i];
        if(p->state==PROCESS_READY){
            uint32_t owner=node->queue_cpu;
            if(node->queued!=1u||owner>=LIONOS_MAX_CPUS||
               owner!=p->cpu_owner||node->priority!=p->priority){
                valid=0;
                continue;
            }
            if(ready_totals[owner]>=LIONOS_PROCESS_MAX){
                valid=0;
                continue;
            }
            ++ready_totals[owner];
        }else if(node->queued!=0u||
                 node->next!=LIONOS_RUNQUEUE_INVALID_INDEX||
                 node->previous!=LIONOS_RUNQUEUE_INVALID_INDEX||
                 node->queue_cpu!=LIONOS_RUNQUEUE_INVALID_INDEX){
            valid=0;
        }
    }

    for(uint32_t cpu=0u;cpu<LIONOS_MAX_CPUS;++cpu)
        if(queue_totals[cpu]!=ready_totals[cpu])valid=0;
    for(uint32_t cpu=LIONOS_MAX_CPUS;cpu>0u;--cpu)
        spinlock_release(&runqueues[cpu-1u].lock);

    return valid?0:-1;
}

/* process_lock must be held. Reposition READY tasks atomically on their owner queue. */
static int process_set_priority_locked(struct process *p,uint32_t priority){
    uint32_t index;
    if(priority>=PROCESS_PRIORITY_LEVELS||process_slot_index(p,&index)!=0||
       p->state==PROCESS_UNUSED)
        return -1;
    if(p->state!=PROCESS_READY){
        /*
         * Non-READY processes must be detached. A stale intrusive node here
         * would otherwise let a priority update mutate process metadata while
         * the scheduler can still discover the process through a queue.
         */
        if(runqueue_nodes[index].queued!=0u||
           runqueue_nodes[index].next!=LIONOS_RUNQUEUE_INVALID_INDEX||
           runqueue_nodes[index].previous!=LIONOS_RUNQUEUE_INVALID_INDEX||
           runqueue_nodes[index].queue_cpu!=LIONOS_RUNQUEUE_INVALID_INDEX){
            __atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);
            return -1;
        }
    }
    if(p->state==PROCESS_READY){
        uint32_t owner=runqueue_nodes[index].queue_cpu;
        /*
         * Priority updates must not repair stale READY metadata. The process
         * record and intrusive node are one scheduler membership invariant;
         * reject divergence before requeueing so diagnostics retain the
         * original corruption and the queue remains unchanged.
         */
        if(owner>=LIONOS_MAX_CPUS||owner!=p->cpu_owner||
           runqueue_nodes[index].queued!=1u||
           runqueue_nodes[index].priority!=p->priority){
            __atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);
            return -1;
        }
        spinlock_acquire(&runqueues[owner].lock);
        int queued=runqueue_requeue_priority_locked(&runqueues[owner],
            runqueue_nodes,LIONOS_PROCESS_MAX,index,priority);
        spinlock_release(&runqueues[owner].lock);
        if(queued!=0){
            __atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);
            return -1;
        }
    }
    p->priority=priority;
    p->base_priority=priority;
    p->time_slice=scheduler_priority_quantum(priority);
    p->ticks_used=0u;
    return 0;
}


/* Only independent user processes are initially migratable. Kernel threads,
 * user threads, and leaders with any unreaped sibling thread retain affinity.
 * A process is migrated at most once until broader migration semantics have
 * dedicated address-space and thread-group coverage. */
static int process_steal_candidate_locked(uint32_t index,uint32_t source_cpu){
    if(index>=LIONOS_PROCESS_MAX||source_cpu>=LIONOS_MAX_CPUS)return 0;
    struct process *p=&processes[index];
    if(p->state!=PROCESS_READY||p->pid<=1u||p->is_thread||
       p->thread_group_pid!=p->pid||p->owns_address_space!=1u||
       !p->page_directory||p->page_directory==paging_kernel_directory()||
       !p->user_page_count||p->user_page_count>LIONOS_PROCESS_MAX_USER_PAGES||
       !p->kernel_stack_top||(p->kernel_stack_top&(PAGE_SIZE-1u))!=0u||
       !__atomic_load_n(&p->saved_frame,__ATOMIC_ACQUIRE)||
       p->cpu_owner!=source_cpu||p->migration_count!=0u||
       p->migration_eligible!=1u||
       p->thread_stack_page||p->deferred_kernel_stack||p->reap_pending||
       p->wait_kind||p->wait_channel||p->wait_timeout_active||
       p->wait_timeout_result||p->wait_pid||p->wait_status_ptr||
       p->wait_options||p->stop_event_pending||
       __atomic_load_n(&p->wait_frame_saved,__ATOMIC_ACQUIRE)!=0u)return 0;
    if(runqueue_nodes[index].queued!=1u||
       runqueue_nodes[index].queue_cpu!=source_cpu||
       runqueue_nodes[index].priority!=p->priority)return 0;
    for(uint32_t cpu=0u;cpu<LIONOS_MAX_CPUS;++cpu)
        if(current_cpu_load(cpu)==p)return 0;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        const struct process *member=&processes[i];
        if(member!=p&&member->state!=PROCESS_UNUSED&&member->is_thread&&
           member->thread_group_pid==p->pid)return 0;
    }
    return 1;
}

/* process_lock is held; queue locks are acquired in ascending CPU order. */
static int process_try_steal_locked(uint32_t cpu,uint32_t *picked,
                                    int idle_destination){
    uint32_t online=smp_online_count();
    if(online==0u||online>LIONOS_MAX_CPUS)online=1u;
    if(!picked||cpu>=online||
       (idle_destination!=0&&idle_destination!=1))return -1;
    if(idle_destination){
        uint32_t restricted=__atomic_load_n(
            &scheduler_execution_guard_idle_steal_mask,__ATOMIC_ACQUIRE);
        if(restricted!=0xFFFFFFFFu&&(restricted&(1u<<cpu))==0u)return 0;
        if(runqueues[cpu].total!=0u)return 0;
    }

    uint32_t visited=0u;
    uint32_t queue_totals[LIONOS_MAX_CPUS];
    for(uint32_t other=0u;other<online;++other)
        queue_totals[other]=runqueues[other].total;
    uint32_t start_cpu=scheduler_cpu[cpu].balance_cursor%online;
    for(uint32_t attempt=0u;attempt+1u<online;++attempt){
        uint32_t victim=scheduler_select_victim(queue_totals,online,cpu,
                                                start_cpu,visited);
        if(victim==LIONOS_RUNQUEUE_INVALID_INDEX)return 0;
        uint32_t largest=queue_totals[victim];
        if(largest==0u)return 0;
        if(!runqueue_should_balance(runqueues[cpu].total,largest,
                                    idle_destination))return 0;
        visited|=1u<<victim;

        uint32_t first=cpu<victim?cpu:victim;
        uint32_t second=cpu<victim?victim:cpu;
        if(!spinlock_try_acquire(&runqueues[first].lock))return 0;
        if(!spinlock_try_acquire(&runqueues[second].lock)){
            spinlock_release(&runqueues[first].lock);return 0;
        }
        if(runqueue_validate_locked(&runqueues[cpu],runqueue_nodes,
                LIONOS_PROCESS_MAX,0)!=0||
           runqueue_validate_locked(&runqueues[victim],runqueue_nodes,
                LIONOS_PROCESS_MAX,0)!=0){
            __atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);
            spinlock_release(&runqueues[second].lock);
            spinlock_release(&runqueues[first].lock);return -1;
        }
        if(!runqueue_should_balance(runqueues[cpu].total,
                                    runqueues[victim].total,
                                    idle_destination)){
            spinlock_release(&runqueues[second].lock);
            spinlock_release(&runqueues[first].lock);return 0;
        }

        uint32_t candidate=LIONOS_RUNQUEUE_INVALID_INDEX;
        for(uint32_t priority=0u;
            priority<LIONOS_RUNQUEUE_PRIORITY_LEVELS&&
            candidate==LIONOS_RUNQUEUE_INVALID_INDEX;++priority){
            uint32_t index=runqueues[victim].head[priority];
            while(index!=LIONOS_RUNQUEUE_INVALID_INDEX){
                if(index>=LIONOS_PROCESS_MAX){
                    __atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);
                    break;
                }
                uint32_t next=runqueue_nodes[index].next;
                if(process_steal_candidate_locked(index,victim)){
                    candidate=index;break;
                }
                index=next;
            }
        }
        if(__atomic_load_n(&runqueue_fault,__ATOMIC_ACQUIRE)!=0u){
            spinlock_release(&runqueues[second].lock);
            spinlock_release(&runqueues[first].lock);return -1;
        }
        if(candidate==LIONOS_RUNQUEUE_INVALID_INDEX){
            spinlock_release(&runqueues[second].lock);
            spinlock_release(&runqueues[first].lock);continue;
        }

        struct process *migrating=&processes[candidate];
        if(!process_steal_candidate_locked(candidate,victim)||
           runqueue_move_locked(&runqueues[victim],&runqueues[cpu],
               runqueue_nodes,LIONOS_PROCESS_MAX,candidate)!=0){
            __atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);
            spinlock_release(&runqueues[second].lock);
            spinlock_release(&runqueues[first].lock);return -1;
        }
        migrating->cpu_owner=cpu;
        migrating->migration_count=1u;
        /* Rotate equal-backlog victim preference after each successful move. */
        scheduler_cpu[cpu].balance_cursor=(victim+1u)%online;

        uint32_t selected=LIONOS_RUNQUEUE_INVALID_INDEX;
        int pick_result=0;
        if(idle_destination)
            pick_result=runqueue_pick_next_locked(&runqueues[cpu],
                runqueue_nodes,LIONOS_PROCESS_MAX,&selected);
        int queues_valid=
            runqueue_validate_locked(&runqueues[victim],runqueue_nodes,
                LIONOS_PROCESS_MAX,0)==0&&
            runqueue_validate_locked(&runqueues[cpu],runqueue_nodes,
                LIONOS_PROCESS_MAX,0)==0;
        spinlock_release(&runqueues[second].lock);
        spinlock_release(&runqueues[first].lock);

        if(!queues_valid||
           (idle_destination&&(pick_result!=0||selected!=candidate))){
            __atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);
            return -1;
        }
        __atomic_fetch_add(&scheduler_cpu[cpu].steal_count,1u,
                           __ATOMIC_RELAXED);
        if(!idle_destination)
            __atomic_fetch_add(&scheduler_cpu[cpu].busy_balance_count,1u,
                               __ATOMIC_RELAXED);
        /* For a busy destination, leave the moved READY task queued. */
        if(idle_destination)*picked=selected;
        return 1;
    }
    return 0;
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
               current_cpu_load(cpu)!=&scheduler_idle_processes[cpu]){
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
     * Wait until each AP-local probe has stopped and its CPU has switched to
     * the idle context. Continue all probes under process_lock, validate
     * READY queue membership, then kick the owners after releasing the lock.
     */
    uint32_t timeout_seen_for_continue=
        __atomic_load_n(&scheduler_ap_probe_timeout_seen,__ATOMIC_ACQUIRE);
    uint32_t continue_issued=
        __atomic_load_n(&scheduler_ap_probe_stop_continue_issued,__ATOMIC_ACQUIRE);
    if((timeout_seen_for_continue&expected)==expected&&!continue_issued){
        struct process *targets[LIONOS_MAX_CPUS]={0};
        uint32_t all_stopped=1u,missing_target=0u,wake_mask=0u;
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
                if(!probe){
                    missing_target=1u;
                    all_stopped=0u;
                    break;
                }
                targets[cpu]=probe;
                if(probe->state!=PROCESS_STOPPED||
                   current_cpu_load(cpu)!=&scheduler_idle_processes[cpu]){
                    all_stopped=0u;
                    break;
                }
            }
            if(missing_target){
                __atomic_store_n(&scheduler_ap_probe_fault,1u,__ATOMIC_RELEASE);
            }else if(all_stopped){
                __atomic_store_n(&scheduler_ap_probe_stop_continue_issued,1u,
                                 __ATOMIC_RELEASE);
                for(uint32_t cpu=1u;cpu<online;++cpu){
                    if(!(expected&(1u<<cpu)))continue;
                    struct process *target=targets[cpu];
                    uint32_t stopped_priority=
                        (target->priority+1u)%PROCESS_PRIORITY_LEVELS;
                    /*
                     * Exercise priority changes while the process is STOPPED.
                     * The node must remain fully detached until continue
                     * publishes it to the owner CPU's READY queue.
                     */
                    if(process_set_priority_locked(target,stopped_priority)!=0||
                       target->state!=PROCESS_STOPPED||
                       target->priority!=stopped_priority||
                       target->base_priority!=stopped_priority||
                       target->time_slice!=scheduler_priority_quantum(stopped_priority)||
                       target->ticks_used!=0u||
                       runqueue_nodes[process_index(target)].queued!=0u||
                       runqueue_nodes[process_index(target)].next!=
                           LIONOS_RUNQUEUE_INVALID_INDEX||
                       runqueue_nodes[process_index(target)].previous!=
                           LIONOS_RUNQUEUE_INVALID_INDEX||
                       runqueue_nodes[process_index(target)].queue_cpu!=
                           LIONOS_RUNQUEUE_INVALID_INDEX){
                        __atomic_store_n(&scheduler_ap_probe_fault,1u,
                                         __ATOMIC_RELEASE);
                        continue;
                    }
                    /*
                     * An out-of-range request must fail without changing a
                     * stopped process or publishing a queue node.
                     */
                    if(process_set_priority_locked(target,PROCESS_PRIORITY_LEVELS)==0||
                       target->state!=PROCESS_STOPPED||
                       target->priority!=stopped_priority||
                       target->base_priority!=stopped_priority||
                       target->time_slice!=scheduler_priority_quantum(stopped_priority)||
                       target->ticks_used!=0u||
                       runqueue_nodes[process_index(target)].queued!=0u||
                       runqueue_nodes[process_index(target)].next!=
                           LIONOS_RUNQUEUE_INVALID_INDEX||
                       runqueue_nodes[process_index(target)].previous!=
                           LIONOS_RUNQUEUE_INVALID_INDEX||
                       runqueue_nodes[process_index(target)].queue_cpu!=
                           LIONOS_RUNQUEUE_INVALID_INDEX){
                        __atomic_store_n(&scheduler_ap_probe_fault,1u,
                                         __ATOMIC_RELEASE);
                        continue;
                    }
                    /*
                     * A failed STOPPED-to-READY enqueue must not publish a
                     * fallback CPU owner or alter the detached node. Inject
                     * stale membership metadata to force the enqueue reject,
                     * then restore the test fixture before normal continue.
                     */
                    uint32_t target_index=process_index(target);
                    uint32_t saved_owner=target->cpu_owner;
                    uint32_t saved_state=target->state;
                    uint32_t saved_priority=target->priority;
                    uint32_t saved_base_priority=target->base_priority;
                    uint32_t saved_time_slice=target->time_slice;
                    uint32_t saved_ticks_used=target->ticks_used;
                    uint32_t saved_fault=__atomic_load_n(&runqueue_fault,
                                                          __ATOMIC_ACQUIRE);
                    /* The report runs on the BSP; failed enqueue falls back to CPU 0. */
                    uint32_t fallback_total=runqueues[0u].total;
                    if(runqueue_nodes[target_index].queued!=0u||
                       runqueue_nodes[target_index].next!=
                           LIONOS_RUNQUEUE_INVALID_INDEX||
                       runqueue_nodes[target_index].previous!=
                           LIONOS_RUNQUEUE_INVALID_INDEX||
                       runqueue_nodes[target_index].queue_cpu!=
                           LIONOS_RUNQUEUE_INVALID_INDEX){
                        __atomic_store_n(&scheduler_ap_probe_fault,1u,
                                         __ATOMIC_RELEASE);
                        continue;
                    }
                    target->cpu_owner=LIONOS_RUNQUEUE_INVALID_INDEX;
                    runqueue_nodes[target_index].queued=1u;
                    int transition_result=
                        process_transition_state_locked(target,PROCESS_READY);
                    int transition_unchanged=
                        transition_result!=0&&target->state==saved_state&&
                        target->priority==saved_priority&&
                        target->base_priority==saved_base_priority&&
                        target->time_slice==saved_time_slice&&
                        target->ticks_used==saved_ticks_used&&
                        target->cpu_owner==LIONOS_RUNQUEUE_INVALID_INDEX&&
                        runqueue_nodes[target_index].queued==1u&&
                        runqueue_nodes[target_index].next==
                            LIONOS_RUNQUEUE_INVALID_INDEX&&
                        runqueue_nodes[target_index].previous==
                            LIONOS_RUNQUEUE_INVALID_INDEX&&
                        runqueue_nodes[target_index].queue_cpu==
                            LIONOS_RUNQUEUE_INVALID_INDEX&&
                        runqueues[0u].total==fallback_total;
                    runqueue_node_init(&runqueue_nodes[target_index]);
                    target->cpu_owner=saved_owner;
                    __atomic_store_n(&runqueue_fault,saved_fault,__ATOMIC_RELEASE);
                    if(!transition_unchanged||
                       process_runqueue_validate_locked(0u)!=0||
                       process_runqueue_validate_locked(cpu)!=0){
                        __atomic_store_n(&scheduler_ap_probe_fault,1u,
                                         __ATOMIC_RELEASE);
                        continue;
                    }
                    wake_mask|=continue_process_locked(target);
                    /*
                     * A READY-to-WAITING transition must be failure-atomic
                     * when the owning priority lane is structurally invalid.
                     * Corrupt only the lane tail while process_lock excludes
                     * scheduler/lifecycle users, then verify dequeue rejects
                     * the transition without changing process or node state.
                     */
                    {
                        uint32_t ready_index=process_index(target);
                        uint32_t ready_priority=target->priority;
                        struct runqueue_node *ready_node=&runqueue_nodes[ready_index];
                        uint32_t ready_state=target->state;
                        uint32_t ready_owner=target->cpu_owner;
                        uint32_t ready_base_priority=target->base_priority;
                        uint32_t ready_time_slice=target->time_slice;
                        uint32_t ready_ticks_used=target->ticks_used;
                        uint32_t ready_queued=ready_node->queued;
                        uint32_t ready_next=ready_node->next;
                        uint32_t ready_previous=ready_node->previous;
                        uint32_t ready_queue_cpu=ready_node->queue_cpu;
                        uint32_t ready_node_priority=ready_node->priority;
                        if(ready_priority>=PROCESS_PRIORITY_LEVELS){
                            __atomic_store_n(&scheduler_ap_probe_fault,1u,
                                             __ATOMIC_RELEASE);
                            continue;
                        }
                        uint32_t saved_runqueue_fault=
                            __atomic_load_n(&runqueue_fault,__ATOMIC_ACQUIRE);
                        uint32_t ready_head,ready_tail,ready_count,ready_total;
                        spinlock_acquire(&runqueues[cpu].lock);
                        ready_head=runqueues[cpu].head[ready_priority];
                        ready_tail=runqueues[cpu].tail[ready_priority];
                        ready_count=runqueues[cpu].count[ready_priority];
                        ready_total=runqueues[cpu].total;
                        if(ready_state!=PROCESS_READY||ready_owner!=cpu||
                           ready_queued!=1u||ready_queue_cpu!=cpu||
                           ready_node_priority!=ready_priority||
                           ready_count==0u||ready_head>=LIONOS_PROCESS_MAX||
                           ready_tail>=LIONOS_PROCESS_MAX){
                            spinlock_release(&runqueues[cpu].lock);
                            __atomic_store_n(&scheduler_ap_probe_fault,1u,
                                             __ATOMIC_RELEASE);
                            continue;
                        }
                        runqueues[cpu].tail[ready_priority]=
                            LIONOS_RUNQUEUE_INVALID_INDEX;
                        spinlock_release(&runqueues[cpu].lock);

                        /*
                         * A failed READY priority requeue must preserve both
                         * process scheduling metadata and intrusive membership.
                         */
                        uint32_t requeue_priority=
                            (ready_priority+1u)%PROCESS_PRIORITY_LEVELS;
                        int failed_priority_update=
                            process_set_priority_locked(target,requeue_priority);
                        uint32_t requeue_fault=
                            __atomic_load_n(&runqueue_fault,__ATOMIC_ACQUIRE);
                        spinlock_acquire(&runqueues[cpu].lock);
                        runqueues[cpu].tail[ready_priority]=ready_tail;
                        int requeue_queue_unchanged=
                            runqueues[cpu].head[ready_priority]==ready_head&&
                            runqueues[cpu].count[ready_priority]==ready_count&&
                            runqueues[cpu].tail[ready_priority]==ready_tail&&
                            runqueues[cpu].total==ready_total;
                        spinlock_release(&runqueues[cpu].lock);
                        int requeue_process_unchanged=
                            target->state==ready_state&&
                            target->cpu_owner==ready_owner&&
                            target->priority==ready_priority&&
                            target->base_priority==ready_base_priority&&
                            target->time_slice==ready_time_slice&&
                            target->ticks_used==ready_ticks_used&&
                            ready_node->queued==ready_queued&&
                            ready_node->next==ready_next&&
                            ready_node->previous==ready_previous&&
                            ready_node->queue_cpu==ready_queue_cpu&&
                            ready_node->priority==ready_node_priority;
                        __atomic_store_n(&runqueue_fault,saved_runqueue_fault,
                                         __ATOMIC_RELEASE);
                        if(saved_runqueue_fault!=0u||failed_priority_update!=-1||
                           requeue_fault==saved_runqueue_fault||
                           !requeue_queue_unchanged||!requeue_process_unchanged||
                           process_runqueue_validate_locked(cpu)!=0){
                            __atomic_store_n(&scheduler_ap_probe_fault,1u,
                                             __ATOMIC_RELEASE);
                            continue;
                        }

                        /* Re-inject corruption for the READY-to-WAITING case. */
                        spinlock_acquire(&runqueues[cpu].lock);
                        runqueues[cpu].tail[ready_priority]=
                            LIONOS_RUNQUEUE_INVALID_INDEX;
                        spinlock_release(&runqueues[cpu].lock);

                        int failed_ready_transition=
                            process_transition_state_locked(target,PROCESS_WAITING);
                        uint32_t transition_fault=
                            __atomic_load_n(&runqueue_fault,__ATOMIC_ACQUIRE);

                        spinlock_acquire(&runqueues[cpu].lock);
                        runqueues[cpu].tail[ready_priority]=ready_tail;
                        int queue_unchanged=
                            runqueues[cpu].head[ready_priority]==ready_head&&
                            runqueues[cpu].count[ready_priority]==ready_count&&
                            runqueues[cpu].tail[ready_priority]==ready_tail&&
                            runqueues[cpu].total==ready_total;
                        spinlock_release(&runqueues[cpu].lock);
                        int process_unchanged=
                            target->state==ready_state&&
                            target->cpu_owner==ready_owner&&
                            target->priority==ready_priority&&
                            target->base_priority==ready_base_priority&&
                            target->time_slice==ready_time_slice&&
                            target->ticks_used==ready_ticks_used&&
                            ready_node->queued==ready_queued&&
                            ready_node->next==ready_next&&
                            ready_node->previous==ready_previous&&
                            ready_node->queue_cpu==ready_queue_cpu&&
                            ready_node->priority==ready_node_priority;
                        __atomic_store_n(&runqueue_fault,saved_runqueue_fault,
                                         __ATOMIC_RELEASE);
                        if(saved_runqueue_fault!=0u||transition_fault==saved_runqueue_fault||
                           failed_ready_transition!=-1||!queue_unchanged||
                           !process_unchanged||
                           process_runqueue_validate_locked(cpu)!=0){
                            __atomic_store_n(&scheduler_ap_probe_fault,1u,
                                             __ATOMIC_RELEASE);
                            continue;
                        }
                    }
                    uint32_t next_priority=
                        (target->priority+1u)%PROCESS_PRIORITY_LEVELS;
                    if(target->state!=PROCESS_READY||
                       runqueue_nodes[process_index(target)].queued!=1u||
                       runqueue_nodes[process_index(target)].queue_cpu!=cpu||
                       runqueue_nodes[process_index(target)].priority!=target->priority||
                       process_set_priority_locked(target,PROCESS_PRIORITY_LEVELS)==0||
                       target->state!=PROCESS_READY||
                       target->priority!=stopped_priority||
                       target->base_priority!=stopped_priority||
                       target->time_slice!=scheduler_priority_quantum(stopped_priority)||
                       target->ticks_used!=0u||
                       runqueue_nodes[process_index(target)].queued!=1u||
                       runqueue_nodes[process_index(target)].queue_cpu!=cpu||
                       runqueue_nodes[process_index(target)].priority!=stopped_priority||
                       process_set_priority_locked(target,next_priority)!=0||
                       target->state!=PROCESS_READY||
                       target->priority!=next_priority||
                       target->base_priority!=next_priority||
                       target->time_slice!=scheduler_priority_quantum(next_priority)||
                       target->ticks_used!=0u||
                       runqueue_nodes[process_index(target)].queued!=1u||
                       runqueue_nodes[process_index(target)].queue_cpu!=cpu||
                       runqueue_nodes[process_index(target)].priority!=next_priority)
                        __atomic_store_n(&scheduler_ap_probe_fault,1u,
                                         __ATOMIC_RELEASE);
                }
                for(uint32_t cpu=0u;cpu<online;++cpu)
                    if(process_runqueue_validate_locked(cpu)!=0)
                        __atomic_store_n(&scheduler_ap_probe_fault,1u,
                                         __ATOMIC_RELEASE);
            }
            spinlock_release(&process_lock);
            process_kick_mask(wake_mask,cpu_current_index());
        }
        if(all_stopped&&!missing_target&&
           __atomic_load_n(&scheduler_ap_probe_stop_continue_issued,
                           __ATOMIC_ACQUIRE)&&
           __atomic_load_n(&scheduler_ap_probe_fault,__ATOMIC_ACQUIRE)==0u){
            uint32_t online=smp_online_count();
            if(online>LIONOS_MAX_CPUS)online=LIONOS_MAX_CPUS;
            for(uint32_t cpu=1u;cpu<online;++cpu)
                if(expected&(1u<<cpu))process_kick_cpu(cpu);
        }
    }

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
            uint32_t snapshot[LIONOS_MAX_CPUS][13];
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
                    struct process *current=current_cpu_load(cpu);
                    snapshot[snapshot_count][7]=__atomic_load_n(&reschedule_requested[cpu],__ATOMIC_ACQUIRE);
                    snapshot[snapshot_count][8]=probe?probe->wait_channel:0xFFFFFFFFu;
                    snapshot[snapshot_count][9]=probe?probe->wait_kind:0xFFFFFFFFu;
                    snapshot[snapshot_count][10]=(current==&scheduler_idle_processes[cpu])?1u:0u;
                    snapshot[snapshot_count][11]=current?current->pid:0xFFFFFFFFu;
                    snapshot[snapshot_count][12]=current?current->state:0xFFFFFFFFu;
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
                    debug_write(" WAIT=");process_scheduler_ap_probe_diag_hex(snapshot[i][8]);
                    debug_write(" WKIND=");process_scheduler_ap_probe_diag_hex(snapshot[i][9]);
                    debug_write(" CURIDLE=");process_scheduler_ap_probe_diag_hex(snapshot[i][10]);
                    debug_write(" CURPID=");process_scheduler_ap_probe_diag_hex(snapshot[i][11]);
                    debug_write(" CURSTATE=");process_scheduler_ap_probe_diag_hex(snapshot[i][12]);
                    debug_write("\n");
                }
                debug_write(" EXPECTED-WAIT=");
                process_scheduler_ap_probe_diag_hex((uint32_t)(uintptr_t)&scheduler_ap_probe_wait_token);
                debug_write("\nLIONOS:SMP-AP-RUNQUEUE-DIAG-END\n");
            }else{
                __atomic_store_n(&scheduler_ap_probe_diag_reported,0u,__ATOMIC_RELEASE);
            }
        }
    }

    uint32_t fault=__atomic_load_n(&scheduler_ap_probe_fault,__ATOMIC_ACQUIRE);
    uint32_t seen=__atomic_load_n(&scheduler_ap_probe_seen,__ATOMIC_ACQUIRE);
    uint32_t timeout_seen=__atomic_load_n(&scheduler_ap_probe_timeout_seen,
                                           __ATOMIC_ACQUIRE);
    uint32_t continue_seen=__atomic_load_n(&scheduler_ap_probe_continue_seen,
                                            __ATOMIC_ACQUIRE);
    uint32_t expected_count=__atomic_load_n(&scheduler_ap_probe_expected_count,
                                             __ATOMIC_ACQUIRE);
    uint32_t reaped=__atomic_load_n(&scheduler_ap_probe_reaped,__ATOMIC_ACQUIRE);
    if(!fault&&((seen&expected)!=expected||
                (timeout_seen&expected)!=expected||
                (continue_seen&expected)!=expected||
                __atomic_load_n(&scheduler_ap_probe_stop_continue_issued,
                                __ATOMIC_ACQUIRE)==0u||
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
        debug_write("LIONOS:SMP-AP-RUNQUEUE-STOP-CONT-OK\n");
        debug_write("LIONOS:SMP-AP-RUNQUEUE-TRANSITION-FAILURE-ATOMIC-OK\n");
        debug_write("LIONOS:SMP-AP-RUNQUEUE-PRIORITY-REQUEUE-OK\n");
        debug_write("LIONOS:SMP-AP-RUNQUEUE-REAP-VALIDATION-OK\n");
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
static void clear_process(struct process *p) { if(p){uint32_t index;if(process_slot_index(p,&index)==0){runqueue_node_init(&runqueue_nodes[index]);__atomic_store_n(&scheduler_execution_cpu[index],0u,__ATOMIC_RELEASE);}} p->pid=0;p->state=PROCESS_UNUSED;p->parent_pid=0;p->adopted_by_init=0u;p->pgid=0;p->sid=0;p->exit_code=0;p->wait_pid=0;p->wait_status_ptr=0;p->wait_options=0;p->stop_event_pending=0;p->stop_signal=0;p->continued_event_pending=0;p->wait_channel=0;p->wait_kind=0;__atomic_store_n(&p->wait_frame_saved,0,__ATOMIC_RELEASE);p->wait_timeout_active=0;p->wait_timeout_ticks=0;p->wait_deadline=0;p->wait_cpu=0;p->wait_timeout_result=0; p->reap_pending=0;p->deferred_kernel_stack=0;p->page_directory=0;p->entry=0;p->user_stack=0; p->kernel_stack_top=0;__atomic_store_n(&p->saved_frame,0,__ATOMIC_RELEASE);p->user_code_page=0;p->user_stack_page=0;p->user_page_count=0;p->pending_signals=0;p->signal_active=0u;p->signal_active_restorer=0u;for(uint32_t i=0u;i<32u;++i){p->signal_handlers[i]=LIONOS_SIG_DFL;p->signal_restorers[i]=0u;}for(uint32_t i=0u;i<PROCESS_CONTEXT_WORDS;++i)p->signal_saved_frame[i]=0u;p->cpu_owner=0;p->migration_count=0;p->migration_eligible=0;p->priority=PROCESS_PRIORITY_DEFAULT;p->base_priority=PROCESS_PRIORITY_DEFAULT;p->time_slice=0;p->ticks_used=0;p->fpu_valid=0;p->tid=0;p->thread_group_pid=0;p->is_thread=0;p->owns_address_space=0;p->thread_stack_page=0;p->thread_stack_va=0;p->join_tid=0;p->join_status_ptr=0;p->pipe_read_mask=0;p->pipe_write_mask=0;p->fd_closing=0; for(uint32_t i=0;i<PROCESS_FD_MAX;++i){p->fd_used[i]=0;p->fd_object[i]=0;p->fd_backend[i]=0;p->fd_flags[i]=0;p->fd_offset[i]=0;p->fd_path[i][0]=0;} for(uint32_t i=0;i<LIONOS_PROCESS_MAX_USER_PAGES;++i){p->user_pages[i]=0;p->user_page_vas[i]=0;} for(uint32_t i=0;i<PROCESS_FD_MAX;++i){p->fd_used[i]=0;p->fd_backend[i]=0;p->fd_flags[i]=0;p->fd_offset[i]=0;p->fd_path[i][0]=0;} }
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
    __atomic_store_n(&scheduler_execution_guard_enabled,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&scheduler_execution_guard_idle_steal_mask,0xFFFFFFFFu,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&scheduler_execution_guard_fault,0u,__ATOMIC_RELAXED);
    for(uint32_t i=0u;i<LIONOS_PROCESS_MAX;++i){
        __atomic_store_n(&scheduler_execution_cpu[i],0u,__ATOMIC_RELAXED);
        runqueue_node_init(&runqueue_nodes[i]);
        clear_process(&processes[i]);
    }
    scheduler_idle_ready_mask=0u;
    scheduler_idle_expected_mask=0u;
    __atomic_store_n(&scheduler_idle_entered_mask,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&scheduler_idle_reported,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&runqueue_fault,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&runqueue_fault_reported,0u,__ATOMIC_RELAXED);
    for(uint32_t i=0u;i<LIONOS_MAX_CPUS;++i){
        runqueue_init(&runqueues[i],i);
        __atomic_store_n(&current_by_cpu[i],(struct process *)0,__ATOMIC_RELAXED);
        scheduler_cpu[i].cursor=0u;
        scheduler_cpu[i].schedule_count=0u;
        scheduler_cpu[i].steal_count=0u;
        scheduler_cpu[i].busy_balance_count=0u;
        scheduler_cpu[i].timer_ticks=0u;
        scheduler_cpu[i].pending_schedule_ticks=0u;
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
    __atomic_store_n(&scheduler_ap_probe_stop_continue_issued,0u,__ATOMIC_RELEASE);
    __atomic_store_n(&scheduler_ap_probe_continue_seen,0u,__ATOMIC_RELEASE);
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
    uint32_t orphan_wake_mask=process_notify_orphan_transition_locked(
        old_pgid,old_sid,old_orphaned);
    debug_write("LIONOS:JOBCTL:SETSID-OLD-GROUP-DONE\n");
    debug_write("LIONOS:JOBCTL:SETSID-CHILD-GROUPS-ENTER\n");
    orphan_wake_mask|=process_notify_child_group_transitions_locked(
        child_groups,child_group_count);
    debug_write("LIONOS:JOBCTL:SETSID-CHILD-GROUPS-DONE\n");
    debug_write("LIONOS:JOBCTL:SETSID-DONE\n");
    spinlock_irqrestore_release(&process_lock,flags);
    process_kick_mask(orphan_wake_mask,cpu_current_index());
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
    uint32_t orphan_wake_mask=0u;
    if(new_pgid!=old_pgid){
        orphan_wake_mask|=process_notify_orphan_transition_locked(
            old_pgid,old_sid,old_orphaned);
        orphan_wake_mask|=process_notify_orphan_transition_locked(
            new_pgid,new_sid,new_orphaned);
        orphan_wake_mask|=process_notify_child_group_transitions_locked(
            child_groups,child_group_count);
    }
    spinlock_irqrestore_release(&process_lock,flags);
    process_kick_mask(orphan_wake_mask,cpu_current_index());
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

static struct process *process_create_ex_vas_locked(uint32_t entry,uint32_t stack,uint32_t pd,const uint32_t *pages,const uint32_t *vas,uint32_t count){ struct process*p=find_free_slot(); if(!p||!pd||!pages||!vas||!count||count>LIONOS_PROCESS_MAX_USER_PAGES)return 0; void *ks=page_alloc();if(!ks)return 0; p->pid=next_pid++;if(next_pid==0)next_pid=2;p->tid=next_tid++;if(next_tid==0)next_tid=1;p->thread_group_pid=p->pid;p->is_thread=0;p->owns_address_space=1;p->thread_stack_page=0;p->thread_stack_va=0;p->join_tid=0;p->join_status_ptr=0;p->state=PROCESS_STOPPED; struct process *parent=current_local();p->parent_pid=parent?parent->pid:1;p->adopted_by_init=0u;p->pgid=p->pid;p->sid=p->pid; p->exit_code=0;p->wait_pid=0;p->wait_status_ptr=0;p->wait_options=0;p->stop_event_pending=0;p->stop_signal=0;p->wait_channel=0;p->wait_kind=0;__atomic_store_n(&p->wait_frame_saved,0,__ATOMIC_RELEASE);p->reap_pending=0;p->deferred_kernel_stack=0; p->page_directory=pd;p->entry=entry;p->user_stack=stack;p->kernel_stack_top=(uint32_t)(uintptr_t)ks+4096u; __atomic_store_n(&p->saved_frame,0,__ATOMIC_RELEASE);p->user_page_count=count;p->user_code_page=pages[0];p->user_stack_page=pages[count-1u];p->pending_signals=0;p->signal_active=0u;p->signal_active_restorer=0u;for(uint32_t i=0u;i<32u;++i){p->signal_handlers[i]=LIONOS_SIG_DFL;p->signal_restorers[i]=0u;}for(uint32_t i=0u;i<PROCESS_CONTEXT_WORDS;++i)p->signal_saved_frame[i]=0u;p->capabilities=PROCESS_CAP_USER_DEFAULT;p->cpu_owner=cpu_current_index();p->migration_count=0u;p->migration_eligible=0u;p->priority=PROCESS_PRIORITY_DEFAULT;p->base_priority=PROCESS_PRIORITY_DEFAULT;p->time_slice=scheduler_priority_quantum(PROCESS_PRIORITY_DEFAULT);p->ticks_used=0;p->fpu_valid=0;p->pipe_read_mask=0;p->pipe_write_mask=0; for(uint32_t i=0;i<count;++i){p->user_pages[i]=pages[i];p->user_page_vas[i]=vas[i];} for(uint32_t i=count;i<LIONOS_PROCESS_MAX_USER_PAGES;++i){p->user_pages[i]=0;p->user_page_vas[i]=0;} init_interrupt_frame(p);return p;}
static int process_create_ex_vas_internal(uint32_t entry,uint32_t stack,
    uint32_t pd,const uint32_t *pages,const uint32_t *vas,uint32_t count,
    uint32_t migration_eligible,uint32_t *pid_out,
    struct process **process_out){
    if(pid_out)*pid_out=0u;
    if(process_out)*process_out=0;
    if(migration_eligible>1u||(!pid_out&&!process_out))return -1;

    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *p=process_create_ex_vas_locked(entry,stack,pd,pages,vas,count);
    uint32_t created_pid=p?p->pid:0u;
    spinlock_irqrestore_release(&process_lock,flags);
    if(!p)return -1;

    if(vfs_stdio_init_process(p)!=0){
        flags=spinlock_irqsave_acquire(&process_lock);
        if(p->state==PROCESS_STOPPED&&p->pid==created_pid){
            if(p->kernel_stack_top)
                page_free((void *)(uintptr_t)(p->kernel_stack_top-PAGE_SIZE));
            clear_process(p);
        }
        spinlock_irqrestore_release(&process_lock,flags);
        /* The caller retains ownership of the address space and user pages. */
        return -1;
    }

    flags=spinlock_irqsave_acquire(&process_lock);
    int ready_failed=0;
    if(p->state!=PROCESS_STOPPED||p->stop_event_pending){
        ready_failed=1;
    }else{
        /* Publish migration policy before making the process runnable. */
        p->migration_eligible=migration_eligible;
        if(process_transition_state_locked(p,PROCESS_READY)!=0)
            ready_failed=1;
    }
    spinlock_irqrestore_release(&process_lock,flags);

    if(ready_failed){
        vfs_close_all_process(p);
        flags=spinlock_irqsave_acquire(&process_lock);
        if(p->state==PROCESS_STOPPED&&p->pid==created_pid){
            if(p->kernel_stack_top)
                page_free((void *)(uintptr_t)(p->kernel_stack_top-PAGE_SIZE));
            clear_process(p);
        }
        spinlock_irqrestore_release(&process_lock,flags);
        /* The caller retains ownership of the address space and user pages. */
        return -1;
    }

    /* Never dereference the process slot after READY publication to obtain PID:
       the scheduler may run, exit, and recycle that slot immediately. */
    if(pid_out)*pid_out=created_pid;
    if(process_out)*process_out=p;
    return 0;
}

struct process *process_create_ex_vas(uint32_t entry,uint32_t stack,
    uint32_t pd,const uint32_t *pages,const uint32_t *vas,uint32_t count){
    struct process *created=0;
    if(process_create_ex_vas_internal(entry,stack,pd,pages,vas,count,0u,
                                      0,&created)!=0)return 0;
    return created;
}

int process_create_ex_vas_pid(uint32_t entry,uint32_t stack,uint32_t pd,
    const uint32_t *pages,const uint32_t *vas,uint32_t count,
    uint32_t migration_eligible,uint32_t *pid_out){
    if(!pid_out)return -1;
    return process_create_ex_vas_internal(entry,stack,pd,pages,vas,count,
                                          migration_eligible,pid_out,0);
}
struct process *process_create_ex(uint32_t entry,uint32_t stack,uint32_t pd,const uint32_t *pages,uint32_t count){ uint32_t vas[LIONOS_PROCESS_MAX_USER_PAGES];for(uint32_t i=0;i<count&&i<LIONOS_PROCESS_MAX_USER_PAGES;++i)vas[i]=i*4096u;return process_create_ex_vas(entry,stack,pd,pages,vas,count); }
struct process *process_create(uint32_t entry,uint32_t stack,uint32_t pd,uint32_t code,uint32_t user_stack_page){ uint32_t p[2]={code,user_stack_page},v[2]={0x10000000u,0x10001000u};return process_create_ex_vas(entry,stack,pd,p,v,2u); }

int process_exec_replace_current(uint32_t entry,uint32_t stack,uint32_t pd,
    const uint32_t *pages,const uint32_t *vas,uint32_t count){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *c=current_local();
    if(!c||c==&processes[0]||c->is_thread||!pd||!pages||!vas||
       !count||count>LIONOS_PROCESS_MAX_USER_PAGES){
        spinlock_irqrestore_release(&process_lock,flags);
        return -1;
    }
    void *new_ks=page_alloc();
    if(!new_ks){
        spinlock_irqrestore_release(&process_lock,flags);
        return -1;
    }

    /*
     * Validate every sibling's queue membership before retiring any thread.
     * process_lock prevents scheduler transitions between this pass and the
     * retirement pass below.
     */
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *t=&processes[i];
        if(t==c||t->state==PROCESS_UNUSED||!t->is_thread||
           t->thread_group_pid!=c->thread_group_pid)continue;
        uint32_t index;
        if(process_slot_index(t,&index)!=0){
            page_free(new_ks);
            spinlock_irqrestore_release(&process_lock,flags);
            return -1;
        }
        const struct runqueue_node *node=&runqueue_nodes[index];
        if(t->state==PROCESS_READY){
            if(node->queued!=1u||node->queue_cpu>=LIONOS_MAX_CPUS||
               node->queue_cpu!=t->cpu_owner||
               process_runqueue_validate_locked(node->queue_cpu)!=0){
                page_free(new_ks);
                spinlock_irqrestore_release(&process_lock,flags);
                return -1;
            }
        }else if(node->queued!=0u||
                 node->next!=LIONOS_RUNQUEUE_INVALID_INDEX||
                 node->previous!=LIONOS_RUNQUEUE_INVALID_INDEX||
                 node->queue_cpu!=LIONOS_RUNQUEUE_INVALID_INDEX){
            page_free(new_ks);
            spinlock_irqrestore_release(&process_lock,flags);
            return -1;
        }
    }

    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *t=&processes[i];
        if(t==c||t->state==PROCESS_UNUSED||!t->is_thread||
           t->thread_group_pid!=c->thread_group_pid)continue;
        if(t->state!=PROCESS_ZOMBIE&&
           process_transition_state_locked(t,PROCESS_ZOMBIE)!=0){
            /* Never replace the address space while a sibling can still run. */
            page_free(new_ks);
            spinlock_irqrestore_release(&process_lock,flags);
            return -1;
        }
        reap_process(t);
        if(t->state!=PROCESS_UNUSED){
            /* A failed thread-stack teardown must not outlive the old space. */
            page_free(new_ks);
            spinlock_irqrestore_release(&process_lock,flags);
            return -1;
        }
    }

    uint32_t old_pd=c->page_directory,old_ks=c->kernel_stack_top;
    uint32_t old_pages[LIONOS_PROCESS_MAX_USER_PAGES];
    uint32_t old_count=c->user_page_count;
    for(uint32_t i=0;i<old_count;++i)old_pages[i]=c->user_pages[i];
    c->page_directory=pd;
    c->entry=entry;
    c->user_stack=stack;
    c->kernel_stack_top=(uint32_t)(uintptr_t)new_ks+4096u;
    __atomic_store_n(&c->saved_frame,0,__ATOMIC_RELEASE);
    c->user_page_count=count;
    c->user_code_page=pages[0];
    c->user_stack_page=pages[count-1u];
    c->exit_code=0;
    c->wait_pid=0;
    c->wait_status_ptr=0;
    c->wait_options=0;
    __atomic_store_n(&c->wait_frame_saved,0u,__ATOMIC_RELEASE);
    c->stop_event_pending=0;
    c->stop_signal=0;
    c->continued_event_pending=0;
    c->reap_pending=0;
    c->pending_signals=0;
    c->signal_active=0u;
    c->signal_active_restorer=0u;
    for(uint32_t i=0u;i<32u;++i){
        if(c->signal_handlers[i]!=LIONOS_SIG_IGN)
            c->signal_handlers[i]=LIONOS_SIG_DFL;
        c->signal_restorers[i]=0u;
    }
    for(uint32_t i=0u;i<PROCESS_CONTEXT_WORDS;++i)
        c->signal_saved_frame[i]=0u;
    c->deferred_kernel_stack=old_ks;
    for(uint32_t i=0;i<count;++i){
        c->user_pages[i]=pages[i];
        c->user_page_vas[i]=vas[i];
    }
    for(uint32_t i=count;i<LIONOS_PROCESS_MAX_USER_PAGES;++i){
        c->user_pages[i]=0;
        c->user_page_vas[i]=0;
    }
    paging_switch_address_space(pd);
    tss_set_kernel_stack(c->kernel_stack_top);
    init_interrupt_frame(c);
    for(uint32_t i=0;i<old_count;++i)
        if(old_pages[i])page_free((void *)(uintptr_t)old_pages[i]);
    if(old_pd&&old_pd!=pd)paging_destroy_address_space(old_pd);
    spinlock_irqrestore_release(&process_lock,flags);
    return 0;
}

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
    slot->time_slice=scheduler_priority_quantum(slot->priority);
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
/* process_lock must be held. A zombie's stack/address space cannot be
   released until every CPU has stopped using that process context. */
static int process_is_current_locked(const struct process *p){
    if(!p)return 0;
    for(uint32_t cpu=0u;cpu<LIONOS_MAX_CPUS;++cpu)
        if(current_cpu_load(cpu)==p)return 1;
    return 0;
}

/*
 * process_lock must be held. READY processes must be detached from every
 * current-CPU slot; a RUNNING process must be the unique current context on
 * its recorded, online owner CPU. This prevents migration eligibility from
 * being published for a process whose state and execution ownership diverge.
 */
static int process_migration_state_valid_locked(const struct process *p){
    uint32_t matches=0u;
    uint32_t current_owner=LIONOS_RUNQUEUE_INVALID_INDEX;
    uint32_t online=smp_online_count();
    if(!p)return 0;
    if(online>LIONOS_MAX_CPUS)online=LIONOS_MAX_CPUS;
    for(uint32_t cpu=0u;cpu<LIONOS_MAX_CPUS;++cpu){
        if(current_cpu_load(cpu)==p){
            ++matches;
            current_owner=cpu;
        }
    }
    if(p->state==PROCESS_READY)return matches==0u;
    if(p->state==PROCESS_RUNNING)
        return matches==1u&&current_owner==p->cpu_owner&&
               current_owner<online;
    return 0;
}

static void reap_process(struct process*p){
    if(!p||p->state!=PROCESS_ZOMBIE)return;
    if(process_is_current_locked(p)){
        /* The scheduler reaper retries after the current CPU switches away. */
        p->reap_pending=1u;
        return;
    }
    if(p->is_thread){
        uint32_t is_ap_probe=(p->pid==0u&&p->thread_group_pid==0u&&
            p->entry==(uint32_t)(uintptr_t)&process_scheduler_ap_probe);
        uint32_t probe_index=LIONOS_PROCESS_MAX;
        if(is_ap_probe){
            for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i)
                if(&processes[i]==p){probe_index=i;break;}
            if(probe_index>=LIONOS_PROCESS_MAX||
               runqueue_nodes[probe_index].queued!=0u||
               runqueue_nodes[probe_index].next!=LIONOS_RUNQUEUE_INVALID_INDEX||
               runqueue_nodes[probe_index].previous!=LIONOS_RUNQUEUE_INVALID_INDEX||
               runqueue_nodes[probe_index].queue_cpu!=LIONOS_RUNQUEUE_INVALID_INDEX||
               !p->kernel_stack_top||
               p->kernel_stack_top<PAGE_SIZE||
               (p->kernel_stack_top&(PAGE_SIZE-1u))!=0u){
                __atomic_store_n(&scheduler_ap_probe_fault,1u,__ATOMIC_RELEASE);
                return;
            }
            for(uint32_t cpu=0u;cpu<LIONOS_MAX_CPUS;++cpu){
                if(current_cpu_load(cpu)==p){
                    __atomic_store_n(&scheduler_ap_probe_fault,1u,__ATOMIC_RELEASE);
                    return;
                }
            }
        }
        if(p->thread_stack_page){
            if(paging_unmap_user_page_in(p->page_directory,p->thread_stack_va,
                                         p->thread_stack_page)!=0)return;
            page_free((void *)(uintptr_t)p->thread_stack_page);
        }
        if(p->kernel_stack_top)
            page_free((void *)(uintptr_t)(p->kernel_stack_top-PAGE_SIZE));
        clear_process(p);
        if(is_ap_probe){
            if(p->state!=PROCESS_UNUSED||
               runqueue_nodes[probe_index].queued!=0u||
               runqueue_nodes[probe_index].next!=LIONOS_RUNQUEUE_INVALID_INDEX||
               runqueue_nodes[probe_index].previous!=LIONOS_RUNQUEUE_INVALID_INDEX||
               runqueue_nodes[probe_index].queue_cpu!=LIONOS_RUNQUEUE_INVALID_INDEX){
                __atomic_store_n(&scheduler_ap_probe_fault,1u,__ATOMIC_RELEASE);
                return;
            }
            __atomic_fetch_add(&scheduler_ap_probe_reaped,1u,__ATOMIC_ACQ_REL);
        }
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
int process_get_migration_info(uint32_t pid,uint32_t *cpu_owner,
                               uint32_t *migration_count){
    if(!pid||!cpu_owner||!migration_count)return -1;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *p=find_pid(pid);
    if(!p||p->state==PROCESS_UNUSED){
        spinlock_irqrestore_release(&process_lock,flags);return -1;
    }
    *cpu_owner=p->cpu_owner;*migration_count=p->migration_count;
    spinlock_irqrestore_release(&process_lock,flags);return 0;
}

int process_set_migration_eligible(uint32_t pid){
    if(!pid)return -1;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    /*
     * Keep the single-process API fail-closed just like batch publication:
     * eligibility must never be published while any CPU queue or process/node
     * ownership invariant is already inconsistent.
     */
    if(process_runqueues_validate_all_locked()!=0){
        __atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);
        spinlock_irqrestore_release(&process_lock,flags);
        return -1;
    }
    struct process *p=find_pid(pid);
    if(!p||(p->state!=PROCESS_READY&&p->state!=PROCESS_RUNNING)||
       !process_migration_state_valid_locked(p)||
       p->pid<=1u||p->is_thread||p->owns_address_space!=1u||
       p->thread_group_pid!=p->pid||p->migration_eligible!=0u||
       p->migration_count!=0u){
        spinlock_irqrestore_release(&process_lock,flags);
        return -1;
    }
    p->migration_eligible=1u;
    spinlock_irqrestore_release(&process_lock,flags);
    return 0;
}

/*
 * Atomically opt in a complete batch of READY user processes. The scheduler
 * takes process_lock before inspecting migration eligibility, so no AP can
 * steal a prefix of the batch while the remaining entries are being checked.
 */
int process_set_migration_eligible_batch(const uint32_t *pids,uint32_t count){
    if(!pids||count==0u||count>LIONOS_PROCESS_MAX)return -1;
    struct process *validated[LIONOS_PROCESS_MAX];
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    /*
     * Validate every CPU queue and the global process/node ownership invariant
     * while process_lock excludes lifecycle changes. The validator acquires
     * all queue locks in ascending CPU order and releases them in reverse.
     */
    if(process_runqueues_validate_all_locked()!=0){
        __atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);
        spinlock_irqrestore_release(&process_lock,flags);
        return -1;
    }
    for(uint32_t i=0u;i<count;++i){
        if(!pids[i]){
            spinlock_irqrestore_release(&process_lock,flags);
            return -1;
        }
        for(uint32_t j=0u;j<i;++j){
            if(pids[j]==pids[i]){
                spinlock_irqrestore_release(&process_lock,flags);
                return -1;
            }
        }
        struct process *p=find_pid(pids[i]);
        if(!p||p->state!=PROCESS_READY||
           !process_migration_state_valid_locked(p)||
           p->pid<=1u||p->is_thread||p->owns_address_space!=1u||
           p->thread_group_pid!=p->pid||p->cpu_owner!=0u||
           p->migration_count!=0u||p->migration_eligible!=0u){
            spinlock_irqrestore_release(&process_lock,flags);
            return -1;
        }
        validated[i]=p;
    }
    /* Keep the validated references: process_lock prevents slot reuse or
       state changes between validation and this all-or-nothing publication. */
    for(uint32_t i=0u;i<count;++i)
        validated[i]->migration_eligible=1u;
    spinlock_irqrestore_release(&process_lock,flags);
    return 0;
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
    /* Return 1 while the process is still a CPU's live execution context.
       The caller may retry after the scheduler has switched that CPU away. */
    if(process_is_current_locked(p)){
        spinlock_irqrestore_release(&process_lock,flags);
        return 1;
    }
    reap_process(p);
    int result=p->state==PROCESS_UNUSED?0:-1;
    spinlock_irqrestore_release(&process_lock,flags);
    return result;
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
static uint32_t process_wake_interrupted_waitpid_locked(void){
    uint32_t wake_mask=0u;
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
        if(process_transition_state_locked(p,PROCESS_READY)!=0)continue;
        saved[11]=(uint32_t)LIONOS_EINTR;
        process_clear_waitpid_locked(p);
        uint32_t cpu=p->cpu_owner<LIONOS_MAX_CPUS?p->cpu_owner:0u;
        if(cpu<32u)wake_mask|=1u<<cpu;
    }
    return wake_mask;
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
    if(process_transition_state_locked(caller,PROCESS_WAITING)!=0){
        /* Do not report a blocked wait unless WAITING was published. */
        process_clear_waitpid_locked(caller);
        spinlock_irqrestore_release(&process_lock,flags);
        return -1;
    }
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
    if(!channel||!held_lock||!scheduler_tick_delta_valid(timeout_ticks)){
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
    if(process_transition_state_locked(c,PROCESS_WAITING)!=0){
        /* The caller must not return as blocked unless WAITING was published. */
        c->wait_channel=0u;
        c->wait_kind=0u;
        c->wait_timeout_active=0u;
        c->wait_timeout_result=0u;
        c->wait_timeout_ticks=0u;
        c->wait_cpu=0u;
        c->wait_deadline=0u;
        spinlock_irqrestore_release(&process_lock,flags);
        spinlock_irqrestore_release(held_lock,irq_flags);
        return -1;
    }
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

void process_scheduler_tick(uint32_t cpu){
    if(cpu>=LIONOS_MAX_CPUS||
       __atomic_load_n(&process_scheduler_ready,__ATOMIC_ACQUIRE)==0u)return;
    /* Only timer interrupts that enter process_schedule() contribute to a
       task quantum. Keep BSP frame-timer ticks out of this accounting path. */
    (void)scheduler_tick_accumulate(
        &scheduler_cpu[cpu].pending_schedule_ticks);
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
           p->wait_cpu!=cpu)continue;
        /*
         * A scheduler lock-contention fallback may switch a blocked task to
         * idle without reaching the normal deadline-arm point. Complete that
         * pending arm here while process_lock is held, before testing expiry.
         */
        if(p->wait_timeout_active==2u){
            p->wait_deadline=now+p->wait_timeout_ticks;
            p->wait_timeout_active=1u;
            continue;
        }
        if(p->wait_timeout_active!=1u||
           !scheduler_tick_deadline_reached(now,p->wait_deadline))continue;
        uint32_t kind=p->wait_kind;
        /*
         * Keep the timeout registration intact until READY publication has
         * successfully inserted the process into its owner run queue. If the
         * queue rejects the transition, the expired deadline is retried on
         * the next local timer tick instead of stranding a WAITING process.
         */
        if(process_transition_state_locked(p,PROCESS_READY)!=0)
            continue;
        p->wait_channel=0u;
        p->wait_kind=0u;
        p->wait_timeout_active=0u;
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
    uint32_t wake_mask=0u;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock),woken=0u;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX&&woken<maximum;++i){
        struct process *p=&processes[i];
        if(p->state!=PROCESS_WAITING||p->wait_kind!=1u||
           p->wait_channel!=(uint32_t)channel)continue;

        /*
         * Publish READY only after the queue accepts the node. On failure,
         * retain the WAITING metadata so the process is not stranded in a
         * partially-cleared wait state.
         */
        if(process_transition_state_locked(p,PROCESS_READY)!=0)
            continue;

        p->wait_channel=0u;
        p->wait_kind=0u;
        p->wait_timeout_active=0u;
        p->wait_timeout_result=0u;
        uint32_t *frame=(uint32_t *)(uintptr_t)__atomic_load_n(
            &p->saved_frame,__ATOMIC_ACQUIRE);
        if(frame)frame[11]=0u;

        uint32_t owner=p->cpu_owner;
        if(owner<LIONOS_MAX_CPUS)wake_mask|=1u<<owner;
        ++woken;
    }
    spinlock_irqrestore_release(&process_lock,flags);

    /* Wake queue owners only after READY publication and lock release. */
    for(uint32_t cpu=0u;cpu<LIONOS_MAX_CPUS;++cpu)
        if(wake_mask&(1u<<cpu))process_kick_cpu(cpu);
    return woken;
}

static uint32_t wake_waiting_parent(struct process *child){
    if(!child||child->state!=PROCESS_ZOMBIE)return LIONOS_MAX_CPUS;
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

        /* Keep WAITING metadata intact if queue publication fails. */
        if(process_transition_state_locked(parent,PROCESS_READY)!=0)
            return LIONOS_MAX_CPUS;
        uint32_t owner=parent->cpu_owner<LIONOS_MAX_CPUS?
            parent->cpu_owner:LIONOS_MAX_CPUS;
        parent->wait_pid=0u;
        parent->wait_status_ptr=0u;
        parent->wait_options=0u;
        parent->wait_channel=0u;
        parent->wait_kind=0u;
        __atomic_store_n(&parent->wait_frame_saved,0u,__ATOMIC_RELEASE);

        /* Keep the zombie available for a retry when status delivery failed. */
        if(status_ok)child->reap_pending=1u;
        else debug_write("LIONOS:WAITPID-ASYNC-STATUS-WRITE-FAIL\n");
        return owner;
    }
    return LIONOS_MAX_CPUS;
}

static uint32_t wake_waiting_parent_stop(struct process *child){
    if(!child||!child->stop_event_pending)return 0u;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *parent=&processes[i];
        if(parent->state!=PROCESS_WAITING||parent->wait_kind!=3u||parent->pid!=child->parent_pid||
           !(parent->wait_options&LIONOS_WAIT_UNTRACED)||
           (parent->wait_pid!=PROCESS_WAIT_ANY&&parent->wait_pid!=child->pid))
            continue;
        uint32_t status=LIONOS_WAIT_STATUS_STOPPED(child->stop_signal);
        int status_ok=!parent->wait_status_ptr||
            write_user_u32(parent,parent->wait_status_ptr,status)==0;
        uint32_t *frame=(uint32_t *)(uintptr_t)__atomic_load_n(&parent->saved_frame,__ATOMIC_ACQUIRE);
        if(process_transition_state_locked(parent,PROCESS_READY)!=0)return 0u;
        if(frame)frame[11]=status_ok?child->pid:PROCESS_SYSCALL_ERR;
        if(status_ok)child->stop_event_pending=0u;
        uint32_t owner=parent->cpu_owner;
        parent->wait_pid=0u;parent->wait_status_ptr=0u;
        parent->wait_options=0u;parent->wait_channel=0u;
        parent->wait_kind=0u;
        __atomic_store_n(&parent->wait_frame_saved,0u,__ATOMIC_RELEASE);
        return owner<LIONOS_MAX_CPUS?(1u<<owner):0u;
    }
    return 0u;
}

static uint32_t wake_waiting_parent_continued(struct process *child){
    if(!child||!child->continued_event_pending)return 0u;
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
        if(process_transition_state_locked(parent,PROCESS_READY)!=0)return 0u;
        if(frame)frame[11]=status_ok?child->pid:PROCESS_SYSCALL_ERR;
        if(status_ok)child->continued_event_pending=0u;
        uint32_t owner=parent->cpu_owner;
        parent->wait_pid=0u;parent->wait_status_ptr=0u;
        parent->wait_options=0u;parent->wait_channel=0u;
        parent->wait_kind=0u;
        __atomic_store_n(&parent->wait_frame_saved,0u,__ATOMIC_RELEASE);
        return owner<LIONOS_MAX_CPUS?(1u<<owner):0u;
    }
    return 0u;
}

static uint32_t wake_pending_waitpid_event(struct process *parent){
    if(!parent||parent->state!=PROCESS_WAITING||parent->wait_kind!=3u)
        return 0u;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *child=&processes[i];
        if(child->state==PROCESS_UNUSED||child->reap_pending||
           child->parent_pid!=parent->pid||
           (parent->wait_pid!=PROCESS_WAIT_ANY&&parent->wait_pid!=child->pid))
            continue;
        if(child->state==PROCESS_ZOMBIE){
            uint32_t owner=wake_waiting_parent(child);
            return owner<LIONOS_MAX_CPUS?(1u<<owner):0u;
        }
        if((parent->wait_options&LIONOS_WAIT_UNTRACED)&&
           child->state==PROCESS_STOPPED&&child->stop_event_pending)
            return wake_waiting_parent_stop(child);
        if((parent->wait_options&LIONOS_WAIT_CONTINUED)&&child->continued_event_pending)
            return wake_waiting_parent_continued(child);
    }
    return 0u;
}

static uint32_t stop_process_locked(struct process *process,uint32_t signal){
    if(!process||process->state==PROCESS_UNUSED||process->state==PROCESS_ZOMBIE)return 0u;
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
    if(current_cpu_load(cpu)==process)
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
            if(current_cpu_load(cpu)==thread)
                process_request_blocked_reschedule(cpu);
            else
                __atomic_store_n(&reschedule_requested[cpu],1u,__ATOMIC_RELEASE);
        }
        if(!was_stopped)return wake_waiting_parent_stop(process);
    }
    return 0u;
}

static uint32_t continue_process_locked(struct process *process){
    if(!process||process->state==PROCESS_UNUSED||process->state==PROCESS_ZOMBIE)return 0u;
    uint32_t wake_mask=0u;
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
    if(was_stopped&&process_transition_state_locked(
           process,resume_waitpid?PROCESS_WAITING:PROCESS_READY)!=0)return 0u;
    if(was_stopped&&!resume_waitpid&&process->cpu_owner<LIONOS_MAX_CPUS)
        wake_mask|=1u<<process->cpu_owner;
    if(!process->is_thread){
        for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
            struct process *thread=&processes[i];
            if(thread->state==PROCESS_UNUSED||thread->state==PROCESS_ZOMBIE||
               !thread->is_thread||thread->thread_group_pid!=process->pid)continue;
            thread->pending_signals&=~stop_mask;
            if(thread->state==PROCESS_STOPPED&&
               process_transition_state_locked(thread,PROCESS_READY)==0){
                uint32_t cpu=thread->cpu_owner<LIONOS_MAX_CPUS?thread->cpu_owner:0u;
                wake_mask|=1u<<cpu;
                __atomic_store_n(&reschedule_requested[cpu],1u,__ATOMIC_RELEASE);
            }
        }
    }
    if(was_stopped&&!process->is_thread)
        wake_mask|=wake_waiting_parent_continued(process);
    if(resume_waitpid)wake_mask|=wake_pending_waitpid_event(process);
    return wake_mask;
}

static int process_scheduler_ap_probe_stop_current(void){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    struct process *current=current_local();
    if(!current||!current->is_thread||current->thread_group_pid!=0u||
       current->entry!=(uint32_t)(uintptr_t)&process_scheduler_ap_probe||
       current->cpu_owner!=cpu_current_index()||current->state!=PROCESS_RUNNING){
        spinlock_irqrestore_release(&process_lock,flags);
        return -1;
    }
    uint32_t wake_mask=stop_process_locked(current,LIONOS_SIG_STOP);
    int stopped=current->state==PROCESS_STOPPED&&
        runqueue_nodes[process_index(current)].queued==0u;
    spinlock_irqrestore_release(&process_lock,flags);
    for(uint32_t cpu=0u;cpu<LIONOS_MAX_CPUS;++cpu)
        if(wake_mask&(1u<<cpu))process_kick_cpu(cpu);
    return stopped?0:-1;
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

static uint32_t process_notify_child_group_transitions_locked(
    const struct process_group_snapshot *groups,uint32_t count){
    uint32_t wake_mask=0u;
    if(!groups)return 0u;
    for(uint32_t i=0u;i<count;++i)
        wake_mask|=process_notify_orphan_transition_locked(
            groups[i].pgid,groups[i].sid,groups[i].was_orphaned);
    return wake_mask;
}

static uint32_t process_notify_orphan_transition_locked(
    uint32_t pgid,uint32_t sid,int was_orphaned){
    if(!was_orphaned&&process_group_orphaned_locked(pgid,sid)&&
       process_group_has_stopped_locked(pgid,sid))
        return process_orphan_group_notify_locked(pgid,sid);
    return 0u;
}

static uint32_t process_orphan_group_notify_locked(uint32_t pgid,uint32_t sid){
    uint32_t wake_mask=0u;
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
            if(saved&&process_transition_state_locked(member,PROCESS_READY)==0){
                saved[11]=(uint32_t)LIONOS_EINTR;
                process_clear_waitpid_locked(member);
                uint32_t cpu=member->cpu_owner<LIONOS_MAX_CPUS?member->cpu_owner:0u;
                if(cpu<32u)wake_mask|=1u<<cpu;
            }
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
            wake_mask|=continue_process_locked(member);
            debug_write("LIONOS:JOBCTL:ORPHAN-CONTINUE-DONE\n");
        }
    }
    debug_write("LIONOS:JOBCTL:ORPHAN-NOTIFY-DONE\n");
    return wake_mask;
}

/* Called with process_lock held. Reparenting is immediate, as required when
   a parent exits; init (PID 1) is the adopter and auto-reaps its children. */
static uint32_t process_reparent_children_locked(struct process *exiting){
    uint32_t pgids[LIONOS_PROCESS_MAX],sids[LIONOS_PROCESS_MAX];
    uint8_t was_orphaned[LIONOS_PROCESS_MAX];
    uint32_t group_count=0u;
    uint32_t wake_mask=0u;
    if(!exiting||!exiting->pid)return 0u;
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
            wake_mask|=process_orphan_group_notify_locked(pgids[g],sids[g]);
    }
    return wake_mask;
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

static __attribute__((noreturn)) void process_lifecycle_fault(
    const char *reason,uint32_t pid){
    __asm__ volatile("cli" ::: "memory");
    debug_write("LIONOS:PROCESS-LIFECYCLE-FAULT ");
    debug_write(reason);
    debug_write(" pid=0x");
    process_diag_hex32(pid);
    debug_write("\n");
    for(;;)__asm__ volatile("hlt");
}

/* process_lock is held. A thread-group leader may not release its address
 * space while any sibling remains runnable, current on a CPU, or unreaped. */
static int process_validate_thread_retirement_locked(
    const struct process *owner){
    if(!owner||owner->is_thread||owner->thread_group_pid!=owner->pid||
       owner->cpu_owner>=LIONOS_MAX_CPUS)
        return -1;
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        const struct process *thread=&processes[i];
        if(thread->state==PROCESS_UNUSED||!thread->is_thread||
           thread->thread_group_pid!=owner->thread_group_pid)continue;
        uint32_t index;
        if(process_slot_index(thread,&index)!=0||
           thread->cpu_owner!=owner->cpu_owner)return -1;
        for(uint32_t cpu=0u;cpu<LIONOS_MAX_CPUS;++cpu)
            if(current_cpu_load(cpu)==thread)return -1;
        const struct runqueue_node *node=&runqueue_nodes[index];
        if(thread->state==PROCESS_READY){
            if(node->queued!=1u||node->queue_cpu>=LIONOS_MAX_CPUS||
               node->queue_cpu!=thread->cpu_owner||
               process_runqueue_validate_locked(node->queue_cpu)!=0)
                return -1;
        }else{
            if((thread->state!=PROCESS_ZOMBIE&&
                thread->state!=PROCESS_WAITING&&
                thread->state!=PROCESS_STOPPED)||
               node->queued!=0u||
               node->next!=LIONOS_RUNQUEUE_INVALID_INDEX||
               node->previous!=LIONOS_RUNQUEUE_INVALID_INDEX||
               node->queue_cpu!=LIONOS_RUNQUEUE_INVALID_INDEX)
                return -1;
        }
    }
    return 0;
}

static void process_retire_siblings_locked(struct process *owner){
    if(process_validate_thread_retirement_locked(owner)!=0)
        process_lifecycle_fault("thread-group-queue-validation",
                                owner?owner->pid:0u);
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *thread=&processes[i];
        if(thread->state==PROCESS_UNUSED||!thread->is_thread||
           thread->thread_group_pid!=owner->thread_group_pid)continue;
        if(thread->state!=PROCESS_ZOMBIE&&
           process_transition_state_locked(thread,PROCESS_ZOMBIE)!=0)
            process_lifecycle_fault("thread-group-zombie-transition",owner->pid);
        reap_process(thread);
        if(thread->state!=PROCESS_UNUSED)
            process_lifecycle_fault("thread-group-reap",owner->pid);
    }
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
        /* Stop every sibling before closing process-wide descriptors. Threads
         * share the leader's FD table and address space; no sibling may race
         * pipe/VFS teardown once this point has been reached. */
        flags=spinlock_irqsave_acquire(&process_lock);
        if(current_local()!=c||c->state==PROCESS_UNUSED||
           c->state==PROCESS_ZOMBIE)
            process_lifecycle_fault("process-exit-owner-changed",c->pid);
        process_retire_siblings_locked(c);
        spinlock_irqrestore_release(&process_lock,flags);
        pipe_close_all(c);
        vfs_close_all_process(c);
    }
    if(is_hup_exit)debug_write("LIONOS:SIGHUP:FD-CLOSE-DONE\n");

    /* Keep a zombie from being preempted away before exit cleanup completes. */
    uint32_t exit_flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(exit_flags) : : "memory","cc");
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
        __asm__ volatile("pushl %0; popfl" : : "r"(exit_flags) : "memory","cc");
        return;
    }
    if(is_hup_exit)debug_write("LIONOS:SIGHUP:LOCK-ACQUIRED\n");
    if(c->is_thread){
        process_diag_status33("THREAD-EXIT",c->tid,c->thread_group_pid,code);
        c->exit_code=code;
        if(c->thread_group_pid==0u)c->reap_pending=1u;
        if(process_transition_state_locked(c,PROCESS_ZOMBIE)!=0)
            process_lifecycle_fault("current-thread-zombie-transition",c->pid);
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
            if(process_transition_state_locked(w,PROCESS_READY)!=0)
                process_lifecycle_fault("thread-joiner-wakeup",w->pid);
            w->join_tid=0u;
            w->join_status_ptr=0u;
            c->reap_pending=1u;
            break;
        }
        spinlock_irqrestore_release(&process_lock,flags);
        uint32_t exit_cpu=cpu_current_index();
        process_request_blocked_reschedule(exit_cpu);
        __asm__ volatile("pushl %0; popfl" : : "r"(exit_flags) : "memory","cc");
        process_kick_cpu(exit_cpu);
        return;
    }

    /* Siblings were retired before process-wide resource teardown. */
    uint32_t exiting_sid=c->sid;
    uint32_t diagnostic_pid=c->pid;
    uint32_t diagnostic_parent_pid=c->parent_pid;
    c->exit_code=code;
    uint32_t orphan_wake_mask=process_reparent_children_locked(c);
    if(is_hup_exit)debug_write("LIONOS:SIGHUP:REPARENT-DONE\n");
    if(process_transition_state_locked(c,PROCESS_ZOMBIE)!=0)
        process_lifecycle_fault("process-zombie-transition",diagnostic_pid);
    if(is_hup_exit)debug_write("LIONOS:SIGHUP:ZOMBIE-SET\n");
    uint32_t parent_wake_cpu=wake_waiting_parent(c);
    if(is_hup_exit)debug_write("LIONOS:SIGHUP:PARENT-WAKE-DONE\n");
    spinlock_irqrestore_release(&process_lock,flags);
    if(is_hup_exit)debug_write("LIONOS:SIGHUP:UNLOCKED\n");
    process_diag_status33("PROCESS-EXIT-STATUS",diagnostic_pid,
                          diagnostic_parent_pid,code);
    process_release_tty_if_session_empty(exiting_sid);
    if(is_hup_exit)debug_write("LIONOS:SIGHUP:TTY-RELEASE-DONE\n");
    /* Publish the non-runnable handoff only after all exit cleanup is done. */
    uint32_t exit_cpu=cpu_current_index();
    process_kick_mask(orphan_wake_mask,exit_cpu);
    if(parent_wake_cpu<LIONOS_MAX_CPUS&&parent_wake_cpu!=exit_cpu)
        process_kick_cpu(parent_wake_cpu);
    process_request_blocked_reschedule(exit_cpu);
    __asm__ volatile("pushl %0; popfl" : : "r"(exit_flags) : "memory","cc");
    process_kick_cpu(exit_cpu);
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
    uint32_t wake_mask=0u;
    debug_write("LIONOS:JOBCTL:BACKGROUND-STOP-REQUEST\n");
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *member=&processes[i];
        if(member->state==PROCESS_UNUSED||member->state==PROCESS_ZOMBIE||
           member->is_thread||member->pgid!=caller->pgid)continue;
        wake_mask|=stop_process_locked(member,signal);
    }
    debug_write("LIONOS:JOBCTL:BACKGROUND-STOP-DONE\n");
    spinlock_irqrestore_release(&process_lock,flags);
    for(uint32_t cpu=0u;cpu<LIONOS_MAX_CPUS;++cpu)
        if(wake_mask&(1u<<cpu))process_kick_cpu(cpu);
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
            if(current_cpu_load(target_cpu)==p)
                process_request_blocked_reschedule(target_cpu);
            spinlock_irqrestore_release(&process_lock,flags);
            return LIONOS_SIGNAL_OK;
        }
        p->pending_signals|=(1u<<(signal-1u));
        p->exit_code=128u+signal;
        (void)process_transition_state_locked(p,PROCESS_STOPPED);
        uint32_t target_cpu=p->cpu_owner<LIONOS_MAX_CPUS?p->cpu_owner:0u;
        if(current_cpu_load(target_cpu)==p)
            process_request_blocked_reschedule(target_cpu);
        uint32_t exiting_sid=p->sid;
        spinlock_irqrestore_release(&process_lock,flags);
        pipe_close_all(p);
        vfs_close_all_process(p);
        uint32_t parent_wake_cpu=LIONOS_MAX_CPUS;
        uint32_t orphan_wake_mask=0u;
        flags=spinlock_irqsave_acquire(&process_lock);
        if(p->state==PROCESS_STOPPED){
            orphan_wake_mask=process_reparent_children_locked(p);
            parent_wake_cpu=wake_waiting_parent(p);
        }
        spinlock_irqrestore_release(&process_lock,flags);
        process_kick_mask(orphan_wake_mask,cpu_current_index());
        if(parent_wake_cpu<LIONOS_MAX_CPUS&&
           parent_wake_cpu!=cpu_current_index())
            process_kick_cpu(parent_wake_cpu);
        process_release_tty_if_session_empty(exiting_sid);
        return LIONOS_SIGNAL_OK;
    }
    if(signal==LIONOS_SIG_HUP||signal==LIONOS_SIG_INT){
        uint32_t wake_cpu=LIONOS_MAX_CPUS;
        p->pending_signals|=(1u<<(signal-1u));
        if(p->state==PROCESS_WAITING&&p->wait_kind==3u&&
           __atomic_load_n(&p->wait_frame_saved,__ATOMIC_ACQUIRE)&&
           process_has_interrupting_signal_locked(p)){
            uint32_t frame_address=
                __atomic_load_n(&p->saved_frame,__ATOMIC_ACQUIRE);
            uint32_t *saved=(uint32_t *)(uintptr_t)frame_address;
            if(saved&&process_transition_state_locked(p,PROCESS_READY)==0){
                debug_write("LIONOS:WAITPID-EINTR-REMOTE-WAKE\n");
                saved[11]=(uint32_t)LIONOS_EINTR;
                process_clear_waitpid_locked(p);
                wake_cpu=p->cpu_owner<LIONOS_MAX_CPUS?p->cpu_owner:LIONOS_MAX_CPUS;
            }
        }
        spinlock_irqrestore_release(&process_lock,flags);
        if(wake_cpu<LIONOS_MAX_CPUS&&wake_cpu!=cpu_current_index())
            process_kick_cpu(wake_cpu);
        return LIONOS_SIGNAL_OK;
    }
    if(signal==LIONOS_SIG_STOP||signal==LIONOS_SIG_TTIN||signal==LIONOS_SIG_TTOU){
        uint32_t wake_mask=stop_process_locked(p,signal);
        spinlock_irqrestore_release(&process_lock,flags);
        for(uint32_t cpu=0u;cpu<LIONOS_MAX_CPUS;++cpu)
            if((wake_mask&(1u<<cpu))&&cpu!=cpu_current_index())
                process_kick_cpu(cpu);
        return LIONOS_SIGNAL_OK;
    }
    if(signal==LIONOS_SIG_CONT){
        uint32_t wake_mask=continue_process_locked(p);
        spinlock_irqrestore_release(&process_lock,flags);
        for(uint32_t cpu=0u;cpu<LIONOS_MAX_CPUS;++cpu)
            if((wake_mask&(1u<<cpu))&&cpu!=cpu_current_index())
                process_kick_cpu(cpu);
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
    scheduler_execution_guard_leave(old,cpu);
    set_current_local(p);
    scheduler_execution_guard_enter(p,cpu);
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
    clear_process(p);p->pid=0;p->tid=next_tid++;if(next_tid==0)next_tid=1;p->thread_group_pid=parent->thread_group_pid;p->pgid=parent->pgid;p->sid=parent->sid;p->is_thread=1;p->owns_address_space=0;(void)process_transition_state_locked(p,PROCESS_STOPPED);p->cpu_owner=cpu_current_index();p->page_directory=parent->page_directory;p->entry=entry;p->user_stack=(uint32_t)(uintptr_t)sp;p->kernel_stack_top=(uint32_t)(uintptr_t)ks+4096u;p->thread_stack_page=(uint32_t)(uintptr_t)kp;p->thread_stack_va=va;p->capabilities=parent->capabilities;p->priority=parent->priority;p->base_priority=parent->base_priority;p->time_slice=scheduler_priority_quantum(p->priority);p->fpu_valid=0;
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
        if(process_transition_state_locked(c,PROCESS_WAITING)!=0){
            /* A failed transition must not leave a stale join registration. */
            c->join_tid=0u;
            c->join_status_ptr=0u;
            spinlock_irqrestore_release(&process_lock,flags);
            return -1;
        }
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
    uint32_t owner=LIONOS_MAX_CPUS;
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    int result=process_set_priority_locked(p,priority);
    if(result==0&&p->state==PROCESS_READY&&p->cpu_owner<LIONOS_MAX_CPUS)
        owner=p->cpu_owner;
    spinlock_irqrestore_release(&process_lock,flags);

    /* A priority promotion can change the next task selected by an idle or
       lower-priority owner CPU. Notify it only after queue metadata is stable
       and process_lock has been released. */
    if(result==0&&owner<LIONOS_MAX_CPUS)
        process_kick_cpu(owner);
    return result;
}

uint32_t process_scheduler_ticks(uint32_t cpu){return cpu<LIONOS_MAX_CPUS?scheduler_cpu[cpu].schedule_count:0u;}
uint32_t process_scheduler_steals(uint32_t cpu){
    return cpu<LIONOS_MAX_CPUS
        ? __atomic_load_n(&scheduler_cpu[cpu].steal_count,__ATOMIC_ACQUIRE)
        : 0u;
}
uint32_t process_scheduler_busy_balances(uint32_t cpu){
    return cpu<LIONOS_MAX_CPUS
        ? __atomic_load_n(&scheduler_cpu[cpu].busy_balance_count,__ATOMIC_ACQUIRE)
        : 0u;
}

int process_scheduler_execution_guard_enable(void){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    if(__atomic_load_n(&scheduler_execution_guard_enabled,__ATOMIC_ACQUIRE)!=0u){
        spinlock_irqrestore_release(&process_lock,flags);
        return -1;
    }
    __atomic_store_n(&scheduler_execution_guard_fault,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&scheduler_execution_guard_idle_steal_mask,0xFFFFFFFFu,
                     __ATOMIC_RELAXED);
    for(uint32_t i=0u;i<LIONOS_PROCESS_MAX;++i)
        __atomic_store_n(&scheduler_execution_cpu[i],0u,__ATOMIC_RELAXED);
    for(uint32_t cpu=0u;cpu<LIONOS_MAX_CPUS;++cpu){
        struct process *current=current_cpu_load(cpu);
        uint32_t index;
        if(process_slot_index(current,&index)!=0)continue;
        uint32_t expected=0u;
        if(!__atomic_compare_exchange_n(&scheduler_execution_cpu[index],
                &expected,cpu+1u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
            __atomic_store_n(&scheduler_execution_guard_fault,1u,__ATOMIC_RELEASE);
    }
    __atomic_store_n(&scheduler_execution_guard_enabled,1u,__ATOMIC_RELEASE);
    int result=__atomic_load_n(&scheduler_execution_guard_fault,__ATOMIC_ACQUIRE)
        ?-1:0;
    spinlock_irqrestore_release(&process_lock,flags);
    return result;
}

int process_scheduler_execution_guard_set_idle_steal_mask(uint32_t cpu_mask){
    uint32_t online=smp_online_count();
    if(online==0u||online>LIONOS_MAX_CPUS||online>=32u)return -1;
    uint32_t online_mask=(1u<<online)-1u;
    if(cpu_mask!=0xFFFFFFFFu&&(cpu_mask&~online_mask)!=0u)
        return -1;

    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    if(__atomic_load_n(&scheduler_execution_guard_enabled,
                       __ATOMIC_ACQUIRE)==0u){
        spinlock_irqrestore_release(&process_lock,flags);
        return -1;
    }
    __atomic_store_n(&scheduler_execution_guard_idle_steal_mask,cpu_mask,
                     __ATOMIC_RELEASE);
    spinlock_irqrestore_release(&process_lock,flags);
    return 0;
}

int process_scheduler_execution_guard_finish(void){
    uint32_t flags=spinlock_irqsave_acquire(&process_lock);
    int result=0;
    uint32_t bad_cpu=LIONOS_MAX_CPUS;
    uint32_t bad_state=0xFFFFFFFFu;
    uint32_t bad_owner=0xFFFFFFFFu;
    uint32_t online=smp_online_count();
    uint32_t expected[LIONOS_PROCESS_MAX]={0u};
    if(online==0u||online>LIONOS_MAX_CPUS)online=1u;
    if(__atomic_load_n(&scheduler_execution_guard_enabled,__ATOMIC_ACQUIRE)==0u)
        result=-1;
    uint32_t guard_fault=__atomic_load_n(
        &scheduler_execution_guard_fault,__ATOMIC_ACQUIRE);
    if(guard_fault!=0u){
        result=-1;
        debug_write("LIONOS:SMP-EXECUTION-GUARD-FAULT value=");
        process_scheduler_ap_probe_diag_hex(guard_fault);
        debug_write("\n");
    }

    /*
     * The execution token alone proves uniqueness, not that the current
     * context is runnable. Audit every online CPU's current pointer as well:
     * process contexts must be RUNNING and owned by that CPU; AP idle contexts
     * must be the CPU-local idle object and remain RUNNING.
     */
    for(uint32_t cpu=0u;cpu<LIONOS_MAX_CPUS;++cpu){
        struct process *current=current_cpu_load(cpu);
        if(cpu>=online){
            if(current){
                result=-1;
                if(bad_cpu==LIONOS_MAX_CPUS)bad_cpu=cpu;
            }
            continue;
        }
        if(!current){
            result=-1;
            if(bad_cpu==LIONOS_MAX_CPUS)bad_cpu=cpu;
            continue;
        }
        uint32_t index;
        if(process_slot_index(current,&index)==0){
            if(expected[index]!=0u){
                result=-1;
                if(bad_cpu==LIONOS_MAX_CPUS){
                    bad_cpu=cpu;
                    bad_state=current->state;
                    bad_owner=current->cpu_owner;
                }
            }else{
                expected[index]=cpu+1u;
            }
            if(current->state!=PROCESS_RUNNING||current->cpu_owner!=cpu){
                result=-1;
                if(bad_cpu==LIONOS_MAX_CPUS){
                    bad_cpu=cpu;
                    bad_state=current->state;
                    bad_owner=current->cpu_owner;
                }
            }
        }else if(cpu!=0u&&current==&scheduler_idle_processes[cpu]){
            if((scheduler_idle_ready_mask&(1u<<cpu))==0u||
               current->state!=PROCESS_RUNNING||current->cpu_owner!=cpu){
                result=-1;
                if(bad_cpu==LIONOS_MAX_CPUS){
                    bad_cpu=cpu;
                    bad_state=current->state;
                    bad_owner=current->cpu_owner;
                }
            }
        }else{
            result=-1;
            if(bad_cpu==LIONOS_MAX_CPUS)bad_cpu=cpu;
        }
    }
    for(uint32_t i=0u;i<LIONOS_PROCESS_MAX;++i){
        uint32_t actual=__atomic_load_n(&scheduler_execution_cpu[i],
                                        __ATOMIC_ACQUIRE);
        if(actual!=expected[i]){
            result=-1;
            debug_write("LIONOS:SMP-EXECUTION-GUARD-TOKEN-FAIL slot=");
            process_scheduler_ap_probe_diag_hex(i);
            debug_write(" pid=");
            process_scheduler_ap_probe_diag_hex(processes[i].pid);
            debug_write(" state=");
            process_scheduler_ap_probe_diag_hex(processes[i].state);
            debug_write(" owner=");
            process_scheduler_ap_probe_diag_hex(processes[i].cpu_owner);
            debug_write(" expected=");
            process_scheduler_ap_probe_diag_hex(expected[i]);
            debug_write(" actual=");
            process_scheduler_ap_probe_diag_hex(actual);
            debug_write("\n");
        }
    }
    __atomic_store_n(&scheduler_execution_guard_idle_steal_mask,0xFFFFFFFFu,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&scheduler_execution_guard_enabled,0u,__ATOMIC_RELEASE);
    for(uint32_t i=0u;i<LIONOS_PROCESS_MAX;++i)
        __atomic_store_n(&scheduler_execution_cpu[i],0u,__ATOMIC_RELAXED);
    spinlock_irqrestore_release(&process_lock,flags);

    if(bad_cpu!=LIONOS_MAX_CPUS){
        debug_write("LIONOS:SMP-EXECUTION-CURRENT-STATE-FAIL cpu=");
        process_scheduler_ap_probe_diag_hex(bad_cpu);
        debug_write(" state=");
        process_scheduler_ap_probe_diag_hex(bad_state);
        debug_write(" owner=");
        process_scheduler_ap_probe_diag_hex(bad_owner);
        debug_write("\n");
    }
    return result;
}

uint32_t *process_schedule(uint32_t *frame){
    process_scheduler_ap_probe_report();
    if(__atomic_load_n(&process_scheduler_ready,__ATOMIC_ACQUIRE)==0u)
        return frame;
    /*
     * Keep IRQ scheduling non-blocking: process state remains serialized by
     * process_lock. The timer path accumulates its tick before entering here,
     * so a failed try-lock defers quantum charging to a later successful
     * scheduling decision instead of discarding elapsed ticks or spinning
     * in interrupt context.
     */
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    uint32_t cpu=cpu_current_index();
    if(cpu>=LIONOS_MAX_CPUS)cpu=0u;
    struct process *contended_prev=current_local();
    if(cpu!=0u){
        uint32_t bit=1u<<cpu;
        uint32_t old=__atomic_fetch_or(&smp_ap_schedule_enter_seen,bit,
                                       __ATOMIC_ACQ_REL);
        if(!(old&bit)){
            static const char digits[]="0123456789ABCDEF";
            char marker[]="LIONOS:SMP-AP-SCHEDULE-ENTER cpu=00000000\n";
            const uint32_t prefix_length=
                (uint32_t)(sizeof("LIONOS:SMP-AP-SCHEDULE-ENTER cpu=")-1u);
            for(uint32_t i=0u;i<8u;++i)
                marker[prefix_length+i]=digits[(cpu>>(28u-i*4u))&0xFu];
            /* Emit one complete record; debug_write serializes each call. */
            debug_write(marker);
        }
    }
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
            scheduler_execution_guard_leave(contended_prev,cpu);
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
                    scheduler_execution_guard_enter(bootstrap,cpu);
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
    /*
     * Read the fault flag only while holding process_lock. Scheduler probes
     * deliberately inject temporary queue failures under this lock; checking
     * before acquiring it lets another CPU report that test fixture as a
     * production run-queue failure.
     */
    if(__atomic_load_n(&runqueue_fault,__ATOMIC_ACQUIRE)!=0u){
        uint32_t expected=0u;
        if(__atomic_compare_exchange_n(&runqueue_fault_reported,&expected,1u,0,
                __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
            debug_write("LIONOS:RUNQUEUE-FAULT\n");
    }
    __atomic_store_n(&reschedule_blocked[cpu],0u,__ATOMIC_RELEASE);
    __atomic_store_n(&reschedule_ipi_pending[cpu],0u,__ATOMIC_RELEASE);
    struct scheduler_cpu_state*sc=&scheduler_cpu[cpu];
    uint32_t elapsed_timer_ticks=__atomic_exchange_n(
        &sc->pending_schedule_ticks,0u,__ATOMIC_ACQ_REL);
    ++sc->schedule_count;
    uint32_t waitpid_wake_mask=0u;
    struct process*prev=current_local();
    scheduler_execution_guard_leave(prev,cpu);
    int prev_is_idle=scheduler_is_idle_context(prev);
    if(prev){
        __atomic_store_n(&prev->saved_frame,(uint32_t)(uintptr_t)frame,
                         __ATOMIC_RELEASE);
        if(prev->state==PROCESS_WAITING&&prev->wait_kind==3u&&frame)
            __atomic_store_n(&prev->wait_frame_saved,1u,__ATOMIC_RELEASE);
        waitpid_wake_mask=process_wake_interrupted_waitpid_locked();
        /* Reap adopted or explicitly deferred zombies only after no CPU
         * references them as its current execution context. */
    for(uint32_t i=1u;i<LIONOS_PROCESS_MAX;++i){
        struct process *zombie=&processes[i];
        if(zombie==prev||zombie->state!=PROCESS_ZOMBIE||
           (!zombie->adopted_by_init&&!zombie->reap_pending))continue;
        int current_on_cpu=0;
        for(uint32_t c=0u;c<LIONOS_MAX_CPUS;++c)
            if(current_cpu_load(c)==zombie){current_on_cpu=1;break;}
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
            /* Preserve the legacy one-charge behavior for scheduler entries
               that are not paired with process_futex_tick() (for example,
               the PIT fallback while the LAPIC frame timer is active). */
            uint32_t charge_ticks=elapsed_timer_ticks?elapsed_timer_ticks:1u;
            if(scheduler_tick_charge(&prev->ticks_used,&prev->priority,
                    &prev->time_slice,charge_ticks)!=0){
                __atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);
            }else if(prev->ticks_used!=0u){
                scheduler_execution_guard_enter(prev,cpu);
                spinlock_irqrestore_release(&process_lock,flags);
                process_kick_mask(waitpid_wake_mask,cpu);
                return frame;
            }
        } else {
            prev->ticks_used=0u;
        }
    }
    if(prev&&prev->deferred_kernel_stack){page_free((void *)(uintptr_t)(prev->deferred_kernel_stack-4096u));prev->deferred_kernel_stack=0;}

    struct process*next=0;
    uint32_t picked=LIONOS_RUNQUEUE_INVALID_INDEX;
    uint32_t prev_index=LIONOS_RUNQUEUE_INVALID_INDEX;
    int prev_enqueued=0;
    int steal_attempted=0;
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
                scheduler_execution_guard_enter(prev,cpu);
                spinlock_irqrestore_release(&process_lock,flags);
                process_kick_mask(waitpid_wake_mask,cpu);
                return frame;
            }
            prev->state=PROCESS_READY;
            prev_enqueued=1;
        }
        pick_result=runqueue_pick_next_locked(&runqueues[cpu],runqueue_nodes,
                                               LIONOS_PROCESS_MAX,&picked);
        if(pick_result==1&&!prev_enqueued){
            spinlock_release(&runqueues[cpu].lock);
            queue_lock_acquired=0;
            steal_attempted=1;
            int steal_result=process_try_steal_locked(cpu,&picked,1);
            if(steal_result>0)pick_result=0;
            else if(steal_result<0)pick_result=-1;
        }
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
                process_kick_mask(waitpid_wake_mask,cpu);
                for(;;)__asm__ volatile("cli; hlt");
            }
            prev->state=PROCESS_RUNNING;
            prev_enqueued=0;
        }
        if(queue_lock_acquired)spinlock_release(&runqueues[cpu].lock);
        if(pick_result<0||(pick_result==0&&picked>=LIONOS_PROCESS_MAX)||
           process_runqueue_validate_locked(cpu)!=0)
            __atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);
        /*
         * Once a local task has been selected, use the remaining queued
         * backlog for bounded balancing. The selected task stays RUNNING;
         * at most one eligible READY process is moved for this invocation.
         */
        if(next&&!steal_attempted){
            int balance_result=process_try_steal_locked(cpu,&picked,0);
            if(balance_result<0)
                __atomic_store_n(&runqueue_fault,1u,__ATOMIC_RELEASE);
        }
    }else{
        /*
         * Do not return a blocked task's frame merely because a local queue
         * lock is contended. Leave state unchanged and retry next tick; the
         * common idle fallback below handles blocked/stopped/zombie contexts.
         */
        /* Lock contention is transient, not a run-queue invariant failure. */
        __atomic_store_n(&reschedule_requested[cpu],1u,__ATOMIC_RELEASE);
    }
    /*
     * Idle stealing and busy-queue balancing are both serialized by
     * process_lock. Transfers use ascending queue-lock order and exclude
     * current tasks, kernel threads, live thread groups, and already-migrated
     * processes. A busy destination keeps its selected task running.
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
        scheduler_execution_guard_enter(next,cpu);
        spinlock_irqrestore_release(&process_lock,flags);
        process_kick_mask(waitpid_wake_mask,cpu);
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
            scheduler_execution_guard_enter(b,cpu);
            spinlock_irqrestore_release(&process_lock,flags);
            process_kick_mask(waitpid_wake_mask,cpu);
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
            process_kick_mask(waitpid_wake_mask,cpu);
            return idle_frame?idle_frame:frame;
        }
        set_current_local(0);

        spinlock_irqrestore_release(&process_lock,flags);
        process_kick_mask(waitpid_wake_mask,cpu);
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
            process_kick_mask(waitpid_wake_mask,cpu);
            return idle_frame?idle_frame:frame;
        }
    }
    if(prev_is_idle&&prev->state==PROCESS_RUNNING){
        spinlock_irqrestore_release(&process_lock,flags);
        process_kick_mask(waitpid_wake_mask,cpu);
        return frame;
    }
    if(!prev&&cpu==0){
        set_current_local(&processes[0]);
        processes[0].cpu_owner=0;
        processes[0].state=PROCESS_RUNNING;
        paging_switch_address_space(processes[0].page_directory);
        tss_set_kernel_stack(processes[0].kernel_stack_top);
    }
    scheduler_execution_guard_enter(
        prev?prev:(cpu==0u?&processes[0]:0),cpu);
    spinlock_irqrestore_release(&process_lock,flags);
    process_kick_mask(waitpid_wake_mask,cpu);
    return frame;
}
void scheduler_idle(void){for(;;)__asm__ volatile("sti; hlt");}
