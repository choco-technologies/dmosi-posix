#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include "dmosi.h"
#include "dmod.h"

/**
 * @brief Internal structure implementing a bounded FIFO queue
 *
 * A simple ring buffer guarded by a mutex, with two condition variables
 * used to block senders while full and receivers while empty.
 */
struct dmosi_queue {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    uint8_t* buffer;
    size_t item_size;
    uint32_t capacity;
    uint32_t count;
    uint32_t head;   /**< Index of the next item to read */
    uint32_t tail;   /**< Index of the next free slot to write */
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
//                              QUEUE API Implementation
//==============================================================================

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, dmosi_queue_t, _queue_create, (size_t item_size, uint32_t queue_length) )
{
    if (item_size == 0 || queue_length == 0) {
        DMOD_LOG_ERROR("Invalid queue parameters: item_size=%zu, queue_length=%u\n", item_size, queue_length);
        return NULL;
    }

    struct dmosi_queue* queue = Dmod_MallocEx(sizeof(*queue), DMOSI_SYSTEM_MODULE_NAME);
    if (queue == NULL) {
        DMOD_LOG_ERROR("Failed to allocate memory for queue\n");
        return NULL;
    }

    queue->buffer = Dmod_MallocEx(item_size * queue_length, DMOSI_SYSTEM_MODULE_NAME);
    if (queue->buffer == NULL) {
        DMOD_LOG_ERROR("Failed to allocate memory for queue buffer\n");
        Dmod_Free(queue);
        return NULL;
    }

    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);

    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->not_empty, &cond_attr);
    pthread_cond_init(&queue->not_full, &cond_attr);
    pthread_condattr_destroy(&cond_attr);

    queue->item_size = item_size;
    queue->capacity = queue_length;
    queue->count = 0;
    queue->head = 0;
    queue->tail = 0;

    return (dmosi_queue_t)queue;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, void, _queue_destroy, (dmosi_queue_t queue) )
{
    if (queue == NULL) {
        return;
    }

    pthread_cond_destroy(&queue->not_empty);
    pthread_cond_destroy(&queue->not_full);
    pthread_mutex_destroy(&queue->mutex);
    Dmod_Free(queue->buffer);
    Dmod_Free(queue);
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _queue_send, (dmosi_queue_t queue, const void* item, int32_t timeout_ms) )
{
    if (queue == NULL || item == NULL) {
        DMOD_LOG_ERROR("Invalid queue or item (NULL)\n");
        return -EINVAL;
    }

    pthread_mutex_lock(&queue->mutex);

    int result = 0;
    if (queue->count == queue->capacity) {
        if (timeout_ms == 0) {
            result = -EAGAIN;
        } else {
            struct timespec deadline;
            if (timeout_ms > 0) {
                compute_deadline((uint32_t)timeout_ms, &deadline);
            }

            while (queue->count == queue->capacity) {
                int wait_result = (timeout_ms < 0)
                    ? pthread_cond_wait(&queue->not_full, &queue->mutex)
                    : pthread_cond_timedwait(&queue->not_full, &queue->mutex, &deadline);

                if (wait_result == ETIMEDOUT) {
                    result = -ETIMEDOUT;
                    break;
                }
            }
        }
    }

    if (result == 0) {
        memcpy(queue->buffer + (size_t)queue->tail * queue->item_size, item, queue->item_size);
        queue->tail = (queue->tail + 1) % queue->capacity;
        queue->count++;
        pthread_cond_signal(&queue->not_empty);
    }

    pthread_mutex_unlock(&queue->mutex);
    return result;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _queue_receive, (dmosi_queue_t queue, void* item, int32_t timeout_ms) )
{
    if (queue == NULL || item == NULL) {
        DMOD_LOG_ERROR("Invalid queue or item buffer (NULL)\n");
        return -EINVAL;
    }

    pthread_mutex_lock(&queue->mutex);

    int result = 0;
    if (queue->count == 0) {
        if (timeout_ms == 0) {
            result = -EAGAIN;
        } else {
            struct timespec deadline;
            if (timeout_ms > 0) {
                compute_deadline((uint32_t)timeout_ms, &deadline);
            }

            while (queue->count == 0) {
                int wait_result = (timeout_ms < 0)
                    ? pthread_cond_wait(&queue->not_empty, &queue->mutex)
                    : pthread_cond_timedwait(&queue->not_empty, &queue->mutex, &deadline);

                if (wait_result == ETIMEDOUT) {
                    result = -ETIMEDOUT;
                    break;
                }
            }
        }
    }

    if (result == 0) {
        memcpy(item, queue->buffer + (size_t)queue->head * queue->item_size, queue->item_size);
        queue->head = (queue->head + 1) % queue->capacity;
        queue->count--;
        pthread_cond_signal(&queue->not_full);
    }

    pthread_mutex_unlock(&queue->mutex);
    return result;
}
