# API Reference

`dmosi-posix` implements the API declared in
[`dmosi.h`](https://github.com/choco-technologies/dmosi/blob/main/include/dmosi.h).
This document does not repeat that reference; it covers what's specific to the
POSIX backend - the underlying primitive each group is built on, and any
behavior that a caller coming from an RTOS backend should know about.

## Initialization API

`dmosi_init()` / `dmosi_deinit()` / `dmosi_is_started()`

- POSIX threads run immediately - there is no separate "start the scheduler"
  step like FreeRTOS's `vTaskStartScheduler()`. `dmosi_init()` therefore
  returns right away (it does not block), and `dmosi_is_started()` becomes
  `true` as soon as `dmosi_init()` succeeds.
- `dmosi_init()` creates the `"system"` process and lazily registers the
  calling thread (typically your `main()` thread) as a dmosi thread belonging
  to it.

## Mutex API

Backed directly by `pthread_mutex_t`. `dmosi_mutex_create(true)` uses
`PTHREAD_MUTEX_RECURSIVE`; `dmosi_mutex_create(false)` uses the default type.
Unlike the FreeRTOS backend, lock/unlock always take the real mutex - there is
no "nothing to lock before the scheduler starts" shortcut, since POSIX has no
such concept.

## Semaphore API

POSIX unnamed semaphores (`sem_t`) don't enforce a maximum count, so this is
implemented as a small bounded counting semaphore using a mutex + condition
variable instead. This also makes multi-unit `dmosi_semaphore_wait(sem, count,
...)` / `dmosi_semaphore_post(sem, count)` a single atomic update rather than
a loop of one-at-a-time operations.

- `timeout_ms == 0`: checked immediately, no wait (`-EAGAIN` if unavailable).
- `timeout_ms < 0`: waits forever (`pthread_cond_wait`).
- `timeout_ms > 0`: bounded wait against a `CLOCK_MONOTONIC` deadline
  (`pthread_cond_timedwait`), returns `-ETIMEDOUT` on expiry.
- `dmosi_semaphore_post()` returns `-EOVERFLOW` if it would push the count
  past `max_count`.

## Thread API

Backed by real `pthread_t` threads. A few things are structurally different
from an RTOS backend:

- **Priority** is stored as metadata only and returned verbatim by
  `dmosi_thread_get_priority()`. POSIX's default scheduling policy
  (`SCHED_OTHER`) doesn't support arbitrary per-thread priority levels, and
  switching to a real-time policy (`SCHED_FIFO`/`SCHED_RR`) generally requires
  elevated privileges - so the OS scheduling of the thread is never actually
  changed.
- **Enumeration** (`dmosi_thread_get_all()`, `dmosi_thread_get_by_process()`)
  is backed by an internal registry (a lock-protected linked list), not by
  querying the OS. Unlike FreeRTOS, POSIX gives no way to attach metadata to
  an arbitrary thread from outside that thread, so a thread is registered
  either at `dmosi_thread_create()` time (before the OS thread is even
  started, so it's visible to lookups immediately) or lazily, the first time
  a thread that wasn't created via `dmosi_thread_create()` calls into dmosi
  (e.g. the thread that called `dmosi_init()`).
- **`dmosi_thread_kill()`** uses `pthread_cancel()` (with asynchronous
  cancellation enabled on the target thread) for other threads, and
  `pthread_exit()` for self-termination. As documented on the interface
  itself, this may leave shared resources in an inconsistent state - prefer
  cooperative shutdown when possible.
- **`dmosi_thread_destroy()`** on a thread that hasn't finished yet
  cancels it and waits for the cancellation to land (`pthread_join`) before
  freeing its memory, to avoid the thread touching freed state. Destroying an
  already-finished thread just detaches it. Calling `dmosi_thread_join()` (or
  any other dmosi thread API) on a handle *after* it has been destroyed is
  undefined behavior, same as using any other freed pointer.
- **Stack usage** (`stack_current`/`stack_peak` in
  `dmosi_thread_get_info()`) is reported as unavailable (`0`). Unlike an
  RTOS, POSIX has no equivalent of FreeRTOS's stack-painting/high-water-mark
  tracking, so this can't be measured portably.
- **Runtime/CPU usage** in `dmosi_thread_get_info()` is computed from
  `pthread_getcpuclockid()` + `clock_gettime()` (the thread's own CPU-time
  clock), as a percentage of wall-clock time elapsed since the process's
  reference epoch (see System Time API below).
- `Dmod_GetLeftStackSize()` (a `Dmod` SAL hook, not part of `dmosi.h` itself)
  is also implemented, using `pthread_getattr_np()` to find the calling
  thread's real stack bounds.

## Process API

Reused as-is from
[dmosi-proc](https://github.com/choco-technologies/dmosi-proc) (vendored as a
git submodule under `lib/dmosi-proc`). That module's process/thread
bookkeeping has no RTOS-specific code - it's built entirely on top of the
Thread API and `Dmod_MallocEx`/`Dmod_Free`, so it works unmodified against
this backend's thread implementation.

## Queue API

A bounded ring buffer (mutex + two condition variables: one signaled when the
queue becomes non-empty, one when it becomes non-full). Timeout semantics
match the Semaphore API above.

## Timer API

Each timer owns a single long-lived worker thread that waits on a condition
variable with an absolute deadline, rather than being built on POSIX
per-process interval timers (`timer_create`/`SIGEV_THREAD`). This was a
deliberate choice, not the first thing tried - see
[architecture.md](architecture.md#timers) for why.

This makes `dmosi_timer_stop()` fully deterministic: it is applied under the
same lock the worker thread checks immediately after waking up (whether it
woke because the deadline passed or because of a state change), so a stop()
that happens-before the wake-up is guaranteed to suppress that firing - no
race window where a firing already "in flight" runs anyway.

`dmosi_timer_reset()` is equivalent to `dmosi_timer_start()`: both
(re)compute the deadline as `now + period` and arm the timer, regardless of
whether it was previously dormant or already running.

## Interrupt Handler API

POSIX processes have no notion of hardware interrupt context or NVIC-style
priority levels. `dmosi_context_switch_handler()`, `dmosi_syscall_handler()`,
and `dmosi_tick_handler()` are no-ops, and
`dmosi_get_min_interrupt_priority()` always returns `0` - exactly the
fallback documented in `dmosi.h` for backends with no such concept, meaning
any "interrupt priority" is safe to use.

## System Time API

`dmosi_get_tick_count()` returns milliseconds elapsed since the first call to
it in the process's lifetime, using `CLOCK_MONOTONIC` as the reference (so
it's immune to wall-clock adjustments). Note this reference point is the
*first call*, not `dmosi_init()` - in practice these are close together since
`dmosi_thread_get_info()` (the earliest internal caller) is normally only
reached after `dmosi_init()`.
