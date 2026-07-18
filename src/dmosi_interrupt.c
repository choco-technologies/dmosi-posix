#include <stdint.h>
#include "dmosi.h"

//==============================================================================
//                              Interrupt Handler API Implementation
//==============================================================================
//
// POSIX processes have no notion of hardware interrupt context or NVIC-style
// priority levels, so the three RTOS-essential handlers are no-ops and the
// minimum interrupt priority is unconditionally 0 - exactly the fallback
// documented in dmosi.h for backends with no such concept, meaning any
// "interrupt priority" is safe to use.
//

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, void, _context_switch_handler, (void) )
{
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, void, _syscall_handler, (void) )
{
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, void, _tick_handler, (void) )
{
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, uint32_t, _get_min_interrupt_priority, (void) )
{
    return 0;
}
