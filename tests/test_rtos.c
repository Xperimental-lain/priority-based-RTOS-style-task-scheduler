#include "rtos.h"

#include <stdio.h>
#include <string.h>

static int failures;
static rtos_mutex_t test_mutex;
static rtos_sem_t test_sem;
static rtos_sem_t direct_sem;
static rtos_queue_t test_queue;
static rtos_event_t test_event;
static int queue_storage[2];
static int sleep_order[2];
static int sleep_order_count;

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL: %s\n", message); \
            failures++; \
        } \
    } while (0)

static void producer(void *arg)
{
    (void)arg;
    int value = 42;

    CHECK(rtos_mutex_lock(&test_mutex, 0) == RTOS_OK, "producer locks mutex");
    CHECK(rtos_mutex_lock(&test_mutex, 0) == RTOS_OK, "mutex is recursive");
    CHECK(rtos_queue_send(&test_queue, &value, 0) == RTOS_OK,
          "producer sends queue item");
    CHECK(rtos_mutex_unlock(&test_mutex) == RTOS_OK, "producer unlocks once");
    CHECK(rtos_mutex_unlock(&test_mutex) == RTOS_OK, "producer unlocks twice");
    CHECK(rtos_sem_give(&test_sem) == RTOS_OK, "producer gives semaphore");
}

static void consumer(void *arg)
{
    (void)arg;
    int value = 0;
    uint32_t wake = rtos_get_tick();

    CHECK(rtos_sem_take(&test_sem, 20) == RTOS_OK, "consumer takes semaphore");
    CHECK(rtos_queue_receive(&test_queue, &value, 20) == RTOS_OK,
          "consumer receives queue item");
    CHECK(value == 42, "consumer receives the expected value");
    task_delay_until(&wake, 2);
    CHECK(rtos_get_tick() >= wake, "periodic delay reaches its deadline");
}

static void timeout_task(void *arg)
{
    (void)arg;
    rtos_sem_t empty;

    rtos_sem_init(&empty, 0, 1);
    CHECK(rtos_sem_take(&empty, 2) == RTOS_ERR_TIMEOUT,
          "empty semaphore times out");
}

static void event_waiter(void *arg)
{
    (void)arg;
    CHECK(rtos_event_wait(&test_event, 0x04, 1, 20) == RTOS_OK,
          "event waiter receives all requested bits");
    CHECK(task_stack_check(task_self()) == RTOS_OK, "task stack canary is intact");
}

static void event_setter(void *arg)
{
    (void)arg;
    task_delay(2);
    CHECK(rtos_event_set(&test_event, 0x04) == RTOS_OK,
          "event setter signals the waiter");
}

static void suspended_task(void *arg)
{
    (void)arg;
}

static void direct_waiter(void *arg)
{
    (void)arg;
    CHECK(rtos_sem_take(&direct_sem, 20) == RTOS_OK,
          "blocked semaphore waiter wakes from signal");
}

static void direct_signaler(void *arg)
{
    (void)arg;
    task_delay(2);
    CHECK(rtos_sem_give(&direct_sem) == RTOS_OK,
          "semaphore signal wakes a waiter directly");
}

static void long_sleep_task(void *arg)
{
    (void)arg;
    task_delay(5);
    sleep_order[sleep_order_count++] = 1;
}

static void short_sleep_task(void *arg)
{
    (void)arg;
    task_delay(1);
    sleep_order[sleep_order_count++] = 2;
}

int main(void)
{
    rtos_init(1);
    rtos_mutex_init(&test_mutex);
    rtos_sem_init(&test_sem, 0, 1);
    rtos_sem_init(&direct_sem, 0, 1);
    rtos_event_init(&test_event);
    CHECK(rtos_queue_init(&test_queue, queue_storage, sizeof(queue_storage[0]), 2)
              == RTOS_OK,
          "queue initializes");

    CHECK(task_create("producer", producer, NULL, 1, 0) >= 0,
          "producer task creates");
    CHECK(task_create("consumer", consumer, NULL, 1, 0) >= 0,
          "consumer task creates");
    CHECK(task_create("timeout", timeout_task, NULL, 2, 0) >= 0,
          "timeout task creates");
        CHECK(task_create("event-waiter", event_waiter, NULL, 0, 0) >= 0,
            "event waiter task creates");
        CHECK(task_create("event-setter", event_setter, NULL, 1, 0) >= 0,
            "event setter task creates");
            CHECK(task_create("direct-waiter", direct_waiter, NULL, 0, 0) >= 0,
                "direct waiter task creates");
            CHECK(task_create("direct-signaler", direct_signaler, NULL, 1, 0) >= 0,
                "direct signaler task creates");
        int suspended_id = task_create("suspended", suspended_task, NULL, 3, 0);
        CHECK(suspended_id >= 0, "suspended task creates");
        CHECK(task_suspend(suspended_id) == RTOS_OK, "task suspends");
        task_info_t suspended_info;
        CHECK(task_get_info(suspended_id, &suspended_info) == RTOS_OK &&
              suspended_info.state == TASK_SUSPENDED,
            "suspended task is reported correctly");
        CHECK(task_resume(suspended_id) == RTOS_OK, "task resumes");

        FILE *trace = tmpfile();
        CHECK(trace != NULL, "trace file opens");
        rtos_trace_enable(trace);
    rtos_run();
        rtos_trace_disable();

        if (trace) {
            fseek(trace, 0, SEEK_SET);
          int first = fgetc(trace);
          CHECK(first == 't', "trace output has a CSV header");
          fclose(trace);
        }

    task_info_t info[RTOS_MAX_TASKS];
    int count = rtos_get_task_info(info, RTOS_MAX_TASKS);
    CHECK(count == 8, "task introspection returns all tasks");
    CHECK(info[0].switch_count > 0, "task switch statistics are recorded");
    CHECK(rtos_queue_count(&test_queue) == 0, "queue is empty after receive");

        sleep_order_count = 0;
        rtos_init(1);
        CHECK(task_create("long-sleep", long_sleep_task, NULL, 1, 0) >= 0,
            "long sleep task creates");
        CHECK(task_create("short-sleep", short_sleep_task, NULL, 1, 0) >= 0,
            "short sleep task creates");
        rtos_run();
        CHECK(sleep_order_count == 2 && sleep_order[0] == 2 && sleep_order[1] == 1,
            "delayed task heap wakes the earliest deadline first");

    if (failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    puts("all RTOS tests passed");
    return 0;
}
