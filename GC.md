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
| `USE_MARK_SWEEP` | off | Enable the mark-sweep GC for normal collections |
| `THREADS` | off | Enable pthread-based multi-threading |

The four resulting configurations:

| # | Flags | Primary GC | Allocation | Threading |
|---|-------|-----------|------------|-----------|
| 1 | (none) | Copying | Bump pointer | Single |
| 2 | `USE_MARK_SWEEP` | Mark-sweep (STW) | TLAB + free list + bump | Single |
| 3 | `THREADS` | Copying | Bump pointer + `alloc_lock` | Multi |
| 4 | `USE_MARK_SWEEP + THREADS` | Mark-sweep (concurrent) | TLABs + free list + bump | Multi |

How to build each:

```sh
# After ./configure:

# Config 1: Default (copying GC, single-threaded)
make

# Config 2: Mark-sweep, single-threaded
make CPPFLAGS="-DUSE_MARK_SWEEP"

# Config 3: Copying GC, multi-threaded
make CPPFLAGS="-DTHREADS"

# Config 4: Concurrent mark-sweep, multi-threaded
./configure --enable-threads
make
```

`--enable-threads` gives config 4; configs 2 and 3 have no configure
option and need the flags passed by hand.  All four pass the test
suite (see Threads.md, Testing).  Adding `-DOAK_GC_VERIFY` to a
threads build makes every concurrent mark-sweep cycle check itself:
see "Verify mode" below.


Verify mode
-----------

`make CPPFLAGS="-DOAK_GC_VERIFY"` in a threads build (GCC or Clang)
checks the concurrent collector as it runs.  At the initial mark it
traces the whole snapshot stop-the-world into a separate bitmap; at
the end of the remark it marks the heap again from scratch and
reports every live object below `sweep_limit` that the concurrent
mark missed, with the object referring to it (its type, whether it
was marked, born black, reachable at the snapshot) and where both
were allocated, from a ring of the last four million allocations
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
| `--size-heap N` | 128 | Heap size in kilo-refs (1 ref = 8 bytes on 64-bit) |
| `--trace-gc V` | 0 | GC verbosity: 0=quiet, 1=summary, 2=detailed (each pause with its length), 3=very detailed (plus every pause and stall as it ends, and the report below) |
| `--gc-stats` | off | report the pauses and stalls at exit (Pause Statistics, below) |
| `--predump-gc B` | 1 | Run GC before world dump: 0=no, 1=yes |

The default heap is 128 kilo-refs = 128K × 8 bytes = 1 MB on 64-bit
systems.  This is sufficient for most interactive use and compilation
of individual `.oak` files.

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
(`DEFAULT_NEWSPACE`, 512K words), since every cycle scans spatic and
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

Enabled by defining `USE_MARK_SWEEP` at compile time.


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

When a free-list block is larger than the request, the unused gap is
zeroed with `memset` to prevent the copying GC's `scavenge()` from
seeing stale pointer values.

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

TLABs are retired before every STW pause (`ms_tlab_retire_all`):
unused TLAB space is zeroed and given an objstart bit so sweep can
reclaim it.


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

Under `THREADS + USE_MARK_SWEEP`, the GC uses a three-phase concurrent
collection with two brief STW pauses, run by a thread of its own.

### The GC thread

Every cycle runs on the GC thread (`ms_gc_thread_main`), started at
the first request, so that no mutator is ever out of action for the
length of a mark.  It has no slot in the thread arrays: it runs no
Oaklisp code, allocates nothing and holds no roots, so the
stop-the-world handshakes it runs do not wait for it, and
`%thread-count` does not count it; its index (`index_key`,
`oak_thread_index_var`) is `MS_GC_THREAD_INDEX`, -1.  A mutator asks
for a cycle (`ms_request_cycle`, under `gc_concurrent_lock`) and goes
on; one that has no room left waits for the next cycle to end
(`ms_wait_cycle`, parked at a safepoint with `gc_ready` set, leaving
that state under `gc_lock`), and the GC and FULL-GC instructions wait
the same way.  A dump (`gc()`) and a world load (`ms_destroy`), which
move or unmap the heap, hold the GC thread off (`ms_gc_hold`: a cycle
in progress ends first, parked meanwhile, and none starts until
`ms_gc_release`).  Verify mode's record of who has the world stopped
(`ms_verify_stw_owner`) is the GC thread's index during a pause and
`MS_NO_STW_OWNER` otherwise.

### Phase 1: Initial Mark (STW)

The GC thread, having finished the previous cycle's sweep (a block at
a time, releasing `alloc_lock` between blocks so the allocator gets
in) and cleared the mark bitmaps under `alloc_lock`:
1. Sets `gc_pending` — all mutators park via `wait_for_gc()`
2. Retires every TLAB and snapshots every region's `sweep_top`, and
   measures the allocation rate since the last snapshot (Pacing)
3. Scans the registers and stacks of every thread
   (`ms_mark_thread_roots()`): what no barrier covers
4. Activates SATB write barriers (`gc_satb_active = 1`)
5. Releases all threads — mutators resume

### Phase 2: Concurrent Mark

The GC thread traces the heap while mutators run:
- Scans spatic first (`ms_mark_spatic_concurrent()`): every pointer
  from the world image into the heap is a root, but stores into
  spatic are barriered like any, so the scan can run under the
  barrier rather than in the pause -- a word read old was there since
  the snapshot, one read new refers to what the snapshot's rules mark
  anyway, and what was overwritten is in a barrier buffer
- Uses `ms_trace_concurrent()` with atomic mark-bitmap operations
  (`__atomic_fetch_or`)
- Mutators' SATB barriers log old pointer values before any heap
  write; a buffer that fills (1024 entries) is handed to the marker
  (`satb_full_list`, under `satb_lock`) and a fresh one begun, and the
  marker drains the full ones (`ms_satb_drain_concurrent`) each time
  its mark stack runs dry, until neither has anything left
- Objects allocated from free lists during this phase are marked
  atomically ("born-black") via `ms_mark_alloc()`; a TLAB carved from
  a free run is blackened whole (`ms_blacken`)
- Objects allocated from virgin tails (TLABs included) are above
  their region's `sweep_top` and are implicitly live (not swept); a
  region mapped during the mark has `sweep_top` at its start
- The region table may grow meanwhile (a mutator mapping a region
  under `alloc_lock` publishes it with release stores; the marker's
  lookups are acquire loads); regions are only unmapped between
  cycles

### Phase 3: Remark (STW)

The GC thread parks all threads again, then:
1. Retires TLABs allocated during concurrent mark, and counts what
   was allocated during the mark (Pacing)
2. Drains the SATB buffers into the mark stack
   (`satb_drain_to_mark_stack()`): any full ones handed over since the
   marker last looked, and each thread's partial one
3. Re-scans the registers and stacks (C arrays, not covered by SATB;
   `ms_mark_thread_roots()`).  Spatic is not rescanned (its stores
   are barriered), and what was allocated during the mark is black
   and is not scanned: everything it can refer to was reachable at
   the snapshot (marked by the snapshot's rules), was allocated after
   it (black), or was read through a weak pointer, which the
   weak-read barrier logged
4. Traces all newly discovered references
5. Deactivates SATB barriers
6. Sets the budget, prepares the lazy sweep (clears free lists and
   the free bits, flushes the global method cache and the native
   call-site caches, whose raw refs are no roots), and sets
   `gc_weak_clearing`
7. Releases all threads
8. Then, outside the pause, clears the weak pointer table under
   `wp_lock` (`ms_post_gc_wp`): an entry is cleared unless its object
   is alive by the marks (`ms_weak_alive`: outside the heap,
   allocated since the snapshot -- above its region's `sweep_top`,
   born black or implicitly live and not necessarily marked -- or
   marked).  A mutator reading a weak pointer meanwhile makes the
   same test itself (`satb_weak_read`, data.h, on
   `gc_weak_clearing`), so an object the clearing has not reached yet
   is never resurrected, and an object allocated from a free entry
   meanwhile is born black (`MS_BORN_BLACK`), so a weak pointer made
   to it survives; then the flag is cleared.  The sweep, started in
   the pause, goes on throughout: it leaves the mark bits alone (the
   next cycle clears them all), and resets no region while the
   clearing lasts (a reset moves `sweep_top`)
9. Sweeps enough blocks (the lock taken a block at a time) for the
   largest request any waiter made to be met, or for the budget to
   allow mapping a region, and announces the cycle's end to the
   waiters

So the two pauses hold the registers and stacks only: on the stress
test the initial mark takes at most 0.06 ms (p50 0.01) and the
remark 1.1 ms (p50 0.035), against 0.24 and 0.89 before, 0.2 ms of
each being the scan of the world image.

### Post-Remark: Lazy Sweep

After threads are released, sweep runs on demand, region by region:
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
  bit -- when its thread replaces it as well as at the pauses, so no
  object's extent absorbs stale words
- Called from `ms_alloc_slow_locked()` when free lists are empty
- Naturally adapts sweep rate to allocation pressure
- `ms_lazy_sweep_finish()` flushes remaining blocks at the start of
  the next GC cycle

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

At the remark the GC thread sets the next budget from what it
measured (`ms_set_budget`, `ms_set_pacing`):

- the live data `L` (the words the mark traced), the words `A`
  allocated during the mark (smoothed: half this cycle's, half the
  last value), and the allocation rate `R` since the previous
  snapshot (words per ms, smoothed the same way);
- budget = max(`--size-heap`, 3L + 2A, L + 2A + max(16A, R ×
  `MS_MIN_CYCLE_MS`)); what is allocated during a mark is born black
  and freed only by the next cycle, so it counts twice, and a cycle
  costs every thread two pauses whatever the heap size, so cycles
  come at least 16 marks' worth and 32 ms of allocation apart however
  little is live; grown by half after a cycle in which a thread
  waited, and never more than doubled in one cycle (one long cycle is
  not a trend); capped by `MAX_NEW_SPACE_SIZE`;
- headroom = A, between an eighth and a half of the budget.

Without threads there is no headroom and no trigger: the cycle runs
in the allocating thread when the budget is exhausted.  `--trace-gc 2`
prints the measurements and every change of the budget and headroom,
and each wait with the heap's state; `--trace-gc 3` adds the
allocator's view when it finds no room, and the remark's parts.

### GC Phase Tracking

The `gc_phase` variable tracks the current state:

```
GC_PHASE_IDLE
  → GC_PHASE_INITIAL_MARK   (STW pause 1)
  → GC_PHASE_CONCURRENT_MARK (mutators running)
  → GC_PHASE_REMARK          (STW pause 2)
  → GC_PHASE_SWEEP           (lazy sweep, mutators running)
  → GC_PHASE_IDLE
```


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

and one read barrier: a value read through a weak pointer
(`wp_to_ref`, the WP-CONTENTS instruction, open-coded in generated C
too) is logged the same way (`SATB_READ_WEAK`, `satb_log_value`).
Every object reachable at the snapshot is marked by the snapshot's
own rules, but one the snapshot held only weakly, read through its
weak pointer during the mark and stored into a black object, would
not be; logging the read makes the remark mark it.

When `gc_satb_active == 0` (the normal state), the barrier is a single
well-predicted branch with negligible overhead.  When active, the old
pointer value is logged to a per-thread buffer (1024 entries); a full
buffer is handed to the marker, which drains it concurrently, and a
fresh one begun.

Fresh allocations (CONS, MAKE-CELL, ALLOCATE, etc.) are not barriered
because they write to newly created objects, not overwrite existing
pointer fields.


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


                    USE_MARK_SWEEP (single-threaded)
                    ================================

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


                    Default (single-threaded, no USE_MARK_SWEEP)
                    =============================================

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
; Mark-sweep GC due to space crunch in CONS instruction.
; Marking... [heap=262144 words in 2 regions, used=260000, free_list=0]
; STW initial mark... roots 0.226 ms.
; Concurrent tracing... done.
; STW remark... done.
; Mark complete.
; Scanning weak pointer table... 0 entries discarded.
; Lazy sweep prepared (2 regions).
; Eagerly swept 3576 words.
; Remark pause 0.248 ms.
; MS-GC complete.  3576 free on the lists, 262144 words mapped in 2 regions, budget 262144.
```

Regions mapped and unmapped are traced as above (`mapped a region`,
`unmapped a region`), as is a budget change (`budget 262144 ->
579375`) and a request met past the budget (`growing past the
budget`).

At `--trace-gc 2` every pause ends with its length: `; GC pause
0.694 ms.` after a copying collection, `; MS-GC pause 2.1 ms.` after
a single-threaded mark-sweep one, and in the concurrent collector the
initial-mark line ends with its pause and `; Remark pause ...`
follows the sweep preparation, when the mutators are released.


Pause Statistics
================

`--gc-stats` (or `--trace-gc 3`; `gc-stats.c`) times every pause and
stall by the monotonic clock and prints a report at exit, in ms:

```
; GC pauses and stalls, ms: count, total, mean, max, p50, p90, p99, and how many under 0.1, 1, 10, 100 ms and over
; copying collection                   8     25.32    3.165    6.276    2.315    6.276    6.276       0     0     8     0     0
; initial-mark pause                  24      5.74    0.239    0.345    0.228    0.318    0.345       0    24     0     0     0
; remark pause                        24     10.29    0.429    1.749    0.241    1.033    1.749       0    21     3     0     0
;   threads stopping                  56      0.02    0.000    0.002    0.000    0.001    0.002      56     0     0     0     0
; concurrent cycle, its thread        24     55.81    2.325    9.800    0.751    5.611    9.800       0    14    10     0     0
; safepoint wait                      65     62.94    0.968    4.293    0.350    3.445    4.293       0    53    12     0     0
; allocation wait                     46     97.23    2.114    9.869    0.566    8.218    9.869       0    32    14     0     0
; all global pauses                   56     41.35    0.738    6.276    0.244    2.246    6.276       0    45    11     0     0
; GC: 56 global pauses, total 41.35 ms, max 6.276 ms, p99 6.276 ms, 7.2% of 573 ms
```

The kinds:

- **Global pauses**, every thread stopped: a *copying collection*
  (configs 1 and 3; in 2 and 4 the fallback, the growth and the
  pre-dump compaction), a *mark-sweep collection* with one thread
  (config 2: mark and sweep together, a fallback copy nested inside
  it counted under its own kind only), and the concurrent collector's
  *initial-mark* and *remark* pauses (config 4).  *All global pauses*
  is their union, less the nested ones, and the `; GC:` line sums it
  up against the run's wall-clock time.
- **Threads stopping**: inside a pause with threads, from the request
  until the last thread has reached a safepoint -- the handshake
  latency Phase 5 of GCEnhancements.md cares about.
- **Stalls of one thread**: a *safepoint wait* (a thread parked while
  the GC thread held a pause), and an *allocation wait* (a thread out
  of room, waiting for the next cycle to end; its safepoint waits
  meanwhile count under both).  The *concurrent cycle* is the whole
  cycle as the GC thread sees it, from finishing the previous sweep
  to the end of the eager sweep; it stalls nobody.

The percentiles come from up to 4096 samples per kind, a uniform
reservoir beyond that; count, total and max are exact.  The report
goes to stderr; a stand-alone executable gets the option through
`OAKLISP_OPTIONS`.  `make bench` runs every benchmark with it and
prints the longest and the 99th-percentile pause of each run beside
the time (tests/bench.sh, `-o` saves them too), and `-p N` prints
the `; GC:` line of its one-thread and N-thread runs.

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
held only by an old object, which a remark that cleared every
unmarked entry lost (the objects were implicitly live, above their
regions' `sweep_top`, and unmarked), one run in three.

Measured 2026-10-31, with the region heap, the GC thread and the
pauses of Phase 4 (i9-14900KF, `make bench` pinned to one P core,
FAST builds of the same sources): the longest pause and the p99
pause in ms of each benchmark's run (compile plus five repetitions),
in the four configurations.  Few collections happen per run, so the
two are mostly equal; dispatch, fib and tak collect nothing.  (The
config 3 column is the region heap's measurement of 2026-10-29: its
code did not change.)

| Benchmark | 1 copying | 2 mark-sweep | 3 threads, copying | 4 threads, mark-sweep |
|-----------|-----------|--------------|--------------------|-----------------------|
| bignum | 0.31 / 0.31 | 0.27 / 0.27 | 0.30 / 0.29 | 0.036 / 0.036 |
| compile | 0.40 / 0.40 | 0.33 / 0.33 | 0.35 / 0.35 | 0.011 / 0.011 |
| cons | 0.40 / 0.30 | 0.28 / 0.27 | 0.31 / 0.29 | 0.056 / 0.056 |
| eval | 0.86 / 0.83 | 1.00 / 1.00 | 0.86 / 0.81 | 0.019 / 0.019 |
| format | 0.35 / 0.35 | 0.24 / 0.24 | 0.33 / 0.33 | 0.009 / 0.009 |
| hash | 6.52 / 6.52 | 4.73 / 4.73 | 6.19 / 6.19 | 0.171 / 0.171 |
| sort | 2.42 / 2.42 | 1.08 / 1.08 | 7.17 / 7.17 | 0.011 / 0.011 |
| strings | 0.36 / 0.35 | 0.25 / 0.25 | 0.31 / 0.29 | 0.012 / 0.012 |

Before the region heap (2026-10-29, three commits earlier), the
long pauses in the mark-sweep configurations were the copying
collector growing the heap, a whole-heap copy each time: 28.3 ms on
hash and 8.4 on sort in config 2, 20.6 and 6.5 in config 4, with
eval and compile at 1.5 to 2.3 ms.  Now a mark-sweep build never
copies except for a dump.  In config 4 every pause is an initial
mark or a remark, and both scan the registers and stacks only: the
longest is 0.17 ms (hash's remark), the rest 0.01 to 0.06 ms, where
one commit earlier, with the spatic scan, the barrier drain and the
weak table inside the pauses, hash paused 1.1 ms and the rest 0.21
to 0.43.  In config 2 the pause is the stop-the-world mark of the
live data, which is why hash and eval pause 1.0 to 4.7 ms there
(Phase 7 of GCEnhancements.md is about that build).  In
`gc-stress.oak` on four threads (config 4, `--size-heap 256`, 19
cycles in 0.54 s, twenty runs) initial mark took at most 0.06 ms
(p50 0.01), remark at most 1.1 ms (p50 0.035: the last partial
barrier buffers, the stacks, and the trace from them), the threads
stopped within 0.12 ms (p50 1 us) -- 38 global pauses, 0.4% of the
run, against 1.9% one commit earlier (initial mark up to 0.24 ms,
remark 0.89, p50 0.23) and 5.1% with the copying growth before the
region heap (71 pauses, copies up to 6.2 ms, the run 1.03 s).  No
thread ran a cycle or waited for memory in the twenty runs (the
region heap alone: the cycle's thread lost up to 3.1 ms, 46 waits up
to 2.9 ms); the first run after a build, its pages cold, saw the
four threads wait 0.6 ms once each at the limit of the runway
("Pacing").  What remains is the safepoint wait of a thread parked
for a pause, at most 1.2 ms (140 times, p50 0.05) -- Phases 5 and
6.  The GC thread's own time per cycle was up to 6.4 ms (p50 0.9),
stalling nobody.  The same program in the default build (95 copying
collections) paused at most 1.2 ms, p50 0.47, 3.1% of the run, and
in config 2 (141 cycles) at most 0.72 ms, p50 0.36, 2.9%.  Four
copies of tak on four OS threads (`tests/bench.sh -p 4`, config 4)
ran 3.96 times faster than on one, with no collection.  A
measurement note: pinned to one core, the GC thread does not run
again for a scheduler's slice after it releases the mutators, so the
pauses are timed up to the release; the mutators' safepoint waits are
the pauses as they felt them.


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
ended.  On a thread of its own the mark costs the mutators only the
two pauses, the heap is paced so that a cycle is asked for while
there is still room (Pacing), and a thread waits only when the
mutators together allocate faster than the GC thread marks.

**Sweep uses objstart bitmap, not type-pointer length.**  The
`MAKE-CELL` instruction allocates 1-word cells where `word[0]` stores
an arbitrary value, not a type pointer.  Following it as a type pointer
gives incorrect object lengths.  The objstart bitmap is the
authoritative source of object boundaries.

**Free-list gap zeroing.**  When a free-list block is larger than the
request, the gap is zeroed.  This prevents the copying GC's linear
`scavenge()` from seeing stale `PTR_TAG` values in the gap during a
fallback collection.

**Lazy sweep.**  Rather than sweeping the entire heap in a single
STW pass after marking, sweep runs incrementally during allocation.
This eliminates the STW sweep pause entirely under `THREADS` and
amortizes sweep cost across allocations in single-threaded mode.

**Born-black allocation.**  Objects allocated from free lists during
concurrent mark are below their region's `sweep_top` and would be
incorrectly freed.  Setting their mark bits atomically prevents this.
Objects allocated via a virgin tail or TLAB are above `sweep_top` and
are never swept (implicitly live).  Neither kind is scanned at the
remark: what they can refer to is covered by the snapshot's rules and
the weak-read barrier, and scanning a mark's worth of allocation
inside the pause cost milliseconds.

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
| `gc-ms.c` | Mark-sweep GC: regions, bitmaps, free lists, mark, sweep, TLABs, SATB, concurrent collection, the dump compaction's hooks |
| `gc-ms.h` | Mark-sweep GC declarations, the region struct and lookup, constants, SATB barrier macro |
| `gc.c` | Copying GC: transport, scavenge, locative fixup, heap expansion (the dump compaction in mark-sweep builds) |
| `gc.h` | Copying GC declarations, `GC_MEMORY`/`GC_RECALL` macros |
| `data.h` | `ALLOCATE_PROT` macro (three variants), heap data structures |
| `loop.c` | SATB barrier insertion sites, allocation instruction handlers |
| `oaklisp.c` | GC initialization (`ms_init`, `gc_safepoint_init`) |
| `weak.c` | Weak pointer table processing (used by both collectors) |
| `gc-stats.c`, `gc-stats.h` | Pause and stall timing and the `--gc-stats` report |


Tuning Constants
================

These are compile-time constants in the header files:

| Constant | Value | File | Description |
|----------|-------|------|-------------|
| `DEFAULT_NEWSPACE` | 128 (512 with mark-sweep) | `data.h` | Default heap size in kilo-refs; the mark-sweep budget's floor |
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
| `MAX_NEW_SPACE_SIZE` | (optional) | `Makefile.am` | Hard cap on heap size and on the mark-sweep budget (disabled by default) |

To limit maximum heap growth, compile with:
```sh
make CPPFLAGS="-DUSE_MARK_SWEEP -DMAX_NEW_SPACE_SIZE=16000000"
```


Recommendations
===============

**For single-threaded use:**  The default build (Config 1, copying GC).

**For multi-threaded use:**  Config 4 (`THREADS + USE_MARK_SWEEP`) is
strongly recommended.  The concurrent mark-sweep collector minimizes
STW pause times (only initial mark and remark are STW; sweep is lazy),
and TLABs eliminate allocation lock contention for the common case.
Config 3 (`THREADS` alone) has longer pauses (every collection is a
stop-the-world copy) and all allocations contend on `alloc_lock`.

**Heap sizing:**  The default 128K refs (1 MB; 512K, 4 MB, with
mark-sweep) is adequate for interactive use.  For compiling large
files or running allocation-heavy programs, 1024-4096K (8-32 MB) is
recommended.  The GC automatically expands the heap when needed, but
starting larger avoids unnecessary early collections; with mark-sweep
the size is a budget the heap is mapped up to as needed, and shrinks
back towards when the live data shrinks.
