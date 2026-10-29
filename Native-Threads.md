Native Thread Support in Oaklisp
=================================

Status: EXPERIMENTAL / INCOMPLETE (disabled by default)

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
- **Process object and fluid bindings**: `init-process-object` runs as
  the first warm boot action, giving the machine a process object so
  the scheduler (and so DELAY/FORCE) works.  `--pthreads N` calls
  `setup-initial-process-object`, whose `init-thread-infrastructure`
  sets `%no-threading` to `#f`, so `fluid.oak` binds per process, makes
  the guard mutexes, and then spawns the extra heavyweight threads.
  None of that happens at every warm boot because the compiler
  deadlocks when the mutexes are active.
- **Guard mutexes**: each is a variable that is `#f` until
  `init-thread-infrastructure` sets it to a real mutex, and the code it
  guards acquires and releases it only when it is set:
  `*symbol-table-mutex*` (`intern`, `gensym` in symbols.oak),
  `*add-method-mutex*` (the part of `%install-method-with-env` in
  kernel1-install.oak that mutates `operation-method-alist`),
  `*expression-table-mutex*`, `*stream-primitive-mutex*`,
  `*locale-mutex*` and `*fluid-table-mutex*`.
- **Method cache**: `OP_TYPE_METH_CACHE` is enabled under threads.
  Cache writes use a seqlock pattern: method and offset are written
  first, then the type field is stored last with `__atomic_store_n`
  (release semantics) as a commit flag.  Cache reads load the type
  with `__atomic_load_n` (acquire semantics), read method/offset, then
  re-verify the type to ensure a consistent snapshot.
  `OP_METH_ALIST_MTF` is disabled under threads since it mutates the
  shared alist structure.
- **Mutex spinning**: `acquire-mutex` calls `%thread-yield` (opcode 72,
  `sched_yield()`) between attempts rather than busy-waiting.

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
