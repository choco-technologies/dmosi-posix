#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include "dmosi.h"
#include "dmod.h"

static pthread_mutex_t g_rand_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_state;
static bool g_seeded = false;

/**
 * @brief Seed the xorshift generator on first use
 *
 * Reads a seed from /dev/urandom when available; falls back to a mix of
 * wall-clock time and PID otherwise. /dev/urandom is used instead of
 * arc4random() because arc4random() only became available in glibc 2.36
 * (2022) and this project does not assume so recent a glibc.
 */
static void seed_if_needed(void)
{
    if (g_seeded) return;

    FILE* f = fopen("/dev/urandom", "rb");
    if (f != NULL && fread(&g_state, sizeof(g_state), 1, f) == 1)
    {
        fclose(f);
    }
    else
    {
        if (f != NULL) fclose(f);
        g_state = (uint32_t)time(NULL) ^ (uint32_t)getpid();
    }

    // xorshift32 is degenerate at state 0 (it would stay 0 forever), so
    // nudge it to a fixed non-zero constant in that unlikely case.
    if (g_state == 0) g_state = 0x9E3779B9u;

    g_seeded = true;
}

static uint32_t next_xorshift(void)
{
    g_state ^= g_state << 13;
    g_state ^= g_state >> 17;
    g_state ^= g_state << 5;
    return g_state;
}

//==============================================================================
//                              Random Number API Implementation
//==============================================================================

/**
 * @brief Get a pseudo-random 32-bit value
 *
 * Not cryptographically secure - see DMOSI_RAND_API group documentation in
 * dmosi.h.
 *
 * @return uint32_t Pseudo-random value
 */
DMOD_INPUT_API_DECLARATION( dmosi, 1.0, uint32_t, _rand32, (void) )
{
    pthread_mutex_lock(&g_rand_mutex);
    seed_if_needed();
    uint32_t value = next_xorshift();
    pthread_mutex_unlock(&g_rand_mutex);
    return value;
}

/**
 * @brief Fill a buffer with pseudo-random bytes
 *
 * Not cryptographically secure - see DMOSI_RAND_API group documentation in
 * dmosi.h.
 *
 * @param buffer Output buffer, at least `len` bytes
 * @param len    Number of bytes to write
 */
DMOD_INPUT_API_DECLARATION( dmosi, 1.0, void, _rand_bytes, (uint8_t* buffer, size_t len) )
{
    if (buffer == NULL) return;

    pthread_mutex_lock(&g_rand_mutex);
    seed_if_needed();
    for (size_t i = 0; i < len; i++)
    {
        if (i % 4 == 0) g_state = next_xorshift();
        buffer[i] = (uint8_t)(g_state >> (8 * (i % 4)));
    }
    pthread_mutex_unlock(&g_rand_mutex);
}
