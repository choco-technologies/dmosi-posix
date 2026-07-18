#ifdef __linux__
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include "dmosi.h"
#include "dmod.h"

/**
 * @brief Sentinel TLS value marking "wrapper creation in progress" for the current thread
 *
 * Mirrors the equivalent FreeRTOS-backend guard: thread_new() allocates via
 * Dmod_MallocEx(), which can end up (indirectly, through module accounting)
 * touching thread-local state again. Storing this sentinel before the real
 * struct is ready lets a re-entrant lookup detect "already being created"
 * and return NULL instead of recursing forever.
 */
#define DMOSI_THREAD_TLS_BOOTSTRAPPING    ((struct dmosi_thread*)1)

/**
 * @brief Fallback process used during system initialization
 *
 * Set by dmosi_thread_set_init_process() before the first call to
 * dmosi_thread_current(), so that the lazy-registration path below does not
 * need to call dmosi_process_current() and trigger infinite recursion while
 * no process has been associated with the calling thread yet.
 *
 * @note Only written from dmosi_posix _init/_deinit, which run before any
 * other dmosi API is exercised concurrently, so no synchronization is
 * required for this variable itself.
 */
static dmosi_process_t g_init_process = NULL;

/**
 * @brief Internal structure wrapping a POSIX thread
 */
struct dmosi_thread {
    pthread_t handle;                 /**< POSIX thread handle */
    dmosi_thread_entry_t entry;       /**< Thread entry function (NULL for lazily-registered threads) */
    void* arg;                        /**< Argument passed to thread entry */
    atomic_bool completed;            /**< Whether the thread has finished running */
    bool joined;                      /**< Whether the thread has already been joined/reclaimed */
    dmosi_process_t process;          /**< Process that the thread belongs to */
    size_t stack_size;                /**< Requested stack size in bytes (0 if unknown) */
    char* name;                       /**< Thread name (owned copy) */
    int priority;                     /**< Requested priority (metadata only - see note below) */
    struct dmosi_thread* next;        /**< Intrusive link for the global thread registry */
};

/*
 * POSIX (SCHED_OTHER, the default scheduling policy) does not support
 * per-thread priority levels the way an RTOS does, and changing scheduling
 * policy/priority generally requires elevated privileges. The requested
 * priority is therefore stored as metadata only and reported back verbatim
 * by dmosi_thread_get_priority(), without altering how the OS schedules the
 * thread.
 */

//==============================================================================
//                              Thread registry
//==============================================================================

static pthread_mutex_t g_registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct dmosi_thread* g_registry_head = NULL;

static pthread_key_t g_tls_key;
static pthread_once_t g_tls_once = PTHREAD_ONCE_INIT;

static void tls_key_create(void)
{
    pthread_key_create(&g_tls_key, NULL);
}

static void ensure_tls_key(void)
{
    pthread_once(&g_tls_once, tls_key_create);
}

static void registry_add(struct dmosi_thread* thread)
{
    pthread_mutex_lock(&g_registry_mutex);
    thread->next = g_registry_head;
    g_registry_head = thread;
    pthread_mutex_unlock(&g_registry_mutex);
}

static void registry_remove(struct dmosi_thread* thread)
{
    pthread_mutex_lock(&g_registry_mutex);
    struct dmosi_thread** cursor = &g_registry_head;
    while (*cursor != NULL) {
        if (*cursor == thread) {
            *cursor = thread->next;
            break;
        }
        cursor = &(*cursor)->next;
    }
    pthread_mutex_unlock(&g_registry_mutex);
}

/**
 * @brief Enumerate registered threads, optionally filtered by process
 *
 * @param process  Filter: only include threads whose process matches this value,
 *                 or NULL to include every registered thread.
 * @param threads  Output array, or NULL for a count-only query.
 * @param max_count Maximum entries to write into @p threads.
 * @return Number of matching threads found, capped at @p max_count when @p threads
 *         is non-NULL.
 */
static size_t thread_enumerate(dmosi_process_t process, dmosi_thread_t* threads, size_t max_count)
{
    size_t count = 0;

    pthread_mutex_lock(&g_registry_mutex);
    for (struct dmosi_thread* t = g_registry_head; t != NULL; t = t->next) {
        if (process != NULL && t->process != process) {
            continue;
        }
        if (threads != NULL && count < max_count) {
            threads[count] = (dmosi_thread_t)t;
        }
        count++;
    }
    pthread_mutex_unlock(&g_registry_mutex);

    if (threads != NULL && count > max_count) {
        return max_count;
    }
    return count;
}

//==============================================================================
//                              Helpers
//==============================================================================

static struct dmosi_thread* thread_new(pthread_t handle, dmosi_thread_entry_t entry, void* arg,
                                        dmosi_process_t process, size_t stack_size, const char* name, int priority)
{
    const char* allocator_name = (process != NULL) ? dmosi_process_get_module_name(process) : NULL;
    if (allocator_name == NULL) {
        allocator_name = DMOSI_SYSTEM_MODULE_NAME;
    }

    struct dmosi_thread* thread = Dmod_MallocEx(sizeof(*thread), allocator_name);
    if (thread == NULL) {
        return NULL;
    }

    thread->handle = handle;
    thread->entry = entry;
    thread->arg = arg;
    // Lazily-registered threads (entry == NULL, e.g. the thread that called
    // dmosi_init(), or any other externally-created thread that calls into
    // dmosi for the first time) are running right now, by construction - the
    // very call that reaches thread_new() for them is happening on their own
    // stack. There is no separate "not yet started" state to model here, so
    // completed always starts false regardless of entry.
    atomic_init(&thread->completed, false);
    thread->joined = false;
    thread->process = process;
    thread->stack_size = stack_size;
    thread->priority = priority;
    thread->name = (name != NULL) ? Dmod_StrDup(name) : NULL;
    thread->next = NULL;

    return thread;
}

static void thread_free(struct dmosi_thread* thread)
{
    Dmod_Free(thread->name);
    Dmod_Free(thread);
}

/**
 * @brief POSIX thread entry point wrapping the user's dmosi_thread_entry_t
 */
static void* thread_wrapper(void* arg)
{
    struct dmosi_thread* thread = (struct dmosi_thread*)arg;

    // Allow dmosi_thread_kill() to terminate this thread promptly rather than
    // only at the next cancellation point. The interface documents kill() as
    // potentially leaving shared state inconsistent - this is the trade-off
    // that makes "forceful" termination actually forceful under POSIX.
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    ensure_tls_key();
    pthread_setspecific(g_tls_key, thread);

    if (thread->entry != NULL) {
        thread->entry(thread->arg);
    }

    atomic_store(&thread->completed, true);
    pthread_setspecific(g_tls_key, NULL);

    return NULL;
}

//==============================================================================
//                              THREAD API Implementation
//==============================================================================

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, dmosi_thread_t, _thread_create,
    (dmosi_thread_entry_t entry, void* arg, int priority, size_t stack_size, const char* name, dmosi_process_t process) )
{
    if (entry == NULL || stack_size == 0 || name == NULL) {
        return NULL;
    }

    if (process == NULL) {
        process = dmosi_process_current();
    }

    struct dmosi_thread* thread = thread_new((pthread_t)0, entry, arg, process, stack_size, name, priority);
    if (thread == NULL) {
        return NULL;
    }

    // Register before starting the OS thread (and before it is fully populated)
    // so that a caller scanning the registry for this process right after
    // dmosi_thread_create() returns is guaranteed to find it - unlike FreeRTOS's
    // task-local-storage-based lookup, POSIX gives no way to attach TLS to a
    // thread other than from within that thread itself, so the registry is the
    // only mechanism available for external lookups.
    registry_add(thread);

    pthread_attr_t attr;
    pthread_attr_init(&attr);

    size_t effective_stack_size = stack_size;
    long min_stack = sysconf(_SC_THREAD_STACK_MIN);
    if (min_stack > 0 && effective_stack_size < (size_t)min_stack) {
        effective_stack_size = (size_t)min_stack;
    }
    pthread_attr_setstacksize(&attr, effective_stack_size);

    pthread_t handle;
    int result = pthread_create(&handle, &attr, thread_wrapper, thread);
    pthread_attr_destroy(&attr);

    if (result != 0) {
        registry_remove(thread);
        thread_free(thread);
        return NULL;
    }

    // Publish the real handle under the registry lock so that concurrent
    // readers of the registry never observe a torn/stale value.
    pthread_mutex_lock(&g_registry_mutex);
    thread->handle = handle;
    pthread_mutex_unlock(&g_registry_mutex);

#if defined(__linux__)
    {
        char short_name[16];
        snprintf(short_name, sizeof(short_name), "%s", name);
        pthread_setname_np(handle, short_name);
    }
#endif

    return (dmosi_thread_t)thread;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, void, _thread_destroy, (dmosi_thread_t thread) )
{
    if (thread == NULL) {
        return;
    }

    bool is_self = pthread_equal(thread->handle, pthread_self());
    bool done = atomic_load(&thread->completed);

    if (!done && !is_self) {
        // Forcefully terminate a still-running thread before releasing its
        // memory, to avoid the thread touching freed state. pthread_join()
        // here waits for the (asynchronous) cancellation to actually land.
        pthread_cancel(thread->handle);
        pthread_join(thread->handle, NULL);
        thread->joined = true;
    } else if (!thread->joined) {
        // Either already finished or destroying ourselves: just release the
        // OS-level thread resources without waiting.
        pthread_detach(thread->handle);
    }

    registry_remove(thread);
    thread_free(thread);
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _thread_join, (dmosi_thread_t thread) )
{
    if (thread == NULL) {
        return -EINVAL;
    }

    if (thread->joined) {
        return -EINVAL;
    }

    int result = pthread_join(thread->handle, NULL);
    if (result != 0) {
        return -result;
    }

    thread->joined = true;
    return 0;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _thread_kill, (dmosi_thread_t thread, int status) )
{
    if (thread == NULL) {
        return -EINVAL;
    }

    (void)status;

    atomic_store(&thread->completed, true);

    if (pthread_equal(thread->handle, pthread_self())) {
        // Self-termination: does not return, mirroring dmosi_thread_kill()
        // being called by a module on its own thread to end itself.
        ensure_tls_key();
        pthread_setspecific(g_tls_key, NULL);
        pthread_exit(NULL);
    }

    if (pthread_cancel(thread->handle) != 0) {
        return -EIO;
    }

    return 0;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, dmosi_thread_t, _thread_current, (void) )
{
    ensure_tls_key();

    struct dmosi_thread* thread = (struct dmosi_thread*)pthread_getspecific(g_tls_key);

    if (thread == DMOSI_THREAD_TLS_BOOTSTRAPPING) {
        return NULL;
    }

    if (thread != NULL) {
        return (dmosi_thread_t)thread;
    }

    pthread_setspecific(g_tls_key, DMOSI_THREAD_TLS_BOOTSTRAPPING);

    // Use g_init_process as a fallback during system initialization to break
    // the circular dependency: _thread_current -> _process_current -> _thread_current.
    dmosi_process_t process = (g_init_process != NULL) ? g_init_process : dmosi_process_current();

    thread = thread_new(pthread_self(), NULL, NULL, process, 0, NULL, 0);
    if (thread == NULL) {
        pthread_setspecific(g_tls_key, NULL);
        return NULL;
    }

#if defined(__linux__)
    {
        char os_name[16] = { 0 };
        if (pthread_getname_np(pthread_self(), os_name, sizeof(os_name)) == 0 && os_name[0] != '\0') {
            thread->name = Dmod_StrDup(os_name);
        }
    }
#endif

    registry_add(thread);
    pthread_setspecific(g_tls_key, thread);

    return (dmosi_thread_t)thread;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, void, _thread_sleep, (uint32_t ms) )
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;

    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        // Interrupted by a signal: nanosleep() already updated ts with the
        // remaining time, just resume sleeping.
    }
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, const char*, _thread_get_name, (dmosi_thread_t thread) )
{
    if (thread == NULL) {
        thread = dmosi_thread_current();
        if (thread == NULL) {
            return NULL;
        }
    }

    return thread->name;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, const char*, _thread_get_module_name, (dmosi_thread_t thread) )
{
    if (thread == NULL) {
        thread = dmosi_thread_current();
        if (thread == NULL) {
            return NULL;
        }
    }

    if (thread->process == NULL) {
        return NULL;
    }

    return dmosi_process_get_module_name(thread->process);
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _thread_get_priority, (dmosi_thread_t thread) )
{
    if (thread == NULL) {
        thread = dmosi_thread_current();
        if (thread == NULL) {
            return 0;
        }
    }

    return thread->priority;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, dmosi_process_t, _thread_get_process, (dmosi_thread_t thread) )
{
    if (thread == NULL) {
        thread = dmosi_thread_current();
        if (thread == NULL) {
            return NULL;
        }
    }

    return thread->process;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, size_t, _thread_get_all, (dmosi_thread_t* threads, size_t max_count) )
{
    return thread_enumerate(NULL, threads, max_count);
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, size_t, _thread_get_by_process, (dmosi_process_t process, dmosi_thread_t* threads, size_t max_count) )
{
    return thread_enumerate(process, threads, max_count);
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _thread_get_info, (dmosi_thread_t thread, dmosi_thread_info_t* info) )
{
    if (info == NULL) {
        return -EINVAL;
    }

    if (thread == NULL) {
        thread = dmosi_thread_current();
        if (thread == NULL) {
            return -EFAULT;
        }
    }

    bool done = atomic_load(&thread->completed);

    info->stack_total   = thread->stack_size;
    // Instantaneous/peak stack usage cannot be measured portably under POSIX
    // without instrumenting the stack (no equivalent of FreeRTOS's stack
    // painting / high-water mark); report as unavailable.
    info->stack_current = 0;
    info->stack_peak     = 0;
    info->state          = done ? DMOSI_THREAD_STATE_TERMINATED : DMOSI_THREAD_STATE_RUNNING;

    uint64_t runtime_ms = 0;
    clockid_t cpu_clock_id;
    if (pthread_getcpuclockid(thread->handle, &cpu_clock_id) == 0) {
        struct timespec ts;
        if (clock_gettime(cpu_clock_id, &ts) == 0) {
            runtime_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
        }
    }
    info->runtime_ms = runtime_ms;

    uint32_t elapsed_ms = dmosi_get_tick_count();
    if (elapsed_ms > 0) {
        float usage = (float)runtime_ms / (float)elapsed_ms * 100.0f;
        info->cpu_usage = (usage < 100.0f) ? usage : 100.0f;
    } else {
        info->cpu_usage = 0.0f;
    }

    return 0;
}

//==============================================================================
//                              Initialization helpers
//==============================================================================

/**
 * @brief Set the fallback process used during system initialization
 *
 * @note Only called from dmosi_posix _init/_deinit.
 *
 * @param process Process handle to use as fallback (NULL to clear)
 */
void dmosi_thread_set_init_process(dmosi_process_t process)
{
    g_init_process = process;
}

/**
 * @brief Unregister the calling thread's dmosi thread and clear its TLS entry
 *
 * Called from dmosi_posix _deinit to clean up the thread that was implicitly
 * registered for the calling thread during _init.
 */
void dmosi_thread_unregister_current(void)
{
    ensure_tls_key();

    struct dmosi_thread* thread = (struct dmosi_thread*)pthread_getspecific(g_tls_key);
    if (thread == NULL || thread == DMOSI_THREAD_TLS_BOOTSTRAPPING) {
        return;
    }

    pthread_setspecific(g_tls_key, NULL);
    registry_remove(thread);
    thread_free(thread);
}

//==============================================================================
//                              Dmod SAL Implementation
//==============================================================================

/**
 * @brief Get remaining stack size of the calling thread
 *
 * Compares the address of a local variable (approximating the current stack
 * pointer) against the pthread stack base obtained via pthread_getattr_np().
 *
 * @return Remaining stack size in bytes, or SIZE_MAX if not determinable
 */
DMOD_INPUT_API_DECLARATION( Dmod, 1.0, size_t, _GetLeftStackSize, (void) )
{
    volatile char stack_var;

    pthread_attr_t attr;
    void* stack_addr;
    size_t stack_size;

    if (pthread_getattr_np(pthread_self(), &attr) != 0) {
        return SIZE_MAX;
    }

    if (pthread_attr_getstack(&attr, &stack_addr, &stack_size) != 0) {
        pthread_attr_destroy(&attr);
        return SIZE_MAX;
    }

    pthread_attr_destroy(&attr);

    uintptr_t current_sp = (uintptr_t)&stack_var;
    uintptr_t stack_base = (uintptr_t)stack_addr;

    if (current_sp <= stack_base) {
        return 0;
    }

    return (size_t)(current_sp - stack_base);
}
