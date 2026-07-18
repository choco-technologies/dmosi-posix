#define DMOD_ENABLE_REGISTRATION
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>
#include "dmod.h"
#include "dmod_sal.h"
#include "dmosi.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT( condition, test_name )                       \
    do {                                                          \
        if( condition )                                           \
        {                                                         \
            printf( "  \xE2\x9C\x93 PASS: %s\n", test_name );   \
            tests_passed++;                                       \
        }                                                         \
        else                                                      \
        {                                                         \
            printf( "  \xE2\x9C\x97 FAIL: %s\n", test_name );   \
            tests_failed++;                                       \
        }                                                         \
    } while( 0 )

/* =========================================================================
 * Mutex tests
 * ========================================================================= */
static void test_mutex( void )
{
    printf( "\n=== Testing mutex ===\n" );

    dmosi_mutex_t m = dmosi_mutex_create( false );
    TEST_ASSERT( m != NULL, "Create non-recursive mutex" );

    TEST_ASSERT( dmosi_mutex_lock( m ) == 0, "Lock non-recursive mutex" );
    TEST_ASSERT( dmosi_mutex_unlock( m ) == 0, "Unlock non-recursive mutex" );

    dmosi_mutex_destroy( m );
    TEST_ASSERT( true, "Destroy non-recursive mutex" );

    dmosi_mutex_t rm = dmosi_mutex_create( true );
    TEST_ASSERT( rm != NULL, "Create recursive mutex" );

    TEST_ASSERT( dmosi_mutex_lock( rm ) == 0, "Lock recursive mutex (1st)" );
    TEST_ASSERT( dmosi_mutex_lock( rm ) == 0, "Lock recursive mutex (2nd)" );
    TEST_ASSERT( dmosi_mutex_unlock( rm ) == 0, "Unlock recursive mutex (1st)" );
    TEST_ASSERT( dmosi_mutex_unlock( rm ) == 0, "Unlock recursive mutex (2nd)" );

    dmosi_mutex_destroy( rm );

    TEST_ASSERT( dmosi_mutex_lock( NULL ) == -EINVAL, "Lock NULL mutex returns -EINVAL" );
    TEST_ASSERT( dmosi_mutex_unlock( NULL ) == -EINVAL, "Unlock NULL mutex returns -EINVAL" );
    dmosi_mutex_destroy( NULL );
    TEST_ASSERT( true, "Destroy NULL mutex does not crash" );
}

/* =========================================================================
 * Semaphore tests
 * ========================================================================= */
static void test_semaphore( void )
{
    printf( "\n=== Testing semaphore ===\n" );

    dmosi_semaphore_t s = dmosi_semaphore_create( 1, 5 );
    TEST_ASSERT( s != NULL, "Create counting semaphore (initial=1, max=5)" );

    TEST_ASSERT( dmosi_semaphore_wait( s, 1, 0 ) == 0,
                 "Wait on semaphore with count=1 succeeds" );

    TEST_ASSERT( dmosi_semaphore_wait( s, 1, 0 ) == -EAGAIN,
                 "Wait on semaphore with count=0, no timeout returns -EAGAIN" );

    TEST_ASSERT( dmosi_semaphore_post( s, 1 ) == 0, "Post to semaphore" );

    TEST_ASSERT( dmosi_semaphore_wait( s, 1, 100 ) == 0,
                 "Wait on semaphore with timeout=100ms succeeds" );

    for( int i = 0; i < 5; i++ )
    {
        dmosi_semaphore_post( s, 1 );
    }
    TEST_ASSERT( dmosi_semaphore_post( s, 1 ) == -EOVERFLOW,
                 "Post beyond max_count returns -EOVERFLOW" );

    dmosi_semaphore_destroy( s );

    TEST_ASSERT( dmosi_semaphore_create( 0, 0 ) == NULL,
                 "Create semaphore with max_count=0 returns NULL" );
    TEST_ASSERT( dmosi_semaphore_create( 5, 3 ) == NULL,
                 "Create semaphore with initial_count>max_count returns NULL" );

    TEST_ASSERT( dmosi_semaphore_wait( NULL, 1, 0 ) == -EINVAL,
                 "Wait on NULL semaphore returns -EINVAL" );
    TEST_ASSERT( dmosi_semaphore_post( NULL, 1 ) == -EINVAL,
                 "Post to NULL semaphore returns -EINVAL" );
    dmosi_semaphore_destroy( NULL );
    TEST_ASSERT( true, "Destroy NULL semaphore does not crash" );
}

/* =========================================================================
 * Queue tests
 * ========================================================================= */
static void test_queue( void )
{
    printf( "\n=== Testing queue ===\n" );

    int item = 42;
    int received = 0;

    dmosi_queue_t q = dmosi_queue_create( sizeof( int ), 5 );
    TEST_ASSERT( q != NULL, "Create queue (item_size=4, length=5)" );

    TEST_ASSERT( dmosi_queue_send( q, &item, 0 ) == 0,
                 "Send item to queue" );
    TEST_ASSERT( dmosi_queue_receive( q, &received, 0 ) == 0,
                 "Receive item from queue" );
    TEST_ASSERT( received == 42, "Received value matches sent value" );

    for( int i = 0; i < 5; i++ )
    {
        dmosi_queue_send( q, &i, 0 );
    }
    TEST_ASSERT( dmosi_queue_send( q, &item, 0 ) == -EAGAIN,
                 "Send to full queue (no timeout) returns -EAGAIN" );

    for( int i = 0; i < 5; i++ )
    {
        dmosi_queue_receive( q, &received, 0 );
    }
    TEST_ASSERT( dmosi_queue_receive( q, &received, 0 ) == -EAGAIN,
                 "Receive from empty queue (no timeout) returns -EAGAIN" );

    TEST_ASSERT( dmosi_queue_send( q, NULL, 0 ) == -EINVAL,
                 "Send NULL item returns -EINVAL" );
    TEST_ASSERT( dmosi_queue_receive( q, NULL, 0 ) == -EINVAL,
                 "Receive into NULL buffer returns -EINVAL" );

    dmosi_queue_destroy( q );

    TEST_ASSERT( dmosi_queue_create( 0, 5 ) == NULL,
                 "Create queue with item_size=0 returns NULL" );
    TEST_ASSERT( dmosi_queue_create( sizeof( int ), 0 ) == NULL,
                 "Create queue with queue_length=0 returns NULL" );

    TEST_ASSERT( dmosi_queue_send( NULL, &item, 0 ) == -EINVAL,
                 "Send to NULL queue returns -EINVAL" );
    TEST_ASSERT( dmosi_queue_receive( NULL, &received, 0 ) == -EINVAL,
                 "Receive from NULL queue returns -EINVAL" );
    dmosi_queue_destroy( NULL );
    TEST_ASSERT( true, "Destroy NULL queue does not crash" );
}

/* =========================================================================
 * Timer tests
 * ========================================================================= */
static volatile int g_timer_callback_count = 0;

static void timer_callback( void * arg )
{
    ( void ) arg;
    g_timer_callback_count++;
}

static void test_timer( void )
{
    printf( "\n=== Testing timer ===\n" );

    dmosi_timer_t one_shot = dmosi_timer_create( timer_callback, NULL, 100, false );
    TEST_ASSERT( one_shot != NULL, "Create one-shot timer" );
    dmosi_timer_destroy( one_shot );

    g_timer_callback_count = 0;
    dmosi_timer_t t = dmosi_timer_create( timer_callback, NULL, 50, true );
    TEST_ASSERT( t != NULL, "Create auto-reload timer (period=50ms)" );

    TEST_ASSERT( dmosi_timer_start( t ) == 0, "Start timer" );

    dmosi_thread_sleep( 200 );
    TEST_ASSERT( g_timer_callback_count >= 2,
                 "Auto-reload timer fires at least twice in 200ms" );

    TEST_ASSERT( dmosi_timer_stop( t ) == 0, "Stop timer" );
    int count_after_stop = g_timer_callback_count;
    dmosi_thread_sleep( 100 );
    TEST_ASSERT( g_timer_callback_count == count_after_stop,
                 "Stopped timer does not fire any more" );

    TEST_ASSERT( dmosi_timer_reset( t ) == 0, "Reset timer" );
    dmosi_thread_sleep( 200 );
    TEST_ASSERT( g_timer_callback_count > count_after_stop,
                 "Timer fires again after reset" );

    TEST_ASSERT( dmosi_timer_set_period( t, 100 ) == 0,
                 "Change timer period to 100ms" );
    TEST_ASSERT( dmosi_timer_get_period( t ) == 100,
                 "Get timer period returns 100ms after change" );

    TEST_ASSERT( dmosi_timer_stop( t ) == 0, "Stop timer before destroy" );
    dmosi_timer_destroy( t );

    TEST_ASSERT( dmosi_timer_create( NULL, NULL, 100, false ) == NULL,
                 "Create timer with NULL callback returns NULL" );
    TEST_ASSERT( dmosi_timer_create( timer_callback, NULL, 0, false ) == NULL,
                 "Create timer with period_ms=0 returns NULL" );

    TEST_ASSERT( dmosi_timer_start( NULL ) == -EINVAL,
                 "Start NULL timer returns -EINVAL" );
    TEST_ASSERT( dmosi_timer_stop( NULL ) == -EINVAL,
                 "Stop NULL timer returns -EINVAL" );
    TEST_ASSERT( dmosi_timer_reset( NULL ) == -EINVAL,
                 "Reset NULL timer returns -EINVAL" );
    TEST_ASSERT( dmosi_timer_set_period( NULL, 100 ) == -EINVAL,
                 "Set period on NULL timer returns -EINVAL" );
    TEST_ASSERT( dmosi_timer_get_period( NULL ) == 0,
                 "Get period of NULL timer returns 0" );
    dmosi_timer_destroy( NULL );
    TEST_ASSERT( true, "Destroy NULL timer does not crash" );
}

/* =========================================================================
 * Thread tests
 * ========================================================================= */
static volatile bool g_thread_ran = false;

static void simple_thread_entry( void * arg )
{
    ( void ) arg;
    g_thread_ran = true;
}

static void slow_thread_entry( void * arg )
{
    ( void ) arg;
    /* "Forever" - used to test kill */
    dmosi_thread_sleep( 60000 );
}

static void test_thread( void )
{
    printf( "\n=== Testing thread ===\n" );

    dmosi_thread_t current = dmosi_thread_current();
    TEST_ASSERT( current != NULL, "Get current thread returns non-NULL" );

    const char * name = dmosi_thread_get_name( current );
    TEST_ASSERT( name != NULL, "Get current thread name returns non-NULL" );

    const char * name_null = dmosi_thread_get_name( NULL );
    TEST_ASSERT( name_null != NULL,
                 "Get thread name with NULL handle returns non-NULL" );

    int prio = dmosi_thread_get_priority( current );
    TEST_ASSERT( prio >= 0, "Get current thread priority >= 0" );

    dmosi_process_t proc = dmosi_thread_get_process( current );
    TEST_ASSERT( proc != NULL, "Get current thread's process returns non-NULL" );

    const char * mod = dmosi_thread_get_module_name( current );
    TEST_ASSERT( mod != NULL, "Get current thread module name returns non-NULL" );

    dmosi_thread_sleep( 10 );
    TEST_ASSERT( true, "Thread sleep(10ms) does not crash" );

    g_thread_ran = false;
    dmosi_thread_t t = dmosi_thread_create(
        simple_thread_entry, NULL, 1, 65536, "simple_t", NULL );
    TEST_ASSERT( t != NULL, "Create simple thread" );

    TEST_ASSERT( dmosi_thread_join( t ) == 0, "Join completed thread returns 0" );
    TEST_ASSERT( g_thread_ran == true, "Thread entry function was executed" );

    /* Re-joining must fail *before* the handle is destroyed - joining after
     * destroy() would read already-freed memory, which is undefined
     * behaviour for any caller, not something the API is expected to
     * tolerate. */
    TEST_ASSERT( dmosi_thread_join( t ) == -EINVAL,
                 "Join already-joined thread returns -EINVAL" );

    dmosi_thread_destroy( t );

    dmosi_thread_t slow = dmosi_thread_create(
        slow_thread_entry, NULL, 1, 65536, "slow_t", NULL );
    TEST_ASSERT( slow != NULL, "Create slow thread for kill test" );
    TEST_ASSERT( dmosi_thread_kill( slow, 0 ) == 0,
                 "Kill running thread returns 0" );
    TEST_ASSERT( dmosi_thread_join( slow ) == 0,
                 "Join killed thread returns 0" );
    dmosi_thread_destroy( slow );

    size_t count = dmosi_thread_get_all( NULL, 0 );
    TEST_ASSERT( count >= 1, "thread_get_all count >= 1" );

    size_t proc_count = dmosi_thread_get_by_process( proc, NULL, 0 );
    TEST_ASSERT( proc_count >= 1, "thread_get_by_process count >= 1" );

    dmosi_thread_info_t info;
    int info_ret = dmosi_thread_get_info( current, &info );
    TEST_ASSERT( info_ret == 0, "thread_get_info returns 0 for current thread" );
    TEST_ASSERT( info.state == DMOSI_THREAD_STATE_RUNNING,
                 "thread_get_info: current thread state is RUNNING" );
    TEST_ASSERT( info.cpu_usage >= 0.0f && info.cpu_usage <= 100.0f,
                 "thread_get_info: cpu_usage in [0, 100]" );

    dmosi_thread_info_t info_null;
    TEST_ASSERT( dmosi_thread_get_info( NULL, &info_null ) == 0,
                 "thread_get_info with NULL thread returns 0" );
    TEST_ASSERT( info_null.state == DMOSI_THREAD_STATE_RUNNING,
                 "thread_get_info with NULL: state is RUNNING" );

    TEST_ASSERT( dmosi_thread_get_info( current, NULL ) == -EINVAL,
                 "thread_get_info with NULL info returns -EINVAL" );

    TEST_ASSERT( dmosi_thread_join( NULL ) == -EINVAL,
                 "Join NULL thread returns -EINVAL" );
    TEST_ASSERT( dmosi_thread_kill( NULL, 0 ) == -EINVAL,
                 "Kill NULL thread returns -EINVAL" );
    TEST_ASSERT( dmosi_thread_get_name( NULL ) != NULL,
                 "Get name with NULL (returns current thread name)" );
    TEST_ASSERT( dmosi_thread_get_priority( NULL ) >= 0,
                 "Get priority with NULL (returns current thread priority)" );
    TEST_ASSERT( dmosi_thread_get_process( NULL ) != NULL,
                 "Get process with NULL (returns current thread's process)" );
    TEST_ASSERT( dmosi_thread_get_module_name( NULL ) != NULL,
                 "Get module name with NULL (returns current thread's module name)" );
    dmosi_thread_destroy( NULL );
    TEST_ASSERT( true, "Destroy NULL thread does not crash" );

    size_t left_stack = Dmod_GetLeftStackSize();
    TEST_ASSERT( left_stack > 0 && left_stack != SIZE_MAX,
                 "Dmod_GetLeftStackSize returns non-zero, non-SIZE_MAX value" );
    TEST_ASSERT( left_stack < ( size_t )1 * 1024 * 1024 * 1024,
                 "Dmod_GetLeftStackSize returns a value below 1 GiB" );
}

/* =========================================================================
 * Tick count tests
 * ========================================================================= */
static void test_tick_count( void )
{
    printf( "\n=== Testing tick count ===\n" );

    uint32_t t1 = dmosi_get_tick_count();

    dmosi_thread_sleep( 50 );
    uint32_t t2 = dmosi_get_tick_count();
    TEST_ASSERT( t2 > t1, "dmosi_get_tick_count() advances after delay" );

    uint32_t elapsed = t2 - t1;
    TEST_ASSERT( elapsed >= 40 && elapsed <= 500,
                 "dmosi_get_tick_count() returns time in milliseconds (not raw ticks)" );
}

/* =========================================================================
 * is_started / interrupt API tests
 * ========================================================================= */
static void test_is_started( void )
{
    printf( "\n=== Testing is_started ===\n" );

    TEST_ASSERT( dmosi_is_started() == true,
                 "dmosi_is_started() returns true after dmosi_init()" );
}

static void test_interrupt_api( void )
{
    printf( "\n=== Testing interrupt API ===\n" );

    TEST_ASSERT( dmosi_get_min_interrupt_priority() == 0,
                 "dmosi_get_min_interrupt_priority() returns 0 on POSIX" );

    dmosi_context_switch_handler();
    dmosi_syscall_handler();
    dmosi_tick_handler();
    TEST_ASSERT( true, "Interrupt handler stubs do not crash" );
}

/* =========================================================================
 * Init / deinit tests
 * ========================================================================= */
static void test_init_deinit( void )
{
    printf( "\n=== Testing init / deinit ===\n" );

    TEST_ASSERT( dmosi_init() == false, "Double dmosi_init() returns false" );

    TEST_ASSERT( dmosi_deinit() == true, "dmosi_deinit() returns true" );
    TEST_ASSERT( dmosi_init() == true, "dmosi_init() succeeds after deinit" );
}

/* =========================================================================
 * main
 * ========================================================================= */
int main( void )
{
    printf( "========================================\n" );
    printf( "  DMOSI POSIX Implementation Tests\n" );
    printf( "========================================\n" );

    if( !dmosi_init() )
    {
        printf( "ERROR: dmosi_init() failed\n" );
        return 1;
    }

    test_mutex();
    test_semaphore();
    test_queue();
    test_timer();
    test_thread();
    test_tick_count();
    test_is_started();
    test_interrupt_api();
    test_init_deinit();

    dmosi_deinit();

    printf( "\n========================================\n" );
    printf( "  Test Summary\n" );
    printf( "========================================\n" );
    printf( "Total tests: %d\n", tests_passed + tests_failed );
    printf( "Passed:      %d\n", tests_passed );
    printf( "Failed:      %d\n", tests_failed );
    printf( "========================================\n" );

    if( tests_failed == 0 )
    {
        printf( "\n\xE2\x9C\x93 ALL TESTS PASSED\n\n" );
        return 0;
    }

    printf( "\n\xE2\x9C\x97 SOME TESTS FAILED\n\n" );
    return 1;
}
