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
any allocation made while another thread has the world stopped.  A
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
- two bitmaps, one bit per word from `start`: the **objstart bitmap**
  (the first word of every allocated object) and the **mark bitmap**
  (the objects found reachable; cleared before each cycle);
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

19 size classes with these maximum sizes (in `ref_t` words):

```
Class:  0   1   2   3   4   5   6    7    8    9
Max:    1   2   3   4   6   8  12   16   24   32

Class: 10  11  12   13   14   15   16    17   18
Max:   48  64  96  128  192  256  512  1024  inf
```

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
room.  An objstart bit is set for the new object.

### Tier 4: Mapping a region

When no region has room and the heap is under its budget, a region
is mapped (a span of several chunks for an object bigger than one)
and bumped from.

### Tier 5: Collection

If all of the above fail, `ms_collect()` runs a mark-sweep cycle,
which sets the next budget, and the tiers are tried again.  If the
thread's own cycle has still left nothing big enough, a region is
mapped past the budget, which then follows (`ms_alloc_beyond_budget`).
There is no copying fallback.


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
   entry of `TLAB_MIN_FREE` words or more, sweeping lazily for one
   (not during a concurrent mark, when what is allocated below
   `sweep_top` must be born black, which the fast path does not
   do), else out of a virgin tail or a newly mapped region
2. Otherwise, or with nothing but scraps free: the free list
   (`ms_free_list_alloc`; an empty size class splits an entry of a
   larger one), then lazy sweep + free list
3. Then a virgin tail, or a new region within the budget
4. If all fail: `ms_collect()`, and back to 1 until the request is
   met.  `ms_collect` either runs the cycle as the coordinator (and
   when that leaves nothing big enough for the request, a region is
   mapped past the budget) or, when another thread's cycle is under
   way, waits for it.  The slow path never calls `gc()`.  The GC and
   FULL-GC instructions likewise go through `ms_collect_explicit` (in
   both mark-sweep builds: a full collection is an ordinary cycle;
   the heap is never merged into spatic).

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
collection with two brief STW pauses:

### Phase 1: Initial Mark (STW)

The GC coordinator thread (whichever thread triggered collection),
having finished the previous cycle's sweep and cleared the mark
bitmaps under `alloc_lock`:
1. Sets `gc_pending` — all other threads park via `wait_for_gc()`
2. Retires every TLAB and snapshots every region's `sweep_top`
3. Scans roots for all threads (`ms_mark_roots()`)
4. Activates SATB write barriers (`gc_satb_active = 1`)
5. Releases all threads — mutators resume

### Phase 2: Concurrent Mark

The GC coordinator traces the heap while mutators run:
- Uses `ms_trace_concurrent()` with atomic mark-bitmap operations
  (`__atomic_fetch_or`)
- Mutators' SATB barriers log old pointer values before any heap write
- Objects allocated from free lists during this phase are marked
  atomically ("born-black") via `ms_mark_alloc()` and put on the
  cycle's *born list*
- Objects allocated from virgin tails (TLABs included) are above
  their region's `sweep_top` and are implicitly live (not swept); a
  region mapped during the mark has `sweep_top` at its start
- The region table may grow meanwhile (a mutator mapping a region
  under `alloc_lock` publishes it with release stores; the marker's
  lookups are acquire loads); regions are only unmapped between
  cycles

### Phase 3: Remark (STW)

The GC coordinator parks all threads again, then:
1. Retires TLABs allocated during concurrent mark
2. Drains all SATB buffers into the mark stack
   (`satb_drain_to_mark_stack()`)
3. Re-scans roots (stacks are C arrays, not covered by SATB)
4. Scans what was allocated during the mark (`ms_scan_born`: every
   region's words above `sweep_top`, and the born list): black
   without a trace, it may hold the last reference to an object the
   snapshot held only weakly (`wp_to_ref` during the mark)
5. Traces all newly discovered references
6. Deactivates SATB barriers
7. Processes weak pointers, sets the budget, prepares the lazy sweep
   (clears free lists, sets the sweep to the first region)
8. Eagerly sweeps enough blocks for the immediate allocation to be
   met (or for the budget to allow mapping a region)
9. Releases all threads

### Post-Remark: Lazy Sweep

After threads are released, sweep runs on demand, region by region:
- Each call to `ms_lazy_sweep_one()` sweeps one 4096-word block of
  the current region, between its start and its `sweep_top`; a run of
  dead objects open at the block's end carries over to the next
- A region found with nothing marked and nothing allocated above its
  `sweep_top` is reset to virgin in one step, without entries; a
  region empty for two sweeps running is unmapped when the sweep
  completes, if the heap is over its budget
- Called from `ms_alloc_slow_locked()` when free lists are empty
- Naturally adapts sweep rate to allocation pressure
- `ms_lazy_sweep_finish()` flushes remaining blocks at the start of
  the next GC cycle

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

When `gc_satb_active == 0` (the normal state), the barrier is a single
well-predicted branch with negligible overhead.  When active, the old
pointer value is logged to a per-thread buffer (1024 entries).

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
- **Stalls of one thread**: the *concurrent cycle* as seen by the
  thread that ran it (it was out of action for the whole cycle,
  concurrent mark included: the collector has no thread of its own),
  a *safepoint wait* (a thread parked while another held a pause),
  and an *allocation wait* (a thread out of memory during a cycle,
  waiting for its sweep; its safepoint waits meanwhile count under
  both).

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
is clean on it in every mode.

Measured 2026-10-29, with the region heap (i9-14900KF, `make bench`
pinned to one P core, FAST builds of the same sources): the longest
pause and the p99 pause in ms of each benchmark's run (compile plus
five repetitions), in the four configurations.  Few collections
happen per run, so the two are mostly equal; dispatch, fib and tak
collect nothing.

| Benchmark | 1 copying | 2 mark-sweep | 3 threads, copying | 4 threads, mark-sweep |
|-----------|-----------|--------------|--------------------|-----------------------|
| bignum | 1.86 / 0.44 | 0.25 / 0.25 | 0.30 / 0.29 | 0.22 / 0.22 |
| compile | 0.39 / 0.39 | 0.38 / 0.38 | 0.35 / 0.35 | 0.31 / 0.31 |
| cons | 0.33 / 0.30 | 0.27 / 0.26 | 0.31 / 0.29 | 0.21 / 0.20 |
| eval | 0.98 / 0.95 | 1.06 / 1.06 | 0.86 / 0.81 | 0.70 / 0.70 |
| format | 0.35 / 0.35 | 0.23 / 0.23 | 0.33 / 0.33 | 0.26 / 0.26 |
| hash | 7.19 / 7.19 | 5.34 / 5.34 | 6.19 / 6.19 | 1.90 / 1.90 |
| sort | 4.91 / 4.91 | 8.81 / 8.81 | 7.17 / 7.17 | 0.23 / 0.23 |
| strings | 0.46 / 0.36 | 0.28 / 0.28 | 0.31 / 0.29 | 0.25 / 0.21 |

Before the region heap (the same day, the commit before), the long
pauses in the mark-sweep configurations were the copying collector
growing the heap, a whole-heap copy each time: 28.3 ms on hash and
8.4 on sort in config 2, 20.6 and 6.5 in config 4, with eval and
compile at 1.5 to 2.3 ms.  Now a mark-sweep build never copies
except for a dump.  In config 4 every pause is an initial mark or a
remark, the longest 1.9 ms (hash's remark); in config 2 the pause is
the stop-the-world mark of the live data, which is why sort, with a
big live list, pauses 8.8 ms there and 0.23 ms concurrently (Phase 7
of GCEnhancements.md is about that build).  In `gc-stress.oak` on
four threads (config 4, `--size-heap 256`, 27 cycles in 1.03 s)
initial mark took at most 0.28 ms, remark 0.31 ms (p50 0.19), the
threads stopped within 2 us -- 54 global pauses, 1.0% of the run
(before: 71 pauses, 5.1%, the seven copies up to 6.2 ms).  The stalls
remain the larger cost, shorter now that no cycle ends in a copy: the
thread that ran a cycle lost up to 3.1 ms to it (p50 0.7), a thread
out of memory during a cycle waited up to 2.9 ms (46 times; it maps a
region within the budget and parks only beyond it), and safepoint
waits reached 0.56 ms (84 times, p50 0.28) -- Phases 3, 5 and 6.  The
same program in the default build (90 copying collections) paused at
most 1.4 ms, p50 0.48, 2.8% of the run, and in config 2 (141 cycles)
at most 1.0 ms, p50 0.35, 2.7%.  Four copies of tak on four OS
threads (`tests/bench.sh -p 4`, config 4) ran 3.88 times faster than
on one, with no collection.


Key Design Decisions
====================

**Non-moving mark-sweep as foundation for concurrency.**  The copying
GC moves every object, requiring all pointers (including locatives and
the raw instruction pointer `e_pc`) to be updated.  This is
fundamentally incompatible with concurrent mutation.  The mark-sweep
GC never moves objects, so no pointer fixup is needed, and mutator
threads can safely dereference pointers during collection.

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
are never swept (implicitly live).  Both kinds are scanned at the
remark.

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
| `NUM_SIZE_CLASSES` | 19 | `gc-ms.h` | Number of segregated free list size classes |
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
