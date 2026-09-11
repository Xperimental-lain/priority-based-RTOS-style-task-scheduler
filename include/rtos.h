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

#ifdef __cplusplus
extern "C" {
#endif

#define RTOS_MAX_TASKS      16
#define RTOS_MAX_PRIORITIES 8      /* 0 = highest priority */
#define RTOS_DEFAULT_STACK  (64 * 1024)

typedef void (*task_func_t)(void *arg);

typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,   /* delayed / sleeping */
    TASK_TERMINATED
} task_state_t;

typedef struct {
    int          id;
    char         name[32];
    task_state_t state;
    int          priority;      /* 0 = highest */
    uint32_t     wake_tick;     /* tick at which a BLOCKED task becomes READY */
    uint32_t     run_count;     /* number of times scheduled, for stats */
} task_info_t;

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

/* Terminate the calling task. */
void task_exit(void);

/* Id of the currently running task. */
int  task_self(void);

/* --- Introspection --- */

/* Fills `out` with info for up to max_tasks tasks; returns count. */
int rtos_get_task_info(task_info_t *out, int max_tasks);

/* Current scheduler tick count (monotonic, since rtos_init). */
uint32_t rtos_get_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_H */
