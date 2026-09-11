/*
 * demo_cpp.cpp — A thin C++ convenience layer over the C scheduler,
 * showing lambdas/std::function used as task bodies instead of raw
 * C function pointers + void* arg.
 */

#include "rtos.h"
#include <cstdio>
#include <functional>
#include <vector>
#include <string>
#include <memory>

/* The C API only knows how to call `void(*)(void*)`. To let C++ users
 * hand us a std::function (capturing lambda, member fn, etc.), we heap-
 * allocate the std::function and pass its pointer as the void* arg;
 * a small trampoline unwraps and invokes it. */
class Task {
public:
    Task(const std::string &name, std::function<void()> body, int priority)
        : body_(std::make_unique<std::function<void()>>(std::move(body)))
    {
        id_ = task_create(name.c_str(), &Task::trampoline, body_.get(), priority, 0);
    }

    int id() const { return id_; }

private:
    static void trampoline(void *arg)
    {
        auto *fn = static_cast<std::function<void()> *>(arg);
        (*fn)();
    }

    std::unique_ptr<std::function<void()>> body_;
    int id_ = -1;
};

/* Keep the Task objects (and their captured std::functions) alive for
 * the lifetime of the scheduler run. */
static std::vector<std::unique_ptr<Task>> g_tasks;

static void spawn(const std::string &name, int priority, std::function<void()> body)
{
    g_tasks.push_back(std::make_unique<Task>(name, std::move(body), priority));
}

int main()
{
    rtos_init(5);

    spawn("cpp-producer", 1, [] {
        for (int i = 0; i < 5; i++) {
            std::printf("[tick %3u] producer: item %d\n", rtos_get_tick(), i);
            task_delay(10);
        }
        std::printf("[tick %3u] producer done\n", rtos_get_tick());
    });

    /* Capture by value so each consumer has its own id. */
    for (int id = 1; id <= 2; id++) {
        spawn("cpp-consumer-" + std::to_string(id), 2, [id] {
            for (int i = 0; i < 3; i++) {
                std::printf("[tick %3u] consumer-%d: processing %d\n",
                            rtos_get_tick(), id, i);
                task_yield();
            }
        });
    }

    std::puts("=== starting scheduler (C++ wrapper) ===");
    rtos_run();

    task_info_t info[RTOS_MAX_TASKS];
    int n = rtos_get_task_info(info, RTOS_MAX_TASKS);
    std::puts("=== task stats ===");
    for (int i = 0; i < n; i++) {
        std::printf("  %-16s prio=%d runs=%u state=%d\n",
                     info[i].name, info[i].priority,
                     info[i].run_count, info[i].state);
    }

    g_tasks.clear();
    return 0;
}
