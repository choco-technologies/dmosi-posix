# dmosi-posix Documentation

Welcome to the `dmosi-posix` documentation.

## Contents

- **[api-reference.md](api-reference.md)** - How each `dmosi.h` API group is
  implemented on top of POSIX (`pthread`, condition variables, `clock_gettime`,
  ...), including behaviors that differ from an RTOS backend such as
  [dmosi-freertos](https://github.com/choco-technologies/dmosi-freertos)
- **[architecture.md](architecture.md)** - How the pieces fit together: the
  thread registry, the timer worker-thread design, and how process management
  is reused from [dmosi-proc](https://github.com/choco-technologies/dmosi-proc)

See the top-level [README.md](../README.md) for building instructions and a
quick-start usage example.

## Quick Reference

```c
#include "dmosi.h"

int main(void)
{
    if (!dmosi_init()) {
        return 1;
    }

    dmosi_mutex_t mutex = dmosi_mutex_create(false);
    dmosi_mutex_lock(mutex);
    /* critical section */
    dmosi_mutex_unlock(mutex);
    dmosi_mutex_destroy(mutex);

    dmosi_deinit();
    return 0;
}
```
