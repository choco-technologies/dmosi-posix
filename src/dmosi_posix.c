#include <stdbool.h>
#include "dmosi.h"
#include "dmod.h"

extern void dmosi_thread_set_init_process(dmosi_process_t process);
extern void dmosi_thread_unregister_current(void);

static dmosi_process_t g_system_process = NULL;
static volatile bool g_started = false;

//==============================================================================
//                              Initialization API Implementation
//==============================================================================

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, bool, _init, (void) )
{
    if (g_system_process != NULL) {
        DMOD_LOG_ERROR("dmosi already initialized\n");
        return false;
    }

    g_system_process = dmosi_process_create("system", DMOSI_SYSTEM_MODULE_NAME, NULL);
    if (g_system_process == NULL) {
        DMOD_LOG_ERROR("Failed to create system process\n");
        return false;
    }

    // POSIX threads run immediately (there is no separate "start the scheduler"
    // step like FreeRTOS's vTaskStartScheduler()), so dmosi_is_started() can be
    // made true right away. g_init_process still needs to be set before the very
    // first dmosi_thread_current() call below, to break the bootstrap recursion
    // dmosi_thread_current() -> dmosi_process_current() -> dmosi_thread_current()
    // that would otherwise occur before any thread is registered.
    dmosi_thread_set_init_process(g_system_process);
    g_started = true;

    if (dmosi_thread_current() == NULL) {
        DMOD_LOG_ERROR("Failed to bootstrap current thread\n");
        dmosi_thread_set_init_process(NULL);
        g_started = false;
        dmosi_process_destroy(g_system_process);
        g_system_process = NULL;
        return false;
    }

    dmosi_thread_set_init_process(NULL);
    return true;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, bool, _is_started, (void) )
{
    return g_started;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, bool, _deinit, (void) )
{
    if (g_system_process == NULL) {
        return true;
    }

    dmosi_thread_set_init_process(NULL);
    dmosi_thread_unregister_current();
    dmosi_process_destroy(g_system_process);
    g_system_process = NULL;
    g_started = false;

    return true;
}
