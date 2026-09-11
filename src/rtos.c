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

#define RTOS_STACK_CANARY_SIZE 16
#define RTOS_STACK_PATTERN 0xA5
#define RTOS_STACK_CANARY 0xC3

typedef enum {
    WAIT_NONE,
    WAIT_MUTEX,
    WAIT_SEM,
    WAIT_QUEUE_SEND,
    WAIT_QUEUE_RECEIVE
} wait_kind_t;

typedef struct {
    task_info_t   info;
    ucontext_t    ctx;
    char         *stack;
    size_t        stack_size;
    task_func_t   func;
    void         *arg;
    int            base_priority;
    task_state_t   suspended_state;
    wait_kind_t    wait_kind;
    void          *wait_object;
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
static FILE             *g_trace_stream = NULL;

/* Simple ready queue: array of task indices per priority level, FIFO. */
static int   g_ready_q[RTOS_MAX_PRIORITIES][RTOS_MAX_TASKS];
static int   g_ready_head[RTOS_MAX_PRIORITIES];
static int   g_ready_tail[RTOS_MAX_PRIORITIES];
static int   g_ready_count[RTOS_MAX_PRIORITIES];

static void ready_push(int prio, int idx);
static void trace_event(const char *event, int idx);

static void remove_waiter(int idx)
{
    tcb_t *task = &g_tasks[idx];
    int *waiters = NULL;
    size_t *count = NULL;

    if (task->wait_kind == WAIT_MUTEX) {
        rtos_mutex_t *mutex = task->wait_object;
        waiters = mutex->waiters;
        count = &mutex->waiter_count;
    } else if (task->wait_kind == WAIT_SEM) {
        rtos_sem_t *sem = task->wait_object;
        waiters = sem->waiters;
        count = &sem->waiter_count;
    } else if (task->wait_kind == WAIT_QUEUE_SEND) {
        rtos_queue_t *queue = task->wait_object;
        waiters = queue->send_waiters;
        count = &queue->send_waiter_count;
    } else if (task->wait_kind == WAIT_QUEUE_RECEIVE) {
        rtos_queue_t *queue = task->wait_object;
        waiters = queue->receive_waiters;
        count = &queue->receive_waiter_count;
    }

    if (waiters && count) {
        for (size_t i = 0; i < *count; i++) {
            if (waiters[i] == idx) {
                memmove(&waiters[i], &waiters[i + 1],
                        (*count - i - 1) * sizeof(waiters[0]));
                (*count)--;
                break;
            }
        }
    }
    task->wait_kind = WAIT_NONE;
    task->wait_object = NULL;
}

static int add_waiter(int idx, wait_kind_t kind, void *object,
                      int *waiters, size_t *count)
{
    if (!waiters || !count || *count >= RTOS_MAX_TASKS) return RTOS_ERR_FULL;
    waiters[(*count)++] = idx;
    g_tasks[idx].wait_kind = kind;
    g_tasks[idx].wait_object = object;
    return RTOS_OK;
}

static void wake_one(int *waiters, size_t *count)
{
    if (!waiters || !count || *count == 0) return;
    size_t selected = 0;
    for (size_t i = 1; i < *count; i++) {
        if (g_tasks[waiters[i]].info.priority <
            g_tasks[waiters[selected]].info.priority) {
            selected = i;
        }
    }
    int idx = waiters[selected];
    remove_waiter(idx);
    if (g_tasks[idx].info.state == TASK_BLOCKED) {
        g_tasks[idx].info.state = TASK_READY;
        ready_push(g_tasks[idx].info.priority, idx);
        trace_event("wake", idx);
    }
}

static void trace_event(const char *event, int idx)
{
    if (!g_trace_stream) return;
    if (idx >= 0 && idx < g_task_count) {
        fprintf(g_trace_stream, "%u,%s,%d,%s\n", g_tick_count, event,
                idx, g_tasks[idx].info.name);
    } else {
        fprintf(g_trace_stream, "%u,%s,-1,scheduler\n", g_tick_count, event);
    }
    fflush(g_trace_stream);
}

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
        while (g_ready_count[p] > 0) {
            int idx = ready_pop(p);
            if (g_tasks[idx].info.state == TASK_READY) return idx;
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
            remove_waiter(i);
            g_tasks[i].info.state = TASK_READY;
            ready_push(g_tasks[i].info.priority, i);
            trace_event("wake", i);
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
    g_trace_stream = NULL;

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
    t->base_priority = priority;
    t->suspended_state = TASK_READY;
    t->func = func;
    t->arg = arg;

    t->stack_size = stack_size;
    t->stack = malloc(stack_size);
    if (!t->stack) { g_task_count--; return -1; }

    memset(t->stack, RTOS_STACK_PATTERN, stack_size);
    if (stack_size >= RTOS_STACK_CANARY_SIZE) {
        memset(t->stack, RTOS_STACK_CANARY, RTOS_STACK_CANARY_SIZE);
    }
    getcontext(&t->ctx);
    t->ctx.uc_stack.ss_sp = t->stack;
    t->ctx.uc_stack.ss_size = stack_size;
    t->ctx.uc_link = &g_sched_ctx; /* return to scheduler when task fn returns */
    makecontext(&t->ctx, task_trampoline, 0);

    ready_push(priority, idx);
    trace_event("create", idx);
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
    trace_event("switch", idx);
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
    trace_event("yield", idx);
    swapcontext(&g_tasks[idx].ctx, &g_sched_ctx);
}

void task_delay(uint32_t ticks)
{
    int idx = g_current;
    if (idx == -1) return;
    g_tasks[idx].info.state = TASK_BLOCKED;
    g_tasks[idx].info.wake_tick = g_tick_count + ticks;
    trace_event("block", idx);
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
    trace_event("exit", idx);
    swapcontext(&g_tasks[idx].ctx, &g_sched_ctx);
}

int task_self(void)
{
    return g_current;
}

static int valid_task_id(int task_id)
{
    return task_id >= 0 && task_id < g_task_count &&
           g_tasks[task_id].info.state != TASK_UNUSED;
}

int task_suspend(int task_id)
{
    if (!valid_task_id(task_id) || task_id == g_current) return RTOS_ERR_INVALID;
    tcb_t *task = &g_tasks[task_id];
    if (task->info.state != TASK_READY && task->info.state != TASK_BLOCKED) {
        return RTOS_ERR_BUSY;
    }
    if (task->wait_kind != WAIT_NONE) return RTOS_ERR_BUSY;
    task->suspended_state = task->info.state;
    task->info.state = TASK_SUSPENDED;
    trace_event("suspend", task_id);
    return RTOS_OK;
}

int task_resume(int task_id)
{
    if (!valid_task_id(task_id)) return RTOS_ERR_INVALID;
    tcb_t *task = &g_tasks[task_id];
    if (task->info.state != TASK_SUSPENDED) return RTOS_ERR_BUSY;
    task->info.state = task->suspended_state;
    if (task->info.state == TASK_READY) ready_push(task->info.priority, task_id);
    trace_event("resume", task_id);
    return RTOS_OK;
}

int task_delete(int task_id)
{
    if (!valid_task_id(task_id) || task_id == g_current) return RTOS_ERR_INVALID;
    tcb_t *task = &g_tasks[task_id];
    if (task->info.state == TASK_TERMINATED) return RTOS_ERR_BUSY;
    if (task->wait_kind != WAIT_NONE) remove_waiter(task_id);
    free(task->stack);
    task->stack = NULL;
    task->info.state = TASK_TERMINATED;
    trace_event("delete", task_id);
    return RTOS_OK;
}

int task_set_priority(int task_id, int priority)
{
    if (!valid_task_id(task_id)) return RTOS_ERR_INVALID;
    if (priority < 0) priority = 0;
    if (priority >= RTOS_MAX_PRIORITIES) priority = RTOS_MAX_PRIORITIES - 1;
    tcb_t *task = &g_tasks[task_id];
    task->base_priority = priority;
    task->info.priority = priority;
    if (task->info.state == TASK_READY) ready_push(priority, task_id);
    return RTOS_OK;
}

int task_get_info(int task_id, task_info_t *out)
{
    if (!valid_task_id(task_id) || !out) return RTOS_ERR_INVALID;
    *out = g_tasks[task_id].info;
    return RTOS_OK;
}

int task_stack_check(int task_id)
{
    if (!valid_task_id(task_id) || !g_tasks[task_id].stack ||
        g_tasks[task_id].stack_size < RTOS_STACK_CANARY_SIZE) {
        return RTOS_ERR_INVALID;
    }
    for (size_t i = 0; i < RTOS_STACK_CANARY_SIZE; i++) {
        if ((unsigned char)g_tasks[task_id].stack[i] != RTOS_STACK_CANARY) {
            return RTOS_ERR_INVALID;
        }
    }
    return RTOS_OK;
}

size_t task_stack_used(int task_id)
{
    if (!valid_task_id(task_id) || !g_tasks[task_id].stack ||
        g_tasks[task_id].stack_size <= RTOS_STACK_CANARY_SIZE) return 0;
    size_t used_start = RTOS_STACK_CANARY_SIZE;
    while (used_start < g_tasks[task_id].stack_size &&
           (unsigned char)g_tasks[task_id].stack[used_start] == RTOS_STACK_PATTERN) {
        used_start++;
    }
    return g_tasks[task_id].stack_size - used_start;
}

static int wait_for_condition(uint32_t start_tick, uint32_t timeout_ticks)
{
    if (timeout_ticks == 0) return 0;
    if ((uint32_t)(g_tick_count - start_tick) >= timeout_ticks) return 0;
    task_delay(1);
    return 1;
}

static int block_on_wait(int idx, wait_kind_t kind, void *object,
                         int *waiters, size_t *count, uint32_t deadline)
{
    if (add_waiter(idx, kind, object, waiters, count) != RTOS_OK) {
        return RTOS_ERR_FULL;
    }
    g_tasks[idx].info.state = TASK_BLOCKED;
    g_tasks[idx].info.wake_tick = deadline;
    trace_event("block", idx);
    swapcontext(&g_tasks[idx].ctx, &g_sched_ctx);
    return RTOS_OK;
}

void rtos_mutex_init(rtos_mutex_t *mutex)
{
    if (!mutex) return;
    memset(mutex->waiters, 0, sizeof(mutex->waiters));
    mutex->owner = -1;
    mutex->recursion = 0;
    mutex->owner_base_priority = -1;
    mutex->waiter_count = 0;
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
            mutex->owner_base_priority = g_tasks[g_current].base_priority;
            return RTOS_OK;
        }
        if (g_tasks[mutex->owner].info.priority > g_tasks[g_current].info.priority) {
            g_tasks[mutex->owner].info.priority = g_tasks[g_current].info.priority;
            if (g_tasks[mutex->owner].info.state == TASK_READY) {
                ready_push(g_tasks[mutex->owner].info.priority, mutex->owner);
            }
            trace_event("priority_boost", mutex->owner);
        }
        if (timeout_ticks == 0 ||
            (uint32_t)(g_tick_count - start) >= timeout_ticks) {
            return RTOS_ERR_TIMEOUT;
        }
        if (block_on_wait(g_current, WAIT_MUTEX, mutex, mutex->waiters,
                          &mutex->waiter_count, start + timeout_ticks) != RTOS_OK) {
            return RTOS_ERR_FULL;
        }
    }
}

int rtos_mutex_unlock(rtos_mutex_t *mutex)
{
    if (!mutex || g_current == -1) return RTOS_ERR_INVALID;
    if (mutex->owner != g_current) return RTOS_ERR_OWNER;
    if (--mutex->recursion == 0) {
        int owner = mutex->owner;
        mutex->owner = -1;
        g_tasks[owner].info.priority = mutex->owner_base_priority;
        mutex->owner_base_priority = -1;
        wake_one(mutex->waiters, &mutex->waiter_count);
        trace_event("mutex_unlock", owner);
    }
    return RTOS_OK;
}

void rtos_sem_init(rtos_sem_t *sem, int initial_count, int maximum)
{
    if (!sem) return;
    memset(sem->waiters, 0, sizeof(sem->waiters));
    if (maximum < 1) maximum = 1;
    if (initial_count < 0) initial_count = 0;
    if (initial_count > maximum) initial_count = maximum;
    sem->count = initial_count;
    sem->maximum = maximum;
    sem->waiter_count = 0;
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
        if (timeout_ticks == 0 ||
            (uint32_t)(g_tick_count - start) >= timeout_ticks) {
            return RTOS_ERR_TIMEOUT;
        }
        if (block_on_wait(g_current, WAIT_SEM, sem, sem->waiters,
                          &sem->waiter_count, start + timeout_ticks) != RTOS_OK) {
            return RTOS_ERR_FULL;
        }
    }
}

int rtos_sem_give(rtos_sem_t *sem)
{
    if (!sem || sem->maximum < 1) return RTOS_ERR_INVALID;
    if (sem->count >= sem->maximum) return RTOS_ERR_FULL;
    sem->count++;
    wake_one(sem->waiters, &sem->waiter_count);
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
    memset(queue->send_waiters, 0, sizeof(queue->send_waiters));
    memset(queue->receive_waiters, 0, sizeof(queue->receive_waiters));
    queue->send_waiter_count = 0;
    queue->receive_waiter_count = 0;
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
            wake_one(queue->receive_waiters, &queue->receive_waiter_count);
            return RTOS_OK;
        }
        if (timeout_ticks == 0 ||
            (uint32_t)(g_tick_count - start) >= timeout_ticks) {
            return RTOS_ERR_TIMEOUT;
        }
        if (block_on_wait(g_current, WAIT_QUEUE_SEND, queue,
                          queue->send_waiters, &queue->send_waiter_count,
                          start + timeout_ticks) != RTOS_OK) {
            return RTOS_ERR_FULL;
        }
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
            wake_one(queue->send_waiters, &queue->send_waiter_count);
            return RTOS_OK;
        }
        if (timeout_ticks == 0 ||
            (uint32_t)(g_tick_count - start) >= timeout_ticks) {
            return RTOS_ERR_TIMEOUT;
        }
        if (block_on_wait(g_current, WAIT_QUEUE_RECEIVE, queue,
                          queue->receive_waiters, &queue->receive_waiter_count,
                          start + timeout_ticks) != RTOS_OK) {
            return RTOS_ERR_FULL;
        }
    }
}

size_t rtos_queue_count(const rtos_queue_t *queue)
{
    return queue ? queue->count : 0;
}

void rtos_event_init(rtos_event_t *event)
{
    if (event) event->bits = 0;
}

int rtos_event_set(rtos_event_t *event, uint32_t bits)
{
    if (!event || bits == 0) return RTOS_ERR_INVALID;
    event->bits |= bits;
    return RTOS_OK;
}

int rtos_event_clear(rtos_event_t *event, uint32_t bits)
{
    if (!event || bits == 0) return RTOS_ERR_INVALID;
    event->bits &= ~bits;
    return RTOS_OK;
}

int rtos_event_wait(rtos_event_t *event, uint32_t bits,
                    int wait_all, uint32_t timeout_ticks)
{
    if (!event || bits == 0 || g_current == -1) return RTOS_ERR_INVALID;
    uint32_t start = g_tick_count;
    for (;;) {
        uint32_t matched = event->bits & bits;
        if ((wait_all && matched == bits) || (!wait_all && matched != 0)) {
            event->bits &= ~matched;
            return RTOS_OK;
        }
        if (!wait_for_condition(start, timeout_ticks)) return RTOS_ERR_TIMEOUT;
    }
}

void rtos_trace_enable(FILE *stream)
{
    g_trace_stream = stream;
    if (g_trace_stream) {
        fprintf(g_trace_stream, "tick,event,task,name\n");
        fflush(g_trace_stream);
    }
}

void rtos_trace_disable(void)
{
    g_trace_stream = NULL;
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
