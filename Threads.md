Thread Support in Oaklisp
==========================

Status: the interpreter, the bytecode compiler and natively compiled
code (mixed mode and oakc executables) work with threads: `make check`
passes in every configuration, `threads-stress.test`,
`threads-api.test` and the threaded stand-alone executable included,
and the concurrent collector passes its verify mode.  Off by default.

Enable with: `./configure --enable-threads`, which builds the emulator
with OS threads (`-DTHREADS`) and makes the mark-sweep collector, the
default build's as well (`-DUSE_MARK_SWEEP`), concurrent.  Portable
across Linux, macOS and Windows (see `Portability.md` and
`Windows.md`).

Green threads (`delay`, `future`, `process-run-fn`) work in every
build.  With threads, `--pthreads N` runs the lightweight scheduler
on N OS threads, and `make-thread` starts OS threads of a program's
own (The API, below).


Architecture
============

Two-level concurrency model:

1. **Heavyweight threads** (OS threads) -- each with its own VM
   registers, value stack, context stack, GC buffer and allocation
   buffer.  Created with `%make-heavyweight-thread` (opcode 70), by
   `--pthreads N` for the scheduler and by `make-thread` for
   programs.  pthreads on POSIX and MinGW, Win32 threads on MSVC
   (`oak-thread.h`).

2. **Lightweight processes** (green threads) -- Oaklisp-level
   continuations multiplexed over the OS threads by a cooperative
   scheduler with timer-driven preemption.  A context switch is
   `pause`, with `call/cc`.

Every OS thread of `--pthreads N` runs the lightweight scheduler; the
queue (`*scheduleQ*`) is shared, under a mutex, so a process
preempted on one OS thread may resume on another.  Idle scheduler
threads park.


C Emulator (`src/emulator/`)
============================

Key files and `#ifdef THREADS` sections:

| File | Thread-related content |
|------|----------------------|
| `threads.h`, `oak-thread.h` | The mutexes, `THREADY()`, the TLS key and `oak_thread_index_var`, `oak_thread0_init`, `oak_thread_entry`, the blocking-call brackets; the POSIX/Win32 abstraction |
| `threads.c` | `create_thread()`, `init_thread()`, `wait_for_gc()` (a thread's safepoint: a pause to wait out, or its handshake), `set_gc_flag()`, thread 0's setup, parking (stream primitives 19-21), thread exit (the barrier buffer handed over, the slot reset), dynamic mutex init for MSVC |
| `data.h` | Per-thread register macros, the per-thread alarm clock in `register_set_t`, `ALLOCATE_PROT` (the TLAB fast path), GC buffer arrays |
| `loop.c` | Opcodes 67-72, `POLL_GC_SIGNALS()`, the method cache's striped seqlock reads, the NATIVE instruction's hand-over |
| `gc.c` | `FORTHREADS` macro, per-thread root scanning, the stop-the-world handshake |
| `gc-ms.c`, `gc-ms.h` | Concurrent mark-sweep: the GC thread and the pacing, the handshake rounds, the parallel markers, the region heap and its lookup table, TLABs, SATB barrier buffers, the coalescing sweep, the budget, verify mode |
| `oaklisp.c`, `embed-native.c` | The two entry points: thread 0 through `oak_thread0_init`, the thread entry (`loop` or the native call driver) |
| `stacks.c` | Thread index retrieval in `init_stacks()`, a thread's exit on context underflow |
| `dispatch.h`, `dispatch.c` | `oak_mcache_read`/`oak_mcache_fill`: the operations' method caches under their striped sequence counts, for the interpreter and native code; the global method cache (`gmc_read`/`gmc_write`, an entry under its own sequence count; `oak_gmc_flush`, an epoch bump by CAS) |
| `oak-native.h`, `native.c`, `native-ops.c` | Natively compiled code: `my_index` is the thread-local `oak_thread_index_var`, `ON_STORE` (barrier), `ON_PENDING` (rendezvous), seqlocked call sites, the registry under `native_lock`, `oak_native_run_thread` |
| `config.h` | `MAX_THREAD_COUNT` (200), disables move-to-front under threads |

Per-thread state (by `my_index`, the thread's slot, from the TLS key
in the interpreter and `oak_thread_index_var` elsewhere):

    register_array[my_index]           VM registers (e_pc, e_env, e_bp, ...,
                                       the alarm clock)
    value_stack_array[my_index]        Value stack
    cntxt_stack_array[my_index]        Context stack
    gc_examine_buffer_array[my_index]  GC worklist
    tlab_cursor_array[my_index]        TLAB bump pointer
    tlab_end_array[my_index]           TLAB end pointer
    satb_buffer_array[my_index][]      SATB write barrier log
    satb_count_array[my_index]         SATB buffer entry count

Global shared state:

    The regions (ms_regions, the     Mapped and unmapped under alloc_lock
      lookup table)                    or with the world stopped; looked
                                       up without a lock (ms_region_of)
    A region's top and bitmaps      Modified under alloc_lock; the TLAB
                                       fast path writes its own bitmap words
    gc_pending, gc_stw, gc_ready[]  GC coordination flags: something to
                                       do at the next safepoint, a pause
                                       to wait out, a thread parked
    gc_hs_request, gc_hs_done[],    The handshake rounds (gc-ms.c): the
      gc_mark_epoch,                   round asked for, each thread's last
      gc_scan_epoch[]                  done (a release store), the cycle's
                                       epoch and each thread's scanned
                                       state (read by its own barrier)
    free_list_heads[]               Segregated free lists
    gc_requested, gc_hold,          The GC thread's requests, holds,
      gc_cycles_done, gc_sweeping      completed cycles and background
                                       sweep, under gc_concurrent_lock
    satb_buffer_array[]             Per thread: old values overwritten,
                                       values stored while the thread's
                                       stacks are unscanned, values read
                                       through weak pointers and raw-ref
                                       caches, and the thread's own roots
                                       at its handshake; a full buffer, and
                                       the buffer at every handshake, goes
                                       to the markers' work list
    ms_work_list, ms_work_active,   The markers' work list and the run
      ms_work_idle, ms_work_done,      (gc-ms.c, "Parallel marking"), under
      ms_run_*                         ms_work_lock; the list and the idle
                                       count are also read without it by a
                                       marker deciding whether to export
    ms_pool_workers[],              A mark stack per marker thread and
      ms_helper_workers[]              per parked mutator helping, each
                                       used by its one thread, in a run it
                                       joined under ms_work_lock
    wp_table[]                      Cleared after a mark under wp_lock,
                                       readers testing liveness meanwhile
                                       (gc_weak_clearing, ms_weak_alive)
    new_space, free_point           The dump compaction's destination
                                       (gc.c), with the world stopped

GC coordination: the GC thread (gc-ms.c; started at the first request,
no slot in the thread arrays, index -1) stops nobody.  It sets
`gc_pending` for a handshake round; each mutator, at its next
safepoint (the interpreter polls every instruction, native code at
every call, return and backward branch, allocation on its slow path),
calls `wait_for_gc()`, which does the thread's part of the round --
at the first of a cycle, retiring its TLAB and scanning its own
registers and stacks into its barrier buffer; at every one, handing
the buffer to the marker -- and goes on; for a parked, sleeping or
blocked-in-C thread, or one waiting for a cycle (`gc_ready` set,
stacks synchronized), the GC thread does the part itself under
`gc_lock`, which the thread takes to leave that state.  A thread that
exits hands its buffer over and is skipped; one created during a
round joins it.  The barrier logs the value stored, as well as the
one overwritten, while the storing thread's stacks are unscanned
(GC.md, "SATB Write Barriers").  The trace runs in parallel on the
GC thread and the marker threads it starts (`--gc-threads N`, by
default one per CPU the process may run on, up to 8), with any
mutator parked for want of memory helping; the mark ends after a
round of flushes that marks nothing new; the sweep is lazy, by the
GC thread in the background after the cycle and by an allocating
thread that needs a run before the sweeper reaches one; a cycle is asked for ahead of the budget so that the
mutators keep allocating through it (GC.md, "Pacing"); the heap grows
and shrinks by mapping and unmapping regions.  Only a dump's
compaction (`gc.c`, and verify mode's check) stops the world: it sets
`gc_stw` with `gc_pending`, and `wait_for_gc` then blocks on
`gc_lock`; `next_index` is snapshotted at its start, so a thread
created during it is never read half-initialized.  The copying
collector runs only for that compaction, holding the GC thread off
meanwhile (see GC.md).

Thread-related opcodes:

| Opcode | Name | Effect |
|--------|------|--------|
| 67 | ENABLE-ALARMS | `timer_increment = 1` (enable preemption) |
| 68 | DISABLE-ALARMS | `timer_increment = 0` (disable preemption) |
| 69 | RESET-ALARM-COUNTER | `timer_counter = 0` |
| 70 | HEAVYWEIGHT-THREAD | Create an OS thread running the given thunk; returns #t/#f |
| 71 | TEST-AND-SET-LOCATIVE | One compare-and-swap on the cell (the mutex primitive) |
| 72 | THREAD-YIELD | `oak_thread_yield()` under THREADS, no-op otherwise |

And LOAD-REG 23 to 25 (`%threads-enabled?`, `%thread-index`,
`%thread-count`), stream primitives 19 to 21 (park, unpark, sleep).
The alarm clock is per thread: one thread's `%enable-alarms` cannot
re-arm preemption inside another's scheduler critical section.


Oaklisp Runtime (`src/world/`)
==============================

| File | Content |
|------|---------|
| `multi-em.oak` | Primitive ops: `%make-heavyweight-thread`, `%load-process`, `%store-process`, `%test-and-set-locative`, `%thread-yield`, `%threads-enabled?`, `%thread-index`, `%thread-count` |
| `multiproc.oak` | Queues, process descriptors, mutexes, scheduler, semaphores, futures, the thread API, condition variables, the atomics |
| `multi-off.oak` | `%guard`, the runtime's guarded section |
| `unwind-protect.oak` | `%with-guard`, the windings (per process) |
| `multiproc-tests.oak` | Basic tests of the lightweight processes |

Synchronization primitives (all in `multiproc.oak`):

- **Mutex**: the held flag taken with `%test-and-set-locative`;
  a waiter does not spin: it runs the processes the scheduler has
  queued (`pause`, as the alarm would), or with none queued parks its
  OS thread on the mutex's waiter list (stream primitive 19, 10 ms at
  a time) until `release-mutex` unparks it; only the scheduler's own
  lock, and a mutex taken inside the scheduler's section, is waited
  for by yielding.  Reentrant, held by the process (a process
  preempted inside a critical section and resumed on another OS
  thread still holds its lock), released only by the holder; may be
  named (`make-named-mutex`); `with-mutex` releases on any exit; a
  wait of a million steps or five seconds parked is reported on
  standard error with the holder, which is how a deadlock shows
  itself.  The scheduler's lock is a `%thread-mutex`, held by the OS
  thread (`%thread-index`): its section switches processes but runs
  with alarms off.
- **Semaphore**: Mutex + waiting queue.  Blocked processes enqueued,
  woken on `signal`.
- **Futures**: Lazy evaluation with dependent process queue;
  auto-scheduled computation.
- **Scheduler**: Global `*scheduleQ*` protected by mutex + alarm
  disable.  `pause()` does `call/cc` context switch.
- **Threads, condition variables, atomics**: The API, below.


Natively compiled code
======================

Natively compiled code (`--compile-native`, the native world, oakc
executables) runs with threads too, by the same rules as the
interpreter:

- The generated C and the helpers (`native.c`, `native-ops.c`) find
  the registers and stacks of their thread through `my_index`, which
  oak-native.h defines as `oak_thread_index_var`, a thread-local
  variable of the executable (initial-exec model, so a module loaded
  with dlopen reads it with a load, not a call).
- Every store of a ref into the heap is an `ON_STORE`, which is where
  the SATB write barrier sits; native-parity.test fails on a raw
  store in the generated C.
- `ON_PENDING`, polled at every call, return and backward branch,
  includes `gc_pending`: a thread in native code returns to its
  dispatcher, which does its part of the handshake, or waits out a
  pause (the interpreter's loop top, or `run_pending` in the call
  driver), so no native loop holds up a collection.
- The alarm clock is per thread: `register_set_t` holds it, the
  interpreter's locals are synchronized with it at every
  LOCALIZE/UNLOCALIZE, and native code reads and sets it there.
- The operations' one-entry method caches are read and filled under
  their striped sequence counts (`oak_mcache_read`, `oak_mcache_fill`
  in dispatch.c, shared with FUNCALL); a call site's cache
  (`oak_native_site`) has a sequence count of its own, odd while a
  fill writes it, and a reader trusts the fields it copied only if the
  count is unchanged, else takes the long way.
- A miss in the one-entry cache asks the global (operation, type) ->
  method cache (`oak_find_method`, dispatch.c) before searching the
  type graph; each entry has a sequence count of its own, read and
  written the same way.
- Module registration, name resolution and site fills take
  `native_lock`/`site_lock`; the function table grows in chunks that
  never move, so a stub's index is read without a lock.
- The C compiler run by `--compile-native` counts as time parked
  (`oak_thread_blocking_begin`/`end`), so a collection in another
  thread does not wait for it.
- `%test-and-set-locative` in native code is `oak_rt_test_and_set`,
  the same compare-and-swap; `%make-heavyweight-thread` and
  `%thread-yield` are real; parking and sleeping synchronize the
  stack first.
- A stand-alone executable's new OS thread runs the call driver
  (`oak_native_run_thread`) instead of `loop`: threads.c calls
  `oak_thread_entry`, which each entry point sets.


Initialization
===============

Every warm boot gives the machine a process object
(`init-process-object`, the first warm boot action), and from then on
fluid bindings and the winding stack are per process: resuming one
process's continuation never unwinds another's `wind-protect`s, and
one process's `bind` is invisible to the others.  A new process starts
with a copy of its creator's fluid bindings and the creator's winding
stack (its extent begins inside the creator's); before a process
exists they are global.

In an emulator with threads the warm boot also runs
`init-thread-infrastructure` (`init-threads-at-boot`, multiproc.oak),
which makes the guard mutexes and registers the main thread, so the
guards are on whether or not `--pthreads` follows and `make-thread`
works at once.  `--pthreads N` (`spawn-heavyweight-threads`) then only
starts the N-1 scheduler threads, each of which begins by giving
itself a process of its own (`start-busy-work`).

In an emulator without threads the guards stay `#f`, so a guarded
section costs a call and nothing more, `%threads-enabled?` is nil,
`make-thread` is an error, and `--pthreads` prints a warning and
exits.


Thread Safety
==============

**C emulator:**

- `signal_poll_flag` -- `volatile sig_atomic_t`, delivered by thread 0
- The collector's flags, counters and bitmaps, each thread's parked
  and dead flags, a region's top and the heap words the markers read
  -- atomic accesses (oak-atomic.h; GC.md, "Shared words"), never
  plain or `volatile`; a ThreadSanitizer build checks it (BUILD.md,
  "Sanitizers")
- `alloc_lock` -- the allocation slow path, the regions' tops, list
  and lookup table, mapping and unmapping; STORE-REG of `free-ptr`
  and `cons-limit` (the copying collector's bump pointer and limit,
  meaningless with mark-sweep)
- `gc_lock` -- held for the length of a stop-the-world pause (a
  dump), and by the GC thread while it does a parked thread's part of
  a handshake round; `wait_for_gc`, parking and the blocking-call
  brackets leave their parked state under it, so a pause that counted
  a thread stays stopped and a scan of its stacks is never overtaken
- `wp_lock` -- `ref_to_wp()` and `rebuild_wp_hashtable()` in `weak.c`
- `dump_lock` -- DUMP-WORLD and LOAD-WORLD, in the interpreter and in
  native code
- `native_lock`, `site_lock` -- the native registry and the call-site
  caches
- The method cache -- a seqlock with sequence counts striped by
  operation address (`mcache_seq`, dispatch.c): a fill takes its
  stripe's count from even to odd with a compare-and-swap (or leaves
  the cache alone if another fill holds it), writes type, method and
  offset, and makes it even again; a hit needs the same even count
  before and after its reads.  `OP_METH_ALIST_MTF` is off under
  threads: it would mutate the shared alists.
- The global method cache -- the same seqlock per entry (`seq` in
  `gmc_entry`), a fill skipped when another holds the entry, and an
  epoch: an entry counts only while its epoch is the current one.
  `oak_gmc_flush` bumps the epoch with a CAS, at every mark-sweep
  cycle (`ms_prepare_lazy_sweep`, before the sweep frees anything),
  every copying collection and every world load, since entries hold
  raw refs and are no roots; and a search that finds an operation's
  one-entry cache parked by `%install-method-with-env` flushes it
  first.  The native call-site caches, raw refs likewise, are flushed
  at the same points (`oak_native_flush_sites`).
- A thread whose thunk returns underflows its context stack, which
  `stack_unflush` turns into the thread's exit (`oak_thread_exit_unwind`);
  a failed `pthread_create` frees what it had allocated; a thread's
  slot is reused once it has exited.

**Oaklisp runtime -- the guarded sections:** each guard is a variable
that is `#f` until `init-thread-infrastructure` sets it to a mutex.
The code it guards runs its critical section through `%guard`
(multi-off.oak), which with the mutex set is `%with-guard`
(unwind-protect.oak): acquire, run, release on any exit, through a
winding whose thunks are the mutex's own; and with it `#f` just runs
the section.  The mutexes are reentrant and held by the process, so a
section may call into another, and a section preempted and resumed on
another OS thread still releases its own lock.  Only
`%install-method-with-env` and `mix-types` take their mutexes
directly: their sections are plain stores.  The guards:
`*symbol-table-mutex*` (`intern`, `gensym` in symbols.oak),
`*add-method-mutex*` (the part of `%install-method-with-env` in
kernel1-install.oak that mutates `operation-method-alist`),
`*expression-table-mutex*` (anonymous.oak),
`*stream-primitive-mutex*` (streams.oak), `*locale-mutex*` (the
setters in locales.oak), `*fluid-table-mutex*` (fluid.oak),
`*mix-types-mutex*` (mix-types.oak), `*output-mutex*` (format.oak),
`*native-compiler-mutex*` (native-compiler.oak), `*threads-mutex*`
and the per-file `load` mutexes (load-file.oak).  The "Shared state"
table below has the rule for each piece of shared state.

**Not protected, by design:** a user's hash table with concurrent
writers (lookups are safe against one writer), and `print` or
`write-string` straight to a shared stream from several threads
(their output interleaves; a `format` call's does not).  Input streams
have one reader each.


Performance
===========

- **TLABs** (under `USE_MARK_SWEEP + THREADS`): Most allocations use
  a per-thread bump pointer with no lock.  Only TLAB refill and large
  allocations acquire `alloc_lock`; a TLAB comes out of a big
  free-list entry (the sweep makes one entry of each run of dead
  objects) or a region's virgin tail, so a heap of short-lived objects
  is allocated from without the lock in steady state too.
- **Concurrent GC** (under `USE_MARK_SWEEP + THREADS`): The mark runs
  concurrently with no global pause (each thread scans its own stacks
  at a safepoint of its own, a few microseconds, when the GC thread
  asks), traced in parallel by the GC thread and its marker threads,
  and sweep runs lazily during allocation.  The heap is
  a set of 1 MB regions mapped on demand within a budget (three times
  the live data, never below the configured size, which starts at
  four times the default build's) and unmapped when they sweep empty;
  there is no copying fallback, and the copying collector runs only
  to compact the heap for a dump.
- **Guards**: a guarded section (`%guard`, `with-mutex`) costs the
  mutex's compare-and-swap (`%test-and-set-locative` is one CAS
  instruction with threads) and a winding pushed through one method
  of the process; the mutex carries its two winding thunks, made
  once.
- **Cost**: `make bench` against the default build, same world, CPU
  time, measured after the region heap: 0.98x to 1.31x (`strings`
  1.31, `eval` 1.14, `hash` 1.13, `compile` 1.15; BUILD.md has the
  table), ratios that move 4 to 9% with the interpreter's code
  layout.  Under oakc the string benchmark takes 1.4x to 1.8x the
  default build's time (Speed.md).  `bench.sh -p N` runs N copies of
  tak on N threads: 3.9x on four, bytecode and native alike.

See `GC.md` for full garbage collection documentation.


The API
=======

Documented in the language manual's Threads chapter
(`doc/lang/threads.tex`) and the summary.  `make-thread` starts an OS
thread running a thunk as one process (a copy of the creator's fluid
bindings, the creator's winding stack); `thread-join` waits for it,
parked, and gives its value or re-signals its error; `thread-alive?`,
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
only another OS thread could notify never ends.  `doc/examples` has
`parallel-map.oak` and `producer-consumer.oak`.


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
| Method cache | dispatch.c, loop.c | striped seqlock; `OP_METH_ALIST_MTF` off |
| Global method cache | dispatch.c | a seqlock per entry; an epoch, bumped by CAS, flushes it at every collection, world load and parked lookup |
| Operation-method alists, bp alists | kernel1-install.oak | `%install-method-with-env` under `*add-method-mutex*`; readers walk lists that only grow at the head or have a cdr replaced |
| Mixed-type cache | mix-types.oak | `*mix-types-mutex*` around the walk and the insert; the type is made outside it |
| Expression table, fancy accessors | anonymous.oak | `*expression-table-mutex*` (reentrant: evaluation inside can print) |
| Stream-primitive cache | streams.oak | `*stream-primitive-mutex*` |
| Output streams | format.oak | a `format` call's output reaches the stream at once, under `*output-mutex*`, rendered first outside it; `print` or `write-string` straight to a shared stream from several threads interleaves |
| Input streams | -- | one reader per stream; not locked |
| Scheduler queue, idle scheduler list | multiproc.oak | `*scheduleQ-mutex*`, held by the OS thread, alarms off |
| Thread objects (`*threads*`), a thread's state and joiners | multiproc.oak | `*threads-mutex*`; each thread's own mutex |
| Process ids | multiproc.oak | lock-free compare-and-swap |
| `load` | load-file.oak | a mutex per file name: the same file loads serially, different files in parallel |
| Bignum division's caches (the last divisor reversed; the last quotient and remainder) | bignum.oak | each cache is one immutable pair or list, replaced whole: a reader sees a divisor with its own reversed form, arguments with their own result. Written one variable at a time, as they were, two threads dividing at once swapped each other's (a wrong quotient one run in thirty of `gc-stress.test`) |
| The bytecode compiler | compiler-locale | per-compilation state in fluids and the AST; `crunch-class-alist`, the opcode tables and `nichevo` are read-only after load; concurrent `compile-file`s give byte-identical objects (`threads-stress.test`), and so does compiling with threads live (`check-threaded-fixpoint`) |
| The native compiler | native-compiler.oak | its per-file state is global, so a native compile holds `*native-compiler-mutex*` throughout; `.ois` summaries are written whole then renamed into place; oakc is a single-threaded build step |
| Native modules, call-site caches | native.c | `native_lock` around registration and resolution, `site_lock` and a per-site sequence count around fills |
| Weak pointers | weak.c | a C mutex |
| `dump-world` | dump-world.oak | refused with more than one OS thread live (`%thread-count`); `--dump` at exit stops the world |
| VM registers (`STORE-REG`) | loop.c | per thread: `bp`, `env`, `nargs`, `process`; set at boot only: `t`, `nil`, the types, the subtype and tag-trap tables; `free-ptr` and `cons-limit` under `alloc_lock`; `next-newspace-size` a global knob |
| Allocation | data.h | TLAB fast path per thread; the slow path under `alloc_lock`; a thread out of memory while a cycle marks parks until the cycle's sweep has begun |
| Warm boot actions, command line, `features` | warm.oak, cmdline*.oak, hash-reader.oak | boot only, or set rarely; not locked |


Testing
=======

`make check` in the threads build runs every test: the bytecode tests,
`threads.test` (the smoke test and `--pthreads 3`), `threads-api.test`
(the API, then the same program compiled natively), the native,
native-parity and stand-alone tests (the latter also builds the
thread API program as a straight-C executable, whose threads run the
native call driver), and `consistency.test` with the bytecode, native
and threaded fixpoints (`check-threaded-fixpoint` compiles every
source with three threads live).  `threads-stress.test` runs
`threads-stress.oak` (bignums, lists, interning, `add-method`,
`catch`/`throw`, `eval`, four threads formatting into one stream,
three defining globals while others look them up; all compiled at
load) three times under `--pthreads 3`, once more compiled natively,
then compiles six files at once in futures and requires the objects
to match a single-threaded compile.  `gc-stress.test` (GC.md) runs
`gc-stress.oak` -- lists, vectors, bignums, closures, continuations,
weak pointers and hash tables made and verified by four OS threads at
once while they rewrite old objects, under `--size-heap 256` so that
collections never stop -- as bytecode, compiled natively, and as
stand-alone executables in every mode oakc has, each run reporting its
pauses and stalls (`--gc-stats`).  Both stress programs, bytecode and
native, pass under `-DOAK_GC_VERIFY` (GC.md), which checks every
concurrent cycle for a missed object.

| Config | Flags | Result |
|--------|-------|--------|
| 1 | Copying GC (`--disable-mark-sweep`) | all pass (the thread tests skip) |
| 2 | `USE_MARK_SWEEP` (the default) | all pass (the thread tests skip) |
| 3 | `THREADS` (`--enable-threads --disable-mark-sweep`) | all pass |
| 4 | `THREADS + USE_MARK_SWEEP` (`--enable-threads`) | all pass |

Every configuration can be configured, and each ran the whole suite
in a tree of its own, with its own world and native world
(2026-11-04, GCEnhancements.md Phase 7).


Future Enhancements
====================

Not required for the current thread support:

- **A remembered set for spatic**: every cycle scans the world image
  for pointers into the heap; logging stores into spatic would make
  that incremental
- **Locks per stream**: `print` to a shared stream from several
  threads without interleaving (only `format` is atomic per call)
