#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include "dmosi.h"
#include "dmod.h"

static pthread_once_t g_epoch_once = PTHREAD_ONCE_INIT;
static struct timespec g_epoch;

static void capture_epoch(void)
{
    clock_gettime(CLOCK_MONOTONIC, &g_epoch);
}

//==============================================================================
//                              System Time API Implementation
//==============================================================================

/**
 * @brief Get the current system time in milliseconds
 *
 * Returns the number of milliseconds elapsed since the first call to this
 * function (used as the reference epoch, captured lazily via CLOCK_MONOTONIC
 * so it is unaffected by wall-clock adjustments).
 *
 * @return uint32_t Elapsed time in milliseconds
 */
DMOD_INPUT_API_DECLARATION( dmosi, 1.0, uint32_t, _get_tick_count, (void) )
{
    pthread_once(&g_epoch_once, capture_epoch);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    // Convert both timestamps to a single signed nanosecond count before
    // subtracting, so that a negative tv_nsec borrow never has to be handled
    // (and never gets mis-cast into a huge unsigned value).
    int64_t start_ns   = (int64_t)g_epoch.tv_sec * 1000000000LL + g_epoch.tv_nsec;
    int64_t now_ns      = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
    int64_t elapsed_ms = (now_ns - start_ns) / 1000000LL;

    return (uint32_t)elapsed_ms;
}
