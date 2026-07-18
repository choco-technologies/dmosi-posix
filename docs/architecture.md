# Architecture

## Where dmosi-posix sits

```
┌─────────────────────────────────────────┐
│     Application / DMOD Modules          │
└─────────────────┬───────────────────────┘
                  │
                  │ Uses DMOSI API
                  │
┌─────────────────▼───────────────────────┐
│            DMOSI Library                │
│  (Weak implementations/stubs by default)│
└─────────────────┬───────────────────────┘
                  │
                  │ Overridden by
                  │
┌─────────────────▼───────────────────────┐
│           dmosi-posix                  │
│   (pthread / condvars / clock_*)        │
└─────────────────┬───────────────────────┘
                  │
┌─────────────────▼───────────────────────┐
│         Linux / POSIX kernel            │
└─────────────────────────────────────────┘
```

`dmosi` declares every `dmosi_*` function as a *weak* symbol with a stub
implementation (return `NULL`/`-ENOSYS`/no-op). `dmosi-posix` provides the
*strong* implementations that override those stubs at link time - the
application never calls anything in this library directly by name, it just
links it in and calls the `dmosi_*` functions declared in `dmosi.h`.

It is built as a **plain static library** (`libdmosi_posix.a`), the same
shape as [dmosi-freertos](https://github.com/choco-technologies/dmosi-freertos)
- neither is a loadable DMOD module (no `dmod_add_library`/`.dmf`); both sit
below DMOD's module system, providing the OS primitives DMOD itself and every
module built on top of it depend on.

## Process management is reused, not reimplemented

[dmosi-proc](https://github.com/choco-technologies/dmosi-proc) implements the
Process API portion of `dmosi.h` entirely in terms of the *Thread* API
(`dmosi_thread_create`, `dmosi_thread_get_by_process`, `dmosi_thread_sleep`,
...) and dmod's own SAL (`Dmod_MallocEx`, `Dmod_StrDup`, `Dmod_EnterCritical`,
...) - it has no FreeRTOS-specific or otherwise RTOS-specific code anywhere.
Because of that, it's vendored unmodified here as a git submodule
(`lib/dmosi-proc`) exactly the way `dmosi-freertos` also vendors it: once the
Thread API below it is implemented for a given backend, the Process API comes
along for free.

## Thread registry

FreeRTOS exposes `uxTaskGetSystemState()`, which lets `dmosi-freertos` build
`dmosi_thread_get_all()`/`dmosi_thread_get_by_process()` by asking the kernel
directly, and lets it attach metadata to *any* task's thread-local storage
from outside that task.

POSIX has neither capability: there's no "list every thread in this process"
syscall, and `pthread_setspecific()` can only ever set the *calling* thread's
own TLS slot. `dmosi-posix` therefore keeps its own registry - a
mutex-protected intrusive linked list of `struct dmosi_thread*` - as the only
mechanism available for the enumeration/lookup APIs.

A thread is added to the registry in one of two ways:

- **`dmosi_thread_create()`** adds the (not-yet-started) thread struct to the
  registry *before* calling `pthread_create()`, so a caller that immediately
  looks it up (e.g. `dmosi-proc`'s `kill_threads()`, called right after a
  module is spawned) is guaranteed to find it - there's no window where the
  thread exists but isn't visible yet.
- **Lazy self-registration**: any thread that wasn't created via
  `dmosi_thread_create()` (most commonly, the thread that called
  `dmosi_init()`) registers itself the first time it calls
  `dmosi_thread_current()`, using thread-local storage (a `pthread_key_t`) to
  remember its own `struct dmosi_thread*` for subsequent calls.

## Timers

The first implementation of the Timer API used POSIX per-process interval
timers (`timer_create()` with `SIGEV_THREAD` notification), which spawns a
fresh notification thread from inside glibc for every expiration - the
"obvious" POSIX-native mapping for a periodic callback. Two problems showed
up under test:

1. **A crash inside glibc's own `timer_sigev_thread` -> notification
   callback path**, non-deterministically, only in the linked test binary
   (which uses the same minimal, from-scratch linker script used everywhere
   else in this ecosystem for `DMOD_EXTERNAL_REGISTRATION` binaries - see
   `tests/main.ld`).
2. **`dmosi_timer_stop()` was not deterministic**: `timer_settime()` disarming
   a timer doesn't retroactively cancel a notification that has already been
   delivered to the kernel and is in the process of spawning its callback
   thread, so a stop() right at the edge of a period could still be followed
   by one more firing - unlike the FreeRTOS backend, where stop/start/reset
   are all processed synchronously by a single timer service task.

Both are avoided by not using `timer_create`/signals at all: each
`dmosi_timer_t` instead owns one long-lived worker thread (created once, in
`dmosi_timer_create()`, and joined in `dmosi_timer_destroy()`) that waits on a
condition variable against an absolute `CLOCK_MONOTONIC` deadline. Every state
change - start, stop, reset, period change, destroy - takes the same lock the
worker checks immediately after waking up, before ever invoking the callback.
That's what makes stop() deterministic: if it happens-before the wake-up (by
lock ordering, not by timing), the callback simply won't fire for that
wake-up. See `src/dmosi_timer.c` for the implementation.
