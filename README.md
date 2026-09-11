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
make run-c      # build + run the C demo
make run-cpp    # build + run the C++ demo
make clean
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
void task_exit(void);
int  task_self(void);

int      rtos_get_task_info(task_info_t *out, int max_tasks);
uint32_t rtos_get_tick(void);
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

## Ideas to extend it

- Mutexes / semaphores with priority inheritance (classic priority-inversion fix)
- A message-queue / mailbox IPC primitive between tasks
- Stack overflow detection (canary word at the low end of each task stack)
- Port the same `rtos.h` API onto real hardware (swap `ucontext`+`SIGALRM`
  for a Cortex-M `PendSV`/`SysTick` context switch) — the task-facing API
  wouldn't need to change at all
