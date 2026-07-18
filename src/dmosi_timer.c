#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include "dmosi.h"
#include "dmod.h"

/**
 * @brief Internal structure implementing a software timer
 *
 * Each timer owns a single long-lived worker thread that waits on a
 * condition variable with an absolute deadline. Using a self-managed thread
 * (rather than a POSIX per-process interval timer/signal notification) makes
 * start/stop/reset fully deterministic: every state change (start, stop,
 * reset, period change, destroy) is applied under @c lock, and the worker
 * always re-checks @c active immediately after waking (whether it woke up
 * because the deadline passed or because a state change signaled it) before
 * ever invoking the callback - so a stop() that happens-before the wake-up
 * is guaranteed to suppress that firing, with no race window.
 */
struct dmosi_timer {
    pthread_t worker;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    dmosi_timer_callback_t callback;
    void* arg;
    uint32_t period_ms;
    bool auto_reload;
    bool active;         /**< Armed: the worker should count down and eventually fire */
    bool shutting_down;   /**< Set by destroy() to stop the worker thread for good */
    struct timespec deadline;
};

/**
 * @brief Compute an absolute CLOCK_MONOTONIC deadline @p ms milliseconds from now
 */
static void compute_deadline(uint32_t ms, struct timespec* deadline)
{
    clock_gettime(CLOCK_MONOTONIC, deadline);
    deadline->tv_sec  += ms / 1000;
    deadline->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_nsec -= 1000000000L;
        deadline->tv_sec  += 1;
    }
}

/**
 * @brief Worker thread body: waits for the deadline, fires, and repeats/parks as needed
 */
static void* timer_worker(void* arg)
{
    struct dmosi_timer* timer = (struct dmosi_timer*)arg;

    pthread_mutex_lock(&timer->lock);

    while (!timer->shutting_down) {
        if (!timer->active) {
            pthread_cond_wait(&timer->cond, &timer->lock);
            continue;
        }

        int wait_result = pthread_cond_timedwait(&timer->cond, &timer->lock, &timer->deadline);

        if (timer->shutting_down) {
            break;
        }

        if (wait_result == ETIMEDOUT && timer->active) {
            dmosi_timer_callback_t callback = timer->callback;
            void* callback_arg = timer->arg;

            if (timer->auto_reload) {
                compute_deadline(timer->period_ms, &timer->deadline);
            } else {
                timer->active = false;
            }

            // Invoke the callback without holding the lock, so that the
            // callback is free to call back into this timer's own API
            // (e.g. stop itself) without deadlocking.
            pthread_mutex_unlock(&timer->lock);
            if (callback != NULL) {
                callback(callback_arg);
            }
            pthread_mutex_lock(&timer->lock);
        }
        // A spurious/other wake-up (wait_result == 0) just loops back around
        // and re-evaluates timer->active / the deadline.
    }

    pthread_mutex_unlock(&timer->lock);
    return NULL;
}

//==============================================================================
//                              TIMER API Implementation
//==============================================================================

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, dmosi_timer_t, _timer_create, (dmosi_timer_callback_t callback, void* arg, uint32_t period_ms, bool auto_reload) )
{
    if (callback == NULL || period_ms == 0) {
        return NULL;
    }

    struct dmosi_timer* timer = Dmod_MallocEx(sizeof(*timer), DMOSI_SYSTEM_MODULE_NAME);
    if (timer == NULL) {
        return NULL;
    }

    timer->callback = callback;
    timer->arg = arg;
    timer->period_ms = period_ms;
    timer->auto_reload = auto_reload;
    timer->active = false;
    timer->shutting_down = false;

    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);

    pthread_mutex_init(&timer->lock, NULL);
    pthread_cond_init(&timer->cond, &cond_attr);
    pthread_condattr_destroy(&cond_attr);

    if (pthread_create(&timer->worker, NULL, timer_worker, timer) != 0) {
        pthread_cond_destroy(&timer->cond);
        pthread_mutex_destroy(&timer->lock);
        Dmod_Free(timer);
        return NULL;
    }

    return (dmosi_timer_t)timer;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, void, _timer_destroy, (dmosi_timer_t timer) )
{
    if (timer == NULL) {
        return;
    }

    pthread_mutex_lock(&timer->lock);
    timer->shutting_down = true;
    pthread_cond_broadcast(&timer->cond);
    pthread_mutex_unlock(&timer->lock);

    pthread_join(timer->worker, NULL);

    pthread_cond_destroy(&timer->cond);
    pthread_mutex_destroy(&timer->lock);
    Dmod_Free(timer);
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _timer_start, (dmosi_timer_t timer) )
{
    if (timer == NULL) {
        return -EINVAL;
    }

    pthread_mutex_lock(&timer->lock);
    timer->active = true;
    compute_deadline(timer->period_ms, &timer->deadline);
    pthread_cond_broadcast(&timer->cond);
    pthread_mutex_unlock(&timer->lock);

    return 0;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _timer_stop, (dmosi_timer_t timer) )
{
    if (timer == NULL) {
        return -EINVAL;
    }

    pthread_mutex_lock(&timer->lock);
    timer->active = false;
    pthread_cond_broadcast(&timer->cond);
    pthread_mutex_unlock(&timer->lock);

    return 0;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _timer_reset, (dmosi_timer_t timer) )
{
    // Rearming always recomputes the expiry time from "now", regardless of
    // whether the timer was previously dormant or active - exactly "start if
    // dormant, else restart the countdown".
    return dmosi_timer_start(timer);
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _timer_set_period, (dmosi_timer_t timer, uint32_t period_ms) )
{
    if (timer == NULL || period_ms == 0) {
        return -EINVAL;
    }

    pthread_mutex_lock(&timer->lock);
    timer->period_ms = period_ms;
    if (timer->active) {
        compute_deadline(timer->period_ms, &timer->deadline);
        pthread_cond_broadcast(&timer->cond);
    }
    pthread_mutex_unlock(&timer->lock);

    return 0;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, uint32_t, _timer_get_period, (dmosi_timer_t timer) )
{
    if (timer == NULL) {
        return 0;
    }

    return timer->period_ms;
}
