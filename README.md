# rtos_scheduler

A small preemptive, priority-based task scheduler — the kind of thing
at the core of an RTOS (FreeRTOS/ChibiOS/Zephyr-style) — implemented
in **C**, with a **C++** convenience wrapper around it.

It runs in userspace on Linux (using `ucontext.h` for task context
switching and `SIGALRM`/`setitimer` as a stand-in for a hardware timer
interrupt), so you can study and tinker with real scheduling behavior
without any target hardware.

## What it demonstrates

- **Task Control Blocks (TCBs)** — id, name, state, priority, stack, saved context
- **Multi-level priority ready queues** — `RTOS_MAX_PRIORITIES` levels (0 = highest), round-robin within a level
- **True preemption** — a `SIGALRM` timer tick interrupts a running task mid-execution (even inside a busy loop) and hands the CPU to a higher/equal-priority task, exactly like a hardware timer interrupt driving a real RTOS
- **Cooperative yielding** — `task_yield()`
- **Blocking delay** — `task_delay(ticks)`, tick-accounted, for periodic/sensor-poll-style tasks
- **Periodic timing** — `task_delay_until()` avoids drift in recurring tasks
- **Synchronization** — recursive mutexes and counting semaphores with tick timeouts
- **Message passing** — fixed-size caller-owned queues with tick timeouts
- **Direct wakeups** — synchronization waiters are queued per object and woken by signals
- **Task lifecycle** — suspend, resume, delete, priority changes, and per-task lookup
- **Event flags** — wait for any or all bits with timeouts
- **Stack diagnostics** — canary checks and approximate high-water usage
- **Priority inheritance** — mutex contention temporarily boosts the owner
- **Task statistics** — run counts and context-switch counts through task introspection
- **Scheduler tracing** — optional CSV trace output for context switches and task state changes
- **Context switching via `ucontext`** — each task has its own stack and saved machine state

See the big comment block at the top of `src/rtos.c` for the full
design write-up, including a documented caveat: this uses
`swapcontext()` inside a signal handler, a well-known (if technically
not "officially" async-signal-safe) trick for building preemptive
userspace schedulers. It's great for learning; don't ship it as a
production kernel.

## Layout

```
rtos_scheduler/
├── include/rtos.h        Public API
├── src/rtos.c             Scheduler implementation (C)
├── examples/demo_c.c       Plain C demo: 3 priority levels, delay + yield
├── examples/demo_cpp.cpp   C++ demo: Task class wrapping std::function/lambdas
└── Makefile
```

## Build & run

```sh
make            # builds build/demo_c and build/demo_cpp
make test       # builds and runs the scheduler integration tests
make sanitize   # runs tests with AddressSanitizer and UBSan
make lib        # builds build/librtos.a for reuse by another program
make run-c      # build + run the C demo
make run-cpp    # build + run the C++ demo
make clean
```

The same targets can be built with CMake:

```sh
cmake -S . -B build-cmake
cmake --build build-cmake
ctest --test-dir build-cmake --output-on-failure
```

## API quick reference (`include/rtos.h`)

```c
void rtos_init(unsigned tick_ms);
int  task_create(const char *name, task_func_t func, void *arg,
                  int priority, size_t stack_size);
void rtos_run(void);          // blocks until all tasks finish
void rtos_shutdown(void);     // call from a task to stop the scheduler

void task_yield(void);
void task_delay(uint32_t ticks);
void task_delay_until(uint32_t *previous_wake, uint32_t period_ticks);
void task_exit(void);
int  task_self(void);

int      rtos_get_task_info(task_info_t *out, int max_tasks);
uint32_t rtos_get_tick(void);
```

Timeouts are expressed in scheduler ticks. A timeout of zero performs a
non-blocking operation. Synchronization and queue operations that may block
must be called from a task. Queue storage belongs to the caller:

```c
rtos_sem_t ready;
rtos_queue_t queue;
int storage[8];

rtos_sem_init(&ready, 0, 1);
rtos_queue_init(&queue, storage, sizeof(storage[0]), 8);
rtos_sem_take(&ready, 100);
```

For periodic tasks, initialize the wake time from the current tick and keep
passing the same variable:

```c
uint32_t wake = rtos_get_tick();
for (;;) {
    read_sensor();
    task_delay_until(&wake, 20);
}
```

Tasks can be inspected and controlled by id. A task may only be deleted when
it is not the currently running task:

```c
task_info_t info;
task_get_info(task_id, &info);
task_suspend(task_id);
task_set_priority(task_id, 2);
task_resume(task_id);
```

Event flags provide lightweight notifications:

```c
rtos_event_t events;
rtos_event_init(&events);
rtos_event_wait(&events, DATA_READY | ERROR, 0, 100);
```

Tracing writes CSV records to a caller-owned `FILE *`:

```c
rtos_trace_enable(stderr);
rtos_run();
rtos_trace_disable();
```

`priority` 0 is highest; `RTOS_MAX_PRIORITIES` (default 8) levels are
available. Tasks at the same priority round-robin.

## C++ usage (`Task` wrapper)

```cpp
Task t("blinker", [] {
    for (int i = 0; i < 10; i++) {
        toggle_led();
        task_delay(50);
    }
}, /*priority=*/1);
```

The wrapper heap-allocates the `std::function` and passes it through
the C API's `void *arg`, so you can capture state in lambdas instead
of threading a raw `void*` around by hand.

## Design boundaries

This project is a Linux userspace simulation. The signal handler and
`swapcontext()` path are intentionally educational and are not suitable for
production kernel use. Priority inheritance covers the common single-mutex
case; complex nested inheritance chains need a more complete inheritance
graph. The scheduler still scans delayed tasks once per tick, while mutex,
semaphore, and queue waits use direct per-object waiter lists.

## Ideas to extend it

- Port the same `rtos.h` API onto real hardware (swap `ucontext`+`SIGALRM`
  for a Cortex-M `PendSV`/`SysTick` context switch) — the task-facing API
  wouldn't need to change at all
