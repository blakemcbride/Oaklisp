Thread Support in Oaklisp
==========================

Status: the interpreter and the bytecode compiler work with threads
(`make check` passes in every configuration, `threads-stress.test`
included); natively compiled code does not run threaded yet.
Disabled by default.

Enable with: `./configure --enable-threads`

Portable across Linux, macOS, and Windows (see `Portability.md` and
`Windows.md`).


Architecture
============

Two-level concurrency model:

1. **Heavyweight threads** (native OS threads) — each with its own VM
   registers, value stack, context stack, and GC buffer.  Created via
   `%make-heavyweight-thread` (opcode 70).  Uses pthreads on
   POSIX/MinGW, native Win32 threads on MSVC (via `oak-thread.h`).

2. **Lightweight processes** (green threads) — Oaklisp-level
   continuations multiplexed within a single native thread via a
   cooperative scheduler with timer-driven preemption.  Context switch
   via `pause()` using `call/cc`.

Each native thread runs its own lightweight process scheduler.  The
global scheduler queue (`*scheduleQ*`) is shared across all threads,
protected by a mutex.


C Emulator (`src/emulator/`)
============================

Key files and `#ifdef THREADS` sections:

| File | Thread-related content |
|------|----------------------|
| `threads.h` | Mutex declarations, `THREADY()` macro, TLS key, `oak_threads_system_init()` |
| `threads.c` | `create_thread()`, `init_thread()`, `wait_for_gc()`, `set_gc_flag()`, dynamic mutex init |
| `data.h` | Per-thread register macros, `ALLOCATE_PROT` spin-lock, GC buffer arrays |
| `loop.c` | Opcodes 67-72, `POLL_GC_SIGNALS()`, thread index init |
| `gc.c` | `FORTHREADS` macro, per-thread root scanning, GC coordination |
| `oaklisp.c` | Thread-0 TLS init, per-thread stack/register allocation |
| `stacks.c` | Thread index retrieval in `init_stacks()` |
| `config.h` | `MAX_THREAD_COUNT` (200), disables move-to-front under threads |
| `gc-ms.c` | Mark-sweep GC, TLABs, TLAB retire/refill, deferred objstart buffer |
| `gc-ms.h` | MS-GC and TLAB declarations, `TLAB_SIZE` (4096 words) |

Per-thread state (via TLS `index_key`):

    register_array[my_index]           VM registers (e_pc, e_env, e_bp, ...)
    value_stack_array[my_index]        Value stack
    cntxt_stack_array[my_index]        Context stack
    gc_examine_buffer_array[my_index]  GC worklist
    tlab_cursor_array[my_index]        TLAB bump pointer (USE_MARK_SWEEP only)
    tlab_end_array[my_index]           TLAB end pointer  (USE_MARK_SWEEP only)
    satb_buffer_array[my_index][]      SATB write barrier log (USE_MARK_SWEEP only)
    satb_count_array[my_index]         SATB buffer entry count (USE_MARK_SWEEP only)

Global shared state:

    Heap (new_space, old_space)     Protected by alloc_lock
    free_point, new_space.end       Modified under alloc_lock
    gc_pending, gc_ready[]          GC coordination flags
    objstart_bitmap, mark_bitmap    GC bitmaps (USE_MARK_SWEEP only)
    free_list_heads[]               Segregated free lists (USE_MARK_SWEEP only)

GC coordination: one thread sets `gc_pending`, all others call
`wait_for_gc()` (blocking on `gc_lock`).  Under `USE_MARK_SWEEP`, the
mark phase runs mostly concurrently with only brief STW pauses for
initial mark and remark; sweep runs lazily during allocation.  Without
`USE_MARK_SWEEP`, GC is fully stop-the-world (copying collector).

Thread-related opcodes:

| Opcode | Name | Effect |
|--------|------|--------|
| 67 | ENABLE-ALARMS | `timer_increment = 1` (enable preemption) |
| 68 | DISABLE-ALARMS | `timer_increment = 0` (disable preemption) |
| 69 | RESET-ALARM-COUNTER | `timer_counter = 0` |
| 70 | HEAVYWEIGHT-THREAD | Create native thread running given thunk; returns #t/#f |
| 71 | TEST-AND-SET-LOCATIVE | Atomic CAS on locative cell (mutex primitive) |
| 72 | THREAD-YIELD | `oak_thread_yield()` under THREADS, no-op otherwise |


Oaklisp Runtime (`src/world/`)
==============================

| File | Content |
|------|---------|
| `multi-em.oak` | Primitive ops: `%make-heavyweight-thread`, `%load-process`, `%store-process`, `%test-and-set-locative`, `%thread-yield`, `%threads-enabled?` |
| `multiproc.oak` | Queues, process descriptors, mutexes, scheduler, semaphores, futures |
| `multi-off.oak` | Guard variable `%no-threading` for cold-boot |

Synchronization primitives (all in `multiproc.oak`):

- **Mutex**: the held flag taken with `%test-and-set-locative`;
  a waiter does not spin: it runs the processes the scheduler has
  queued (`pause`, as the alarm would), or with none queued parks its
  OS thread on the mutex's waiter list (stream primitive 19, 10 ms at
  a time) until `release-mutex` unparks it; only the scheduler's own
  lock, and a mutex taken inside the scheduler's section, is waited
  for by yielding.  Reentrant, held by the process, released only by
  the holder; `with-mutex` releases on any exit; a wait of a million
  steps or five seconds parked is reported on standard error with
  the holder.  The runtime's own guarded sections go through
  `%guard`.  The scheduler's lock is held by the OS thread instead
  (`%thread-mutex`).
- **Semaphore**: Mutex + waiting queue.  Blocked processes enqueued,
  woken on `signal`.
- **Futures**: Lazy evaluation with dependent process queue;
  auto-scheduled computation.
- **Scheduler**: Global `*scheduleQ*` protected by mutex + alarm
  disable.  `pause()` does `call/cc` context switch.


Initialization
===============

Every warm boot gives the machine a process object, and from then on
fluid bindings and the winding stack are per process.  In an emulator
with threads the warm boot also runs `init-thread-infrastructure`
(`init-threads-at-boot` in `multiproc.oak`), which makes the guard
mutexes; the guards are therefore on whether or not `--pthreads`
follows.
Without threads they stay `#f` and the guarded sections cost nothing
beyond a closure.  `--pthreads N` (`setup-initial-process-object`)
then only spawns the N-1 extra OS threads, each of which starts by
giving itself a process of its own.

Then `spawn-heavyweight-threads` creates N-1 additional native threads.

On a non-THREADS emulator, `%threads-enabled?` returns nil and the
`--pthreads` option prints a warning and exits.  Without `--pthreads`,
all guard-variable mutexes remain `#f` — zero overhead.


Thread Safety
==============

The runtime's known shared data structures are protected as follows
(general hash tables are not; see below):

**C emulator (protected by mutex or volatile):**
- `signal_poll_flag` — `volatile sig_atomic_t` for signal safety
- `alloc_lock` — protects `free_point`, `new_space.end`, and
  STORE-REG cases 14/15 in `loop.c`
- `wp_lock` — protects `ref_to_wp()` and `rebuild_wp_hashtable()` in
  `weak.c`
- `dump_lock` — protects DUMP-WORLD (case 73) and LOAD-WORLD (case 74)
  in `loop.c`

**Oaklisp runtime (protected by guard-variable mutex pattern):**
- `*symbol-table-mutex*` — `intern` and `gensym` in `symbols.oak`
- `*add-method-mutex*` — `%install-method-with-env` in `kernel1-install.oak`
- `*expression-table-mutex*` — `expression-table` and `fancy-accessor-*`
  lists in `anonymous.oak`
- `*stream-primitive-mutex*` — `sp-alist` cache in `streams.oak`
- `*locale-mutex*` — `(setter variable-here?)`, `(setter macro-here?)`,
  `(setter frozen-here?)` in `locales.oak`
- `*fluid-table-mutex*` — `add-to-current-fluid-bindings` in `fluid.oak`

All guard variables are `#f` during cold/warm boot (no mutex overhead)
and initialized to `(make mutex)` in `init-thread-infrastructure`
(`multiproc.oak`) when `--pthreads` is used.

**Cold-world files:** the guards in cold-world files (symbols,
kernel1-install, locales, fluid, anonymous, streams) are plain
`if` tests of the guard variable around the mutex calls, since those
files run long before `multiproc.oak` makes any mutex.

**User responsibility:** General user-created hash tables used
concurrently need application-level locking.

**Method cache:** `OP_TYPE_METH_CACHE` is always enabled.  Cache writes
use a seqlock pattern with `OAK_ATOMIC_STORE_REF` (release) /
`OAK_ATOMIC_LOAD_REF` (acquire).  `OP_METH_ALIST_MTF` is disabled
under threads since it mutates the shared alist structure.


Performance
===========

- **TLABs** (under `USE_MARK_SWEEP + THREADS`): Most allocations use
  a per-thread bump pointer with no lock.  Only TLAB refill and large
  allocations acquire `alloc_lock`.
- **Concurrent GC** (under `USE_MARK_SWEEP + THREADS`): The mark phase
  runs mostly concurrently (only two brief STW pauses for initial mark
  and remark), and sweep runs lazily during allocation.  The copying GC
  fallback remains STW but is only triggered for heap expansion.

See `GC.md` for full garbage collection documentation.


The API
=======

Documented in the language manual's Threads chapter
(`doc/lang/threads.tex`).  `make-thread` starts an OS thread running a
thunk as one process (a copy of the creator's fluid bindings, the
creator's winding stack); `thread-join` waits for it, parked, and
gives its value or re-signals its error; `thread-alive?`,
`thread-name`, `current-thread`, `thread-count`, `thread-sleep`.
`condition-variable` with `condition-wait` (releases the mutex, parks,
takes it again), `condition-notify`, `condition-notify-all`.
`compare-and-swap` and `fetch-and-add` on a cell.  Parking is the
emulator's: stream primitives 19 (park, with an optional timeout, the
thread at a safepoint for the collector throughout), 20 (unpark, with
a permit so an unpark before the park counts) and 21 (sleep).  The
scheduler threads of `--pthreads N` park the same way while the queue
is empty, and `release-scheduler` wakes one when a process is queued,
so idle threads cost nothing.  A waiting thread runs no other process
meanwhile; with one OS thread, waiting on a condition variable that
only another OS thread could notify never ends.


Shared state
============

What several threads may touch at once, and the rule that makes it
safe (ThreadUpdate.md Phase 3):

| State | Where | Rule |
|-------|-------|------|
| Symbol table, `gensym` counter | symbols.oak | `*symbol-table-mutex*`, reads and writes |
| Locales: variable tables, macro alists, frozen lists | locales.oak | writes under `*locale-mutex*`; reads unlocked, safe because a hash table's readers see one complete table (below); two installs of one name: last writer wins |
| Hash tables | hash-table.oak | `present?` takes the vector once and sizes its index by it; `resize` fills the new vector before installing it; an insert replaces a bucket's list in one store, a removal builds a new list. So lookups are safe against one writer in another thread; concurrent writers to a user's table need a mutex of the user's |
| Fluid bindings | fluid.oak | per process; installs in the global top-level list under `*fluid-table-mutex*` |
| Winding stack, pending rewind | unwind-protect.oak | per process, a new process starting with its creator's (its extent begins inside the creator's). A throw to another process's catch (an error in a forced promise, handled by the forcer) unwinds the thrower's own windings as the thrower, then restores the catcher as the current process and unwinds its windings to the catch |
| Error handlers | error2.oak | a fluid, so per process |
| Method cache | loop.c | striped seqlock; `OP_METH_ALIST_MTF` off |
| Operation-method alists, bp alists | kernel1-install.oak | `%install-method-with-env` under `*add-method-mutex*`; readers walk lists that only grow at the head or have a cdr replaced |
| Mixed-type cache | mix-types.oak | `*mix-types-mutex*` around the walk and the insert; the type is made outside it |
| Expression table, fancy accessors | anonymous.oak | `*expression-table-mutex*` (reentrant: evaluation inside can print) |
| Stream-primitive cache | streams.oak | `*stream-primitive-mutex*` |
| Output streams | format.oak | a `format` call's output reaches the stream at once, under `*output-mutex*`, rendered first outside it; `print` or `write-string` straight to a shared stream from several threads interleaves |
| Input streams | — | one reader per stream; not locked |
| Scheduler queue, idle scheduler list | multiproc.oak | `*scheduleQ-mutex*`, held by the OS thread, alarms off |
| Thread objects (`*threads*`), a thread's state and joiners | multiproc.oak | `*threads-mutex*`; each thread's own mutex |
| Process ids | multiproc.oak | lock-free compare-and-swap |
| `load` | load-file.oak | a mutex per file name: the same file loads serially, different files in parallel |
| The bytecode compiler | compiler-locale | per-compilation state in fluids and the AST; `crunch-class-alist`, the opcode tables and `nichevo` are read-only after load; concurrent `compile-file`s give byte-identical objects (`threads-stress.test`), and so does compiling with threads live (`check-threaded-fixpoint`) |
| The native compiler | native-compiler.oak | its per-file state is global, so a native compile holds `*native-compiler-mutex*` throughout; `.ois` summaries are written whole then renamed into place; oakc is a single-threaded build step |
| Weak pointers | weak.c | a C mutex |
| `dump-world` | dump-world.oak | refused with more than one OS thread live (`%thread-count`); `--dump` at exit stops the world |
| VM registers (`STORE-REG`) | loop.c | per thread: `bp`, `env`, `nargs`, `process`; set at boot only: `t`, `nil`, the types, the subtype and tag-trap tables; `free-ptr` and `cons-limit` under `alloc_lock`; `next-newspace-size` a global knob |
| Allocation | data.h | TLAB fast path per thread; the slow path under `alloc_lock`; a thread out of memory while a cycle marks parks until the cycle's sweep has begun |
| Warm boot actions, command line, `features` | warm.oak, cmdline*.oak, hash-reader.oak | boot only, or set rarely; not locked |


Testing
=======

`make check`'s bytecode tests (all of `tests/*.test` except the native,
stand-alone and consistency tests, which need the build tree's own
emulator) against the standard world, with the emulator built each
way.  `threads-stress.test` runs `threads-stress.oak` (bignums,
lists, interning, `add-method`, `catch`/`throw`, `eval`, all compiled
at load) three times under `--pthreads 3`, then compiles six files at
once in futures and requires the objects to match a single-threaded
compile.

| Config | Flags | Result |
|--------|-------|--------|
| 1 | Default (copying GC) | all pass (`threads.test` skips) |
| 2 | `USE_MARK_SWEEP` | all pass (the thread tests skip) |
| 3 | `THREADS` | all pass |
| 4 | `THREADS + USE_MARK_SWEEP` (`--enable-threads`) | all pass |

Only configurations 1 and 4 can be configured; 2 and 3 need the flags
passed by hand.


Future Enhancements
====================

Potential improvements, not required for current thread support:

- **Generational hints**: Track old->young pointers via a card table;
  skip scanning old-generation objects in minor collections
- **Parallel marking**: Multiple GC worker threads share the mark stack,
  reducing mark phase latency on multi-core systems
- **Incremental compaction**: Gradually relocate objects to reduce
  fragmentation without a full STW copying collection
