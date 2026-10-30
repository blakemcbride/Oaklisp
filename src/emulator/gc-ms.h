// This file is part of Oaklisp.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation; either version 2 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// The GNU GPL is available at http://www.gnu.org/licenses/gpl.html
// or from the Free Software Foundation, 59 Temple Place - Suite 330,
// Boston, MA 02111-1307, USA


/*
 * gc-ms.h - Non-moving mark-sweep garbage collector
 *
 * This is a non-moving GC designed to support concurrent operation.
 * Objects are never relocated, which means locatives and raw code
 * pointers (e_pc) require no special handling.
 *
 * The heap is a set of regions (GC.md, "Heap Layout"): spans of one
 * or more 1 MB chunks, aligned to the chunk size, each with its own
 * object-start and mark bitmaps, found from any address inside them
 * through a two-level table indexed by the address's chunk number.
 * The heap grows by mapping a region and shrinks by unmapping one
 * that has swept empty; nothing is ever copied.  Allocation uses
 * size-class segregated free lists over all regions, and the virgin
 * tail of each region as a bump area.
 *
 * The copying GC (gc.c) is retained for world-dump compaction only.
 */

#ifndef GC_MS_H_INCLUDED
#define GC_MS_H_INCLUDED

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "oak-atomic.h"

/* Included from data.h, after ref_t and the word size are defined. */
#ifndef OAK_WORD_SIZE
#error "gc-ms.h is included through data.h"
#endif

/* Number of size classes.  Classes 0..NUM_SIZE_CLASSES-2 have bounded
   sizes; the last class is for objects larger than the second-to-last
   class maximum. */
#define NUM_SIZE_CLASSES 19

/* Default TLAB size in ref_t words.  Each thread carves chunks of
   this size from a region's bump area or a big free run for lock-free
   allocation. */
#define TLAB_SIZE 4096


/* ------------------------------------------------------------------ */
/*  Regions                                                            */
/* ------------------------------------------------------------------ */

/* A chunk is 1 MB, aligned to 1 MB; a region is a span of CHUNKS
   consecutive chunks mapped together, which is one chunk for an
   ordinary region, more for an object bigger than a chunk and for
   the span a dump compaction copies the heap into.  The bitmaps have
   one bit per word of the whole span, indexed from its start. */
#define MS_REGION_LOG2_BYTES 20
#define MS_REGION_BYTES ((size_t)1 << MS_REGION_LOG2_BYTES)
#define MS_REGION_WORDS (MS_REGION_BYTES / sizeof(ref_t))

typedef struct ms_region {
    ref_t *start, *end;		/* chunks * MS_REGION_WORDS words */
    ref_t *top;			/* [start, top) allocated, [top, end) virgin */
    ref_t *sweep_top;		/* top at the cycle's start: the sweep's limit */
    uint64_t *objstart;		/* 1 bit per word: an object starts here */
    uint64_t *mark;		/* 1 bit per word: the object here is marked */
    size_t chunks;
    size_t slot;		/* the first of its chunks' slots (GET-DATA) */
    size_t live_words;		/* what the last sweep found live */
    unsigned empty_cycles;	/* sweeps in a row that found nothing live */
    struct ms_region *next;	/* the heap's list, in mapping order */
#ifdef OAK_GC_VERIFY
    uint64_t *verify_snap;	/* reachable at the snapshot */
    uint64_t *verify_born;	/* born black this cycle */
#endif
} ms_region_t;

/* The lookup table: an address's chunk number (its high bits) indexes
   a first-level table of second-level tables, each entry the region
   the chunk belongs to, or NULL.  A 64-bit address is taken to have
   48 significant bits, which is what the OS hands out; mapping a
   region checks it. */
#if OAK_WORD_SIZE == 64
#define MS_ADDR_BITS 48
#else
#define MS_ADDR_BITS 32
#endif
#define MS_CHUNK_BITS (MS_ADDR_BITS - MS_REGION_LOG2_BYTES)
#define MS_L2_BITS (MS_CHUNK_BITS / 2)
#define MS_L1_BITS (MS_CHUNK_BITS - MS_L2_BITS)
#define MS_L2_SIZE ((size_t)1 << MS_L2_BITS)

/* The entries hold pointers as size_t so that the readers' atomic
   loads (the marker runs while mutators map regions) have one type. */
extern size_t ms_region_l1[(size_t)1 << MS_L1_BITS];

/* The region an address lies in, or NULL: spatic, the stacks, C data
   and garbage all give NULL.  Three loads. */
static inline ms_region_t *
ms_region_of(const void *p)
{
    uintptr_t a = (uintptr_t)p;
    size_t *l2;

#if MS_ADDR_BITS < OAK_WORD_SIZE
    if (a >> MS_ADDR_BITS)
	return NULL;
#endif
    a >>= MS_REGION_LOG2_BYTES;
    l2 = (size_t *)OAK_ATOMIC_LOAD_REF(&ms_region_l1[a >> MS_L2_BITS]);
    if (l2 == NULL)
	return NULL;
    return (ms_region_t *)OAK_ATOMIC_LOAD_REF(&l2[a & (MS_L2_SIZE - 1)]);
}

/* Is an address inside a region?  The copying collector's OLD_PTR
   during a dump compaction: every region is then old. */
#define ms_old_ptr(p) (ms_region_of(p) != NULL)
#define ms_is_heap_ptr(p) (ms_region_of(p) != NULL)

/* Words mapped (the sum of the regions' sizes) and allocated (the
   sum of their tops), and the number of regions. */
size_t ms_mapped_words(void);
size_t ms_allocated_words(void);
size_t ms_region_count(void);

/* GET-DATA and CRUNCH (loop.c, native-ops.c): the integer standing for
   a heap address -- its region's first slot times the chunk size plus
   its offset, counted from spatic.size by the callers -- and back.
   ms_get_data gives -1 outside the heap; ms_crunch gives NULL for an
   integer naming no mapped word. */
ssize_t ms_get_data(const ref_t *p);
ref_t *ms_crunch(size_t i);


/* ------------------------------------------------------------------ */
/*  Initialization and the copying collector's hooks                   */
/* ------------------------------------------------------------------ */

/* A fresh, empty heap: regions are mapped as allocation needs them.
   Also after LOAD-WORLD, when it unmaps the old heap first. */
void ms_init(void);

/* Unmap every region. */
void ms_destroy(void);

/* The copying collector (gc.c) runs in these builds only to compact
   the heap into one contiguous span for a world dump.  Begin gives
   it its destination -- new_space and free_point set to a fresh span,
   unregistered, big enough for everything mapped (and spatic too for
   a full collection), so that OLD_PTR is "in a region" -- and end,
   once everything live has been transported, unmaps the old regions
   and makes the span the heap. */
void ms_evacuate_begin(bool full);
void ms_evacuate_end(void);

/* Record a transport destination during copying GC so that
   ms_evacuate_end can set objstart bits for the compacted heap. */
void ms_record_transport(ref_t *new_place);

/* The GC and FULL-GC instructions: a collection asked for by the
   program. */
void ms_collect_explicit(const char *reason);
#ifdef THREADS
bool ms_collect_was_coordinator(void);
#endif

/* Try to allocate n_words from free lists.  Returns pointer to the
   first word of a region of at least n_words, or NULL if no free list
   entry is available.  Does NOT fall back to the bump areas. */
ref_t *ms_free_list_alloc(size_t n_words);

/* Notify the allocator that bump-pointer allocation just occurred
   at address p for n_words.  Sets the object-start bit. */
void ms_bump_alloc_notify(ref_t *p, size_t n_words);

/* Perform a mark-sweep collection.
   pre_dump: if true, delegate to copying GC for compaction.
   reason: human-readable string for trace output.
   amount: minimum free words needed after collection. */
void ms_collect(bool pre_dump, char *reason, size_t amount);

/* Return the size class index for an allocation of n_words. */
int ms_size_class(size_t n_words);

/* Return the maximum object size (words) for a given size class. */
size_t ms_class_max_words(int sc);

/* Per-thread TLAB (thread-local allocation buffer) state arrays.
   tlab_cursor_array[i] points to the next free word in thread i's TLAB.
   tlab_end_array[i] points one past the last word.  Both are NULL when
   the TLAB is exhausted or not yet allocated.  tlab_base_array[i] is
   the TLAB's first word and tlab_bm_array[i] the objstart bitmap word
   for it (the fast path's bit setting, below).  Without threads there
   is one, at index 0: the allocator's bump cursor. */
#ifdef THREADS
#define MS_TLAB_COUNT MAX_THREAD_COUNT
#define MS_TLAB_INDEX oak_thread_index_var
#else
#define MS_TLAB_COUNT 1
#define MS_TLAB_INDEX 0
#endif
extern ref_t *tlab_cursor_array[];
extern ref_t *tlab_end_array[];
extern ref_t *tlab_base_array[];
extern uint64_t *tlab_bm_array[];

/* Slow-path allocator, called with alloc_lock held where there is
   one: a TLAB out of a free run or a region's bump area for a small
   request, the free lists (sweeping lazily), a bump area, or a newly
   mapped region within the heap's budget otherwise.  Returns NULL if
   GC is needed.  Installs the TLAB for thread IDX on a refill. */
ref_t *ms_alloc_slow_locked(size_t n_words, int idx);

/* ALLOCATE's slow path (data.h): the lock where there is one, the
   slow allocator, collections until the request is met.  The caller
   has synchronized its state for a collection. */
ref_t *ms_allocate_slow(size_t words, char *reason);

/* Retire a single TLAB's unused portion so it can be swept. */
void ms_tlab_retire(ref_t *cursor, ref_t *end);

/* Retire all threads' TLABs.  Must be called with the world stopped:
   the TLAB fast path in ALLOCATE_PROT takes no lock. */
void ms_tlab_retire_all(void);

/* The objstart bit of the TLAB fast path (ALLOCATE_PROT, data.h): a
   plain store into the TLAB's own bitmap words (gc-ms.c, TLAB_ALIGN),
   found from the thread's TLAB base and bitmap pointer. */
#define MS_TLAB_OBJSTART(idx, p)					\
  do { size_t _ms_o = (size_t)((p) - tlab_base_array[idx]);		\
       tlab_bm_array[idx][_ms_o / 64] |= 1ULL << (_ms_o % 64); } while (0)


/* ------------------------------------------------------------------ */
/*  SATB write barrier                                                 */
/* ------------------------------------------------------------------ */

/* When gc_satb_active is nonzero, the SATB_BARRIER macro logs the old
   value of a heap slot before it is overwritten.  This is the
   snapshot-at-the-beginning invariant needed for concurrent marking.

   Under THREADS + USE_MARK_SWEEP, gc_satb_active is set to 1 during
   the concurrent mark phase and cleared during the remark STW pause.
   In single-threaded mode it is always 0.  The compiler should predict
   the branch as not-taken (negligible overhead). */

extern int gc_satb_active;

#define SATB_BUFFER_SIZE 1024

/* SATB_BUFFER_SIZE is the initial capacity; the buffers grow rather
   than dropping entries (dropping one would let a live object be
   swept). */
#ifdef THREADS
extern ref_t *satb_buffer_array[];
extern size_t satb_count_array[];
extern size_t satb_size_array[];
#else
extern ref_t *satb_buffer;
extern size_t satb_count;
extern size_t satb_size;
#endif

/* Log the old value at *addr to the per-thread SATB buffer.
   Called from the SATB_BARRIER macro; do not call directly. */
void satb_log_old(ref_t *addr);

/* Drain all SATB buffers.  In Phase 4 this will feed entries to the
   concurrent marker; for now it just clears the buffers. */
void satb_flush_all(void);

/* Barrier macro: insert before any heap write that overwrites an
   existing pointer field.  No-op when gc_satb_active == 0.
   Overrides the no-op fallback defined in data.h. */
#undef SATB_BARRIER
#define SATB_BARRIER(addr) \
    do { if (OAK_UNLIKELY(gc_satb_active)) \
	     satb_log_old(addr); } while (0)


/* ------------------------------------------------------------------ */
/*  Concurrent marking                                                 */
/* ------------------------------------------------------------------ */

/* GC phase tracking.  Controls born-black allocation during
   concurrent mark and determines whether atomic bitmap operations
   are needed. */
enum gc_phase_t {
    GC_PHASE_IDLE = 0,
    GC_PHASE_INITIAL_MARK,
    GC_PHASE_CONCURRENT_MARK,
    GC_PHASE_REMARK,
    GC_PHASE_SWEEP
};
extern volatile int gc_phase;

#ifdef THREADS
/* True while a concurrent mark-sweep cycle is in progress.
   Threads that exhaust the heap wait instead of starting a second
   collection. */
extern volatile int gc_concurrent_in_progress;

/* Initialize concurrent GC synchronization (called once at startup). */
void gc_safepoint_init(void);

/* Wait for an in-progress concurrent GC to complete.
   Called from ms_collect when a second thread needs collection. */
void ms_wait_concurrent_gc(void);
#endif

/* Drain all SATB buffers into the GC mark stack so that the
   concurrent marker can trace them.  Called during remark. */
void satb_drain_to_mark_stack(void);

/* Set mark bit for a newly allocated object during concurrent mark
   (born-black).  Safe to call at any time; no-op if address is
   outside the heap or if no collection is in progress. */
void ms_mark_alloc(ref_t *p);


/* ------------------------------------------------------------------ */
/*  Lazy sweep                                                         */
/* ------------------------------------------------------------------ */

/* Block size for lazy sweep: the sweep cursor advances by this many
   words per ms_lazy_sweep_one() call. */
#define SWEEP_BLOCK_SIZE 4096

/* Prepare for lazy sweep after the mark phase completes.
   Clears free lists and resets the sweep cursor. */
void ms_prepare_lazy_sweep(void);

/* Sweep one block (up to SWEEP_BLOCK_SIZE words) from the sweep cursor.
   Returns true if a block was swept (free lists may now have entries),
   false if the entire heap has been swept.  Thread-safe when called
   under alloc_lock. */
bool ms_lazy_sweep_one(void);

/* Sweep all remaining unsswept blocks.  Called at the start of a new
   GC cycle to flush any residual from the previous cycle. */
void ms_lazy_sweep_finish(void);

#endif
