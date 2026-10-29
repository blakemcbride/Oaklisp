Native Thread Support in Oaklisp
=================================

Status: the interpreter and the bytecode compiler work with threads
(see Threads.md); natively compiled code does not run threaded yet.
Disabled by default.

Enable with: `./configure --enable-threads`


Architecture
============

Two-level concurrency model:

1. **Heavyweight threads** — OS pthreads, each with its own VM
   registers, value stack, context stack, and GC buffer.  Created via
   `%make-heavyweight-thread` (opcode 70).

2. **Lightweight processes** — Oaklisp-level continuations multiplexed
   within a single pthread via a cooperative scheduler with
   timer-driven preemption.  Context switch via `pause()` using
   `call/cc`.

Each pthread runs its own lightweight process scheduler.  The global
scheduler queue (`*scheduleQ*`) is shared across all pthreads,
protected by a mutex.


C Emulator (`src/emulator/`)
============================

Key files and `#ifdef THREADS` sections:

| File | Thread-related content |
|------|----------------------|
| `threads.h` | Mutex declarations, `THREADY()` macro, TLS key |
| `threads.c` | `create_thread()`, `init_thread()`, `wait_for_gc()`, `set_gc_flag()` |
| `data.h` | Per-thread register macros, `ALLOCATE_PROT` spin-lock, GC buffer arrays |
| `loop.c` | Opcodes 67-72, `POLL_GC_SIGNALS()`, thread index init |
| `gc.c` | `FORTHREADS` macro, per-thread root scanning, GC coordination |
| `oaklisp.c` | Thread-0 TLS init, per-thread stack/register allocation |
| `stacks.c` | Thread index retrieval in `init_stacks()` |
| `config.h` | `MAX_THREAD_COUNT` (200), disables move-to-front under threads |

Per-thread state (via TLS `index_key`):

    register_array[my_index]        VM registers (e_pc, e_env, e_bp, ...)
    value_stack_array[my_index]     Value stack
    cntxt_stack_array[my_index]     Context stack
    gc_examine_buffer_array[my_index]  GC worklist

Global shared state:

    Heap (new_space, old_space)     Protected by alloc_lock
    free_point, new_space.end       Modified under alloc_lock
    gc_pending, gc_ready[]          GC coordination flags
    All Oaklisp objects             Mostly UNPROTECTED

GC pauses use a stop-the-world rendezvous: one thread sets
`gc_pending`, all others call `wait_for_gc()` (blocking on `gc_lock`),
the GC thread scans all threads' roots, then releases them.  With the
concurrent mark-sweep collector that threads build with, this happens
twice per cycle, for initial mark and remark (see GC.md).

Thread-related opcodes:

| Opcode | Name | Effect |
|--------|------|--------|
| 67 | ENABLE-ALARMS | `timer_increment = 1` (enable preemption) |
| 68 | DISABLE-ALARMS | `timer_increment = 0` (disable preemption) |
| 69 | RESET-ALARM-COUNTER | `timer_counter = 0` |
| 70 | HEAVYWEIGHT-THREAD | Create pthread running given thunk; returns #t/#f |
| 71 | TEST-AND-SET-LOCATIVE | Atomic CAS on locative cell (mutex primitive) |
| 72 | THREAD-YIELD | `sched_yield()` under THREADS, no-op otherwise |


Oaklisp Runtime (`src/world/`)
==============================

| File | Content |
|------|---------|
| `multi-em.oak` | Primitive ops: `%make-heavyweight-thread`, `%load-process`, `%store-process`, `%test-and-set-locative`, `%thread-yield` |
| `multiproc.oak` | Queues, process descriptors, mutexes, scheduler, semaphores, futures |
| `multi-off.oak` | Guard variable `%no-threading` for cold-boot |
| `multiproc-tests.oak` | Basic tests + TODO list of known issues |

Synchronization primitives (all in `multiproc.oak`):

- **Mutex**: Spin-lock via `%test-and-set-locative`.
  Yields between attempts via `%thread-yield` (opcode 72).
- **Semaphore**: Mutex + waiting queue.  Blocked
  processes enqueued, woken on `signal`.
- **Futures**: Lazy evaluation with dependent process
  queue; auto-scheduled computation.
- **Scheduler**: Global `*scheduleQ*` protected by
  mutex + alarm disable.  `pause()` does `call/cc` context switch.


Thread Safety
=============

What is protected:

- **TEST-AND-SET-LOCATIVE** (loop.c): the comparison happens entirely
  inside the critical section.
- **GC thread iteration** (gc.c): `next_index` is snapshotted into
  `gc_thread_count` at GC start, and every `FORTHREADS` loop iterates
  over the snapshot, so a thread created during GC is never read
  half-initialized.
- **Thread termination**: `init_thread()` (threads.c) prints a warning
  and exits the thread cleanly if its thunk returns.
- **pthread_create failure**: `info_p` is freed.
- **Process object, fluid bindings and windings**: `init-process-object`
  runs as the first warm boot action, giving the machine a process
  object so the scheduler (and so DELAY/FORCE) works.  From then on
  fluid bindings (`fluid.oak`) and the winding stack, `%windings` and
  `%wind-count` (`unwind-protect.oak`), live in the current process
  object, so resuming one process's continuation never unwinds
  another's `wind-protect`s and one process's `bind` is invisible to
  the others; before a process exists they are global.  A new process
  starts with a copy of its creator's fluid bindings and no windings.
  In a threads emulator the warm boot also makes the guard mutexes
  (`init-threads-at-boot`); `--pthreads N` spawns the extra
  heavyweight threads, each starting with a process of its own.
- **Guard mutexes**: each is a variable that is `#f` until
  `init-thread-infrastructure` sets it to a real mutex.  The code it
  guards runs its critical section through `%guard` (multi-off.oak),
  which with the mutex set is `%with-guard` (unwind-protect.oak):
  acquire, run, release on any exit, under `wind-protect`; and with it
  `#f` just runs the section.  The mutexes are reentrant and held by
  the process, so a section may call into another, and a section that
  is preempted and resumed on another OS thread still releases its own
  lock.  Only `%install-method-with-env` takes its mutex directly: its
  section is plain stores.  The guards:
  `*symbol-table-mutex*` (`intern`, `gensym` in symbols.oak),
  `*add-method-mutex*` (the part of `%install-method-with-env` in
  kernel1-install.oak that mutates `operation-method-alist`),
  `*expression-table-mutex*`, `*stream-primitive-mutex*`,
  `*locale-mutex*` and `*fluid-table-mutex*`.
- **Method cache**: `OP_TYPE_METH_CACHE` is enabled under threads,
  behind a seqlock: sequence counts striped by operation address
  (`mcache_seq` in loop.c).  A fill takes its stripe's count from even
  to odd with a compare-and-swap (or leaves the cache alone if another
  fill holds it), writes type, method and offset, and makes it even
  again; a hit needs the same even count before and after its reads.
  `OP_METH_ALIST_MTF` is disabled under threads since it mutates the
  shared alist structure.
- **Mutex spinning**: `acquire-mutex` calls `%thread-yield` (opcode 72,
  `sched_yield()`) between attempts rather than busy-waiting.  A mutex
  records the process holding it and a depth (reentrant; release by
  anyone else is an error), and may be named (`make-named-mutex`);
  after `mutex-report-spins` attempts a waiter prints which mutex it
  waits for and who holds it.  The scheduler's lock is a
  `%thread-mutex`, held by the OS thread (`%thread-index`, LOAD-REG
  24): its section switches processes but runs with alarms off.
  `with-mutex` (exported) holds a mutex for a body and releases it on
  any exit.
- **Alarms**: the timer counter and increment are per thread (locals
  of each thread's `loop()`), so one thread's `%enable-alarms` cannot
  re-arm preemption inside another's scheduler critical section.

What is not protected:

- **Hash tables** -- concurrent insert/lookup can corrupt general hash
  tables (the symbol table aside).
- I/O buffers and other global lists in the runtime have not been
  audited.

The TODO list in `multiproc-tests.oak` lists "race conditions (symbol
tables, hash tables, add-method)"; of these only general hash tables
remain open.


Performance
===========

`--enable-threads` always pairs threads with the mark-sweep collector
(see GC.md): allocation goes through thread-local allocation buffers
and free lists, taking the global `alloc_lock` (`ALLOCATE_PROT`,
data.h) only on the slow path, and collection is concurrent mark with
two short stop-the-world pauses (initial mark and remark).  The cost
relative to the default build is measured in BUILD.md (Thread
Support).


Remaining Work
==============

- Systematic audit of all mutable globals in the runtime (general hash
  tables, I/O buffers, global lists)
- Making the compiler usable with the mutexes active
- Thread-aware cold-boot sequence

The architecture (per-thread VM state, stop-the-world GC rendezvous,
two-level scheduling) is sound.  The hard part is the systematic audit
of every shared mutable data structure in the Oaklisp runtime.
