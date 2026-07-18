#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include "dmosi.h"
#include "dmod.h"

/**
 * @brief Internal structure implementing a bounded counting semaphore
 *
 * POSIX unnamed semaphores (sem_t) do not enforce a maximum count, so a
 * mutex + condition variable pair is used instead - this also gives a
 * straightforward way to support taking/releasing more than one unit at a
 * time, matching the dmosi_semaphore_wait()/_post() count parameter.
 */
struct dmosi_semaphore {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    uint32_t count;
    uint32_t max_count;
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

//==============================================================================
//                              SEMAPHORE API Implementation
//==============================================================================

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, dmosi_semaphore_t, _semaphore_create, (uint32_t initial_count, uint32_t max_count) )
{
    if (max_count == 0 || initial_count > max_count) {
        DMOD_LOG_ERROR("Invalid semaphore parameters: initial_count=%u, max_count=%u\n", initial_count, max_count);
        return NULL;
    }

    struct dmosi_semaphore* semaphore = Dmod_MallocEx(sizeof(*semaphore), DMOSI_SYSTEM_MODULE_NAME);
    if (semaphore == NULL) {
        DMOD_LOG_ERROR("Failed to allocate memory for semaphore\n");
        return NULL;
    }

    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);

    pthread_mutex_init(&semaphore->mutex, NULL);
    pthread_cond_init(&semaphore->cond, &cond_attr);
    pthread_condattr_destroy(&cond_attr);

    semaphore->count = initial_count;
    semaphore->max_count = max_count;

    return (dmosi_semaphore_t)semaphore;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, void, _semaphore_destroy, (dmosi_semaphore_t semaphore) )
{
    if (semaphore == NULL) {
        return;
    }

    pthread_cond_destroy(&semaphore->cond);
    pthread_mutex_destroy(&semaphore->mutex);
    Dmod_Free(semaphore);
}

DMOD_INPUT_API_DECLARATION( dmosi, 2.0, int, _semaphore_wait, (dmosi_semaphore_t semaphore, uint32_t count, int32_t timeout_ms) )
{
    if (semaphore == NULL) {
        DMOD_LOG_ERROR("Invalid semaphore handle (NULL)\n");
        return -EINVAL;
    }

    pthread_mutex_lock(&semaphore->mutex);

    int result = 0;
    if (semaphore->count < count) {
        if (timeout_ms == 0) {
            result = -EAGAIN;
        } else {
            struct timespec deadline;
            if (timeout_ms > 0) {
                compute_deadline((uint32_t)timeout_ms, &deadline);
            }

            while (semaphore->count < count) {
                int wait_result = (timeout_ms < 0)
                    ? pthread_cond_wait(&semaphore->cond, &semaphore->mutex)
                    : pthread_cond_timedwait(&semaphore->cond, &semaphore->mutex, &deadline);

                if (wait_result == ETIMEDOUT) {
                    result = -ETIMEDOUT;
                    break;
                }
            }
        }
    }

    if (result == 0) {
        semaphore->count -= count;
    }

    pthread_mutex_unlock(&semaphore->mutex);
    return result;
}

DMOD_INPUT_API_DECLARATION( dmosi, 2.0, int, _semaphore_post, (dmosi_semaphore_t semaphore, uint32_t count) )
{
    if (semaphore == NULL) {
        DMOD_LOG_ERROR("Invalid semaphore handle (NULL)\n");
        return -EINVAL;
    }

    pthread_mutex_lock(&semaphore->mutex);

    int result = 0;
    if (count > semaphore->max_count - semaphore->count) {
        DMOD_LOG_ERROR("Failed to post semaphore (overflow)\n");
        result = -EOVERFLOW;
    } else {
        semaphore->count += count;
        pthread_cond_broadcast(&semaphore->cond);
    }

    pthread_mutex_unlock(&semaphore->mutex);
    return result;
}
