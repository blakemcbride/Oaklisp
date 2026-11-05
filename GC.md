Oaklisp Garbage Collection
==========================

Oaklisp has two garbage collectors that coexist in the same codebase.
Which one is active depends on compile-time flags.  The two collectors
serve complementary roles: the mark-sweep GC handles every collection
without moving objects, on a heap of regions that grows and shrinks by
mapping and unmapping them, while the copying GC compacts the heap
into one contiguous image for a world dump (and is the collector of
the builds without mark-sweep).


Build Configurations
====================

There are four build configurations, controlled by two C preprocessor
flags:

| Flag | Default | Effect |
|------|---------|--------|
| `USE_MARK_SWEEP` | on | The mark-sweep collector for normal collections (configure's default; `--disable-mark-sweep` leaves it out) |
| `THREADS` | off | Enable pthread-based multi-threading |

The four resulting configurations:

| # | Flags | Primary GC | Allocation | Threading |
|---|-------|-----------|------------|-----------|
| 1 | (none): `--disable-mark-sweep` | Copying | Bump pointer | Single |
| 2 | `USE_MARK_SWEEP`: the default | Mark-sweep (STW) | TLAB + free list + bump | Single |
| 3 | `THREADS`: `--enable-threads --disable-mark-sweep` | Copying | Bump pointer + `alloc_lock` | Multi |
| 4 | `USE_MARK_SWEEP + THREADS`: `--enable-threads` | Mark-sweep (concurrent) | TLABs + free list + bump | Multi |

How to build each:

```sh
# Config 1: Copying GC, single-threaded
./configure --disable-mark-sweep

# Config 2: Default (mark-sweep, single-threaded)
./configure

# Config 3: Copying GC, multi-threaded
./configure --enable-threads --disable-mark-sweep

# Config 4: Concurrent mark-sweep, multi-threaded
./configure --enable-threads
make
```

`--enable-threads` gives config 4 and `--disable-mark-sweep` config 1
(the two together config 3).  All four pass the test suite (see
Threads.md, Testing).  Adding `-DOAK_GC_VERIFY` to a
threads build makes every concurrent mark-sweep cycle check itself:
see "Verify mode" below.


Verify mode
-----------

`make CPPFLAGS="-DOAK_GC_VERIFY"` in a threads build (GCC or Clang)
checks the concurrent collector as it runs.  At the end of every
mark it stops the world (the mode's one pause), retires the TLABs,
marks the heap again from scratch from the roots as they are then,
and reports every live object below its region's `sweep_top` that
the concurrent mark missed, with the object referring to it (its
type, whether it was marked or born black) and where both were
allocated, from a ring of the last four million allocations
(`OAK_GC_VERIFY_LOG=0` in the environment turns the ring off: its
atomic add on every allocation shifts the timing).  It also reports
any allocation made while the GC thread has the world stopped.  A
missed object is kept, so the run goes on.

Interior pointers and splitting
-------------------------------

MAKE-LAMBDA allocates an operation and its method as one object, and
the method is referred to on its own (`e_current_method`, context
frames), often after the operation is garbage; the mark therefore
maps any pointer that does not land on an object start to the object
containing it.  When the free list for a request's size class is
empty, `ms_free_list_alloc` splits an entry of a larger class (whose
end lies behind the lazy sweep's cursor) rather than collecting.

Runtime Options
===============

These emulator command-line flags affect GC behavior:

| Flag | Default | Description |
|------|---------|-------------|
| `--size-heap N` | 512 (128 with the copying collector) | Heap size in kilo-refs (1 ref = 8 bytes on 64-bit); the mark-sweep budget's floor |
| `--trace-gc V` | 0 | GC verbosity: 0=quiet, 1=summary, 2=detailed (each pause with its length), 3=very detailed (plus every pause and stall as it ends, and the report below) |
| `--gc-stats` | off | report the pauses and stalls at exit (Pause Statistics, below) |
| `--gc-threads N` | the CPUs, at most 8 | threads marking in parallel in the concurrent collector (the GC thread and N-1 markers; "Phase 3: Concurrent Mark"); the default is the CPUs the process may run on (its affinity mask), capped |
| `--predump-gc B` | 1 | Run GC before world dump: 0=no, 1=yes |

The default heap is 512 kilo-refs = 512K × 8 bytes = 4 MB on 64-bit
systems, a budget the mark-sweep heap is mapped up to as needed
(128K, 1 MB, with the copying collector).  This is sufficient for
most interactive use and compilation of individual `.oak` files.

The `OAKWORLD` environment variable sets the default world file path.
`OAKLISP_OPTIONS` holds emulator options as they would be written
before the `--`, for a program whose command line is not the user's
to write, a stand-alone executable above all:
`OAKLISP_OPTIONS='--size-heap 4096 --gc-stats' ./prog`.

Example:
```sh
oaklisp --size-heap 4096 --trace-gc 2 --world oakworld.bin -- --load myfile --exit
```


Collector 1: Copying GC (`gc.c`)
================================

The original Oaklisp garbage collector.  This is a semi-space copying
collector: live objects are copied from old space to new space,
compacting the heap in the process.

**How it works:**

1. Allocate a fresh `new_space` of the target size
2. Scan roots (VM registers, stacks, static space) — copy each
   referenced object to new space ("transport")
3. Scavenge new space linearly — for each transported object, scan its
   fields and transport anything still in old space
4. Handle locatives — locatives point to individual cells within
   objects; they are updated after their containing objects are
   transported
5. Process weak pointers — discard entries whose keys were not
   transported
6. Free old space

**Strengths:**
- Simple and well-tested (in use since 1987)
- Compacts the heap — no fragmentation
- Cost is proportional to live data, not heap size
- Automatically expands the heap when live data exceeds 1/3 of space
  (controlled by `RECLAIM_FACTOR = 3`)

**Weaknesses:**
- Stop-the-world — all threads must pause for the entire collection
- Moves all objects — locatives and raw code pointers (`e_pc`) must be
  fixed up, which is complex and incompatible with concurrent operation
- Requires a contiguous destination space at least as large as live data
- Under threads without mark-sweep, every allocation acquires
  `alloc_lock`

**When it runs:**
- Config 1 and 3: Every collection
- Config 2 and 4: Only to compact the heap for a world dump (`--dump`
  at exit, `dump-world` from a program); never to grow it

**Heap expansion:** When live data exceeds 1/3 of the heap after
collection, the copying GC sets `e_next_newspace_size = 3 * live_data`
so the next collection allocates a larger new space.  The mark-sweep
collector applies the same rule to its *budget* after a cycle
(`ms_set_budget`, from the words its mark traced), and maps regions up
to it (Heap Layout, below); its heap also starts four times bigger
(`DEFAULT_NEWSPACE`, 512K words against the copying collector's
128K), since every cycle scans spatic and
the roots and marks the live data whatever the heap size.  An optional
`MAX_NEW_SPACE_SIZE` compile-time cap limits both.


Multi-Threaded Copying GC (Config 3)
-------------------------------------

Under `THREADS` without `USE_MARK_SWEEP`, the copying GC is the only
collector.  Every allocation and every collection contends on global
locks, making this the least scalable configuration for multi-threaded
use.

### Allocation

All allocation goes through the global bump pointer protected by
`alloc_lock`.  The `ALLOCATE_PROT` macro (in `data.h`) works as
follows:

1. Try to acquire `alloc_lock` via `pthread_mutex_trylock`.  While
   waiting, check `gc_pending` — if set, call `wait_for_gc()` to park
   at the GC safepoint (releasing local state first) and resume after
   collection completes.
2. Once the lock is held, check whether `free_point + words` fits
   within `new_space.end`.
3. If space is available, bump `free_point` and release `alloc_lock`.
4. If space is exhausted, trigger a full STW copying collection (see
   below), then bump `free_point` from the fresh heap.  Release
   `alloc_lock`.

Every allocation — even a 3-word cons — acquires and releases the
mutex, which is the main scalability bottleneck of Config 3.

### Collection

When the bump pointer is exhausted, the allocating thread becomes the
GC coordinator:

1. **Signal**: The coordinator sets `gc_pending = true` and acquires
   `gc_lock`.  Other threads notice `gc_pending` during their next
   allocation attempt (in the `pthread_mutex_trylock` spin loop) and
   call `wait_for_gc()`, which sets `gc_ready[thread_index] = 1` and
   blocks on `gc_lock`.
2. **Wait**: The coordinator spins until `gc_ready[i] == 1` for all
   active threads (snapshot of `next_index` at GC start to avoid
   reading uninitialized slots from threads created during GC).
3. **Collect**: With all threads parked, the coordinator runs the
   standard copying GC cycle — transport, scavenge, locative fixup,
   weak pointer processing — scanning roots from **all** threads'
   registers, value stacks, context stacks, and GC examine buffers
   (via the `FORTHREADS` macro).
4. **Resume**: The coordinator clears `gc_ready[my_index]`, sets
   `gc_pending = false`, and releases `gc_lock`.  All parked threads
   unblock from `wait_for_gc()` and resume execution.

The entire collection is stop-the-world: all mutator threads are
paused from step 2 through step 4.  Collection time is proportional
to live data across all threads.


Collector 2: Mark-Sweep GC (`gc-ms.c`)
=======================================

A non-moving mark-sweep collector.  Objects are never relocated, which
means locatives and raw code pointers require no fixup — a prerequisite
for concurrent collection.

Enabled by defining `USE_MARK_SWEEP` at compile time: configure's
default, left out by `--disable-mark-sweep`.


Heap Layout
-----------

The heap is a set of *regions* (`ms_region_t`, gc-ms.h): spans of one
or more 1 MB *chunks* (`MS_REGION_LOG2_BYTES`; 128K words on 64-bit,
256K on 32-bit) mapped together, aligned to the chunk size.  An
ordinary region is one chunk; an object bigger than a chunk gets a
span of as many as it needs, and the span a dump compaction copies
the heap into is one region too.  Each region has

- its memory, `[start, end)`, of which `[start, top)` has been
  allocated into and `[top, end)` is the *virgin tail*, a bump area;
- `sweep_top`, where `top` stood when the current cycle began: what
  lies below it is swept this cycle, what was allocated above it
  since is implicitly live;
- three bitmaps, one bit per word from `start`: the **objstart
  bitmap** (the first word of every allocated object), the **mark
  bitmap** (the objects found reachable; cleared before each cycle)
  and the **free bitmap** (the first word of every free-list entry:
  set as the sweep makes one, cleared as the allocator takes it, so
  that the mark ignores a reference landing in one instead of
  tracing the dead objects in it);
- its slots in the GET-DATA table (below), a count of the sweeps in
  a row that found nothing live in it, and its place on the heap's
  list.

The region an address belongs to is found through a two-level table
indexed by the address's chunk number (`ms_region_of`, three loads;
a 64-bit address is taken to have 48 significant bits, which mapping
a region checks).  Spatic, the stacks and garbage are not in it.
Regions are mapped with `mmap` (`_aligned_malloc` on Windows) on
demand, as allocation needs them, within the heap's **budget**: the
configured heap size (`--size-heap`, `original_newspace_size`, 512K
words by default) to begin with, and after every cycle three times
the live data the mark found (`MS_RECLAIM_FACTOR`, the copying
collector's rule), never below the configured size.  The budget is
`e_next_newspace_size`, which LOAD-REG and STORE-REG
`next-newspace-size` read and set.  A region whose sweep finds
nothing live is reset to virgin in one step; one that has been empty
for two sweeps in a row (`MS_EMPTY_CYCLES`) is unmapped when the heap
is over its budget, the biggest first.  So the heap grows by mapping
a region and shrinks by unmapping one; nothing is ever copied, and
there is no copying fallback.

What the contiguous heap used to serve: GET-DATA maps a heap address
to `spatic.size` plus its region's first slot times the chunk size
plus its offset (`ms_get_data`), and CRUNCH maps back (`ms_crunch`),
trapping on an integer that names no mapped word; the dump writes the
compaction span (Interaction, below); the registers `free-ptr` and
`cons-limit` (LOAD-REG and STORE-REG 14 and 15) are the copying
collector's bump pointer and limit, meaningless in these builds, and
nothing in the world uses them.


Allocation
----------

Allocation uses a multi-tier strategy.  The fast path, in both
mark-sweep builds, is a bump pointer into a TLAB (below); the tiers
are what refills it, or serves a request too big for one.

### Tier 1: Size-class segregated free lists

41 size classes.  Classes 0 to 31 each hold one size, 1 to 32
`ref_t` words; the rest hold ranges, up to these maximum sizes:

```
Class: 32  33  34   35   36   37   38    39   40
Max:   48  64  96  128  192  256  512  1024  inf
```

A request takes the first entry of its class that is big enough.  In
a class holding a range, entries smaller than the request are walked
past, and a program that keeps many small objects alive between dead
ones fills such a class with them: every allocation walked them all,
and building a big live structure took time growing with its square.
With one size per class for the sizes nearly every object has, the
first entry always fits.

Free list entries occupy the dead object's memory.  `word[0]` is the
next pointer, `word[1]` the size (an entry of one word, class 0, has
none); the sweep makes one entry of every run of adjacent dead
objects, clearing the objstart bits inside the run, so a heap of dead
conses yields a few big entries rather than one per cons, and the
entry's extent in the bitmap (to the next objstart bit) is its size.
An entry lies within one region, below its `sweep_top`, and the
sweep that made it has passed it entirely, so any entry on the lists
may be used.  The lists span all regions and are rebuilt from scratch
each cycle.

When the entry taken is larger than the request, the unused gap,
which has no objstart bit and so counts as the new object's tail
wherever the bitmap gives an extent, is zeroed: fixnum zeros, which
nothing can misread as a pointer.

### Tier 2: Lazy sweep (after a collection)

If the free lists have nothing suitable but a collection has recently
completed, `ms_lazy_sweep_one()` sweeps one 4096-word block of the
next region to populate free lists.  This is tried before touching
virgin space.

### Tier 3: Virgin tails

Then the bump areas: the cursor region's virgin tail (`alloc_region`,
the region last bumped from), else the first region on the list with
room.  An objstart bit is set for the new object.  A virgin tail only
shrinks, except when a region is mapped or found all dead, so the
least request a walk of the list found no room for is remembered
(`virgin_fail`, one for aligned requests and one not) and fails at
once until then; the list grows with the live data, and walking it on
every miss was the other half of the slowdown above.

### Tier 4: Mapping a region

When no region has room and the heap is under its budget (plus the
headroom, with threads: Pacing, below), a region is mapped (a span of
several chunks for an object bigger than one) and bumped from.

### Tier 5: Collection

If all of the above fail, `ms_collect()` runs a mark-sweep cycle --
with threads, waits for the GC thread's next cycle to end, asking for
one if none is running -- which sets the next budget, and the tiers
are tried again.  If that has still left nothing big enough, a region
is mapped past the budget, which then follows
(`ms_alloc_beyond_budget`).  There is no copying fallback.  With
threads a cycle is normally asked for well before this point (the
trigger), so that this tier is reached only when the mutators
allocate faster than the GC thread marks.


Thread-Local Allocation Buffers (TLABs)
---------------------------------------

Each thread has a private bump-pointer region (TLAB); without
`THREADS` there is one, the allocator's bump cursor, so `ALLOCATE`'s
fast path is the same few instructions in both mark-sweep builds.
TLAB size: 4096 words (`TLAB_SIZE`), carved from a region's virgin
tail (or what is left of one, down to 257 words) or out of a
free-list entry of at least 257 words (`TLAB_MIN_FREE`,
`ms_tlab_from_free`), trimmed to 64-word edges (`TLAB_ALIGN`): a
TLAB's objstart bits then lie in bitmap words no other thread touches,
and the fast path sets them with a plain store (`MS_TLAB_OBJSTART`,
through the thread's TLAB base and bitmap pointer,
`tlab_base_array`/`tlab_bm_array`); the atomic `ms_objstart_set` is
for the shared words the slow paths and the collector write.  The
words skipped to align a start are zeroed and given an objstart bit, a
dead filler for the next sweep.

**Fast path (no lock, inline in `ALLOCATE_PROT`):**
1. Check if the TLAB has enough space
2. Bump `tlab_cursor_array[thread_index]`
3. Set the objstart bit (a plain store)

**Slow path (`ms_allocate_slow`, out of line; acquires `alloc_lock`
with threads):**
1. A small request (at most `TLAB_SIZE`) wants a TLAB: out of a free
   entry of `TLAB_MIN_FREE` words or more, sweeping lazily for one,
   at most `TLAB_SIZE` words of it (the rest goes back on its list),
   else out of a virgin tail or a newly mapped region.  During a
   concurrent mark a TLAB from a free run lies below its region's
   `sweep_top`, where everything allocated must be born black, which
   the fast path does not do: so its mark bits are all set as it is
   carved (`ms_blacken`, whole bitmap words of its own), so every
   object later placed in it is marked wherever it starts
2. Otherwise, or with nothing but scraps free: the free list
   (`ms_free_list_alloc`; an empty size class splits an entry of a
   larger one), then lazy sweep + free list
3. Then a virgin tail, or a new region within the budget
4. If all fail: `ms_collect()`, and back to 1 until the request is
   met.  Without threads `ms_collect` runs the cycle; with threads it
   waits for the GC thread's next cycle to end (asking for one if none
   is running), parked at a safepoint, which is the *allocation wait*
   of the statistics.  When that leaves nothing big enough for the
   request, a region is mapped past the budget.  The slow path never
   calls `gc()`.  The GC and FULL-GC instructions likewise go through
   `ms_collect_explicit` (in both mark-sweep builds: a full collection
   is an ordinary cycle, waited for; the heap is never merged into
   spatic).

A TLAB is retired (`ms_tlab_retire`: its unused space zeroed and
given an objstart bit, so that the sweep can reclaim it and no
object's extent absorbs it) when its thread replaces it, at the
thread's first handshake of a cycle, and when the thread exits; the
single-threaded cycle and verify mode retire them all at once
(`ms_tlab_retire_all`).


Collection: Single-Threaded Mode
---------------------------------

Under `USE_MARK_SWEEP` without `THREADS`, the mark is stop-the-world
and the sweep lazy, as with threads:

1. Retire the TLAB, clear the mark bitmaps, snapshot every region's
   `sweep_top`
2. Mark roots (VM registers, stacks, static space)
3. Trace: iteratively follow marked objects' pointer fields,
   marking referenced objects (using an explicit mark stack)
4. Process weak pointers (discard unmarked entries)
5. Set the budget, prepare the lazy sweep, and sweep eagerly until
   the request that started the cycle can be met
6. Sweep the rest lazily during allocation


Collection: Multi-Threaded Mode (Concurrent)
---------------------------------------------

Under `THREADS + USE_MARK_SWEEP` a cycle runs on a thread of its own
and stops no thread for any other: the mark's roots are taken by
handshakes, one thread at a time, its end is agreed by rounds of
them, and the sweep is lazy.  There is no global pause (a dump's
compaction and verify mode's check are the exceptions).

### The GC thread

Every cycle runs on the GC thread (`ms_gc_thread_main`), started at
the first request, so that no mutator is ever out of action for the
length of a mark; it starts the marker threads that trace with it
(`ms_markers_start`; Phase 3 below) at the same time, and between
cycles it sweeps in the background (Lazy Sweep).  None of them
has a slot in the thread arrays: they run no Oaklisp code, allocate
nothing and hold no roots, so the handshake rounds ask nothing of
them, and `%thread-count` does not count them; their index
(`index_key`, `oak_thread_index_var`) is `MS_GC_THREAD_INDEX`, -1.  A mutator asks for a cycle
(`ms_request_cycle`, under `gc_concurrent_lock`) and goes on; one
that has no room left waits for the next cycle to end
(`ms_wait_cycle`, parked at a safepoint with `gc_ready` set, leaving
that state under `gc_lock`), and the GC and FULL-GC instructions wait
the same way.  A dump (`gc()`) and a world load (`ms_destroy`), which
move or unmap the heap, hold the GC thread off (`ms_gc_hold`: a cycle
in progress ends first, parked meanwhile, and none starts until
`ms_gc_release`).  Verify mode's record of who has the world stopped
(`ms_verify_stw_owner`) is the GC thread's index during its pause and
`MS_NO_STW_OWNER` otherwise.

### Phase 1: The snapshot

The GC thread, having finished the previous cycle's sweep (a block at
a time, releasing `alloc_lock` between blocks so the allocator gets
in) and holding `alloc_lock`:
1. Clears the mark bitmaps
2. Advances the cycle's epoch (`gc_mark_epoch`): every thread is
   unscanned, every barrier buffer stale
3. Sets `gc_phase` to `GC_PHASE_CONCURRENT_MARK`: what is allocated
   from free entries from now on is born black
4. Snapshots every region's `sweep_top`, and measures the allocation
   rate since the last snapshot (Pacing)
5. Turns the barrier on (`gc_satb_active`, after a release fence, so
   that a thread that sees the flag sees the epoch), and releases
   `alloc_lock`
6. Flushes the global method cache and the native call-site caches
   (raw refs that are no roots: a hit during the mark logs the
   method it yields, `SATB_READ_RAW`, so what the sweep frees is in
   none of them)

A TLAB carved after this is above its region's `sweep_top` (a
virgin tail) or blackened (a free run).  One carved before it and
still in use lies below `sweep_top`: its thread retires it at its
first handshake, and what the thread allocates from it meanwhile,
unmarked, is covered by that handshake's scan of the thread's stacks
and by the barrier's logging of what an unscanned thread stores.
Until it is scanned a thread allocates nothing black: the slow path
(`ms_alloc_slow_locked`), which carves every new TLAB and takes every
born-black entry, scans the thread first if it is unscanned, under
`alloc_lock`, so that the snapshot cannot come between the scan and
the allocation.  A fresh object's initializing stores are not
barriered, so a black object filled from an unscanned stack would
hide a reference only that stack held; verify mode found exactly
that, once in three runs.

### Phase 2: The roots, by handshakes

A *round* (`ms_handshake_round`): the GC thread posts a request
(`gc_hs_request`, a number) and sets `gc_pending`; each mutator, at
its next safepoint (the interpreter polls at every instruction,
native code at every call, return and backward branch: `wait_for_gc`,
then `ms_handshake_poll`, with its stacks synchronized), does its part
(`ms_handshake`) and records the request as done (`gc_hs_done`, a
release store); for a thread parked, sleeping, waiting for a cycle or
blocked in C (`gc_ready` set, its stacks synchronized too) the GC
thread does the part itself, under `gc_lock`, which the thread takes
to leave that state.  The round is over when every live thread is
in; a thread that exits leaves it, its buffer handed over first
(`ms_thread_exiting`), and one created meanwhile joins it (its
slot's record is reset when the slot is handed out,
`ms_thread_slot_reset`).  Nobody waits for anybody: a round holds the
GC thread only, for as long as the slowest thread takes to reach a
safepoint.

The first handshake of a cycle (the thread's `gc_scan_epoch` is not
the cycle's): retire the TLAB; scan the thread's registers, examine
buffer, value and context stacks and their segment pointers into its
own barrier buffer (`ms_scan_thread_roots`); mark the thread scanned;
hand the buffer to the marker.  A few microseconds: the stacks in
memory are at most a few thousand words, the rest lives in heap
segments, which the marker traces.  (Verify mode and the
single-threaded cycle scan the same roots straight into the mark
stack, with the world stopped: `ms_mark_roots`.)

Then the GC thread marks the global registers and tables into its
own mark stack (`ms_mark_global_roots`), puts spatic on the markers'
work list in ranges (`ms_work_put_spatic`) and runs the trace -- only
once every stack is in: tracing earlier could read a TLAB tail not
yet retired as the extent of the object before it.  A round holds
the GC thread for as long as the slowest thread takes to reach a
safepoint: microseconds, a thread that has done its part signalling
it (`gc_hs_cv`; on one core the GC thread would otherwise wait for
the mutator's slice to end).

### Phase 3: Concurrent Mark, in parallel

The heap is traced while the mutators run, by several threads at
once (gc-ms.c, "Parallel marking"): the GC thread, the marker
threads it started with itself (`ms_markers` in all, `--gc-threads
N`: by default one per CPU the process may run on, at most
`MS_MAX_MARKERS`, 8), and any mutator parked for want of memory
while a run of the trace is open, which joins as a worker instead of
idling (`ms_help_mark`, from `ms_wait_cycle`; a run's start wakes the
waiters).

Work is in packets on a shared list (`ms_work_list`, under
`ms_work_lock`; `ms_work_put`):
- refs to mark: a thread's barrier buffer, handed over when it fills
  (1024 entries) or at a handshake, its root scan among them
  (`satb_handoff`).  Mutators' SATB barriers log the old value
  before any heap write, and the value stored too while the storing
  thread's stacks are unscanned (SATB Write Barriers, below).  A
  packet logged in an earlier cycle (its epoch) is dropped: its
  entries refer to objects marked then
- a range of spatic (`MS_SPATIC_CHUNK`, 16384 words) to scan for
  roots: every pointer from the world image into the heap is one,
  and stores into spatic are barriered like any, so the scan runs
  under the barrier -- a word read old was there since the snapshot,
  one read new refers to what the snapshot's rules mark anyway, and
  what was overwritten is in a barrier buffer
- marked objects to trace, exported by a worker from its own mark
  stack

Each worker (`ms_worker_t`) traces from its own stack
(`ms_worker_run`): pops an object, reads its extent from the
objstart bitmap, and marks each pointer field atomically
(`ms_mark_ref_concurrent`, `__atomic_fetch_or` on the mark bit; the
one that sets the bit pushes the object, so each object is traced
once).  When its stack runs dry it takes a packet; when the list is
empty it spins for 20 µs (`MS_SPIN_NS`; not when the markers and the
mutators together outnumber the CPUs, `ms_spin_ok`, when a spinner
would take a CPU from a thread with work), then sleeps on
`ms_work_cv`.  A worker exports the bottom half of its stack, in
packets of up to 512 (`MS_PACKET_SIZE`, `ms_worker_export`), when
another worker is idle and the list is empty, at most once every
256 objects it marks (`MS_EXPORT_INTERVAL`) and only with 8 or more
on its stack (`MS_EXPORT_MIN`): the bottom holds the oldest entries,
the biggest subtrees of a deep structure, so a worker tracing a tree
with a stack never deeper than the tree gives the bulk of it away,
and the interval keeps a worker filling its stack from a wide object
(a big vector, a hash table's buckets) exporting hundreds at a time
rather than a handful at every push.  A run ends when the list is
empty and every worker is idle: the last to go idle declares it done
(`ms_work_done`) and wakes the rest; the GC thread waits for all to
leave (`ms_run_cv`) and sums what they marked and traced
(`mark_stack_pushes`, `marked_words`).  `--trace-gc 3` prints each
run's counts.

Nothing a worker writes is read by another: the mark bits are set
atomically, and the objstart and free bits, the regions' tops and
the objects' fields are read as the single marker read them, racing
only with the mutators, as before.  Each worker's record is on cache
lines of its own (`OAK_CACHE_ALIGNED`): in a contiguous array the
workers slowed each other to a third of their speed (the same
instructions, twice the cycles) through the lines their pushes
shared.  On a heap of 13 million live words (a vector of 300,000
short lists, a hash table of 200,000 entries and a binary tree of a
million nodes) the mark took 33 ms with the one marker of the
previous commit, 18 ms with the GC thread and the caller of `(%gc)`
parked for it, and 13, 10 and 9 ms with 2, 4 and 8 marker threads
(i9-14900KF, the P cores).

- A reference to an object allocated since the snapshot (above its
  region's `sweep_top`), from a buffer or a field, is ignored: the
  object is implicitly live and holds only references the marker
  reaches by other means, and marking it would make a mutator that
  keeps overwriting fields with fresh objects feed the termination
  rounds for as long as it allocates
- Objects allocated from free lists during this phase are marked
  atomically ("born-black") via `ms_mark_alloc()`; a TLAB carved from
  a free run is blackened whole (`ms_blacken`)
- Objects allocated from virgin tails (TLABs included) are above
  their region's `sweep_top` and are implicitly live (not swept); a
  region mapped during the mark has `sweep_top` at its start
- Neither kind is ever traced: what they can refer to was reachable
  at the snapshot (marked by the snapshot's rules), was allocated
  after it (black), was stored by an unscanned thread (logged), or
  was read through a weak pointer or a raw-ref cache (logged)
- The region table may grow meanwhile (a mutator mapping a region
  under `alloc_lock` publishes it with release stores; the marker's
  lookups are acquire loads); regions are only unmapped between
  cycles

### Phase 4: Termination, by rounds

When a run of the trace ends the GC thread runs a round in which
every thread hands its buffer over, and runs the trace again.  If
that marked nothing new (`mark_stack_pushes` unchanged), the mark is
complete; otherwise another round.  Two or three rounds in all is
usual, each a few microseconds of the GC thread's time.

Why a quiet round is enough: suppose an object reachable at the end
is unmarked.  It was reachable at the snapshot (what is allocated
after is black or logged), so some marked object held the last
pointer to it on a path from the roots; the marker traced that
object and did not see the pointer, so the pointer was overwritten
first, and the overwrite was logged.  The entry was logged after the
thread's flush in the quiet round (or it would have been drained),
so the holder was traced after that round began -- it was found from
an entry drained in that round, which marked something new.  Not
quiet after all.

With `OAK_GC_VERIFY` the GC thread now stops the world once, retires
every TLAB, marks the heap again from the roots as they are, and
reports what the concurrent mark missed ("Verify mode").

### Phase 5: After the mark

1. The barrier goes off.  Entries logged after a thread's last
   handshake, before this, carry this cycle's epoch and are dropped
   at the thread's first append of the next cycle (`satb_fresh`);
   they refer to marked objects anyway
2. Counts what was allocated during the mark (Pacing)
3. Under `alloc_lock`: sets the budget, prepares the lazy sweep
   (clears the free lists and the free bits), sets
   `gc_weak_clearing`, starts the sweep and sets `gc_phase` idle
4. Clears the weak pointer table under `wp_lock` (`ms_post_gc_wp`):
   an entry is cleared unless its object is alive by the marks
   (`ms_weak_alive`: outside the heap, allocated since the snapshot
   -- above its region's `sweep_top`, born black or implicitly live
   and not necessarily marked -- or marked).  A mutator reading a
   weak pointer meanwhile makes the same test itself
   (`satb_weak_read`, data.h, on `gc_weak_clearing`), so an object
   the clearing has not reached yet is never resurrected, and an
   object allocated from a free entry meanwhile is born black
   (`MS_BORN_BLACK`), so a weak pointer made to it survives; then the
   flag is cleared.  The sweep goes on throughout: it leaves the
   mark bits alone (the next cycle clears them all), and resets no
   region while the clearing lasts (a reset moves `sweep_top`)
5. Sweeps enough blocks (the lock taken a block at a time) for the
   largest request any waiter made to be met, or for the budget to
   allow mapping a region, and announces the cycle's end to the
   waiters
6. Sweeps the rest in the background (Lazy Sweep)

So the only stop a thread takes for a cycle is the scan of its own
stacks, a few microseconds (Pause Statistics has the numbers); the
GC thread's rounds hold nobody.

### Lazy Sweep

The sweep runs on demand, region by region:
- Each call to `ms_lazy_sweep_one()` sweeps one 4096-word block of
  the current region, between its start and its `sweep_top`; a run of
  dead objects open at the block's end carries over to the next
- A region found with nothing marked and nothing allocated above its
  `sweep_top` is reset to virgin in one step, without entries (not
  while the weak pointer table is being cleared: it is swept block by
  block then); a region empty for two sweeps running is unmapped when
  the sweep completes, if the heap is over its budget
- The mark bits are left as they are (the next cycle clears them
  all); a TLAB's unused tail is retired -- zeroed, given an objstart
  bit -- when its thread replaces it, and at the thread's first
  handshake of a cycle, so no object's extent absorbs stale words
- With threads, the GC thread sweeps in the background after each
  cycle (`ms_background_sweep`): a block at a time under
  `alloc_lock`, until the sweep is done, a cycle is asked for, or a
  dump or a world load holds the collector off (`gc_sweeping`, which
  `ms_gc_hold` waits out with the cycle), yielding between blocks
  when the threads outnumber the CPUs; so the allocating threads
  find free runs ready, and the next cycle has little or nothing to
  finish.  A thread that needs a run before the sweeper reaches one
  sweeps for it itself (`ms_alloc_slow_locked`), and the slow path
  spins on `alloc_lock` only briefly before it yields
  (`ms_allocate_slow`).  `--gc-stats` reports the sweeps as the GC
  thread's *background sweep*
- Without threads, called from `ms_alloc_slow_locked()` when the
  free lists are empty, so the sweep rate follows the allocation
  rate, and `ms_lazy_sweep_finish()` flushes the remaining blocks at
  the start of the next cycle

### Pacing

With threads a cycle is asked for before the heap runs out, so that
it ends before the mutators have to wait.  The *budget* is
`e_next_newspace_size` (never below `--size-heap`), the *headroom*
`ms_headroom`, and the heap's *occupancy* what the last mark found
live plus what the allocator has handed out since that mark's
snapshot (`ms_since_snapshot`: TLABs as they are carved, free-list
entries and virgin blocks as they are taken).  Every slow-path
allocation tests the trigger: occupancy plus headroom at or above the
budget asks the GC thread for a cycle (`ms_request_cycle`, a no-op
while one runs or is pending).  Meanwhile mapping goes on up to the
budget plus the headroom, or while a cycle runs plus at least half
the budget (`ms_may_map`: the headroom sets when a cycle is asked
for, the runway what the mutators may still map before the mark
ends); a thread that finds no room beyond that waits for the cycle
(`ms_wait_cycle`), and when the cycle leaves it nothing, maps a
region past the budget.

At the end of the mark the GC thread sets the next budget from what
it measured (`ms_set_budget`, `ms_set_pacing`):

- the live data `L` (the words the mark traced: what was reachable
  at the snapshot, not what was allocated during the mark, which the
  marker neither traces nor counts), the words `A` allocated during
  the mark (smoothed: half this cycle's, half the last value), and
  the allocation rate `R` since the previous snapshot (words per ms,
  smoothed the same way);
- budget = max(`--size-heap`, 3L + 2A, L + 2A + max(16A, R ×
  `MS_MIN_CYCLE_MS`)); what is allocated during a mark is born black
  and freed only by the next cycle, so it counts twice, and a cycle
  costs every thread a root scan and the marker its rounds whatever
  the heap size, so cycles come at least 16 marks' worth and 32 ms of
  allocation apart however little is live; grown by half after a cycle in which a thread
  waited, and never more than doubled in one cycle (one long cycle is
  not a trend); capped by `MAX_NEW_SPACE_SIZE`;
- headroom = A, between an eighth and a half of the budget.

Without threads there is no headroom and no trigger: the cycle runs
in the allocating thread when the budget is exhausted.  `--trace-gc 2`
prints the measurements and every change of the budget and headroom,
and each wait with the heap's state; `--trace-gc 3` adds the
allocator's view when it finds no room, and every handshake round
(`--gc-stats` counts them).

### GC Phase Tracking

The `gc_phase` variable tracks the current state.  With threads:

```
GC_PHASE_IDLE
  → GC_PHASE_CONCURRENT_MARK (from the snapshot to the end of the mark;
                              born black from free entries meanwhile)
  → GC_PHASE_IDLE            (the lazy sweep; born black while the weak
                              table is cleared, gc_weak_clearing)
```

The single-threaded cycle passes through `GC_PHASE_SWEEP` as it
prepares the sweep; `GC_PHASE_INITIAL_MARK` and `GC_PHASE_REMARK` are
unused since the pauses went.


SATB Write Barriers
--------------------

Snapshot-at-the-beginning (SATB) barriers ensure concurrent marking
correctness.  They are inserted at all 10 heap-write sites in the
bytecode interpreter (`loop.c`), and natively compiled code has the
same ones through `ON_STORE` (oak-native.h), its one store macro,
which native-parity.test checks the generated C for:

1. SET-CONTENTS (locative store)
2. STORE-BP-I (indexed base-pointer store)
3. SET-CAR (cons car mutation)
4. SET-CDR (cons cdr mutation)
5. FILL-CONTINUATION (continuation segments)
6. TEST-AND-SET-LOCATIVE (atomic CAS)
7. STORE-BP n (base-pointer store variant)
8. STORE-ENV n (environment store)
9. STORE-SLOT (general slot store)
10. Operation cache writes (method/type cache)

and two read barriers: a value read through a weak pointer
(`wp_to_ref`, the WP-CONTENTS instruction, open-coded in generated C
too) is logged the same way (`SATB_READ_WEAK`, `satb_log_value`), and
so is the method a hit in the global method cache or a native call
site yields (`SATB_READ_RAW`, dispatch.c and `ON_SITE_JUMP`).  Every
object reachable at the snapshot is marked by the snapshot's own
rules, but one the snapshot held only weakly, or only in a raw-ref
cache that is no root, read from there during the mark and stored
into a black object or kept on a scanned stack, would not be;
logging the read makes the marker mark it.

The barrier takes the value stored as well as the slot
(`SATB_BARRIER(addr, v)`, `satb_log_store`).  Because the stacks are
scanned one thread at a time, a thread whose stacks are not yet
scanned this cycle (`gc_scan_epoch[i]` is not `gc_mark_epoch`) logs
the value it stores too: a reference held only by its stack, stored
into an already-marked object and then dropped from the stack would
otherwise be lost, and a scanned stack can hold only references the
marker reaches anyway (loaded from marked objects, logged, or
allocated black).  Go's collector uses the same hybrid.

When `gc_satb_active == 0` (the normal state), the barrier is a single
well-predicted branch with negligible overhead.  When active, the
values are logged to a per-thread buffer (1024 entries, stamped with
the cycle's epoch; entries of an earlier epoch are dropped at the
first append); a full buffer goes to the markers' work list and a
fresh one is begun, and so does the buffer at every handshake.

Fresh allocations (CONS, MAKE-CELL, ALLOCATE, etc.) are not barriered
because they write to newly created objects, not overwrite existing
pointer fields.


Rules for Instructions and Native Helpers
=========================================

What the collectors ask of every instruction in `loop.c`, every C
function the generated code calls (`native-ops.c`, `native.c`,
`dispatch.c`), and the C that `native-compiler.oak` emits.  The
concurrent collector depends on each of them; a lapse shows up as a
freed object still in use, rarely and far from its cause, which is
what verify mode is for.

**Allocation** goes through the `ALLOCATE` macros (data.h:
`ALLOCATE`, `ALLOCATE_SS` with the interpreter's stack locals
synchronized around it, `ALLOCATE1` keeping one ref across it) or
`rt_allocate` in the helpers, never around them.  The fast path bumps
the thread's TLAB and sets the objstart bit; the slow path takes
`alloc_lock`, may scan the thread's roots, may wait for a cycle, and
in a copying build may collect.  So an allocation is a safepoint
(below).  A new object's initializing stores need no barrier, but
every word of it is written before the next allocation or poll: a
TLAB carved from a free run holds the dead objects' old words, and
verify mode's mark reads a fresh object's fields as they are.

**Heap stores.**  Every store of a ref into an existing heap object
-- a slot, a cons cell, a vector element, an environment, the cell a
locative names, a cache -- is `SATB_STORE(addr, v)` in C and
`ON_STORE(addr, v)` in generated C: the barrier (`SATB_BARRIER`),
before the store so that it reads the old value, then the store
itself, a relaxed atomic one, since a marker may be reading the
word.  A store into a stack or a register is not barriered.
native-parity.test greps the generated C for raw stores; the
interpreter's ten sites are listed above.

**Shared words.**  Every word one thread writes and another may read
at the same time with no lock between them -- the collector's flags
and counters, the bitmaps, a region's top, the heap words the markers
read -- is accessed through the atomic accessors of oak-atomic.h
(`OAK_LOAD_RELAXED` and `OAK_STORE_RELAXED`; where the protocol
needs an order, `OAK_LOAD_ACQUIRE`, `OAK_STORE_RELEASE` and the
fences; `SHARED_GET`, `SHARED_SET` and `SHARED_ADD` in gc-ms.c,
`GC_READY` and `GC_THREAD_DEAD` in gc.h), never with a plain or a
`volatile` access: a plain access to such a word is a data race,
undefined under the C memory model however benign on the machine,
and ThreadSanitizer reports it.  The relaxed accessors are the plain
load and store on every machine the emulator runs on, so they cost
nothing.  A ThreadSanitizer build of the suite (BUILD.md,
"Sanitizers") reports nothing in the collector; what it reports is
the test programs' own shared variables, written from several
threads without a mutex by design (threads-stress), which the
machine's word-sized accesses serve.

**Reads outside the roots.**  A value read through a weak pointer
goes through `wp_to_ref`, which logs it during a mark and answers
nil while the table is being cleared.  A cache of raw refs that no
root covers (the global method cache, a native call site) logs what a
hit yields (`SATB_READ_RAW`) and is flushed as a mark begins and when
a world is loaded; a new one does the same, or holds a ref some root
holds too.

**Safepoints.**  With threads, a handshake round waits for every
live thread to reach a safepoint, and a dump's pause for every thread
to park: the interpreter polls at the top of its loop
(`POLL_GC_SIGNALS`, every instruction) and generated code at every
call, return and backward branch (`ON_PENDING`, `ON_POLL`), each with
its stacks synchronized first (`value_stack.sp`, `context_stack.sp`
and `e_pc` as the structures show them; `ON_SYNC`), since the
handshake scans the stacks through them.  A C loop that can run for
milliseconds -- over a long vector or string -- holds the GC thread's
round for that long, so it polls too (`wait_for_gc`, stacks
synchronized) or stays short.

**Blocking.**  A thread that blocks in C -- a park, a sleep, a
condition variable, a lock it may wait long for -- blocks as a parked
thread: `gc_ready[i] = 1` before, with its stacks synchronized, and
cleared under `gc_lock` after (`oak_thread_park`,
`oak_thread_sleep_ms`, `ms_wait_cycle`), so that the GC thread does
its handshake part for it and a pause counts it.  A lock taken on a
hot path (`alloc_lock`) is taken with `trylock`, polling `gc_pending`
while waiting, because a thread blocked on a mutex can take part in
nothing and the holder may be waiting for the round.

**Refs in C variables.**  Across a safepoint -- a poll, an
allocation, a park -- a ref lives on a stack, in a register or in a
heap object, never only in a C variable: a C variable is no root.
With the copying collector the variable would hold a stale address;
with the concurrent one a ref that was off the thread's stack when
the handshake scanned it, and goes back on afterwards, has escaped
the mark.  `ALLOCATE1` and `GC_MEMORY`/`GC_RECALL` keep a ref across
an allocation in the interpreter; the C-locals lowering (`--ir`)
reserves stack room before a run of locals (`ON_RESERVE`) so that no
flush, which can collect, falls inside it.

**Interior pointers.**  A ref into the middle of an object (a method
inside the operation it was allocated with, a locative to a cell) is
mapped by the objstart bitmap to its object and keeps it alive; a
raw C pointer into an object is not a ref and keeps nothing alive.

**Testing a change.**  `tests/gc-stress.test` in every mode; a
threads build with `-DOAK_GC_VERIFY` run on it several times (a
missed barrier is reported as a missed object, with its allocation
site and its referrer's); native-parity.test for raw stores in
generated C; `--gc-stats` for what a change costs in stops.


Interaction Between the Two Collectors
======================================

When `USE_MARK_SWEEP` is enabled, the two collectors have distinct
roles:

| Role | Collector |
|------|-----------|
| Every collection, GC and FULL-GC included | Mark-sweep |
| Heap growth and shrinking | Mark-sweep (mapping and unmapping regions) |
| Pre-dump compaction | Copying GC (produces the contiguous heap the dump writes) |

The copying GC runs only for a dump (`--dump` at exit, a full
collection with the stacks discarded; `dump-world` from a program,
an ordinary one), through two hooks in gc-ms.c:
1. `ms_evacuate_begin` retires the TLABs and maps one span big
   enough for everything mapped (and spatic, for a full collection),
   unregistered, as `new_space`; `OLD_PTR` is then "in a region"
   (`ms_old_ptr`), and `NEW_PTR` the span
2. `gc()` runs its normal transport/scavenge cycle; during transport,
   `ms_record_transport(new_place)` records each destination address
3. `ms_evacuate_end`, before the other threads are released, unmaps
   the old regions, registers the span as the heap's one region with
   `top` at `free_point`, replays the transport records into its
   objstart bitmap, drops the TLABs and free lists, and sets the
   budget from the live data the copy counted
4. The dump writes `[new_space.start, free_point)`, which is that
   region; a program that goes on after `dump-world` runs in it

Without the replay, objects surviving a compaction would have no
objstart bits and be invisible to subsequent mark-sweep collections.
A world loaded with LOAD-WORLD unmaps every region first (`ms_destroy`)
and starts an empty heap (`ms_init`).


Allocation Flow Summary
========================

```
                    USE_MARK_SWEEP + THREADS
                    ========================

    ┌─ TLAB has space? ─── yes ──→ bump TLAB cursor (no lock)
    │                               set objstart bit (plain store)
    │                               DONE
    │
    no
    │
    ├─ acquire alloc_lock
    │
    ├─ small: TLAB from a big free run (sweeping lazily for one)?
    │       ─── yes ──→ bump TLAB, DONE
    │
    ├─ small: TLAB from a region's virgin tail, or a region mapped
    │   within the budget? ─── yes ──→ bump TLAB, DONE
    │
    ├─ free list (exact class, or a larger entry split)? ─── yes ──→ DONE
    │
    ├─ lazy sweep → free list? ─── yes ──→ DONE
    │
    ├─ a virgin tail, or a region mapped within the budget? ─── yes ──→ DONE
    │
    ├─ ms_collect() (concurrent mark, sets the budget, lazy sweep)
    │
    ├─ retry the above? ─── yes ──→ DONE
    │
    └─ a region mapped past the budget (own cycle only) ──→ DONE


                    USE_MARK_SWEEP (single-threaded, the default)
                    =============================================

    The same, without the lock; ms_collect() is a STW mark, the
    sweep lazy.


                    THREADS (no USE_MARK_SWEEP)
                    ===========================

    ┌─ acquire alloc_lock
    │   (while waiting, check gc_pending → wait_for_gc() if set)
    │
    ├─ bump pointer has space? ─── yes ──→ release lock, DONE
    │
    ├─ gc() (STW copying GC — all threads parked)
    │
    └─ bump pointer ──→ release lock, DONE


                    Copying (single-threaded, no USE_MARK_SWEEP)
                    ============================================

    ┌─ bump pointer has space? ─── yes ──→ DONE
    │
    ├─ gc() (copying GC)
    │
    └─ bump pointer ──→ DONE
```


GC Trace Output
===============

With `--trace-gc 1`, the GC prints a one-line summary per collection:

```
;MS-GC:96%         mark-sweep, 96% of used space reclaimed
;GC:45%            copying GC, 45% reclaimed
;GC:45%,resize:N   copying GC expanded heap to N refs
```

With `--trace-gc 2`:

```
; MS-GC: mapped a region of 1 chunk (262144 words mapped in 2 regions, budget 262144).
; Mark-sweep GC due to space crunch in CONS instruction.
; Marking... [heap=262144 words in 2 regions, used=260000, free_list=0] roots
; Mark complete.
; Scanning weak pointer table... 0 entries discarded.
; MS-GC: 20000 words live in 262144 mapped: budget 262144 -> 262144.
; Lazy sweep prepared (2 regions).
; MS-GC complete.  0 free on the lists, 262144 words mapped in 2 regions, budget 262144.
; MS-GC pause 0.391 ms.
```

Under `THREADS + USE_MARK_SWEEP` with `--trace-gc 2`:

```
; Mark-sweep GC due to heap at its trigger.
; Marking... [heap=393216 words in 3 regions, used=286720, free_list=53519]
; 202560 words allocated in the 9.5 ms since the last snapshot (21316/ms; smoothed 21316/ms). roots
; Concurrent tracing... done in 2 rounds.
; 81920 words allocated in the mark's 0.48 ms (smoothed 47104).
; Mark complete.
; MS-GC: 60935 words live in 393216 mapped: budget 262144 -> 524288.
; MS-GC: 47104 words allocated during the mark: headroom 32768 -> 65536.
; Lazy sweep prepared (3 regions).
; Scanning weak pointer table... 0 entries discarded.
; Eagerly swept 40302 words.
; MS-GC complete.  40302 free on the lists, 393216 words mapped in 3 regions, budget 524288, headroom 65536.
```

"roots" is the first handshake round done; the rounds are those of
the termination, the roots' included.

Regions mapped and unmapped are traced as above (`mapped a region`,
`unmapped a region`), as is a budget change (`budget 262144 ->
579375`) and a request met past the budget (`growing past the
budget`).

At `--trace-gc 2` every pause ends with its length: `; GC pause
0.694 ms.` after a copying collection, `; MS-GC pause 2.1 ms.` after
a single-threaded mark-sweep one, and in the concurrent collector the
concurrent collector's mark ends with its number of handshake
rounds, and `--trace-gc 3` prints every round and every root scan as
a `; GC event:` line.


Pause Statistics
================

`--gc-stats` (or `--trace-gc 3`; `gc-stats.c`) times every pause and
stall by the monotonic clock and prints a report at exit, in ms:

```
; GC pauses and stalls, ms: count, total, mean, max, p50, p90, p99, and how many under 0.1, 1, 10, 100 ms and over
; concurrent cycle, GC thread         19     28.61    1.506    5.569    0.920    3.931    5.569       0    11     8     0     0
; handshake round, GC thread          59      0.21    0.004    0.016    0.002    0.010    0.016      59     0     0     0     0
; root scan, own stacks               71      0.22    0.003    0.011    0.002    0.008    0.011      71     0     0     0     0
; all stops of one thread             71      0.22    0.003    0.011    0.002    0.008    0.011      71     0     0     0     0
; GC: 0 global pauses, total 0.00 ms, 0.0% of 563 ms; 71 stops of one thread, max 0.011 ms, p99 0.011 ms
```

The kinds:

- **Global pauses**, every thread stopped: a *copying collection*
  (configs 1 and 3; in 2 and 4 the pre-dump compaction), a
  *mark-sweep collection* with one thread (config 2: mark and sweep
  together), and verify mode's *verify pause* at the end of a mark
  (config 4 with `OAK_GC_VERIFY`).  *All global pauses* is their
  union, less the nested ones.
- **Threads stopping**: inside a pause with threads, from the request
  until the last thread has reached a safepoint.
- **The GC thread's own time**, stalling nobody: the *concurrent
  cycle* is the whole cycle as the GC thread sees it, from finishing
  the previous sweep to the end of the eager sweep, a *handshake
  round* is from its request to the last thread's part done, and a
  *background sweep* is the sweep after a cycle, from its start to
  its end or interruption.
- **Stalls of one thread**: a *root scan* (a thread scanning its own
  registers and stacks at its first handshake of a cycle), a
  *safepoint wait* (a thread parked while another held a pause), and
  an *allocation wait* (a thread out of room, waiting for the next
  cycle to end).  *All stops of one thread* is their union with the
  pauses (as the thread that runs a pause sees it).
- The `; GC:` line sums the global pauses up against the run's
  wall-clock time, then the stops of one thread: in a build that
  stops the world for every collection the two are the same events;
  with the concurrent collector the pauses are none and the stops are
  the root scans.  `make bench` reports the stops' max and p99.

The percentiles come from up to 4096 samples per kind, a uniform
reservoir beyond that; count, total and max are exact.  The report
goes to stderr; a stand-alone executable gets the option through
`OAKLISP_OPTIONS`.  `make bench` runs every benchmark with it and
prints the longest and the 99th-percentile stop of one thread of
each run beside the time (tests/bench.sh, `-o` saves them too), and
`-p N` prints the `; GC:` line of its one-thread and N-thread runs.

`tests/gc-stress.test` is the collectors' load test: `gc-stress.oak`
makes and verifies lists, vectors, bignums, closures, continuations,
weak pointers and hash tables on four OS threads at once (one thread
without threads) while they rewrite old objects, under `--size-heap
256` so that collections never stop, run as bytecode, compiled
natively, and as stand-alone executables in every mode oakc has
(plain, `-w`, `--ir`, `--interpreter`); each run's report is in the
log.  Its first run found bignum division's caches shared between
threads (Threads.md, "Shared state"), not a collector bug: verify mode
is clean on it in every mode.  The same test then runs
`weak-young.oak`: weak pointers to objects made during a mark and
held only by an old object, which the remark pause of the time,
clearing every unmarked entry, lost (the objects were implicitly live, above their
regions' `sweep_top`, and unmarked), one run in three.

Measured 2026-11-04, with the region heap, the GC thread, the
handshakes of Phase 5 and the parallel marking of Phase 6
(i9-14900KF, `make bench` pinned to one P core -- so one marker --
FAST builds of the same sources, the least disturbed of three runs
per column): the longest stop of one
thread and its p99 in ms of each benchmark's run (compile plus five
repetitions), in the four configurations -- the global pause in
configs 1 to 3, which have no other stop, a thread's own root scan
or wait in config 4, which has no global pause.  Few collections
happen per run, so the two are mostly equal; dispatch, fib and tak
collect nothing.  (The config 3 column is the region heap's
measurement of 2026-10-29: its code did not change.)

| Benchmark | 1 copying | 2 mark-sweep | 3 threads, copying | 4 threads, mark-sweep |
|-----------|-----------|--------------|--------------------|-----------------------|
| bignum | 0.41 / 0.34 | 0.23 / 0.23 | 0.30 / 0.29 | 0.008 / 0.008 |
| compile | 0.41 / 0.41 | 0.30 / 0.30 | 0.35 / 0.35 | 0.010 / 0.010 |
| cons | 0.49 / 0.36 | 0.26 / 0.25 | 0.31 / 0.29 | 0.008 / 0.008 |
| eval | 0.95 / 0.79 | 1.18 / 1.18 | 0.86 / 0.81 | 0.690 / 0.690 |
| format | 0.38 / 0.38 | 0.22 / 0.22 | 0.33 / 0.33 | 0.004 / 0.004 |
| hash | 6.48 / 6.48 | 5.37 / 5.37 | 6.19 / 6.19 | 0.010 / 0.010 |
| sort | 2.39 / 2.39 | 1.09 / 1.09 | 7.17 / 7.17 | 0.007 / 0.007 |
| strings | 0.39 / 0.38 | 0.27 / 0.27 | 0.31 / 0.29 | 0.004 / 0.004 |

Before the region heap (2026-10-29, five commits earlier), the long
pauses in the mark-sweep configurations were the copying collector
growing the heap, a whole-heap copy each time: 28.3 ms on hash and
8.4 on sort in config 2, 20.6 and 6.5 in config 4, with eval and
compile at 1.5 to 2.3 ms.  Now a mark-sweep build never copies
except for a dump.  In config 4 there is no global pause at all: the
column is the longest stop of one thread, its own root scan, 0.004
to 0.010 ms (eval's 0.69 ms is one allocation wait, the thread
outrunning the marker once before the headroom had adapted); two
commits earlier the two pauses, scanning the stacks only, were 0.17
ms on hash and 0.01 to 0.06 elsewhere, and three commits earlier,
with the spatic scan, the barrier drain and the weak table inside
them, 1.1 and 0.21 to 0.43.  In config 2 the pause is the stop-the-world
mark of the live data, which is why hash and eval pause 1.0 to 5.4
ms there, against the copying collector's 1.0 to 6.5 (config 2 is
the default build since Phase 7 of GCEnhancements.md).  In
`gc-stress.oak` on four threads (config 4, `--size-heap 256`, 24
cycles in 0.65 s, pinned to eight P-core threads, 2026-11-04) no
thread stopped another: each thread's root scan took at most 0.03 ms
(p50 0.002), the GC thread's rounds (two per cycle) at most 1.0 ms
(p50 0.005), and no thread waited for memory in bytecode or mixed
native; as oakc executables, which allocate faster, the four threads
each waited once, up to 0.9 ms, for the first big cycle in 4 of 12
runs (every run, up to 1.3 ms, with the one marker of the commit
before parallel marking) -- against 38 global pauses, 0.4% of the
run, two commits earlier (initial mark up to 0.06 ms, remark 1.1;
the threads parked for them up to 1.2 ms), 1.9% three commits
earlier, and 5.1% with the copying growth before the region heap (71
pauses, copies up to 6.2 ms, the run 1.03 s; the region heap alone:
the cycle's thread lost up to 3.1 ms, 46 waits up to 2.9 ms).  The GC
thread's own time per run, stalling nobody, was 30 to 36 ms with one
marker and 13 to 14 with four (the longest cycle 3.6 to 5.3 ms down
to 1.3 to 2.5); on the 13-million-word heap of "Phase 3" the mark
took 33 ms with one marker and 9 with eight.  The same
program in config 1, copying (95 collections), paused at most
1.2 ms, p50 0.47, 3.0% of the run, and in config 2 (141 cycles) at
most 0.70 ms, p50 0.36, 2.7%.  Four copies of tak on four OS threads
(`tests/bench.sh -p 4`, config 4) ran 3.95 times faster than on one,
with no collection.  A measurement note: on one core a round ends
when the last thread signals the GC thread, not when the mutator's
slice ends (`gc_hs_cv`); before that, a round cost the cycle 2.5 ms
of waiting and the throughput 8%.


Key Design Decisions
====================

**Non-moving mark-sweep as foundation for concurrency.**  The copying
GC moves every object, requiring all pointers (including locatives and
the raw instruction pointer `e_pc`) to be updated.  This is
fundamentally incompatible with concurrent mutation.  The mark-sweep
GC never moves objects, so no pointer fixup is needed, and mutator
threads can safely dereference pointers during collection.

**A GC thread.**  A cycle run by whichever mutator ran out of memory
stalled that thread for the length of the mark, and the others once
the heap was full, since nothing could be mapped until the cycle
ended.  On a thread of its own the mark costs the mutators only
their root scans, the heap is paced so that a cycle is asked for
while there is still room (Pacing), and a thread waits only when the
mutators together allocate faster than the GC thread marks.

**Handshakes, not pauses.**  Two stop-the-world pauses per cycle,
however short, held every thread for the slowest to reach a
safepoint, and parked them all for the scan of all their stacks.
Now each thread scans its own stacks at a safepoint of its own and
goes on, a round of such handshakes holds only the GC thread, and
the mark ends when a round of buffer flushes marks nothing new.  The
price is a barrier that also logs the value stored while the storing
thread's stacks are unscanned -- one more test on the barrier's slow
path -- and a read barrier on the raw-ref caches, which the pauses
used to cover by rescanning the stacks.

**Parallel marking by packets.**  The markers share work through a
list of packets -- barrier buffers, spatic ranges, and objects a
worker exports from its own stack -- rather than through per-worker
deques with stealing: a packet costs its owner one allocation and
one lock per few hundred objects, the barrier buffers are packets
already, and a parked mutator can join and leave a run under the
one lock.  What made it scale was not the sharing scheme but three
details: a worker's record on cache lines of its own, exports from
the bottom of the stack (the big subtrees) at most once per few
hundred marks (so a wide object is handed out in large pieces), and
spinning briefly before sleeping, which makes a wake-up the
exception.

**Sweep uses objstart bitmap, not type-pointer length.**  The
`MAKE-CELL` instruction allocates 1-word cells where `word[0]` stores
an arbitrary value, not a type pointer.  Following it as a type pointer
gives incorrect object lengths.  The objstart bitmap is the
authoritative source of object boundaries.

**Free-list gap zeroing.**  When the free-list entry taken is larger
than the request, the gap is zeroed: it has no objstart bit, so the
bitmap counts it as the new object's tail, and zeros are fixnums
that no collector can misread as a pointer.

**Lazy sweep.**  Rather than sweeping the entire heap in a single
STW pass after marking, sweep runs incrementally during allocation.
This eliminates the STW sweep pause entirely under `THREADS` and
amortizes sweep cost across allocations in single-threaded mode.
With threads the GC thread does most of it in the background after
the cycle, so the allocating threads mostly find free runs ready.

**Born-black allocation.**  Objects allocated from free lists during
concurrent mark are below their region's `sweep_top` and would be
incorrectly freed.  Setting their mark bits atomically prevents this.
Objects allocated via a virgin tail or TLAB are above `sweep_top` and
are never swept (implicitly live).  Neither kind is ever traced: what
they can refer to is covered by the snapshot's rules, the logging of
an unscanned thread's stores, and the read barriers.

**A heap of regions.**  A contiguous heap can only grow by being
copied, which is a whole-heap pause.  Regions (spans of aligned 1 MB
chunks, each with its own bitmaps, found by a two-level table) make
growth a `mmap` and shrinking a `munmap`, and give the sweep a unit
at which "nothing live" is cheap to see and act on.  The objstart
and mark bitmaps, the free lists and the TLABs work as before within
a region; the only costs are a table lookup where a bounds check
used to do, and a slot encoding for GET-DATA.

**Copying GC retained for dumps.**  Rather than implementing heap
compaction within the mark-sweep framework, the proven copying GC
produces the contiguous, zero-based image a world dump writes.  It
never runs otherwise in a mark-sweep build.


Source Files
============

| File | Purpose |
|------|---------|
| `gc-ms.c` | Mark-sweep GC: regions, bitmaps, free lists, mark, sweep, TLABs, SATB, concurrent collection, parallel marking, the dump compaction's hooks |
| `gc-ms.h` | Mark-sweep GC declarations, the region struct and lookup, constants, SATB barrier macro |
| `gc.c` | Copying GC: transport, scavenge, locative fixup, heap expansion (the dump compaction in mark-sweep builds) |
| `gc.h` | Copying GC declarations, `GC_MEMORY`/`GC_RECALL` macros |
| `data.h` | `ALLOCATE_PROT` macro (three variants), heap data structures |
| `loop.c` | SATB barrier insertion sites, allocation instruction handlers, the safepoint poll |
| `oak-native.h` | Generated code's side: `ON_STORE` (the barrier), `ON_PENDING`/`ON_POLL` (the safepoint), `ON_RESERVE` (no ref only in a C local) |
| `dispatch.c` | The global method cache: raw refs, flushed as a mark begins, a hit logged (`SATB_READ_RAW`) |
| `threads.c` | `wait_for_gc`: a thread's safepoint (a pause to wait out, or its handshake), the parked states (`gc_ready`), thread exit |
| `oaklisp.c` | GC initialization (`ms_init`, `gc_safepoint_init`) |
| `weak.c` | Weak pointer table processing (used by both collectors) |
| `gc-stats.c`, `gc-stats.h` | Pause and stall timing and the `--gc-stats` report |


Tuning Constants
================

These are compile-time constants in the header files:

| Constant | Value | File | Description |
|----------|-------|------|-------------|
| `DEFAULT_NEWSPACE` | 512 (128 with the copying collector) | `data.h` | Default heap size in kilo-refs; the mark-sweep budget's floor |
| `RECLAIM_FACTOR` | 3 | `gc.c` | Copying GC target: live data should be < 1/3 of heap |
| `MS_RECLAIM_FACTOR` | 3 | `gc-ms.c` | The mark-sweep budget: three times the live data |
| `MS_REGION_LOG2_BYTES` | 20 | `gc-ms.h` | A chunk is 1 MB; regions are spans of chunks |
| `MS_EMPTY_CYCLES` | 2 | `gc-ms.c` | Sweeps a region must stay empty before it is unmapped |
| `MS_CYCLE_SPACING` | 16 | `gc-ms.c` | Cycles at least this many marks' worth of allocation apart (threads) |
| `MS_MIN_CYCLE_MS` | 32 | `gc-ms.c` | Cycles at least this much allocation time apart, at the measured rate (threads) |
| `NUM_SIZE_CLASSES` | 41 | `gc-ms.h` | Number of segregated free list size classes (one size each up to 32 words) |
| `TLAB_SIZE` | 4096 | `gc-ms.h` | Per-thread allocation buffer size in words |
| `TLAB_MIN_FREE` | 257 | `gc-ms.c` | The least free run (or virgin tail) worth a TLAB |
| `SWEEP_BLOCK_SIZE` | 4096 | `gc-ms.h` | Lazy sweep granularity in words |
| `SATB_BUFFER_SIZE` | 1024 | `gc-ms.h` | SATB write barrier buffer entries per thread |
| `MS_MAX_MARKERS` | 8 | `gc-ms.c` | The default `--gc-threads`: one per CPU the process may run on, up to this |
| `MS_PACKET_SIZE` | 512 | `gc-ms.c` | Objects in a packet a marker exports from its stack |
| `MS_EXPORT_MIN` | 8 | `gc-ms.c` | A marker's stack is worth exporting half of from this depth |
| `MS_EXPORT_INTERVAL` | 256 | `gc-ms.c` | Objects a marker marks between exports |
| `MS_SPIN_NS` | 20000 | `gc-ms.c` | A marker out of work spins this long (ns) before sleeping, when the CPUs allow |
| `MS_SPATIC_CHUNK` | 16384 | `gc-ms.c` | Words of spatic per root-scan packet |
| `MAX_NEW_SPACE_SIZE` | (optional) | `Makefile.am` | Hard cap on heap size and on the mark-sweep budget (disabled by default) |

To limit maximum heap growth, compile with:
```sh
make CPPFLAGS="-DMAX_NEW_SPACE_SIZE=16000000"
```


Recommendations
===============

**For single-threaded use:**  The default build (Config 2, the
mark-sweep collector with a stop-the-world mark; "Phase 7" in
GCEnhancements.md has the decision).  It executes within 1.4% of the
copying collector's instructions on every benchmark, fewer on most,
its pauses are the marks, shorter than the copying collections on
every benchmark but eval (hash: at most 5.4 ms against 6.5), and its
heap is mapped on demand, with no second semispace: on the small
benchmarks it takes 0.5 to 4 MB more (the 4 MB default budget), on
sort and hash 15% and 7% less.  Config 1 (`--disable-mark-sweep`) is
the copying collector, as before.

**For multi-threaded use:**  Config 4 (`THREADS + USE_MARK_SWEEP`) is
strongly recommended.  The concurrent mark-sweep collector stops no
thread for another (a thread's one stop is the scan of its own
stacks, microseconds; the sweep is lazy), and TLABs eliminate
allocation lock contention for the common case.
Config 3 (`--enable-threads --disable-mark-sweep`) has longer pauses
(every collection is a stop-the-world copy) and all allocations
contend on `alloc_lock`.

**Heap sizing:**  The default 512K refs (4 MB; 128K, 1 MB, with the
copying collector) is adequate for interactive use.  For compiling large
files or running allocation-heavy programs, 1024-4096K (8-32 MB) is
recommended.  The GC automatically expands the heap when needed, but
starting larger avoids unnecessary early collections; with mark-sweep
the size is a budget the heap is mapped up to as needed, and shrinks
back towards when the live data shrinks.
