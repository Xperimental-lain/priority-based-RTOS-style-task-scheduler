/*
 * rtos.c — Core scheduler implementation.
 *
 * Design:
 *   - Each task has its own ucontext_t + private stack.
 *   - RTOS_MAX_PRIORITIES ready queues (0 = highest priority).
 *     The scheduler always picks the highest non-empty priority
 *     queue and round-robins within it.
 *   - A SIGALRM signal, fired periodically via setitimer(), acts as
 *     the "timer interrupt". Its handler performs the context switch,
 *     simulating preemptive scheduling: a running task can be cut
 *     off mid-execution and resumed later, just like on real hardware.
 *   - task_delay() moves a task to BLOCKED with a wake_tick; every
 *     tick the scheduler scans blocked tasks and wakes due ones.
 *
 * Caveat: swapcontext() is not officially async-signal-safe, but in
 * practice (glibc/Linux) this pattern is a well-known and widely used
 * technique for teaching/simulating preemptive schedulers in
 * userspace. Do not ship this as a production kernel.
 */

/* Expose ucontext.h, sigaction, setitimer, etc. under strict -std=c11 */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700

#include "rtos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct {
    task_info_t   info;
    ucontext_t    ctx;
    char         *stack;
    size_t        stack_size;
    task_func_t   func;
    void         *arg;
} tcb_t;

/* --- Kernel state --- */
static tcb_t          g_tasks[RTOS_MAX_TASKS];
static int             g_task_count = 0;
static int             g_current   = -1;   /* index of running task */
static ucontext_t      g_sched_ctx;         /* scheduler's own context */
static volatile sig_atomic_t g_tick_pending = 0;
static uint32_t         g_tick_count = 0;
static volatile sig_atomic_t g_running = 0;
static unsigned          g_tick_ms = 10;

/* Simple ready queue: array of task indices per priority level, FIFO. */
static int   g_ready_q[RTOS_MAX_PRIORITIES][RTOS_MAX_TASKS];
static int   g_ready_head[RTOS_MAX_PRIORITIES];
static int   g_ready_tail[RTOS_MAX_PRIORITIES];
static int   g_ready_count[RTOS_MAX_PRIORITIES];

static void ready_push(int prio, int idx)
{
    int t = g_ready_tail[prio];
    g_ready_q[prio][t] = idx;
    g_ready_tail[prio] = (t + 1) % RTOS_MAX_TASKS;
    g_ready_count[prio]++;
}

static int ready_pop(int prio)
{
    int h = g_ready_head[prio];
    int idx = g_ready_q[prio][h];
    g_ready_head[prio] = (h + 1) % RTOS_MAX_TASKS;
    g_ready_count[prio]--;
    return idx;
}

static int pick_next(void)
{
    for (int p = 0; p < RTOS_MAX_PRIORITIES; p++) {
        if (g_ready_count[p] > 0) {
            return ready_pop(p);
        }
    }
    return -1; /* nothing ready */
}

static int all_tasks_terminated(void)
{
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].info.state != TASK_TERMINATED) return 0;
    }
    return 1;
}

static void wake_due_tasks(void)
{
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].info.state == TASK_BLOCKED &&
            g_tasks[i].info.wake_tick <= g_tick_count) {
            g_tasks[i].info.state = TASK_READY;
            ready_push(g_tasks[i].info.priority, i);
        }
    }
}

/* --- Timer interrupt (simulated) --- */

static void alarm_handler(int signo)
{
    (void)signo;
    g_tick_pending = 1;

    /* Real preemption: if a task is currently running, forcibly switch
     * back to the scheduler's context right here, mid-instruction-
     * stream, exactly like a hardware timer interrupt would trap into
     * the kernel. The task's full machine state (registers, stack
     * pointer, PC) is saved into its ucontext_t and it will resume
     * exactly here, transparently, whenever the scheduler runs it again. */
    if (g_current != -1) {
        int idx = g_current;
        swapcontext(&g_tasks[idx].ctx, &g_sched_ctx);
    }
}

static void arm_timer(void)
{
    struct itimerval it;
    long usec = (long)g_tick_ms * 1000;
    it.it_value.tv_sec  = usec / 1000000;
    it.it_value.tv_usec = usec % 1000000;
    it.it_interval = it.it_value;
    setitimer(ITIMER_REAL, &it, NULL);
}

static void disarm_timer(void)
{
    struct itimerval it;
    memset(&it, 0, sizeof(it));
    setitimer(ITIMER_REAL, &it, NULL);
}

/* Trampoline: ucontext's makecontext can only pass ints portably, so
 * we stash func/arg in the TCB and read them from g_current here. */
static void task_trampoline(void)
{
    tcb_t *t = &g_tasks[g_current];
    t->func(t->arg);
    task_exit();
    /* task_exit() never returns (switches away); unreachable. */
}

/* --- Public API --- */

void rtos_init(unsigned tick_ms)
{
    memset(g_tasks, 0, sizeof(g_tasks));
    memset(g_ready_head, 0, sizeof(g_ready_head));
    memset(g_ready_tail, 0, sizeof(g_ready_tail));
    memset(g_ready_count, 0, sizeof(g_ready_count));
    g_task_count = 0;
    g_current = -1;
    g_tick_count = 0;
    g_tick_pending = 0;
    g_running = 0;
    g_tick_ms = tick_ms ? tick_ms : 10;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = alarm_handler;
    sigemptyset(&sa.sa_mask);
    /* SA_NODEFER: do NOT auto-block SIGALRM during handler execution.
     * We swapcontext() out of the handler instead of returning from it
     * normally, so the kernel never gets a chance to unblock SIGALRM
     * via sigreturn on our behalf. Without SA_NODEFER, SIGALRM would
     * stay permanently blocked for a task after its first preemption. */
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGALRM, &sa, NULL);
}

int task_create(const char *name, task_func_t func, void *arg,
                 int priority, size_t stack_size)
{
    if (g_task_count >= RTOS_MAX_TASKS) return -1;
    if (!func) return -1;
    if (priority < 0) priority = 0;
    if (priority >= RTOS_MAX_PRIORITIES) priority = RTOS_MAX_PRIORITIES - 1;
    if (stack_size == 0) stack_size = RTOS_DEFAULT_STACK;

    int idx = g_task_count++;
    tcb_t *t = &g_tasks[idx];
    memset(t, 0, sizeof(*t));

    t->info.id = idx;
    strncpy(t->info.name, name ? name : "task", sizeof(t->info.name) - 1);
    t->info.state = TASK_READY;
    t->info.priority = priority;
    t->func = func;
    t->arg = arg;

    t->stack_size = stack_size;
    t->stack = malloc(stack_size);
    if (!t->stack) { g_task_count--; return -1; }

    getcontext(&t->ctx);
    t->ctx.uc_stack.ss_sp = t->stack;
    t->ctx.uc_stack.ss_size = stack_size;
    t->ctx.uc_link = &g_sched_ctx; /* return to scheduler when task fn returns */
    makecontext(&t->ctx, task_trampoline, 0);

    ready_push(priority, idx);
    return idx;
}

/* Always called with g_current == -1 (scheduler is "current"). Saves
 * the scheduler's context and jumps into task `idx`. Returns once that
 * task yields, delays, exits, or is preempted by the timer tick. */
static void switch_to(int idx)
{
    g_current = idx;
    g_tasks[idx].info.state = TASK_RUNNING;
    g_tasks[idx].info.run_count++;
    g_tasks[idx].info.switch_count++;
    swapcontext(&g_sched_ctx, &g_tasks[idx].ctx);
}

/* The scheduler loop itself runs on g_sched_ctx. Each time control
 * returns here (task blocked, yielded, was preempted, or exited),
 * we pick the next task and switch to it. */
static void scheduler_loop(void)
{
    while (g_running) {
        if (g_tick_pending) {
            g_tick_pending = 0;
            g_tick_count++;
            wake_due_tasks();
        }

        int next = pick_next();
        if (next == -1) {
            /* Nothing ready. Either every task has run to completion,
             * or everything is BLOCKED and just waiting on a future
             * tick to wake it up. */
            if (g_task_count == 0 || all_tasks_terminated()) break;
            pause();
            continue;
        }

        g_current = -1; /* force "from scheduler" path in switch_to */
        switch_to(next);
        /* Control returns here after the task yields/blocks/exits/
         * gets preempted. Re-queue it if it's still READY (i.e. was
         * preempted mid-run rather than voluntarily blocking). */
        if (g_current != -1) {
            tcb_t *t = &g_tasks[g_current];
            /* RUNNING here means it was preempted mid-execution by the
             * timer tick (state never changed). READY here means it
             * called task_yield() itself. Either way it goes back on
             * the ready queue. BLOCKED (task_delay) and TERMINATED
             * (task_exit) are left alone. */
            if (t->info.state == TASK_RUNNING || t->info.state == TASK_READY) {
                t->info.state = TASK_READY;
                ready_push(t->info.priority, g_current);
            }
            g_current = -1;
        }
    }
}

void rtos_run(void)
{
    g_running = 1;
    arm_timer();
    getcontext(&g_sched_ctx);
    /* We re-enter scheduler_loop every time we're switched back to. */
    scheduler_loop();
    disarm_timer();

    for (int i = 0; i < g_task_count; i++) {
        free(g_tasks[i].stack);
    }
}

void rtos_shutdown(void)
{
    g_running = 0;
}

void task_yield(void)
{
    int idx = g_current;
    if (idx == -1) return; /* not in a task context */
    g_tasks[idx].info.state = TASK_READY;
    swapcontext(&g_tasks[idx].ctx, &g_sched_ctx);
}

void task_delay(uint32_t ticks)
{
    int idx = g_current;
    if (idx == -1) return;
    g_tasks[idx].info.state = TASK_BLOCKED;
    g_tasks[idx].info.wake_tick = g_tick_count + ticks;
    swapcontext(&g_tasks[idx].ctx, &g_sched_ctx);
}

void task_delay_until(uint32_t *previous_wake, uint32_t period_ticks)
{
    if (!previous_wake || period_ticks == 0) {
        task_yield();
        return;
    }

    uint32_t now = g_tick_count;
    uint32_t next = *previous_wake + period_ticks;
    *previous_wake = next;

    if ((int32_t)(next - now) > 0) {
        task_delay(next - now);
    } else {
        task_yield();
    }
}

void task_exit(void)
{
    int idx = g_current;
    if (idx == -1) return;
    g_tasks[idx].info.state = TASK_TERMINATED;
    swapcontext(&g_tasks[idx].ctx, &g_sched_ctx);
}

int task_self(void)
{
    return g_current;
}

static int wait_for_condition(uint32_t start_tick, uint32_t timeout_ticks)
{
    if (timeout_ticks == 0) return 0;
    if ((uint32_t)(g_tick_count - start_tick) >= timeout_ticks) return 0;
    task_delay(1);
    return 1;
}

void rtos_mutex_init(rtos_mutex_t *mutex)
{
    if (!mutex) return;
    mutex->owner = -1;
    mutex->recursion = 0;
}

int rtos_mutex_lock(rtos_mutex_t *mutex, uint32_t timeout_ticks)
{
    if (!mutex || g_current == -1) return RTOS_ERR_INVALID;
    if (mutex->owner == g_current) {
        mutex->recursion++;
        return RTOS_OK;
    }

    uint32_t start = g_tick_count;
    for (;;) {
        if (mutex->owner == -1) {
            mutex->owner = g_current;
            mutex->recursion = 1;
            return RTOS_OK;
        }
        if (!wait_for_condition(start, timeout_ticks)) return RTOS_ERR_TIMEOUT;
    }
}

int rtos_mutex_unlock(rtos_mutex_t *mutex)
{
    if (!mutex || g_current == -1) return RTOS_ERR_INVALID;
    if (mutex->owner != g_current) return RTOS_ERR_OWNER;
    if (--mutex->recursion == 0) mutex->owner = -1;
    return RTOS_OK;
}

void rtos_sem_init(rtos_sem_t *sem, int initial_count, int maximum)
{
    if (!sem) return;
    if (maximum < 1) maximum = 1;
    if (initial_count < 0) initial_count = 0;
    if (initial_count > maximum) initial_count = maximum;
    sem->count = initial_count;
    sem->maximum = maximum;
}

int rtos_sem_take(rtos_sem_t *sem, uint32_t timeout_ticks)
{
    if (!sem || g_current == -1 || sem->maximum < 1) return RTOS_ERR_INVALID;
    uint32_t start = g_tick_count;
    for (;;) {
        if (sem->count > 0) {
            sem->count--;
            return RTOS_OK;
        }
        if (!wait_for_condition(start, timeout_ticks)) return RTOS_ERR_TIMEOUT;
    }
}

int rtos_sem_give(rtos_sem_t *sem)
{
    if (!sem || sem->maximum < 1) return RTOS_ERR_INVALID;
    if (sem->count >= sem->maximum) return RTOS_ERR_FULL;
    sem->count++;
    return RTOS_OK;
}

int rtos_queue_init(rtos_queue_t *queue, void *buffer,
                    size_t item_size, size_t capacity)
{
    if (!queue || !buffer || item_size == 0 || capacity == 0) {
        return RTOS_ERR_INVALID;
    }
    queue->buffer = buffer;
    queue->item_size = item_size;
    queue->capacity = capacity;
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    return RTOS_OK;
}

int rtos_queue_send(rtos_queue_t *queue, const void *item,
                    uint32_t timeout_ticks)
{
    if (!queue || !queue->buffer || !item || queue->capacity == 0 ||
        g_current == -1) {
        return RTOS_ERR_INVALID;
    }
    uint32_t start = g_tick_count;
    for (;;) {
        if (queue->count < queue->capacity) {
            memcpy(queue->buffer + queue->tail * queue->item_size,
                   item, queue->item_size);
            queue->tail = (queue->tail + 1) % queue->capacity;
            queue->count++;
            return RTOS_OK;
        }
        if (!wait_for_condition(start, timeout_ticks)) return RTOS_ERR_TIMEOUT;
    }
}

int rtos_queue_receive(rtos_queue_t *queue, void *item,
                       uint32_t timeout_ticks)
{
    if (!queue || !queue->buffer || !item || queue->capacity == 0 ||
        g_current == -1) {
        return RTOS_ERR_INVALID;
    }
    uint32_t start = g_tick_count;
    for (;;) {
        if (queue->count > 0) {
            memcpy(item, queue->buffer + queue->head * queue->item_size,
                   queue->item_size);
            queue->head = (queue->head + 1) % queue->capacity;
            queue->count--;
            return RTOS_OK;
        }
        if (!wait_for_condition(start, timeout_ticks)) return RTOS_ERR_TIMEOUT;
    }
}

size_t rtos_queue_count(const rtos_queue_t *queue)
{
    return queue ? queue->count : 0;
}

int rtos_get_task_info(task_info_t *out, int max_tasks)
{
    if (!out || max_tasks <= 0) return 0;
    int n = g_task_count < max_tasks ? g_task_count : max_tasks;
    for (int i = 0; i < n; i++) out[i] = g_tasks[i].info;
    return n;
}

uint32_t rtos_get_tick(void)
{
    return g_tick_count;
}
