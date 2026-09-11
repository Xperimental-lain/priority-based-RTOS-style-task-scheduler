/*
 * rtos.h — Public API for a cooperative/preemptive priority-based
 *          task scheduler, simulated in userspace using ucontext.
 *
 * This mimics the core of a small RTOS kernel:
 *   - Fixed-size pool of Task Control Blocks (TCBs)
 *   - Multi-level priority ready queues (round-robin within a level)
 *   - Preemptive tick via SIGALRM (simulates a hardware timer interrupt)
 *   - task_yield() for cooperative yielding
 *   - task_delay() for simulated sleep (tick-based)
 *
 * NOTE: This is an educational simulation of RTOS scheduling concepts
 * running on top of Linux (ucontext + signals), not a bare-metal kernel.
 * The concepts (TCBs, ready queues, priority scheduling, preemption,
 * context switching) map directly onto real embedded RTOS design
 * (FreeRTOS, ChibiOS, Zephyr, etc.).
 */

#ifndef RTOS_H
#define RTOS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTOS_MAX_TASKS      16
#define RTOS_MAX_PRIORITIES 8      /* 0 = highest priority */
#define RTOS_DEFAULT_STACK  (64 * 1024)

typedef enum {
    RTOS_OK = 0,
    RTOS_ERR_INVALID = -1,
    RTOS_ERR_FULL = -2,
    RTOS_ERR_EMPTY = -3,
    RTOS_ERR_TIMEOUT = -4,
    RTOS_ERR_BUSY = -5,
    RTOS_ERR_OWNER = -6
} rtos_status_t;

typedef void (*task_func_t)(void *arg);

typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,   /* delayed / sleeping */
    TASK_SUSPENDED,
    TASK_TERMINATED
} task_state_t;

typedef struct {
    int          id;
    char         name[32];
    task_state_t state;
    int          priority;      /* 0 = highest */
    uint32_t     wake_tick;     /* tick at which a BLOCKED task becomes READY */
    uint32_t     run_count;     /* number of times scheduled, for stats */
    uint32_t     switch_count;  /* number of context switches into this task */
} task_info_t;

typedef struct {
    int owner;
    unsigned recursion;
    int owner_base_priority;
    int waiters[RTOS_MAX_TASKS];
    size_t waiter_count;
} rtos_mutex_t;

typedef struct {
    int count;
    int maximum;
    int waiters[RTOS_MAX_TASKS];
    size_t waiter_count;
} rtos_sem_t;

typedef struct {
    unsigned char *buffer;
    size_t item_size;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    int send_waiters[RTOS_MAX_TASKS];
    size_t send_waiter_count;
    int receive_waiters[RTOS_MAX_TASKS];
    size_t receive_waiter_count;
} rtos_queue_t;

typedef struct {
    uint32_t bits;
} rtos_event_t;

/* --- Kernel lifecycle --- */

/* Must be called once before creating tasks. tick_ms sets the
 * simulated timer-interrupt period (preemption granularity). */
void rtos_init(unsigned tick_ms);

/* Create a new task. Returns task id (>=0) or -1 on failure.
 * stack_size of 0 uses RTOS_DEFAULT_STACK. */
int task_create(const char *name, task_func_t func, void *arg,
                 int priority, size_t stack_size);

/* Starts the scheduler. Does not return until all tasks have
 * terminated (or rtos_shutdown() is called from a task). */
void rtos_run(void);

/* Called from within a task to stop the entire scheduler. */
void rtos_shutdown(void);

/* --- Task-facing API (call only from within a task function) --- */

/* Voluntarily give up the CPU; task stays READY. */
void task_yield(void);

/* Block the calling task for `ticks` scheduler ticks. */
void task_delay(uint32_t ticks);

/* Sleep until an absolute tick, avoiding drift in periodic tasks. */
void task_delay_until(uint32_t *previous_wake, uint32_t period_ticks);

/* Terminate the calling task. */
void task_exit(void);

/* Id of the currently running task. */
int  task_self(void);

/* Task lifecycle and diagnostics. Task deletion is for non-running tasks. */
int  task_suspend(int task_id);
int  task_resume(int task_id);
int  task_delete(int task_id);
int  task_set_priority(int task_id, int priority);
int  task_get_info(int task_id, task_info_t *out);
int  task_stack_check(int task_id);
size_t task_stack_used(int task_id);

/* --- Synchronization and IPC --- */

void rtos_mutex_init(rtos_mutex_t *mutex);
int  rtos_mutex_lock(rtos_mutex_t *mutex, uint32_t timeout_ticks);
int  rtos_mutex_unlock(rtos_mutex_t *mutex);

void rtos_sem_init(rtos_sem_t *sem, int initial_count, int maximum);
int  rtos_sem_take(rtos_sem_t *sem, uint32_t timeout_ticks);
int  rtos_sem_give(rtos_sem_t *sem);

/* Queue storage is supplied by the caller: buffer must hold capacity items. */
int  rtos_queue_init(rtos_queue_t *queue, void *buffer,
                     size_t item_size, size_t capacity);
int  rtos_queue_send(rtos_queue_t *queue, const void *item,
                     uint32_t timeout_ticks);
int  rtos_queue_receive(rtos_queue_t *queue, void *item,
                        uint32_t timeout_ticks);
size_t rtos_queue_count(const rtos_queue_t *queue);

/* Event bits provide lightweight task notifications. */
void rtos_event_init(rtos_event_t *event);
int  rtos_event_set(rtos_event_t *event, uint32_t bits);
int  rtos_event_clear(rtos_event_t *event, uint32_t bits);
int  rtos_event_wait(rtos_event_t *event, uint32_t bits,
                     int wait_all, uint32_t timeout_ticks);

/* Trace output is CSV-like: tick,event,task,name. */
void rtos_trace_enable(FILE *stream);
void rtos_trace_disable(void);

/* --- Introspection --- */

/* Fills `out` with info for up to max_tasks tasks; returns count. */
int rtos_get_task_info(task_info_t *out, int max_tasks);

/* Current scheduler tick count (monotonic, since rtos_init). */
uint32_t rtos_get_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_H */
