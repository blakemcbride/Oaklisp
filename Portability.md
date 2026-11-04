Portability (Mac/Linux/Windows)
================================

All platform-specific code is abstracted behind portable
headers.  See `Windows.md` for Windows-specific build details.


Portability Headers
====================

All headers are header-only (inline functions and macros) with zero
runtime cost.

oak-atomic.h — Portable atomic operations
------------------------------------------

Provides a uniform API for atomic operations across compilers:

    OAK_ATOMIC_FETCH_OR_U64(ptr, val) — relaxed atomic OR on uint64_t
    OAK_ATOMIC_LOAD_REF(ptr)          — acquire load on ref_t/size_t
    OAK_ATOMIC_STORE_REF(ptr, val)    — release store on ref_t/size_t
    OAK_UNLIKELY(x) / OAK_LIKELY(x)  — branch prediction hints
    CLZ64(x) / CTZ64(x)              — count leading/trailing zeros

Three-tier strategy:
1. C11 `<stdatomic.h>` (detected via `__STDC_VERSION__` or
   `HAVE_STDATOMIC_H` from configure)
2. GCC/Clang `__atomic_*` builtins (fallback)
3. MSVC `_Interlocked*` intrinsics (fallback)


oak-thread.h — Portable thread primitives
------------------------------------------

Abstracts the threading API with two backends:

    oak_mutex_t               pthread_mutex_t or CRITICAL_SECTION
    oak_mutex_init(m)         static or dynamic init
    oak_mutex_lock(m)
    oak_mutex_unlock(m)
    oak_mutex_trylock(m)      returns 0 on success (like pthread)
    oak_mutex_destroy(m)

    oak_cond_t                pthread_cond_t or CONDITION_VARIABLE
    oak_cond_init(cv)
    oak_cond_wait(cv, m)
    oak_cond_broadcast(cv)
    oak_cond_destroy(cv)

    oak_tls_key_t             pthread_key_t or DWORD (FlsAlloc)
    oak_tls_create(key, dtor)
    oak_tls_set(key, val)
    oak_tls_get(key)

    oak_thread_t              pthread_t or HANDLE
    oak_thread_create(t, fn, arg)
    oak_thread_yield()        sched_yield() or SwitchToThread()

Static initializer approach:
- On POSIX: `PTHREAD_MUTEX_INITIALIZER` / `PTHREAD_COND_INITIALIZER`.
- On Windows: `OAK_NEEDS_DYNAMIC_MUTEX_INIT` flag triggers explicit
  initialization via `oak_threads_system_init()` (called from main)
  and `gc_safepoint_init()`.

MinGW-w64 ships with winpthreads, so MinGW builds use the POSIX
path.  Only MSVC builds need the native Windows thread path.


oak-getopt.h — Portable command-line parsing
---------------------------------------------

Provides `getopt_long_only()`, `struct option`, `optarg`, `optind`,
`no_argument`, and `required_argument`.  On POSIX/MinGW, includes
the system `<getopt.h>`.  On MSVC, provides a self-contained
implementation.


signals.h — Portable signal/longjmp
-------------------------------------

Provides `oak_jmp_buf`, `oak_setjmp()`, `oak_longjmp()`.  On POSIX
these expand to `sigjmp_buf`/`sigsetjmp`/`siglongjmp`.  On MSVC
they expand to `jmp_buf`/`setjmp`/`longjmp`.

The crash handler uses `sigaction` with `SA_RESETHAND` on POSIX, and
plain `signal()` on MSVC (which auto-deregisters on MSVC).  `SIGBUS`
is handled on POSIX only (does not exist on Windows).


Platform-Specific Notes
========================

**`_REENTRANT`** — Set in nearly every .c file.  Harmless on all
platforms; no effect on Windows, conventional on POSIX.

**`__WORDSIZE`** — glibc-specific; not available on MSVC or some macOS
configurations.  configure.ac provides a fallback definition in
`config.h` based on `sizeof(void *)`.

**Integer types** — the standard `uint8_t`, `uint16_t`, `uint32_t`
are used throughout, never the BSD `u_int*_t` names.

**No direct pthread calls** outside `oak-thread.h`, and no direct GCC
atomic builtins or `__builtin_expect` outside `oak-atomic.h`.


Design Decisions
=================

**C11 stdatomic as the primary target.**  C11 `<stdatomic.h>` is
supported by GCC 4.9+, Clang 3.1+, and MSVC 17.5+
(VS 2022 17.5).  GCC builtins serve as a fallback for older
GCC/Clang, and MSVC intrinsics cover older MSVC.

**Thin wrapper, not a library.**  The portability layer is
header-only (inline functions and macros).  This avoids adding new
.c files to the build and keeps the abstraction cost zero at runtime.

**MinGW gets pthreads path.**  MinGW-w64 ships with winpthreads, so
MinGW builds use the POSIX path.  Only MSVC builds need the native
Windows thread path.

**Autotools only.**  The project builds with autotools.  Windows
builds with MSVC could use a separate `Makefile.msvc` or CMake, but
the portability layer aims at C code that compiles on all platforms,
not at a second build system.


Where the Portable Code Lives
=============================

Portability headers:
- `src/emulator/oak-atomic.h` — portable atomics, bit intrinsics,
  branch hints
- `src/emulator/oak-getopt.h` — portable getopt_long_only (system
  header on POSIX, self-contained on MSVC)
- `src/emulator/oak-thread.h` — portable thread primitives (POSIX +
  Windows)

Their users:
- `configure.ac` — `__WORDSIZE` fallback, stdatomic.h/windows.h
  detection, graceful pthread handling
- `src/emulator/Makefile.am` — the headers in the source list;
  THREADS and USE_MARK_SWEEP from configure (`--enable-threads`,
  `--disable-mark-sweep`)
- `src/emulator/data.h` — no `<pthread.h>`, standard integer types,
  guarded `<unistd.h>`, ALLOCATE_PROT macros on the portable layer
- `src/emulator/threads.h`, `threads.c` — thread types and calls
  through oak-thread.h, `oak_threads_system_init()`
- `src/emulator/gc-ms.c`, `gc-ms.h` — thread, scheduling and atomic
  calls, the SATB_BARRIER macro
- `src/emulator/gc.c`, `gc.h` — thread-local lookups, the gc_lock type
- `src/emulator/loop.c` — thread, scheduling, atomic and signal calls
- `src/emulator/oaklisp.c` — TLS calls; no `<pthread.h>` or
  `<sys/wait.h>`
- `src/emulator/stacks.c` — thread-local lookups
- `src/emulator/cmdline.c` — thread-local lookups, oak-getopt.h
- `src/emulator/signals.c` — full Windows signal path
- `src/emulator/signals.h` — oak_jmp_buf/oak_setjmp/oak_longjmp
  abstraction
- `src/emulator/oak-cold-linker.c` — strdup compat macro for MSVC
