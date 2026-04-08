// k64_sched.h – preemptive scheduler
#pragma once
#include <stdint.h>

typedef enum {
    K64_TASK_STATE_UNUSED = 0,
    K64_TASK_STATE_READY  = 1,
    K64_TASK_STATE_RUNNING= 2,
    K64_TASK_STATE_BLOCKED= 3,
    K64_TASK_STATE_ZOMBIE = 4,
} k64_task_state_t;

typedef struct k64_task {
    uint64_t         id;
    uint64_t         rsp;
    k64_task_state_t state;
    int              priority;
    uint32_t         base_timeslice;
    uint32_t         remaining_ticks;
    uint64_t         runtime_ticks;
    uint64_t         wait_ticks;
    struct k64_task* next;
} k64_task_t;

void        k64_sched_init(void);
k64_task_t* k64_task_create(void (*entry)(void));
k64_task_t* k64_task_create_ex(void (*entry)(void), int priority);
uint64_t    k64_sched_handle_timer(uint64_t old_rsp);
void        k64_sched_yield(void);
k64_task_t* k64_sched_current_task(void);
void        k64_sched_dump_stats(void);
