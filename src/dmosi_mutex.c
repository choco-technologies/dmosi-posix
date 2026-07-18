#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <pthread.h>
#include "dmosi.h"
#include "dmod.h"

/**
 * @brief Internal structure wrapping a POSIX mutex
 */
struct dmosi_mutex {
    pthread_mutex_t handle;   /**< POSIX mutex handle */
};

//==============================================================================
//                              MUTEX API Implementation
//==============================================================================

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, dmosi_mutex_t, _mutex_create, (bool recursive) )
{
    struct dmosi_mutex* mutex = Dmod_MallocEx(sizeof(*mutex), DMOSI_SYSTEM_MODULE_NAME);
    if (mutex == NULL) {
        return NULL;
    }

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    if (recursive) {
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    }

    int result = pthread_mutex_init(&mutex->handle, &attr);
    pthread_mutexattr_destroy(&attr);

    if (result != 0) {
        Dmod_Free(mutex);
        return NULL;
    }

    return (dmosi_mutex_t)mutex;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, void, _mutex_destroy, (dmosi_mutex_t mutex) )
{
    if (mutex == NULL) {
        return;
    }

    pthread_mutex_destroy(&mutex->handle);
    Dmod_Free(mutex);
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _mutex_lock, (dmosi_mutex_t mutex) )
{
    if (mutex == NULL) {
        return -EINVAL;
    }

    int result = pthread_mutex_lock(&mutex->handle);
    return (result == 0) ? 0 : -result;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _mutex_unlock, (dmosi_mutex_t mutex) )
{
    if (mutex == NULL) {
        return -EINVAL;
    }

    int result = pthread_mutex_unlock(&mutex->handle);
    return (result == 0) ? 0 : -result;
}
