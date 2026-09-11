#include "rtos.h"

#include <stdio.h>
#include <string.h>

static int failures;
static rtos_mutex_t test_mutex;
static rtos_sem_t test_sem;
static rtos_queue_t test_queue;
static int queue_storage[2];

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

int main(void)
{
    rtos_init(1);
    rtos_mutex_init(&test_mutex);
    rtos_sem_init(&test_sem, 0, 1);
    CHECK(rtos_queue_init(&test_queue, queue_storage, sizeof(queue_storage[0]), 2)
              == RTOS_OK,
          "queue initializes");

    CHECK(task_create("producer", producer, NULL, 1, 0) >= 0,
          "producer task creates");
    CHECK(task_create("consumer", consumer, NULL, 1, 0) >= 0,
          "consumer task creates");
    CHECK(task_create("timeout", timeout_task, NULL, 2, 0) >= 0,
          "timeout task creates");
    rtos_run();

    task_info_t info[RTOS_MAX_TASKS];
    int count = rtos_get_task_info(info, RTOS_MAX_TASKS);
    CHECK(count == 3, "task introspection returns all tasks");
    CHECK(info[0].switch_count > 0, "task switch statistics are recorded");
    CHECK(rtos_queue_count(&test_queue) == 0, "queue is empty after receive");

    if (failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    puts("all RTOS tests passed");
    return 0;
}
