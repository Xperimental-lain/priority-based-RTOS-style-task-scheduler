/*
 * demo_c.c — Three tasks at different priorities.
 *
 * - "high" (priority 0): a short high-priority task that runs a few
 *   times then exits, demonstrating that it always preempts/precedes
 *   lower-priority work.
 * - "worker" (priority 2): does a chunk of busy "work" then yields,
 *   round-robining with other same-priority tasks.
 * - "periodic" (priority 1): uses task_delay() to simulate a
 *   periodic sensor-poll style task.
 */

#define _DEFAULT_SOURCE
#include "rtos.h"
#include <stdio.h>
#include <unistd.h>

static void high_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 3; i++) {
        printf("[tick %3u] HIGH   task running (iter %d)\n", rtos_get_tick(), i);
        usleep(2000); /* simulate a little work */
        task_yield();
    }
    printf("[tick %3u] HIGH   task done\n", rtos_get_tick());
}

static void worker_task(void *arg)
{
    int id = *(int *)arg;
    for (int i = 0; i < 4; i++) {
        printf("[tick %3u] WORKER-%d running (iter %d)\n", rtos_get_tick(), id, i);
        /* simulate CPU-bound work long enough that the timer tick
         * may preempt it mid-loop */
        for (volatile long j = 0; j < 20000000L; j++) { }
        task_yield();
    }
    printf("[tick %3u] WORKER-%d done\n", rtos_get_tick(), id);
}

static void periodic_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 3; i++) {
        printf("[tick %3u] PERIODIC poll #%d\n", rtos_get_tick(), i);
        task_delay(20); /* sleep ~20 ticks before polling again */
    }
    printf("[tick %3u] PERIODIC done\n", rtos_get_tick());
}

int main(void)
{
    rtos_init(5 /* ms per tick */);

    static int w1 = 1, w2 = 2;
    task_create("high",     high_task,     NULL, 0, 0);
    task_create("periodic", periodic_task, NULL, 1, 0);
    task_create("worker1",  worker_task,   &w1,  2, 0);
    task_create("worker2",  worker_task,   &w2,  2, 0);

    printf("=== starting scheduler ===\n");
    rtos_run();
    printf("=== all tasks finished at tick %u ===\n", rtos_get_tick());
    return 0;
}
