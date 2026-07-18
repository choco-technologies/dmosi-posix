# dmosi-posix

POSIX implementation of [DMOSI](https://github.com/choco-technologies/dmosi) (DMOD OS Interface).

## Overview

`dmosi-posix` implements the DMOSI abstraction layer on top of standard POSIX
threading primitives (`pthread`, condition variables, `clock_gettime`),
letting the [DMOD](https://github.com/choco-technologies/dmod) framework and
any module built on top of DMOSI run on a regular Linux/POSIX host instead of
an RTOS - useful for host-side testing, simulation, and tooling.

It plays the same role for POSIX hosts that
[dmosi-freertos](https://github.com/choco-technologies/dmosi-freertos) plays
for FreeRTOS targets: it provides strong implementations of the weak
`dmosi_*` functions declared by `dmosi`, and is built as a **plain static
library** (`libdmosi_posix.a`), not a loadable DMOD module.

## Features

- **Mutex API** - `pthread_mutex_t`-backed, recursive or non-recursive
- **Semaphore API** - bounded counting semaphore (mutex + condition variable),
  supporting multi-unit wait/post, timeouts, and `-EOVERFLOW` on overflow
- **Thread API** - backed by real `pthread_t` threads; a small in-process
  registry stands in for the enumeration/lookup APIs (`dmosi_thread_get_all`,
  `dmosi_thread_get_by_process`) that an RTOS scheduler would otherwise
  provide directly
- **Process API** - reused as-is from
  [dmosi-proc](https://github.com/choco-technologies/dmosi-proc) (vendored as
  a git submodule under `lib/dmosi-proc`), since process/thread bookkeeping in
  that module has no RTOS-specific code
- **Queue API** - bounded ring-buffer FIFO (mutex + condition variables)
- **Timer API** - each timer owns a dedicated worker thread waiting on a
  condition variable, giving deterministic start/stop/reset semantics (see
  [docs/architecture.md](docs/architecture.md#timers) for why this was chosen
  over POSIX interval timers)
- **Interrupt Handler API** - no-op stubs; `dmosi_get_min_interrupt_priority()`
  returns `0`, matching the fallback documented in `dmosi.h` for backends with
  no interrupt-priority concept
- **System Time API** - millisecond tick count derived from
  `CLOCK_MONOTONIC`

## Building

This project uses CMake (>= 3.10) and fetches `dmod`/`dmosi` automatically via
`FetchContent`. Process management comes from the `lib/dmosi-proc` git
submodule.

```sh
git clone --recurse-submodules https://github.com/choco-technologies/dmosi-posix.git
cd dmosi-posix
cmake -B build
cmake --build build
```

This produces `build/libdmosi_posix.a`.

If you already have a checkout without submodules initialized:

```sh
git submodule update --init --recursive
```

### Building with tests

```sh
cmake -B build -DDMOSI_POSIX_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Usage

Link `dmosi_posix` (which transitively pulls in `dmod`, `dmosi`,
`dmosi_proc`, and `pthread`) into your executable, call `dmosi_init()` once at
startup, and use the `dmosi_*` API from [`dmosi.h`](
https://github.com/choco-technologies/dmosi/blob/main/include/dmosi.h):

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

```cmake
target_link_libraries(your_target PRIVATE dmosi_posix)
```

## Documentation

- **[docs/api-reference.md](docs/api-reference.md)** - how each `dmosi.h` API
  group is implemented on POSIX, and behaviors that differ from an RTOS
  backend (thread priority, stack usage reporting, `dmosi_thread_kill()`
  semantics, timer determinism, ...)
- **[docs/architecture.md](docs/architecture.md)** - how the pieces fit
  together: the thread registry, the timer worker-thread design (and why it
  replaced an earlier POSIX-interval-timer attempt), and process management
  reuse

## Developing in VS Code

Opening this folder in VS Code picks up `.vscode/tasks.json`, which wires up
the usual build/test loop as tasks (**Ctrl+Shift+B** / **Terminal > Run
Task...**):

- **CMake Configure** / **CMake Build** / **CMake Clean**
- **Build Tests** (configures with `-DDMOSI_POSIX_BUILD_TESTS=ON` and builds)
- **Run Tests** (builds tests, then runs `ctest`)
- **Update Submodules** (`git submodule update --init --recursive`)

`.vscode/launch.json` provides a **Debug Tests** configuration (gdb) that
builds the tests and launches `build/tests/dmosi_posix_tests` under the
debugger. Recommended extensions (`.vscode/extensions.json`) include the
CMake Tools and C/C++ extensions used for IntelliSense (backed by
`build/compile_commands.json`).

## Project Structure

```
dmosi-posix/
├── CMakeLists.txt        # FetchContent's dmod + dmosi, builds libdmosi_posix.a
├── dmosi-posix.dmr       # Release package resource mapping (used by mkdmrpkg)
├── manifest.dmm          # DMOD manifest entry
├── src/
│   ├── dmosi_posix.c      # _init / _deinit / _is_started
│   ├── dmosi_mutex.c
│   ├── dmosi_thread.c
│   ├── dmosi_semaphore.c
│   ├── dmosi_queue.c
│   ├── dmosi_timer.c
│   ├── dmosi_time.c
│   └── dmosi_interrupt.c
├── lib/
│   └── dmosi-proc/        # git submodule - process API implementation
├── tests/
│   ├── CMakeLists.txt
│   ├── main.c
│   └── main.ld
├── docs/
│   ├── README.md            # Documentation index
│   ├── api-reference.md     # POSIX-specific API behavior
│   └── architecture.md      # Design notes (thread registry, timers, ...)
├── .vscode/                # Build/debug tasks for VS Code
├── .github/workflows/
│   ├── build.yml           # CI: build + run tests on push/PR
│   └── release.yml         # Release CI: mkdmrpkg-based x86_64 package
├── LICENSE
└── README.md
```

## Related Projects

- [DMOD](https://github.com/choco-technologies/dmod) - Dynamic Module System
- [DMOSI](https://github.com/choco-technologies/dmosi) - DMOD OS Interface (this library implements it)
- [dmosi-proc](https://github.com/choco-technologies/dmosi-proc) - Process API implementation, vendored here
- [dmosi-freertos](https://github.com/choco-technologies/dmosi-freertos) - The equivalent FreeRTOS implementation

## License

See [LICENSE](LICENSE) file for details.
