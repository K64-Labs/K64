// k64_sched.c
#include "k64_sched.h"
#include "k64_pmm.h"
#include "k64_log.h"
#include "k64_pit.h"
#include "k64_pic.h"
#include "k64_terminal.h"

#define K64_TASK_STACK_SIZE 4096
#define K64_DEFAULT_TIMESLICE 2U
#define K64_MAX_PRIORITY 5

static k64_task_t* current_task = NULL;
static k64_task_t* task_list    = NULL;
static uint64_t    next_task_id = 1;

static void k64_task_trampoline(void (*entry)(void));
static void on_tick_accounting(void);

static uint32_t timeslice_for_priority(int priority) {
    if (priority < 0) {
        priority = 0;
    }
    if (priority > K64_MAX_PRIORITY) {
        priority = K64_MAX_PRIORITY;
    }
    return K64_DEFAULT_TIMESLICE + (uint32_t)priority;
}

static int build_initial_stack(k64_task_t* t, void (*entry)(void)) {
    void* stack_base = k64_pmm_alloc_frame();
    if (!stack_base) {
        K64_LOG_ERROR("Scheduler: failed to allocate stack frame for task.");
        return 0;
    }

    uint64_t* sp = (uint64_t*)((uintptr_t)stack_base + K64_TASK_STACK_SIZE);

    uint64_t rflags = (1ULL << 9); // IF=1
    *--sp = rflags;
    *--sp = 0x08;
    *--sp = (uint64_t)k64_task_trampoline;

    *--sp = 0; // RAX
    *--sp = 0; // RCX
    *--sp = 0; // RDX
    *--sp = 0; // RBX
    *--sp = 0; // RBP
    *--sp = 0; // RSI
    *--sp = (uint64_t)entry; // RDI -> task entry for SysV ABI
    *--sp = 0; // R8
    *--sp = 0; // R9
    *--sp = 0; // R10
    *--sp = 0; // R11
    *--sp = 0; // R12
    *--sp = 0; // R13
    *--sp = 0; // R14
    *--sp = 0; // R15

    t->rsp = (uint64_t)sp;
    return 1;
}

static void on_tick_accounting(void) {
    if (!task_list) {
        return;
    }

    k64_task_t* task = task_list;
    do {
        if (task == current_task && task->state == K64_TASK_STATE_RUNNING) {
            task->runtime_ticks++;
        } else if (task->state == K64_TASK_STATE_READY) {
            task->wait_ticks++;
        }
        task = task->next;
    } while (task && task != task_list);
}

__attribute__((noreturn))
static void task_entry_trampoline(void (*entry)(void)) {
    entry();
    K64_LOG_INFO("Task finished, entering idle loop.");
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

__attribute__((noreturn))
static void k64_task_trampoline(void (*entry)(void)) {
    task_entry_trampoline(entry);
}

void k64_sched_init(void) {
    static k64_task_t bootstrap;
    bootstrap.id              = 0;
    bootstrap.rsp             = 0;
    bootstrap.state           = K64_TASK_STATE_RUNNING;
    bootstrap.priority        = 0;
    bootstrap.base_timeslice  = K64_DEFAULT_TIMESLICE;
    bootstrap.remaining_ticks = K64_DEFAULT_TIMESLICE;
    bootstrap.runtime_ticks   = 0;
    bootstrap.wait_ticks      = 0;
    bootstrap.next            = &bootstrap;

    current_task = &bootstrap;
    task_list    = &bootstrap;

    K64_LOG_INFO("Scheduler initialized with bootstrap task.");
}

k64_task_t* k64_task_create_ex(void (*entry)(void), int priority) {
    k64_task_t* t = (k64_task_t*)k64_pmm_alloc_frame();
    if (!t) {
        K64_LOG_ERROR("Scheduler: failed to allocate TCB.");
        return NULL;
    }

    t->id             = next_task_id++;
    t->state          = K64_TASK_STATE_READY;
    if (priority < 0) {
        priority = 0;
    }
    if (priority > K64_MAX_PRIORITY) {
        priority = K64_MAX_PRIORITY;
    }

    t->priority       = priority;
    t->base_timeslice = timeslice_for_priority(priority);
    t->remaining_ticks = t->base_timeslice;
    t->runtime_ticks   = 0;
    t->wait_ticks      = 0;
    t->next           = NULL;
    t->rsp            = 0;

    if (!build_initial_stack(t, entry)) {
        k64_pmm_free_frame(t);
        return NULL;
    }

    t->next = current_task->next;
    current_task->next = t;

    K64_LOG_INFO("New kernel task created.");
    return t;
}

k64_task_t* k64_task_create(void (*entry)(void)) {
    return k64_task_create_ex(entry, 1);
}

static uint64_t pick_next_rsp(void) {
    if (!current_task || !current_task->next) {
        return current_task ? current_task->rsp : 0;
    }

    k64_task_t* candidate = current_task;
    for (;;) {
        candidate = candidate->next;
        if (candidate->state == K64_TASK_STATE_READY) {
            if (current_task->state == K64_TASK_STATE_RUNNING) {
                current_task->state = K64_TASK_STATE_READY;
            }
            candidate->state = K64_TASK_STATE_RUNNING;
            candidate->remaining_ticks = candidate->base_timeslice;
            current_task = candidate;
            break;
        }
        if (candidate == current_task) {
            break;
        }
    }

    return current_task->rsp;
}

uint64_t k64_sched_handle_timer(uint64_t old_rsp) {
    if (current_task) {
        current_task->rsp = old_rsp;
    }

    k64_pit_on_tick();
    on_tick_accounting();
    k64_pic_send_eoi(0);

    if (!current_task || current_task->state != K64_TASK_STATE_RUNNING) {
        return pick_next_rsp();
    }

    if (current_task->id == 0) {
        return pick_next_rsp();
    }

    if (current_task->remaining_ticks > 0) {
        current_task->remaining_ticks--;
    }

    if (current_task->remaining_ticks == 0) {
        return pick_next_rsp();
    }

    return current_task->rsp;
}

void k64_sched_yield(void) {
    if (current_task) {
        current_task->remaining_ticks = 0;
    }
}

k64_task_t* k64_sched_current_task(void) {
    return current_task;
}

void k64_sched_dump_stats(void) {
    if (!task_list) {
        return;
    }

    k64_term_write("Scheduler stats:\n");
    k64_task_t* t = task_list;
    do {
        k64_term_write("  task=");
        k64_term_write_dec(t->id);
        k64_term_write(" prio=");
        k64_term_write_dec((uint64_t)t->priority);
        k64_term_write(" run=");
        k64_term_write_dec(t->runtime_ticks);
        k64_term_write(" wait=");
        k64_term_write_dec(t->wait_ticks);
        k64_term_putc('\n');
        t = t->next;
    } while (t && t != task_list);
}
