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
 * gc-ms.c - Non-moving mark-sweep garbage collector
 *
 * Design (GC.md):
 *   - The heap is a set of regions: spans of 1 MB chunks, mapped on
 *     demand within a budget and unmapped when they sweep empty, each
 *     with its own object-start and mark bitmaps (1 bit per word).
 *   - Allocation: segregated free lists by size class over all
 *     regions, TLABs out of big free runs or a region's virgin tail.
 *   - Collection: mark from the roots (concurrently with threads),
 *     sweep lazily, region by region, during allocation.
 *   - The copying GC (gc.c) is retained for pre-dump compaction
 *     only; the heap grows by mapping a region, never by copying.
 */

#define _REENTRANT

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "data.h"
#include "gc-ms.h"
#include "gc.h"
#include "gc-stats.h"
#include "weak.h"
#include "xmalloc.h"
#include "stacks.h"
#include "signals.h"
#include "oak-atomic.h"
#include "native.h"
#include "threads.h"

#ifdef _WIN32
#include <malloc.h>
#else
#include <sys/mman.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif


/* ------------------------------------------------------------------ */
/*  Regions                                                            */
/* ------------------------------------------------------------------ */

size_t ms_region_l1[(size_t)1 << MS_L1_BITS];

/* The heap's regions, in mapping order, and what they add up to. */
static ms_region_t *ms_regions, *ms_regions_tail;
static size_t ms_mapped;		/* words in all regions */
static size_t ms_nregions;

/* The region TLABs and large requests are bumped out of first. */
static ms_region_t *alloc_region;

/* GET-DATA's slots (gc-ms.h): a table of the regions by slot, each
   region holding as many consecutive slots as it has chunks.  Grown
   by replacing the table; a reader that still holds the old one
   finds its regions there, so the old tables are kept. */
typedef struct { size_t n; ms_region_t *r[]; } ms_slot_table_t;
static size_t ms_slots_ptr;		/* ms_slot_table_t *, published */

/* The copying collector's destination span (ms_evacuate_begin). */
static size_t evac_chunks;

/* Pacing (with threads; 0 without): the words the mutators may map
   past the budget while a cycle runs, and before it that a cycle is
   asked for ahead of the budget (ms_set_pacing). */
static size_t ms_headroom;
/* Words handed out since the last cycle's snapshot (its initial
   mark): with what that mark found live, the heap's occupancy, which
   the trigger compares with the budget. */
static size_t ms_since_snapshot;
#ifdef THREADS
static void ms_request_cycle(const char *reason);
static int gc_requested;
#endif

/* The lazy sweep's state (defined with it, below). */
static ms_region_t *sweep_region;
static bool lazy_sweep_active;

#define BM_WORDS(r)	(((r)->chunks * MS_REGION_WORDS + 63) / 64)
#define BM_IDX(r, p)	((size_t)((p) - (r)->start))
#define BM_SET(bm, i)	((bm)[(i)/64] |=  (1ULL << ((i) % 64)))
#define BM_CLR(bm, i)	((bm)[(i)/64] &= ~(1ULL << ((i) % 64)))
#define BM_TEST(bm, i)	((bm)[(i)/64] &   (1ULL << ((i) % 64)))

/* Objstart bits are set by the locked slow paths and the collector in
   words they may share, so those updates are atomic with threads; the
   TLAB fast path on every thread sets them with no lock, in words
   that are the TLAB's alone (TLAB_ALIGN), with a plain store. */
static inline void
ms_objstart_set(ms_region_t *r, ref_t *p)
{
    size_t i = BM_IDX(r, p);

#ifdef THREADS
    (void)OAK_ATOMIC_FETCH_OR_U64(&r->objstart[i / 64], 1ULL << (i % 64));
#else
    BM_SET(r->objstart, i);
#endif
}

/* The free bit of a free-list entry's first word: set as the entry is
   made, cleared as it is taken (under alloc_lock), and tested by the
   marker, which may run meanwhile: a reference landing in a free
   entry is garbage, and marking the entry would trace the dead
   objects in it and whatever they point to, as it did before -- harmless,
   but it kept garbage alive and made verify mode follow it.  A stale
   reading costs nothing: an entry taken during a mark is born black
   by its taker, and one not yet seen as free is merely marked as
   before. */
static inline void
ms_free_set(ms_region_t *r, ref_t *p)
{
    size_t i = BM_IDX(r, p);

#ifdef THREADS
    (void)OAK_ATOMIC_FETCH_OR_U64(&r->freebm[i / 64], 1ULL << (i % 64));
#else
    BM_SET(r->freebm, i);
#endif
}

static inline void
ms_free_clear(ms_region_t *r, ref_t *p)
{
    size_t i = BM_IDX(r, p);

#ifdef THREADS
    (void)OAK_ATOMIC_FETCH_AND_U64(&r->freebm[i / 64], ~(1ULL << (i % 64)));
#else
    BM_CLR(r->freebm, i);
#endif
}

static void
ms_fatal(const char *what, size_t words)
{
    fprintf(stderr, "\nFatal error: %s (%zu words; %zu words mapped in %zu"
	    " regions).\n", what, words, ms_mapped, ms_nregions);
    exit(EXIT_FAILURE);
}

/* BYTES of memory aligned to the chunk size, zeroed, or NULL. */
static void *
ms_map_aligned(size_t bytes)
{
#ifdef _WIN32
    void *p = _aligned_malloc(bytes, MS_REGION_BYTES);

    if (p)
	memset(p, 0, bytes);
    return p;
#else
    size_t len = bytes + MS_REGION_BYTES;
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uintptr_t a;
    size_t head, tail;

    if (p == MAP_FAILED)
	return NULL;
    a = ((uintptr_t)p + MS_REGION_BYTES - 1) & ~(uintptr_t)(MS_REGION_BYTES - 1);
    head = (size_t)(a - (uintptr_t)p);
    tail = len - head - bytes;
    if (head)
	munmap(p, head);
    if (tail)
	munmap((char *)a + bytes, tail);
    return (void *)a;
#endif
}

static void
ms_unmap_aligned(void *p, size_t bytes)
{
#ifdef _WIN32
    (void)bytes;
    _aligned_free(p);
#else
    munmap(p, bytes);
#endif
}

/* Every chunk of R in the lookup table maps to VALUE (NULL to clear).
   A reader (the marker, running concurrently) sees either the old
   entry or the new one. */
static void
ms_table_set(ms_region_t *r, ms_region_t *value)
{
    uintptr_t c = (uintptr_t)r->start >> MS_REGION_LOG2_BYTES;
    size_t k;

    for (k = 0; k < r->chunks; k++, c++) {
	size_t *l2 = (size_t *)ms_region_l1[c >> MS_L2_BITS];

	if (l2 == NULL) {
	    l2 = (size_t *)xmalloc(MS_L2_SIZE * sizeof(size_t));
	    memset(l2, 0, MS_L2_SIZE * sizeof(size_t));
	    OAK_ATOMIC_STORE_REF(&ms_region_l1[c >> MS_L2_BITS], (size_t)l2);
	}
	OAK_ATOMIC_STORE_REF(&l2[c & (MS_L2_SIZE - 1)], (size_t)value);
    }
}

static size_t
ms_slot_alloc(ms_region_t *r)
{
    ms_slot_table_t *t = (ms_slot_table_t *)ms_slots_ptr;
    size_t n = t ? t->n : 0, i, k, run = 0;

    for (i = 0; i < n; i++) {
	run = t->r[i] ? 0 : run + 1;
	if (run == r->chunks) {
	    i = i + 1 - run;
	    goto found;
	}
    }
    {
	size_t want = n * 2;
	ms_slot_table_t *bigger;

	if (want < n + r->chunks)
	    want = n + r->chunks;
	if (want < 64)
	    want = 64;
	bigger = (ms_slot_table_t *)xmalloc(sizeof *bigger
					    + want * sizeof(ms_region_t *));
	bigger->n = want;
	memset(bigger->r, 0, want * sizeof(ms_region_t *));
	if (t)
	    memcpy(bigger->r, t->r, n * sizeof(ms_region_t *));
	OAK_ATOMIC_STORE_REF(&ms_slots_ptr, (size_t)bigger);
	t = bigger;
	i = n;
    }
found:
    for (k = 0; k < r->chunks; k++)
	t->r[i + k] = r;
    return i;
}

static void
ms_slot_free(ms_region_t *r)
{
    ms_slot_table_t *t = (ms_slot_table_t *)ms_slots_ptr;
    size_t k;

    for (k = 0; k < r->chunks; k++)
	t->r[r->slot + k] = NULL;
}

/* The least request, unaligned and aligned, that a look through every
   region found no virgin room for.  A virgin tail only shrinks, except
   when a region is made (ms_region_new) or found all dead
   (ms_region_reset), which forget these; until then any request at
   least as big fails at once.  Without them every allocation that
   missed the cursor region walked the whole list, and the regions grow
   with the live data.  Under alloc_lock, as the tails are. */
static size_t virgin_fail[2] = { SIZE_MAX, SIZE_MAX };

static inline void
ms_virgin_grew(void)
{
    virgin_fail[0] = virgin_fail[1] = SIZE_MAX;
}

/* A region over the CHUNKS chunks at MEM (aligned, zeroed): its
   struct and bitmaps, its slots, its table entries, on the list. */
static ms_region_t *
ms_region_new(void *mem, size_t chunks)
{
    size_t words = chunks * MS_REGION_WORDS;
    ms_region_t *r = (ms_region_t *)xmalloc(sizeof *r);
    size_t bm;

    memset(r, 0, sizeof *r);
    r->start = r->top = r->sweep_top = (ref_t *)mem;
    ms_virgin_grew();
    r->end = r->start + words;
    r->chunks = chunks;
    bm = BM_WORDS(r) * sizeof(uint64_t);
    r->objstart = (uint64_t *)xmalloc(bm);
    r->mark = (uint64_t *)xmalloc(bm);
    r->freebm = (uint64_t *)xmalloc(bm);
    memset(r->objstart, 0, bm);
    memset(r->mark, 0, bm);
    memset(r->freebm, 0, bm);
    r->slot = ms_slot_alloc(r);
    ms_table_set(r, r);
    if (ms_regions_tail)
	ms_regions_tail->next = r;
    else
	ms_regions = r;
    ms_regions_tail = r;
    ms_mapped += words;
    ms_nregions++;
    return r;
}

/* Map a region of CHUNKS chunks.  Called under alloc_lock, or with
   the world stopped. */
static ms_region_t *
ms_region_map(size_t chunks)
{
    size_t words = chunks * MS_REGION_WORDS;
    void *mem;

#ifdef MAX_NEW_SPACE_SIZE
    if (ms_mapped + words > MAX_NEW_SPACE_SIZE)
	ms_fatal("the heap would exceed MAX_NEW_SPACE_SIZE", words);
#endif
    mem = ms_map_aligned(chunks * MS_REGION_BYTES);
    if (mem == NULL)
	ms_fatal("cannot map a region", words);
#if MS_ADDR_BITS < OAK_WORD_SIZE
    if ((uintptr_t)mem >> MS_ADDR_BITS)
	ms_fatal("a region was mapped beyond the address range the"
		 " lookup table covers", words);
#endif
    if (trace_gc > 1)
	fprintf(stderr, "; MS-GC: mapped a region of %zu chunk%s"
		" (%zu words mapped in %zu regions, budget %zu).\n",
		chunks, chunks == 1 ? "" : "s", ms_mapped + words,
		ms_nregions + 1, (size_t)e_next_newspace_size);
    return ms_region_new(mem, chunks);
}

static void
ms_region_free(ms_region_t *r)
{
    free(r->objstart);
    free(r->mark);
    free(r->freebm);
#ifdef OAK_GC_VERIFY
    free(r->verify_snap);
    free(r->verify_born);
#endif
    free(r);
}

/* Unmap R, which nothing refers into.  Under alloc_lock, no sweep in
   progress and no marker running. */
static void
ms_region_unmap(ms_region_t *r)
{
    ms_region_t **pp = &ms_regions, *prev = NULL;

    while (*pp != r) {
	prev = *pp;
	pp = &(*pp)->next;
    }
    *pp = r->next;
    if (ms_regions_tail == r)
	ms_regions_tail = prev;
    if (alloc_region == r)
	alloc_region = NULL;
    ms_table_set(r, NULL);
    ms_slot_free(r);
    ms_mapped -= r->chunks * MS_REGION_WORDS;
    ms_nregions--;
    if (trace_gc > 1)
	fprintf(stderr, "; MS-GC: unmapped a region of %zu chunk%s"
		" (%zu words mapped in %zu regions, budget %zu).\n",
		r->chunks, r->chunks == 1 ? "" : "s", ms_mapped,
		ms_nregions, (size_t)e_next_newspace_size);
    ms_unmap_aligned(r->start, r->chunks * MS_REGION_BYTES);
    ms_region_free(r);
}

size_t
ms_mapped_words(void)
{
    return ms_mapped;
}

size_t
ms_allocated_words(void)
{
    size_t n = 0;
    ms_region_t *r;

    for (r = ms_regions; r; r = r->next)
	n += (size_t)(r->top - r->start);
    return n;
}

size_t
ms_region_count(void)
{
    return ms_nregions;
}

ssize_t
ms_get_data(const ref_t *p)
{
    ms_region_t *r = ms_region_of(p);

    if (r == NULL)
	return -1;
    return (ssize_t)(r->slot * MS_REGION_WORDS + (size_t)(p - r->start));
}

ref_t *
ms_crunch(size_t i)
{
    ms_slot_table_t *t = (ms_slot_table_t *)OAK_ATOMIC_LOAD_REF(&ms_slots_ptr);
    size_t j = i / MS_REGION_WORDS;
    ms_region_t *r;
    ref_t *p;

    if (t == NULL || j >= t->n || (r = t->r[j]) == NULL)
	return NULL;
    p = r->start + (i - r->slot * MS_REGION_WORDS);
    return p < r->end ? p : NULL;
}

/* Whether a region may be mapped: within the budget plus the headroom.
   The budget is e_next_newspace_size (LOAD-REG/STORE-REG
   next-newspace-size), set after every cycle to the copying
   collector's rule, never below the configured heap size (--size-heap,
   original_newspace_size).  With threads a cycle is asked for when the
   heap reaches the budget less the headroom (ms_region_map), and the
   mutators go on mapping up to the budget plus the headroom while it
   runs; a thread that needs more than that waits for the cycle.  The
   headroom is twice what was allocated during the last cycle's mark,
   between an eighth and a half of the budget. */
#define MS_RECLAIM_FACTOR 3

static bool
ms_may_map(void)
{
    size_t room = ms_headroom;
    size_t limit;

#ifdef THREADS
    /* While a cycle runs the runway is at least half the budget,
       whatever the headroom has learnt: the headroom sets when a
       cycle is asked for, this what the mutators may still map before
       the mark ends -- a wait costs them more than a region. */
    if (gc_concurrent_in_progress && room < (size_t)e_next_newspace_size / 2)
	room = (size_t)e_next_newspace_size / 2;
#endif
    limit = (size_t)e_next_newspace_size + room;
#ifdef MAX_NEW_SPACE_SIZE
    if (limit > MAX_NEW_SPACE_SIZE)
	limit = MAX_NEW_SPACE_SIZE;
#endif
    return ms_mapped < limit;
}

/* Words of the objects the mark traced this cycle: the live data, less
   what was born black meanwhile. */
static size_t marked_words;

/* Words allocated while the last cycle's mark ran, and whether a
   thread ran out of room and waited during the cycle.  What is
   allocated during a mark is born black and freed only by the next
   cycle, so the budget counts twice that on top of three times the
   live data (a program allocating faster than a mark runs would
   otherwise find each sweep freeing next to nothing, and wait at
   every cycle); it is also at least the live data plus
   MS_CYCLE_SPACING times that, so that a program allocating fast
   against little live data gets a cycle every so many cycle lengths
   rather than one after another (every cycle costs every thread two
   pauses), and at least MS_MIN_CYCLE_MS of allocation at the rate
   measured between the last two snapshots, for the same reason; and
   it grows by half after a cycle in which a thread waited.  The
   headroom is what was allocated during the mark, between an eighth
   and a half of the budget. */
static size_t ms_alloc_during_mark;	/* smoothed over the cycles */
#ifdef THREADS
static double ms_alloc_rate;		/* words per ms, smoothed */
static gc_ns_t ms_snapshot_ns;		/* when the mark began */
static bool ms_waited;
#endif
#define MS_CYCLE_SPACING 16
#define MS_MIN_CYCLE_MS 32

static void
ms_set_pacing(void)
{
#ifdef THREADS
    size_t budget = (size_t)e_next_newspace_size;
    size_t h = ms_alloc_during_mark;

    if (h < budget / 8)
	h = budget / 8;
    if (h > budget / 2)
	h = budget / 2;
    if (trace_gc > 1 && h != ms_headroom)
	fprintf(stderr, "; MS-GC: %zu words allocated during the mark%s:"
		" headroom %zu -> %zu.\n", ms_alloc_during_mark,
		ms_waited ? ", a thread waited" : "", ms_headroom, h);
    ms_headroom = h;
    ms_waited = false;
#else
    ms_headroom = 0;
#endif
}

static void
ms_set_budget(size_t amount)
{
    size_t want = MS_RECLAIM_FACTOR * marked_words + amount;

#ifdef THREADS
    {
	/* Between cycles: at least MS_CYCLE_SPACING marks' worth of
	   allocation, and at least MS_MIN_CYCLE_MS of it at the rate
	   measured between the last two snapshots. */
	size_t spacing = MS_CYCLE_SPACING * ms_alloc_during_mark;
	size_t period = (size_t)(ms_alloc_rate * MS_MIN_CYCLE_MS);
	size_t old = (size_t)e_next_newspace_size;

	want += 2 * ms_alloc_during_mark;
	if (period > spacing)
	    spacing = period;
	if (want < marked_words + 2 * ms_alloc_during_mark + spacing)
	    want = marked_words + 2 * ms_alloc_during_mark + spacing;
	if (ms_waited && want < old + old / 2)
	    want = old + old / 2;
	/* No more than doubled in one cycle: one long cycle, with a
	   lot allocated during it, is not a trend. */
	if (want > 2 * old)
	    want = 2 * old;
    }
#endif
    if (want < original_newspace_size)
	want = original_newspace_size;
#ifdef MAX_NEW_SPACE_SIZE
    if (want > MAX_NEW_SPACE_SIZE)
	want = MAX_NEW_SPACE_SIZE;
#endif
    if (trace_gc > 1 && want != (size_t)e_next_newspace_size)
	fprintf(stderr, "; MS-GC: %zu words live in %zu mapped: "
		"budget %zu -> %zu.\n", marked_words, ms_mapped,
		(size_t)e_next_newspace_size, want);
    e_next_newspace_size = want;
    ms_set_pacing();
}


/* ------------------------------------------------------------------ */
/*  Size classes                                                       */
/* ------------------------------------------------------------------ */

/*
 * Classes 0 to 31 each hold one size, 1 to 32 words; above that a
 * class holds a range, to its maximum:
 *
 * Class   Max words
 *  32      48
 *  33      64
 *  34      96
 *  35     128
 *  36     192
 *  37     256
 *  38     512
 *  39    1024
 *  40    inf   (large objects)
 *
 * The allocator takes the first entry of a request's class that is
 * big enough.  In a class holding a range, entries smaller than the
 * request are walked past, and a heap that keeps many small objects
 * alive between dead ones fills a class with such holes: every
 * allocation then walked them all, and the time to build a big live
 * structure grew with its square.  One size per class for the sizes
 * nearly every object has makes the first entry always fit.
 */

#define MS_EXACT_CLASSES 32

static const size_t class_max[NUM_SIZE_CLASSES] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
    48, 64, 96, 128, 192, 256, 512, 1024,
    (size_t)-1   /* large: no upper bound */
};

int
ms_size_class(size_t n)
{
    /* The smallest class whose max >= n: direct for the exact classes,
       a short scan above them. */
    if (n <= MS_EXACT_CLASSES)
	return n == 0 ? 0 : (int)n - 1;
    for (int i = MS_EXACT_CLASSES; i < NUM_SIZE_CLASSES; i++)
	if (n <= class_max[i])
	    return i;
    return NUM_SIZE_CLASSES - 1;
}

size_t
ms_class_max_words(int sc)
{
    return class_max[sc];
}


/* ------------------------------------------------------------------ */
/*  Free lists                                                         */
/* ------------------------------------------------------------------ */

/*
 * Each free-list entry occupies the dead object's memory:
 *   word[0] = next pointer (ref_t* cast to ref_t), 0 = end of list
 *   word[1] = the entry's size in words (class 0 entries have none)
 * An entry lies within one region, below its sweep_top; the sweep
 * that made it has passed it entirely (the sweeps, below), so the
 * allocator may use any entry on the lists.
 *
 * free_list_heads[sc] points to the first entry in class sc, or NULL.
 */

static ref_t *free_list_heads[NUM_SIZE_CLASSES];

/* Total words on free lists (for statistics / GC triggering). */
static size_t free_list_total_words;

/* A run of dead words onto its free list (the sweeps, below). */
static void ms_sweep_run_end(ref_t **run_start, size_t *run_len);

#define TLAB_MIN_FREE 257	/* a free run this big can be a TLAB */
static bool tlab_free_none;

/* Whether an object allocated below its region's sweep_top must be
   born black: during a concurrent mark (the sweep would free it
   otherwise), and while the weak pointer table is being cleared
   after one (the clearing, and a weak pointer's reader, test the
   mark bits: a new object's weak pointer would be cleared). */
#ifdef THREADS
#define MS_BORN_BLACK() \
    OAK_UNLIKELY(gc_phase >= GC_PHASE_CONCURRENT_MARK || gc_weak_clearing)
#else
#define MS_BORN_BLACK() OAK_UNLIKELY(gc_phase >= GC_PHASE_CONCURRENT_MARK)
#endif

/* An entry's size field, checked: a zero would loop the allocator,
   and in a debugging build an entry running past its region is a
   corrupt list. */
static inline bool
ms_entry_ok(ref_t *p, size_t size)
{
#ifdef FAST
    return size != 0;
#else
    ms_region_t *r;

    return size != 0 && (r = ms_region_of(p)) != NULL && p + size <= r->end;
#endif
}

ref_t *
ms_free_list_alloc(size_t n_words)
{
    int sc = ms_size_class(n_words);

    /* Only try the exact class — don't search larger classes.
       Within the class, only return entries with actual_size >= n_words. */
    {
	ref_t **prev = &free_list_heads[sc];
	ref_t *p = *prev;
	while (p) {
	    size_t actual_size = (sc == 0) ? 1 : (size_t)p[1];
	    if (!ms_entry_ok(p, actual_size)) {
		prev = (ref_t **)p;
		p = (ref_t *)(*p);
		continue;
	    }
	    if (actual_size >= n_words) {
		ms_region_t *r = ms_region_of(p);

		*prev = (ref_t *)(*p);
		ms_objstart_set(r, p);
		ms_free_clear(r, p);
		free_list_total_words -= actual_size;
		ms_since_snapshot += actual_size;

		/* Born-black: during concurrent mark, objects allocated
		   from the free list (below sweep_top) must have their
		   mark bit set so sweep doesn't reclaim them. */
		if (MS_BORN_BLACK())
		    ms_mark_alloc(p);
#if defined(THREADS) && defined(OAK_GC_VERIFY)
		ms_verify_log_alloc(p, 3, n_words);
#endif

		/* Zero gap words beyond what the caller will use.
		   This prevents the copying GC's linear scavenge
		   from seeing stale PTR_TAG values in the gap,
		   and ensures no stale data leaks into live objects
		   via the sweep's length computation. */
		if (actual_size > n_words)
		    memset(p + n_words, 0,
			   (actual_size - n_words) * sizeof(ref_t));
		return p;
	    }
	    prev = (ref_t **)p;
	    p = (ref_t *)(*p);
	}
    }

    /* The exact class is empty: split the first entry of a larger
       class, and put the rest back on the list for its size.  Without
       this, a request whose class has run dry starts a collection
       however much else is free.  Each cycle starts the lists afresh. */
    for (int c = sc + 1; c < NUM_SIZE_CLASSES; c++) {
	ref_t **prev = &free_list_heads[c];
	ref_t *p = *prev;

	while (p) {
	    size_t actual_size = (size_t)p[1];

	    if (!ms_entry_ok(p, actual_size)) {
		prev = (ref_t **)p;
		p = (ref_t *)(*p);
		continue;
	    }
	    if (actual_size > n_words) {
		ref_t *rest = p + n_words;
		size_t rest_size = actual_size - n_words;
		int rc = ms_size_class(rest_size);
		ms_region_t *r = ms_region_of(p);

		*prev = (ref_t *)(*p);
		ms_objstart_set(r, p);
		ms_free_clear(r, p);
		ms_objstart_set(r, rest);
		rest[0] = (ref_t)free_list_heads[rc];
		if (rest_size >= 2)
		    rest[1] = (ref_t)rest_size;
		ms_free_set(r, rest);
		free_list_heads[rc] = rest;
		free_list_total_words -= n_words;
		ms_since_snapshot += n_words;
		if (rest_size >= TLAB_MIN_FREE)
		    tlab_free_none = false;

		if (MS_BORN_BLACK())
		    ms_mark_alloc(p);
#if defined(THREADS) && defined(OAK_GC_VERIFY)
		ms_verify_log_alloc(p, 4, n_words);
#endif
		return p;
	    }
	    prev = (ref_t **)p;
	    p = (ref_t *)(*p);
	}
    }
    return NULL;
}


/* ------------------------------------------------------------------ */
/*  Bump areas: the regions' virgin tails                              */
/* ------------------------------------------------------------------ */

/* A TLAB begins and ends on a bitmap word's boundary (64 words), so
   its objstart bits are in words no other thread touches, and the
   fast path sets them with a plain store (MS_TLAB_OBJSTART) -- the
   atomic ms_objstart_set is for the shared words the slow paths and
   the collector write.  The words skipped to align a start are zeroed
   and given an objstart bit: a dead filler for the next sweep. */
#define TLAB_ALIGN 64

static ref_t *
ms_align_up(ms_region_t *r, ref_t *p)
{
    size_t off = BM_IDX(r, p) % TLAB_ALIGN;

    return off == 0 ? p : p + (TLAB_ALIGN - off);
}

static void
ms_filler(ms_region_t *r, ref_t *p, size_t words)
{
    if (words == 0)
	return;
    memset(p, 0, words * sizeof(ref_t));
    ms_objstart_set(r, p);
}

/* Words available in R's virgin tail, from an aligned start if asked. */
static size_t
ms_virgin_room(ms_region_t *r, bool aligned)
{
    ref_t *start = aligned ? ms_align_up(r, r->top) : r->top;

    return start < r->end ? (size_t)(r->end - start) : 0;
}

/* WORDS out of R's virgin tail. */
static ref_t *
ms_virgin_take(ms_region_t *r, size_t words, bool aligned)
{
    ref_t *start = aligned ? ms_align_up(r, r->top) : r->top;

    if (r->top == r->start)
	r->empty_cycles = 0;
    ms_filler(r, r->top, (size_t)(start - r->top));
    ms_since_snapshot += (size_t)(start + words - r->top);
    r->top = start + words;
    return start;
}

/* A region with WORDS of virgin room: the cursor region, else the
   first on the list, which becomes the cursor. */
static ms_region_t *
ms_region_with_room(size_t words, bool aligned)
{
    ms_region_t *r = alloc_region;

    if (r && ms_virgin_room(r, aligned) >= words)
	return r;
    if (words >= virgin_fail[aligned])
	return NULL;
    for (r = ms_regions; r; r = r->next)
	if (ms_virgin_room(r, aligned) >= words) {
	    alloc_region = r;
	    return r;
	}
    virgin_fail[aligned] = words;
    return NULL;
}

/* A new region with room for WORDS, the cursor from now on. */
static ms_region_t *
ms_map_for(size_t words)
{
    size_t chunks = (words + MS_REGION_WORDS - 1) / MS_REGION_WORDS;

    alloc_region = ms_region_map(chunks);
    return alloc_region;
}

/* WORDS from a virgin tail: a region with room, else a new region
   within the budget, or regardless of it when FORCE. */
static ref_t *
ms_virgin_alloc(size_t words, bool aligned, bool force)
{
    ms_region_t *r = ms_region_with_room(words, aligned);

    if (r == NULL) {
	if (!force && !ms_may_map())
	    return NULL;
	r = ms_map_for(words);
    }
    return ms_virgin_take(r, words, aligned);
}

void
ms_bump_alloc_notify(ref_t *p, size_t n_words)
{
    ms_objstart_set(ms_region_of(p), p);
    (void)n_words;
}


/* ------------------------------------------------------------------ */
/*  TLABs (thread-local allocation buffers)                            */
/* ------------------------------------------------------------------ */

/* With threads a TLAB per thread; without, the one at index 0 is the
   allocator's bump cursor, so that ALLOCATE's fast path is the same
   few instructions in both builds (data.h). */

ref_t *tlab_cursor_array[MS_TLAB_COUNT];
ref_t *tlab_end_array[MS_TLAB_COUNT];
ref_t *tlab_base_array[MS_TLAB_COUNT];
uint64_t *tlab_bm_array[MS_TLAB_COUNT];

static void
ms_tlab_install(int idx, ms_region_t *r, ref_t *start, ref_t *end)
{
    /* The unused tail of the TLAB being replaced is retired: zeroed,
       and given an objstart bit, so that it is neither absorbed into
       the extent of the object before it (a stale word there would
       be taken for a reference) nor left for a sweep to count as that
       object's. */
    ms_tlab_retire(tlab_cursor_array[idx], tlab_end_array[idx]);
    tlab_base_array[idx] = start;
    tlab_bm_array[idx] = &r->objstart[BM_IDX(r, start) / 64];
    tlab_cursor_array[idx] = start;
    tlab_end_array[idx] = end;
}

/* A TLAB out of a virgin tail: TLAB_SIZE words from a region with
   that much, else what a region has, down to TLAB_MIN_FREE or the
   request, else from a new region (within the budget, or regardless
   when FORCE). */
static ref_t *
ms_tlab_from_virgin(size_t min_words, bool force,
		    ms_region_t **out_r, ref_t **out_end)
{
    size_t least = min_words > TLAB_MIN_FREE ? min_words : TLAB_MIN_FREE;
    size_t words = TLAB_SIZE;
    ms_region_t *r = ms_region_with_room(TLAB_SIZE, true);
    ref_t *start;

    if (r == NULL && (r = ms_region_with_room(least, true)) != NULL)
	words = ms_virgin_room(r, true);
    if (r == NULL) {
	if (!force && !ms_may_map())
	    return NULL;
	r = ms_map_for(TLAB_SIZE);
    }
    start = ms_virgin_take(r, words, true);
    *out_r = r;
    *out_end = start + words;
    return start;
}

/* A TLAB out of a free-list entry: the biggest-class entry of at
   least TLAB_MIN_FREE words, trimmed to aligned edges, the trimmings
   going back on the lists.  During a concurrent mark the TLAB lies
   below its region's sweep_top, so everything allocated in it must be
   born black, which the fast path does not do: the TLAB's mark bits
   are all set as it is carved (ms_blacken), whole bitmap words of its
   own, so every object later placed in it is marked wherever it
   starts.  The sweep looks at objstart bits only, so the bits at
   other positions are harmless, and the next cycle clears them. */
/* No free run served last time, and none has been added since: the
   search would find nothing again (cleared by the sweeps and the
   split).  TLAB_MIN_FREE is the least size of every entry in the
   classes searched. */
static bool tlab_free_none = true;

#ifdef THREADS
static void ms_blacken(ms_region_t *r, ref_t *start, ref_t *end);
#endif

static ref_t *
ms_tlab_from_free(size_t min_words, ms_region_t **out_r, ref_t **out_end)
{
    size_t need = min_words + 2 * TLAB_ALIGN;

    if (need < TLAB_MIN_FREE)
	need = TLAB_MIN_FREE;
    if (tlab_free_none)
	return NULL;
    for (int c = NUM_SIZE_CLASSES - 1; c >= 0 && class_max[c] >= need; c--) {
	ref_t **prev = &free_list_heads[c];
	ref_t *p = *prev;

	while (p) {
	    size_t actual_size = (size_t)p[1];

	    if (!ms_entry_ok(p, actual_size)) {
		prev = (ref_t **)p;
		p = (ref_t *)(*p);
		continue;
	    }
	    if (actual_size >= need) {
		ms_region_t *r = ms_region_of(p);
		ref_t *start = ms_align_up(r, p);
		ref_t *end = p + actual_size;
		size_t tail = BM_IDX(r, end) % TLAB_ALIGN;
		size_t head = (size_t)(start - p);

		*prev = (ref_t *)(*p);
		ms_free_clear(r, p);
		free_list_total_words -= actual_size;
		end -= tail;
		if (head != 0) {
		    size_t len = head;
		    ms_sweep_run_end(&p, &len);	/* keeps p's objstart bit */
		}
		if (tail != 0) {
		    ref_t *t = end;
		    size_t len = tail;

		    ms_objstart_set(r, t);
		    ms_sweep_run_end(&t, &len);
		}
		/* No more than a TLAB's worth; the rest goes back on its
		   list.  A whole big run as one TLAB counted as handed out
		   at once, which brought cycles on far too early, and
		   during a mark its unused tail is retired black at the
		   remark and freed only by the next cycle. */
		if ((size_t)(end - start) > TLAB_SIZE) {
		    ref_t *rest = start + TLAB_SIZE;
		    size_t len = (size_t)(end - rest);

		    ms_objstart_set(r, rest);
		    ms_sweep_run_end(&rest, &len);
		    end = start + TLAB_SIZE;
		}
#ifdef THREADS
		if (MS_BORN_BLACK())
		    ms_blacken(r, start, end);
#endif
		ms_since_snapshot += (size_t)(end - start);
		*out_r = r;
		*out_end = end;
		return start;
	    }
	    prev = (ref_t **)p;
	    p = (ref_t *)(*p);
	}
    }
    tlab_free_none = true;
    return NULL;
}


/* Whether a request of N words can be met without collecting: by a
   free-list entry (of its class or a larger one, see the split above)
   big enough for it, from a region's virgin tail, or from a region
   the budget allows mapping.  Total free words say nothing about
   that: the lists could hold any amount in pieces all smaller than
   N.  The same tests as the allocator's, exactly: a request this says
   fits must then be met, or the caller would collect for ever. */
static bool
ms_space_for(size_t n_words)
{
    int sc = ms_size_class(n_words);

    for (int c = sc; c < NUM_SIZE_CLASSES; c++) {
	ref_t *p = free_list_heads[c];
	while (p) {
	    size_t actual_size = (c == 0) ? 1 : (size_t)p[1];
	    if (!ms_entry_ok(p, actual_size)) {
		p = (ref_t *)(*p);
		continue;
	    }
	    if (c == sc ? actual_size >= n_words : actual_size > n_words)
		return true;
	    p = (ref_t *)(*p);
	}
    }
    if (ms_region_with_room(n_words, false))
	return true;
    return ms_may_map();
}


ref_t *
ms_alloc_slow_locked(size_t n_words, int idx)
{
    ref_t *p;

#ifdef THREADS
    /* The trigger: a cycle is asked for when the heap's occupancy --
       what the last mark found live, and what has been handed out
       since its snapshot -- comes within the headroom of the budget,
       so that the cycle ends before the mutators run out of room. */
    if (!gc_concurrent_in_progress && !gc_requested
	&& marked_words + ms_since_snapshot + ms_headroom
	   >= (size_t)e_next_newspace_size)
	ms_request_cycle("heap at its trigger");
#endif

    /* 1. A small request wants a TLAB, so that the ones after it take
       no lock: out of a big free entry, sweeping for one if need be,
       else out of a virgin tail. */
    if (n_words <= TLAB_SIZE) {
	ms_region_t *r;
	ref_t *new_end, *new_start;

	for (;;) {
	    new_start = ms_tlab_from_free(n_words, &r, &new_end);
	    if (new_start || !ms_lazy_sweep_one())
		break;
	}
	if (!new_start)
	    new_start = ms_tlab_from_virgin(n_words, false, &r, &new_end);
	if (new_start) {
	    ms_tlab_install(idx, r, new_start, new_end);
	    tlab_cursor_array[idx] = new_start + n_words;
	    MS_TLAB_OBJSTART(idx, new_start);
	    return new_start;
	}
    }

    /* 2. Try free list: a large request, or scraps only. */
    p = ms_free_list_alloc(n_words);
    if (p)
	return p;

    /* 2b. Try lazy sweep to populate free lists. */
    while (ms_lazy_sweep_one()) {
	p = ms_free_list_alloc(n_words);
	if (p)
	    return p;
    }

    /* 3. A virgin tail, or a new region within the budget. */
    p = ms_virgin_alloc(n_words, false, false);
    if (p) {
	ms_bump_alloc_notify(p, n_words);
	return p;
    }

    if (trace_gc > 2) {
	ms_region_t *r;
	size_t rooms = 0, biggest = 0, empties = 0;

	for (r = ms_regions; r; r = r->next) {
	    size_t room = ms_virgin_room(r, false);
	    if (room >= n_words)
		rooms++;
	    if (room > biggest)
		biggest = room;
	    if (r->top == r->start)
		empties++;
	}
	fprintf(stderr, "; MS-GC: no room for %zu words: sweep %s at region"
		" %p, tlab_free_none %d, virgin_fail %zu/%zu, %zu regions with"
		" room (biggest %zu), %zu empty, free %zu.\n",
		n_words, lazy_sweep_active ? "active" : "done",
		(void *)sweep_region, tlab_free_none, virgin_fail[0],
		virgin_fail[1], rooms, biggest, empties, free_list_total_words);
    }
    return NULL;  /* GC needed */
}


/* After a cycle has left nothing big enough: the heap grows past its
   budget, which follows.  There is no copying fallback. */
static ref_t *
ms_alloc_beyond_budget(size_t n_words, int idx, const char *reason)
{
    ref_t *p;

    if (trace_gc > 1)
	fprintf(stderr, "; MS-GC: nothing big enough after the cycle"
		" (%s): growing past the budget.\n", reason);
    if (n_words <= TLAB_SIZE) {
	ms_region_t *r;
	ref_t *end;

	p = ms_tlab_from_virgin(n_words, true, &r, &end);
	ms_tlab_install(idx, r, p, end);
	tlab_cursor_array[idx] = p + n_words;
	MS_TLAB_OBJSTART(idx, p);
    } else {
	p = ms_virgin_alloc(n_words, false, true);
	ms_bump_alloc_notify(p, n_words);
    }
    if (ms_mapped > (size_t)e_next_newspace_size) {
	e_next_newspace_size = ms_mapped;
	ms_set_pacing();
    }
    return p;
}

/* ALLOCATE's slow path, out of line (data.h). */
ref_t *
ms_allocate_slow(size_t words, char *reason)
{
    int idx = MS_TLAB_INDEX;
    ref_t *p;

#ifdef THREADS
    while (oak_mutex_trylock(&alloc_lock) != 0) {
	if (gc_pending)
	    wait_for_gc();
    }
#endif
    p = ms_alloc_slow_locked(words, idx);
    /* ms_collect runs a cycle, or with threads waits for the GC
       thread's to end (asking for one if none is running); a request
       still unmet after it maps a region past the budget. */
    while (!p) {
	ms_collect(false, reason, words);
	p = ms_alloc_slow_locked(words, idx);
	if (!p)
	    p = ms_alloc_beyond_budget(words, idx, reason);
    }
#if defined(THREADS) && defined(OAK_GC_VERIFY)
    ms_verify_log_alloc(p, 2, words);
#endif
#ifdef THREADS
    oak_mutex_unlock(&alloc_lock);
#endif
    return p;
}

void
ms_tlab_retire(ref_t *cursor, ref_t *end)
{
    size_t remaining;

    if (!cursor || cursor >= end)
	return;
    remaining = (size_t)(end - cursor);

    /* Zero the unused region so it doesn't confuse the mark phase
       (zeroed words are fixnum 0, not pointers). */
    memset(cursor, 0, remaining * sizeof(ref_t));

    /* Set an objstart bit so the sweep can find and reclaim it. */
    ms_objstart_set(ms_region_of(cursor), cursor);
}


void
ms_tlab_retire_all(void)
{
#ifdef THREADS
    extern int next_index;
    int n = next_index;   /* snapshot under alloc_lock */
#else
    int n = 1;
#endif

    for (int i = 0; i < n; i++) {
	/* A dead thread's TLAB is still worth retiring so the sweep can
	   reclaim it; only its stacks have gone away. */
	ms_tlab_retire(tlab_cursor_array[i], tlab_end_array[i]);
	tlab_cursor_array[i] = NULL;
	tlab_end_array[i] = NULL;
	tlab_base_array[i] = NULL;
	tlab_bm_array[i] = NULL;
    }
}


/* ------------------------------------------------------------------ */
/*  SATB write barrier buffers                                         */
/* ------------------------------------------------------------------ */

int gc_satb_active = 0;

/*
 * Dropping an entry would break the snapshot-at-the-beginning
 * invariant: an object whose last remaining reference was overwritten
 * during concurrent mark would never be marked and would then be
 * swept while still live.  A mutator can easily overwrite more than
 * SATB_BUFFER_SIZE pointer slots in one mark phase, so with threads a
 * full buffer is handed to the marker and a fresh one begun (without
 * threads the one buffer grows; it is never used, the mark being
 * stop-the-world there).
 *
 * Each buffer is private to one thread: only that thread appends to
 * it, and it is drained during a stop-the-world pause.
 */

#ifdef THREADS
ref_t *satb_buffer_array[MAX_THREAD_COUNT];
size_t satb_count_array[MAX_THREAD_COUNT];
size_t satb_size_array[MAX_THREAD_COUNT];
#else
ref_t *satb_buffer;
size_t satb_count = 0;
size_t satb_size = 0;
#endif

/* Make room for one more entry in a SATB buffer. */
static void
satb_room(ref_t **bufp, size_t count, size_t *sizep)
{
    size_t new_size;
    ref_t *bigger;

    if (count < *sizep)
	return;

    new_size = *sizep ? 2 * *sizep : SATB_BUFFER_SIZE;
    bigger = (ref_t *)realloc(*bufp, new_size * sizeof(ref_t));
    if (bigger == NULL) {
	fprintf(stderr,
		"\nFatal error: out of memory growing a GC write barrier"
		" buffer to %zu entries.\n", new_size);
	exit(EXIT_FAILURE);
    }
    *bufp = bigger;
    *sizep = new_size;
}

#ifdef THREADS
/* Full buffers, handed to the marker: it drains them concurrently
   (ms_satb_drain_concurrent) between runs of the mark stack, so the
   remark sees only each thread's last partial buffer. */
static oak_mutex_t satb_lock = OAK_MUTEX_INITIALIZER;
static struct satb_full { ref_t *buf; size_t count; struct satb_full *next; }
    *satb_full_list;

static void
satb_append(ref_t v)
{
    int idx = *(int *)oak_tls_get(index_key);

    if (satb_count_array[idx] >= satb_size_array[idx]) {
	if (satb_buffer_array[idx] != NULL && satb_count_array[idx] > 0) {
	    struct satb_full *full = (struct satb_full *)xmalloc(sizeof *full);

	    full->buf = satb_buffer_array[idx];
	    full->count = satb_count_array[idx];
	    oak_mutex_lock(&satb_lock);
	    full->next = satb_full_list;
	    satb_full_list = full;
	    oak_mutex_unlock(&satb_lock);
	    satb_buffer_array[idx] = NULL;
	    satb_size_array[idx] = 0;
	}
	satb_room(&satb_buffer_array[idx], 0, &satb_size_array[idx]);
	satb_count_array[idx] = 0;
    }
    satb_buffer_array[idx][satb_count_array[idx]++] = v;
}
#else
static void
satb_append(ref_t v)
{
    satb_room(&satb_buffer, satb_count, &satb_size);
    satb_buffer[satb_count++] = v;
}
#endif

void
satb_log_old(ref_t *addr)
{
    ref_t old_val = *addr;

    /* Only log pointer-tagged values (skip fixnums and immediates). */
    if (old_val & PTR_MASK)
	satb_append(old_val);
}

void
satb_log_value(ref_t v)
{
    if (v & PTR_MASK)
	satb_append(v);
}

void
satb_flush_all(void)
{
#ifdef THREADS
    extern int next_index;
    for (int i = 0; i < next_index; i++)
	satb_count_array[i] = 0;
#else
    satb_count = 0;
#endif
}


/* ------------------------------------------------------------------ */
/*  Concurrent marking state                                           */
/* ------------------------------------------------------------------ */

volatile int gc_phase = GC_PHASE_IDLE;
volatile int gc_weak_clearing = 0;

#ifdef THREADS
/*
 * The GC thread.  Every cycle runs on a thread of its own, started at
 * the first request, so that no mutator is out of action for the
 * length of a mark: a mutator asks for a cycle (ms_request_cycle: at
 * the trigger, or an explicit GC) and goes on, or when it has no room
 * left waits for the next cycle to end (ms_wait_cycle), parked at a
 * safepoint.  The GC thread has no slot in the thread arrays: it
 * runs no Oaklisp code, allocates nothing, holds no roots, and the
 * stop-the-world handshakes it runs do not wait for it; its index is
 * MS_GC_THREAD_INDEX.  A dump or a world load, which move or unmap
 * the heap, hold it off (ms_gc_hold) until they are done.
 */
volatile int gc_concurrent_in_progress = 0;

static oak_mutex_t gc_concurrent_lock = OAK_MUTEX_INITIALIZER;
static oak_cond_t  gc_concurrent_done_cv = OAK_COND_INITIALIZER;
static oak_cond_t  gc_request_cv = OAK_COND_INITIALIZER;
static int gc_requested;		/* a cycle is wanted */
static const char *gc_request_reason;
static size_t gc_request_amount;	/* the most a waiter needs */
static unsigned long gc_cycles_done;
static int gc_hold;			/* dumps and world loads in progress */
static bool gc_thread_started;

static void *ms_gc_thread_main(void *arg);

void
gc_safepoint_init(void)
{
#ifdef OAK_NEEDS_DYNAMIC_MUTEX_INIT
    oak_mutex_init(&gc_concurrent_lock);
    oak_cond_init(&gc_concurrent_done_cv);
    oak_cond_init(&gc_request_cv);
    oak_mutex_init(&satb_lock);
#endif
}

/* Under gc_concurrent_lock. */
static void
ms_gc_thread_start_locked(void)
{
    oak_thread_t t;

    if (gc_thread_started)
	return;
    if (oak_thread_create(&t, ms_gc_thread_main, NULL) != 0) {
	fprintf(stderr, "\nFatal error: cannot start the GC thread.\n");
	exit(EXIT_FAILURE);
    }
    gc_thread_started = true;
}

/* Ask for a cycle, unless one is running or already asked for. */
static void
ms_request_cycle(const char *reason)
{
    oak_mutex_lock(&gc_concurrent_lock);
    if (!gc_concurrent_in_progress && !gc_requested) {
	gc_requested = 1;
	gc_request_reason = reason;
	ms_gc_thread_start_locked();
	oak_cond_broadcast(&gc_request_cv);
    }
    oak_mutex_unlock(&gc_concurrent_lock);
}

/* No room for AMOUNT words: wait for the next cycle to end (asking
   for one if none is running), parked at a safepoint.  alloc_lock is
   held on entry and again on return. */
static void
ms_wait_cycle(const char *reason, size_t amount)
{
    int idx = *(int *)oak_tls_get(index_key);
    unsigned long target;
    gc_timer_t wait;

    gc_stats_begin(&wait, GC_EV_ALLOC_WAIT);
    if (trace_gc > 1)
	fprintf(stderr, "; MS-GC: thread %d waits for a cycle (%s, %zu words):"
		" %zu mapped, budget %zu, headroom %zu, %zu free, phase %d.\n",
		idx, reason, amount, ms_mapped, (size_t)e_next_newspace_size,
		ms_headroom, free_list_total_words, gc_phase);
    oak_mutex_unlock(&alloc_lock);
    /* Advertise that we are parked for the whole time we are out of
       action, including while re-acquiring the allocator lock: the
       stop-the-world handshake spins until every thread's gc_ready
       flag is set. */
    gc_ready[idx] = 1;
    oak_mutex_lock(&gc_concurrent_lock);
    target = gc_cycles_done + 1;
    ms_waited = true;
    if (amount > gc_request_amount)
	gc_request_amount = amount;
    if (!gc_concurrent_in_progress && !gc_requested) {
	gc_requested = 1;
	gc_request_reason = reason;
	ms_gc_thread_start_locked();
	oak_cond_broadcast(&gc_request_cv);
    }
    while (gc_cycles_done < target)
	oak_cond_wait(&gc_concurrent_done_cv, &gc_concurrent_lock);
    oak_mutex_unlock(&gc_concurrent_lock);
    /* Leave the parked state the way wait_for_gc does: under gc_lock,
       which the GC thread holds for the whole of a stop-the-world
       pause.  Clearing gc_ready without it let a thread woken here run
       straight through the next cycle's initial mark, which had
       counted it as parked, so what it allocated meanwhile was neither
       a root nor born black. */
    oak_mutex_lock(&gc_lock);
    gc_ready[idx] = 0;
    oak_mutex_unlock(&gc_lock);
    while (oak_mutex_trylock(&alloc_lock) != 0) {
	if (gc_pending)
	    wait_for_gc();
	else
	    oak_thread_yield();
    }
    gc_stats_end(&wait, GC_EV_ALLOC_WAIT, reason);
}

void
ms_gc_hold(void)
{
    int idx = *(int *)oak_tls_get(index_key);

    /* Parked while waiting: the cycle's pauses need us at a
       safepoint. */
    gc_ready[idx] = 1;
    oak_mutex_lock(&gc_concurrent_lock);
    gc_hold++;
    while (gc_concurrent_in_progress)
	oak_cond_wait(&gc_concurrent_done_cv, &gc_concurrent_lock);
    oak_mutex_unlock(&gc_concurrent_lock);
    oak_mutex_lock(&gc_lock);
    gc_ready[idx] = 0;
    oak_mutex_unlock(&gc_lock);
}

void
ms_gc_release(void)
{
    oak_mutex_lock(&gc_concurrent_lock);
    if (gc_hold > 0 && --gc_hold == 0 && gc_requested)
	oak_cond_broadcast(&gc_request_cv);
    oak_mutex_unlock(&gc_concurrent_lock);
}

/* Request all mutator threads to stop (STW pause).
   Sets gc_pending and waits for all gc_ready[] flags.
   Returns the thread count snapshot. */
static int
stw_request_pause(int my_index_val)
{
    int gc_thread_count;
    gc_timer_t handshake;

    if (my_index_val >= 0)
	gc_ready[my_index_val] = 1;
    set_gc_flag(true);   /* locks gc_lock, sets gc_pending */
    gc_stats_begin(&handshake, GC_EV_HANDSHAKE);
    gc_thread_count = next_index;

    /* Busy-wait for all threads to reach a safepoint. */
    {
	bool ready = false;
	while (!ready) {
	    ready = true;
	    for (int i = 0; i < gc_thread_count; i++) {
		if (gc_ready[i] == 0 && !gc_thread_dead[i]) {
		    ready = false;
		    oak_thread_yield();
		    break;
		}
	    }
	}
    }
    gc_stats_end(&handshake, GC_EV_HANDSHAKE, NULL);

    return gc_thread_count;
}

/* Release all mutator threads from a STW pause. */
static void
stw_release(int my_index_val)
{
    if (my_index_val >= 0)
	gc_ready[my_index_val] = 0;
    set_gc_flag(false);   /* gc_pending=false, unlocks gc_lock */
}

#endif /* THREADS */


/* Set mark bit for born-black allocation during concurrent mark. */
void
ms_mark_alloc(ref_t *p)
{
    ms_region_t *r = ms_region_of(p);
    size_t idx;

    if (r == NULL)
	return;
    idx = BM_IDX(r, p);
#if defined(THREADS) && defined(OAK_GC_VERIFY)
    if (r->verify_born == NULL) {
	r->verify_born = (uint64_t *)xmalloc(BM_WORDS(r) * sizeof(uint64_t));
	memset(r->verify_born, 0, BM_WORDS(r) * sizeof(uint64_t));
    }
    OAK_ATOMIC_FETCH_OR_U64(&r->verify_born[idx / 64], 1ULL << (idx % 64));
#endif
#ifdef THREADS
    OAK_ATOMIC_FETCH_OR_U64(&r->mark[idx / 64], 1ULL << (idx % 64));
#else
    BM_SET(r->mark, idx);
#endif
}

#ifdef THREADS
/* Born black, the whole of [START, END): a TLAB carved from a free
   run during the mark.  Whole bitmap words (TLAB_ALIGN), the TLAB's
   own, though the marker may set a bit in them meanwhile, so atomic. */
static void
ms_blacken(ms_region_t *r, ref_t *start, ref_t *end)
{
    size_t w0 = BM_IDX(r, start) / 64, w1 = BM_IDX(r, end) / 64, w;

#ifdef OAK_GC_VERIFY
    if (r->verify_born == NULL) {
	r->verify_born = (uint64_t *)xmalloc(BM_WORDS(r) * sizeof(uint64_t));
	memset(r->verify_born, 0, BM_WORDS(r) * sizeof(uint64_t));
    }
    for (w = w0; w < w1; w++)
	OAK_ATOMIC_FETCH_OR_U64(&r->verify_born[w], ~0ULL);
#endif
    for (w = w0; w < w1; w++)
	OAK_ATOMIC_FETCH_OR_U64(&r->mark[w], ~0ULL);
}
#endif


/* ------------------------------------------------------------------ */
/*  Deferred objstart recording for the dump compaction                */
/* ------------------------------------------------------------------ */

static ref_t **deferred_objstarts;
static size_t  deferred_count;
static size_t  deferred_cap;

void
ms_record_transport(ref_t *new_place)
{
    if (deferred_count >= deferred_cap) {
	deferred_cap = deferred_cap ? deferred_cap * 2 : 4096;
	deferred_objstarts = (ref_t **)realloc(
	    deferred_objstarts, deferred_cap * sizeof(ref_t *));
	if (!deferred_objstarts) {
	    fprintf(stderr, "gc-ms: deferred objstart realloc failed\n");
	    exit(EXIT_FAILURE);
	}
    }
    deferred_objstarts[deferred_count++] = new_place;
}


/* ------------------------------------------------------------------ */
/*  Object extent (from the object-start bitmap)                       */
/* ------------------------------------------------------------------ */

/* Extent of the object starting at obj, taken from the object-start
   bitmap.  This is the authoritative record of allocation boundaries:
   a naked cell (from MAKE-CELL) holds an arbitrary value in word[0],
   which a type-directed length lookup would happily read as a type
   pointer and derive a length from the *next* object's memory.  The
   region's top bounds it: beyond lies the virgin tail. */
static size_t
ms_objstart_extent(ms_region_t *r, ref_t *obj)
{
    size_t limit = BM_IDX(r, r->top);
    size_t idx = BM_IDX(r, obj), next;

    if (idx + 1 >= limit)
	return 1;
    next = idx + 1;
    while (next < limit && !BM_TEST(r->objstart, next))
	next++;
    return next - idx;
}


/* ------------------------------------------------------------------ */
/*  Find object start from an interior address (for locatives)         */
/* ------------------------------------------------------------------ */

static ref_t *
find_object_start(ms_region_t *r, ref_t *addr)
{
    size_t idx = BM_IDX(r, addr);
    size_t w = idx / 64;
    int bit = (int)(idx % 64);

    /* Mask: keep bits 0..bit (at or below the target). */
    uint64_t word = r->objstart[w] & ((2ULL << bit) - 1);

    /* Scan backward through bitmap words. */
    while (word == 0) {
	if (w == 0) return NULL;
	w--;
	word = r->objstart[w];
    }

    int highest = 63 - CLZ64(word);
    return r->start + w * 64 + highest;
}

/* The region and the object a reference lands in, or NULL: spatic
   (one block, rejected first: nearly every pointer in a world image
   is one), the stacks, C data and garbage give NULL.  A locative, or
   a pointer not on an object start, is taken to its containing
   object: MAKE-LAMBDA allocates the operation and its method as one
   object, and the method is referred to on its own (e_current_method,
   context frames), often after the operation has become garbage. */
static inline ms_region_t *
ms_mark_target(ref_t ref, ref_t **objp)
{
    ref_t *p = ANY_TO_PTR(ref);
    ms_region_t *r;

    if (SPATIC_PTR(p))
	return NULL;
    r = ms_region_of(p);
    if (r == NULL)
	return NULL;
    if (TAG_IS(ref, LOC_TAG) || !BM_TEST(r->objstart, BM_IDX(r, p))) {
	p = find_object_start(r, p);
	if (p == NULL)
	    return NULL;
    }
    /* Into a free-list entry: garbage (ms_free_set). */
    if (BM_TEST(r->freebm, BM_IDX(r, p)))
	return NULL;
    *objp = p;
    return r;
}


/* ------------------------------------------------------------------ */
/*  Mark phase                                                         */
/* ------------------------------------------------------------------ */

/* Dynamically growing mark stack. */
static ref_t **mark_stack;
static size_t mark_stack_size;
static size_t mark_stack_top;

#define MARK_STACK_INITIAL 4096

static void
mark_stack_init(void)
{
    mark_stack_size = MARK_STACK_INITIAL;
    mark_stack_top = 0;
    mark_stack = (ref_t **)xmalloc(mark_stack_size * sizeof(ref_t *));
}

static void
mark_stack_push(ref_t *obj)
{
    if (mark_stack_top >= mark_stack_size) {
	mark_stack_size *= 2;
	mark_stack = (ref_t **)realloc(mark_stack,
				       mark_stack_size * sizeof(ref_t *));
	if (!mark_stack) {
	    fprintf(stderr, "gc-ms: mark stack realloc failed\n");
	    exit(EXIT_FAILURE);
	}
    }
    mark_stack[mark_stack_top++] = obj;
}

static ref_t *
mark_stack_pop(void)
{
    return mark_stack[--mark_stack_top];
}


/*
 * Mark an object in the heap and push it onto the mark stack
 * if not already marked.  For spatic objects, do nothing (they
 * are permanent and always considered live).
 */
static void
ms_mark_ref(ref_t ref)
{
    ms_region_t *r;
    ref_t *p;
    size_t idx;

    if (!(ref & PTR_MASK))
	return;   /* fixnum or immediate — not a pointer */
    r = ms_mark_target(ref, &p);
    if (r == NULL)
	return;
    idx = BM_IDX(r, p);
    if (!BM_TEST(r->mark, idx)) {
	BM_SET(r->mark, idx);
	mark_stack_push(p);
    }
}


/*
 * Trace: pop objects from the mark stack, scan their fields,
 * mark any pointer-containing fields.
 */
static void
ms_trace(void)
{
    while (mark_stack_top > 0) {
	ref_t *obj = mark_stack_pop();
	size_t len = ms_objstart_extent(ms_region_of(obj), obj);

	marked_words += len;
	for (size_t i = 0; i < len; i++) {
	    ref_t val = obj[i];
	    if (val & PTR_MASK)
		ms_mark_ref(val);
	}
    }
}


#ifdef THREADS
/*
 * Concurrent variant of ms_mark_ref — uses atomic bitmap operations
 * so the GC thread can mark concurrently with mutator allocations.
 * The mark stack is private to the GC thread so push/pop need not
 * be atomic.
 */
static void
ms_mark_ref_concurrent(ref_t ref)
{
    ms_region_t *r;
    ref_t *p;
    size_t idx;
    uint64_t bit, old;

    if (!(ref & PTR_MASK))
	return;
    r = ms_mark_target(ref, &p);
    if (r == NULL)
	return;
    idx = BM_IDX(r, p);
    bit = 1ULL << (idx % 64);
    old = OAK_ATOMIC_FETCH_OR_U64(&r->mark[idx / 64], bit);
    if (!(old & bit))
	mark_stack_push(p);
}


/*
 * Concurrent trace: pop objects from the mark stack and scan their
 * fields with atomic marking.  The mark stack is only accessed by
 * the GC coordinator thread.
 */
/* The full SATB buffers handed over so far: marked, atomically, and
   freed.  True if there were any. */
static bool
ms_satb_drain_concurrent(void)
{
    struct satb_full *list, *next;

    oak_mutex_lock(&satb_lock);
    list = satb_full_list;
    satb_full_list = NULL;
    oak_mutex_unlock(&satb_lock);
    if (list == NULL)
	return false;
    for (; list; list = next) {
	for (size_t j = 0; j < list->count; j++)
	    ms_mark_ref_concurrent(list->buf[j]);
	next = list->next;
	free(list->buf);
	free(list);
    }
    return true;
}

static void
ms_trace_concurrent(void)
{
    for (;;) {
	while (mark_stack_top > 0) {
	    ref_t *obj = mark_stack_pop();
	    size_t len = ms_objstart_extent(ms_region_of(obj), obj);

	    marked_words += len;
	    for (size_t i = 0; i < len; i++) {
		ref_t val = obj[i];
		if (val & PTR_MASK)
		    ms_mark_ref_concurrent(val);
	    }
	}
	/* The stack is empty: the barrier buffers handed over
	   meanwhile, and whatever they lead to. */
	if (!ms_satb_drain_concurrent())
	    break;
    }
}

/* Spatic, scanned concurrently: any pointer from the world image
   into the heap is a root.  The mutators write it meanwhile, under
   the barrier: a word read old was there since the snapshot, and one
   read new refers to what the snapshot's rules mark anyway, while
   what was overwritten is in a barrier buffer. */
static void
ms_mark_spatic_concurrent(void)
{
    ref_t *p;

    for (p = spatic.start; p < spatic.end; p++) {
	ref_t r = *p;

	if ((r & PTR_MASK) && !SPATIC_PTR(ANY_TO_PTR(r)))
	    ms_mark_ref_concurrent(r);
    }
}

#endif /* THREADS */


#if defined(THREADS) && defined(OAK_GC_VERIFY)
/*
 * Verify mode (-DOAK_GC_VERIFY): at the end of the remark pause, with
 * the world still stopped, mark the heap again from scratch and
 * compare.  An object the fresh mark reaches below its region's
 * sweep_top that the concurrent mark missed would be swept while
 * live: a missing write barrier or born-black.  Each one is reported
 * with the object that refers to it, then kept, so the run can go on.
 */
static unsigned long ms_verify_cycles, ms_verify_missed_total;
static int ms_verify_from_marked;
volatile int ms_verify_stw_owner = MS_NO_STW_OWNER;

/* Verify mode: a ring of recent allocations, to say where a missed
   object came from.  Path 1 is the TLAB fast path, 2 the slow path;
   3 is a free-list allocation (logged too, inside the slow path). */
#define VLOG_SIZE (1 << 22)
static struct vlog { ref_t *p; size_t words; int thread, phase, path; unsigned long cycle; }
    *ms_vlog;
static volatile unsigned long ms_vlog_next;

void
ms_verify_log_alloc(ref_t *p, int path, size_t words)
{
    unsigned long i;
    static int enabled = -1;

    /* The log costs every allocation an atomic add, which shifts the
       timing; OAK_GC_VERIFY_LOG=0 turns it off. */
    if (enabled < 0) {
	const char *e = getenv("OAK_GC_VERIFY_LOG");
	enabled = !(e && *e == '0');
    }
    if (!enabled)
	return;
    if (!ms_vlog) {
	struct vlog *fresh = calloc(VLOG_SIZE, sizeof(*fresh)), *none = NULL;
	if (!__atomic_compare_exchange_n(&ms_vlog, &none, fresh, 0,
					 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
	    free(fresh);
    }
    i = __atomic_fetch_add(&ms_vlog_next, 1, __ATOMIC_RELAXED) & (VLOG_SIZE - 1);
    ms_vlog[i].p = p;
    ms_vlog[i].words = words;
    ms_vlog[i].thread = *(int *)oak_tls_get(index_key);
    ms_vlog[i].phase = gc_phase;
    ms_vlog[i].path = path;
    ms_vlog[i].cycle = ms_verify_cycles;
}

static void
ms_verify_find_alloc(ref_t *p)
{
    unsigned long n = ms_vlog_next, i, k, found = 0;

    for (k = 0; k < VLOG_SIZE && k < n && found < 3; k++) {
	i = (n - 1 - k) & (VLOG_SIZE - 1);
	if (ms_vlog[i].p <= p && p < ms_vlog[i].p + ms_vlog[i].words) {
	    fprintf(stderr, ";   %p (+%ld) allocated %lu allocations ago (%zu words)"
		    " by thread %d, phase %d, path %d, in cycle %lu (now %lu)\n",
		    (void *)ms_vlog[i].p, (long)(p - ms_vlog[i].p), k,
		    ms_vlog[i].words, ms_vlog[i].thread, ms_vlog[i].phase,
		    ms_vlog[i].path, ms_vlog[i].cycle, ms_verify_cycles);
	    found++;
	}
    }
    if (!found)
	fprintf(stderr, ";   %p not among the last %lu allocations\n",
		(void *)p, n < VLOG_SIZE ? n : (unsigned long)VLOG_SIZE);
}

void
ms_verify_stw_alloc(int idx, const char *how)
{
    fprintf(stderr, "; GC VERIFY: thread %d allocating (%s) while thread %d"
	    " has the world stopped (phase %d)\n",
	    idx, how, ms_verify_stw_owner, gc_phase);
}
static void ms_mark_roots(void);

static const char *
ms_verify_type_name(ref_t type)
{
    if (type == e_cons_type) return "cons";
    if (type == e_env_type) return "env";
    if (type == e_segment_type) return "segment";
    if (type == e_method_type) return "method";
    if (type == e_operation_type) return "operation";
    if (type == e_fixnum_type) return "fixnum?";
    return "?";
}

static void
ms_verify_report(const char *what, ref_t *from, size_t field, ref_t *obj)
{
    ms_region_t *r = ms_region_of(obj);
    size_t len = ms_objstart_extent(r, obj);

    fprintf(stderr, "; GC VERIFY: missed %p %s in-snapshot %d (%zu words: %#lx %#lx %#lx)",
	    (void *)obj, ms_verify_type_name(obj[0]),
	    r->verify_snap ? BM_TEST(r->verify_snap, BM_IDX(r, obj)) != 0 : -1,
	    len, (unsigned long)obj[0],
	    (unsigned long)(len > 1 ? obj[1] : 0),
	    (unsigned long)(len > 2 ? obj[2] : 0));
    if (from) {
	ms_region_t *fr = ms_region_of(from);

	fprintf(stderr, " via field %zu of %p %s (%zu words, marked %d,"
		" above sweep_top %d)",
		field, (void *)from, ms_verify_type_name(from[0]),
		ms_objstart_extent(fr, from), ms_verify_from_marked,
		from >= fr->sweep_top);
	fprintf(stderr, " born-black %d",
		fr->verify_born
		? BM_TEST(fr->verify_born, BM_IDX(fr, from)) != 0 : 0);
    } else
	fprintf(stderr, " from %s", what);
    fprintf(stderr, "\n");
    ms_verify_find_alloc(obj);
    if (from)
	ms_verify_find_alloc(from);
}

/* Verify mode: the regions' verify bitmaps for this cycle. */
static void
ms_verify_cycle_begin(void)
{
    ms_region_t *r;

    if (!ms_vlog)
	ms_vlog = calloc(VLOG_SIZE, sizeof(*ms_vlog));
    for (r = ms_regions; r; r = r->next) {
	size_t bytes = BM_WORDS(r) * sizeof(uint64_t);

	free(r->verify_born);
	r->verify_born = (uint64_t *)xmalloc(bytes);
	memset(r->verify_born, 0, bytes);
	free(r->verify_snap);
	r->verify_snap = NULL;
    }
}

/* Verify mode: the marks so far become the snapshot (then cleared). */
static void
ms_verify_take_snapshot(void)
{
    ms_region_t *r;

    for (r = ms_regions; r; r = r->next) {
	size_t bytes = BM_WORDS(r) * sizeof(uint64_t);

	r->verify_snap = (uint64_t *)xmalloc(bytes);
	memcpy(r->verify_snap, r->mark, bytes);
	memset(r->mark, 0, bytes);
    }
}

static void
ms_verify_marks(void)
{
    unsigned long missed = 0, reported = 0, via_field;
    ms_region_t *r;
    size_t nr = 0, k;
    uint64_t **done;

    /* Each region's concurrent result, then its marks cleared. */
    for (r = ms_regions; r; r = r->next)
	nr++;
    done = (uint64_t **)xmalloc((nr ? nr : 1) * sizeof(uint64_t *));
    for (r = ms_regions, k = 0; r; r = r->next, k++) {
	size_t bytes = BM_WORDS(r) * sizeof(uint64_t);

	done[k] = (uint64_t *)xmalloc(bytes);
	memcpy(done[k], r->mark, bytes);
	memset(r->mark, 0, bytes);
    }

    /* What the remark treats as live: everything reachable from the
       roots, the objects allocated during the mark included (the
       remark did not scan those: the weak-read barrier stands in). */
    ms_mark_roots();

    while (mark_stack_top > 0) {
	ref_t *obj = mark_stack_pop();
	ms_region_t *oreg = ms_region_of(obj);
	size_t len = ms_objstart_extent(oreg, obj);

	for (size_t i = 0; i < len; i++) {
	    ref_t val = obj[i];
	    ref_t *t;
	    ms_region_t *tr;
	    size_t idx, tk;

	    if (!(val & PTR_MASK) || !(tr = ms_mark_target(val, &t)))
		continue;
	    idx = BM_IDX(tr, t);
	    for (r = ms_regions, tk = 0; r != tr; r = r->next, tk++)
		;
	    if (idx < BM_IDX(tr, tr->sweep_top) && !BM_TEST(done[tk], idx)
		&& !BM_TEST(tr->mark, idx) && reported < 20) {
		size_t oi;

		for (r = ms_regions, oi = 0; r != oreg; r = r->next, oi++)
		    ;
		ms_verify_from_marked =
		    obj >= oreg->sweep_top
		    || BM_TEST(done[oi], BM_IDX(oreg, obj)) != 0;
		ms_verify_report(NULL, obj, i, t);
		reported++;
	    }
	    ms_mark_ref(val);
	}
    }

    /* Report the rest only if none was reached through a field: those
       were reached straight from the roots. */
    via_field = reported;
    for (r = ms_regions, k = 0; r; r = r->next, k++) {
	size_t limit = BM_IDX(r, r->sweep_top);

	for (size_t w = 0; w < BM_WORDS(r); w++) {
	    uint64_t lost = r->mark[w] & ~done[k][w];
	    while (lost) {
		size_t idx = w * 64 + CTZ64(lost);
		lost &= lost - 1;
		if (idx >= limit)
		    break;
		if (!via_field && reported < 20) {
		    ms_verify_report("the roots", NULL, 0, r->start + idx);
		    reported++;
		}
		missed++;
	    }
	    r->mark[w] |= done[k][w];
	}
	free(done[k]);
    }
    free(done);

    ms_verify_cycles++;
    ms_verify_missed_total += missed;
    if (missed || trace_gc)
	fprintf(stderr, "; GC VERIFY cycle %lu: %lu live object%s missed"
		" (%lu in all)\n", ms_verify_cycles, missed,
		missed == 1 ? "" : "s", ms_verify_missed_total);
}
#endif

/*
 * Drain all SATB buffers into the mark stack.
 * Called during the remark STW pause so that objects whose pointers
 * were overwritten during concurrent mark are traced.
 */
void
satb_drain_to_mark_stack(void)
{
#ifdef THREADS
    extern int next_index;
    struct satb_full *list, *next;

    /* Full buffers handed over since the marker last looked, then
       every thread's partial one. */
    oak_mutex_lock(&satb_lock);
    list = satb_full_list;
    satb_full_list = NULL;
    oak_mutex_unlock(&satb_lock);
    for (; list; list = next) {
	for (size_t j = 0; j < list->count; j++)
	    ms_mark_ref(list->buf[j]);
	next = list->next;
	free(list->buf);
	free(list);
    }
    for (int i = 0; i < next_index; i++) {
	for (size_t j = 0; j < satb_count_array[i]; j++)
	    ms_mark_ref(satb_buffer_array[i][j]);
	satb_count_array[i] = 0;
    }
#else
    for (size_t j = 0; j < satb_count; j++)
	ms_mark_ref(satb_buffer[j]);
    satb_count = 0;
#endif
}


/* A root is worth a call only if it is a pointer not into spatic:
   nearly every word of spatic and of the stacks is a fixnum or points
   into spatic, and this is the collector's one pass over them. */
#define MS_ROOT(r)							\
    do { ref_t _r = (r);						\
	 if ((_r & PTR_MASK) && !SPATIC_PTR(ANY_TO_PTR(_r)))		\
	     ms_mark_ref(_r); } while (0)

/* Spatic space, with the world stopped: any pointer from the world
   image into the heap is a root.  The single-threaded cycle's and
   verify mode's; with threads the scan is concurrent. */
#if !defined(THREADS) || defined(OAK_GC_VERIFY)
static void
ms_mark_spatic(void)
{
    ref_t *p;

    for (p = spatic.start; p < spatic.end; p++)
	MS_ROOT(*p);
}
#endif

/*
 * The roots a pause must scan: the VM registers and every thread's
 * stacks, which no barrier covers.  Spatic is scanned apart
 * (ms_mark_spatic; with threads concurrently, ms_mark_spatic_concurrent,
 * and not at the remark: stores into it are barriered like any).
 */
static void
ms_mark_thread_roots(void)
{
    ref_t *p;

#ifdef THREADS
    int my_index;
    int gc_thread_count = next_index;
    /* Skip the slots of threads that have exited: their stacks are gone. */
#define FORTHREADS for (my_index=0; my_index<gc_thread_count; my_index++)	\
		     if (!gc_thread_dead[my_index])
#else
#define FORTHREADS
#endif

    /* Global VM registers */
    ms_mark_ref(e_nil);
    ms_mark_ref(e_t);
    ms_mark_ref(e_boot_code);
    ms_mark_ref(e_fixnum_type);
    ms_mark_ref(e_loc_type);
    ms_mark_ref(e_cons_type);
    ms_mark_ref(e_env_type);
    ms_mark_ref(e_object_type);
    ms_mark_ref(e_segment_type);
    ms_mark_ref(e_uninitialized);
    ms_mark_ref(e_method_type);
    ms_mark_ref(e_operation_type);

    /* Tables stored as pointers with offset */
    if (e_subtype_table)
	ms_mark_ref(PTR_TO_REF(e_subtype_table - 2));
    if (e_argless_tag_trap_table)
	ms_mark_ref(PTR_TO_REF(e_argless_tag_trap_table - 2));
    if (e_arged_tag_trap_table)
	ms_mark_ref(PTR_TO_REF(e_arged_tag_trap_table - 2));

    /* Per-thread registers */
    FORTHREADS {
	ms_mark_ref(PTR_TO_REF(e_env));
	ms_mark_ref(e_code_segment);
	ms_mark_ref(e_current_method);
	ms_mark_ref(e_process);
	ms_mark_ref(PTR_TO_LOC(e_bp));

	/* GC examine buffer */
	for (p = gc_examine_buffer; p < gc_examine_ptr; p++)
	    MS_ROOT(*p);

	/* Value stack */
	for (p = value_stack.bp; p <= value_stack.sp; p++)
	    MS_ROOT(*p);

	/* Context stack */
	for (p = context_stack.bp; p <= context_stack.sp; p++)
	    MS_ROOT(*p);

	/* Stack segments (linked lists in the heap) */
	ms_mark_ref(value_stack.segment);
	ms_mark_ref(context_stack.segment);
    }

#undef FORTHREADS
}

/* Every root, with the world stopped: the single-threaded cycle's,
   and verify mode's. */
#if !defined(THREADS) || defined(OAK_GC_VERIFY)
static void
ms_mark_roots(void)
{
    ms_mark_thread_roots();
    ms_mark_spatic();
}
#endif
#undef MS_ROOT

/* Before a cycle: every region's marks cleared.  Under alloc_lock,
   so the list is stable; a region mapped after this starts with none
   either. */
static void
ms_clear_marks(void)
{
    ms_region_t *r;

    for (r = ms_regions; r; r = r->next)
	memset(r->mark, 0, BM_WORDS(r) * sizeof(uint64_t));
}

/* The cycle's snapshot: every region's sweep_top is its top.  With
   the world stopped (or one thread). */
static void
ms_snapshot_tops(void)
{
    ms_region_t *r;

    for (r = ms_regions; r; r = r->next)
	r->sweep_top = r->top;
}


/* ------------------------------------------------------------------ */
/*  Sweep phase                                                        */
/* ------------------------------------------------------------------ */

/* A sweep puts each run of adjacent dead objects on a free list as
   one entry (rather than each object on its own): a heap of dead
   conses then yields blocks big enough to serve as TLABs, allocated
   from without the lock, instead of a lock per cons.  1-word entries
   go to class 0 (no room for a size field); all others store their
   size in word[1] so the allocator can check an entry is big enough.
   The entry keeps its first object's objstart bit, by which
   find_object_start still finds it; it is re-used if the entry is
   allocated, and stays set for the next sweep if not. */
static void
ms_sweep_run_end(ref_t **run_start, size_t *run_len)
{
    ref_t *p = *run_start;
    size_t len = *run_len;

    if (len == 0)
	return;
    {
	int sc = ms_size_class(len);

	p[0] = (ref_t)free_list_heads[sc];
	if (len >= 2)
	    p[1] = (ref_t)len;
	ms_free_set(ms_region_of(p), p);
	free_list_heads[sc] = p;
	free_list_total_words += len;
	if (len >= TLAB_MIN_FREE)
	    tlab_free_none = false;
    }
    *run_start = NULL;
    *run_len = 0;
}


/* ------------------------------------------------------------------ */
/*  Lazy sweep                                                         */
/* ------------------------------------------------------------------ */

/* The sweep goes region by region in SWEEP_BLOCK_SIZE-word blocks,
   each block one call under alloc_lock, between the regions' start
   and their sweep_top; what was allocated above sweep_top (during
   the mark, or since) carries no mark bit and must not be swept.  A
   run of dead objects open at a block's end carries over to the
   next block, so an entry never extends past the cursor and the
   allocator may use any entry on the lists.  A region found with
   nothing live and nothing allocated since the snapshot is reset to
   virgin in one step, with no entries, and a region empty for
   MS_EMPTY_CYCLES sweeps in a row is unmapped when the heap is over
   its budget. */
static ms_region_t *sweep_region;	/* being swept, or NULL */
static size_t sweep_cursor;		/* word index into it */
static ref_t *sweep_run;		/* the open run of dead objects */
static size_t sweep_dead;		/* dead words found in it so far */
static bool   lazy_sweep_active;	/* true when regions remain to sweep */

#define MS_EMPTY_CYCLES 2

void
ms_prepare_lazy_sweep(void)
{
    /* The sweep frees what the mark did not reach, and its memory is
       used again: the global method cache's raw refs go first, and so
       do the native call sites' (they are no roots either). */
    oak_gmc_flush();
    oak_native_flush_sites();

    /* Clear all free lists — they will be rebuilt incrementally
       as blocks are lazily swept -- and their entries' free bits. */
    for (int i = 0; i < NUM_SIZE_CLASSES; i++)
	free_list_heads[i] = NULL;
    free_list_total_words = 0;
    tlab_free_none = true;
    for (ms_region_t *r = ms_regions; r; r = r->next)
	memset(r->freebm, 0, BM_WORDS(r) * sizeof(uint64_t));
}

/* The sweep may begin.  Under alloc_lock, or with the world
   stopped. */
void
ms_start_lazy_sweep(void)
{
    sweep_region = ms_regions;
    sweep_cursor = 0;
    sweep_run = NULL;
    sweep_dead = 0;
    lazy_sweep_active = true;
}

/* Nothing marked below sweep_top, nothing allocated above it. */
static bool
ms_region_all_dead(ms_region_t *r)
{
    size_t n = BM_IDX(r, r->sweep_top), w = n / 64, i;

    if (r->top != r->sweep_top)
	return false;
    for (i = 0; i < w; i++)
	if (r->mark[i])
	    return false;
    if (n % 64 && (r->mark[w] & ((1ULL << (n % 64)) - 1)))
	return false;
    return true;
}

/* All dead: virgin again, in one step. */
static void
ms_region_reset(ms_region_t *r)
{
    size_t bytes = (BM_IDX(r, r->top) + 63) / 64 * sizeof(uint64_t);

    memset(r->objstart, 0, bytes);
    memset(r->mark, 0, bytes);
    memset(r->freebm, 0, bytes);
    r->top = r->sweep_top = r->start;
    r->live_words = 0;
    r->empty_cycles++;
    ms_virgin_grew();
}

/* Unmap empty regions beyond the budget (hysteresis: empty for
   MS_EMPTY_CYCLES sweeps), the biggest first.  No sweep in progress. */
static void
ms_unmap_surplus(void)
{
    while (ms_mapped > (size_t)e_next_newspace_size) {
	ms_region_t *r, *best = NULL;

	for (r = ms_regions; r; r = r->next)
	    if (r->top == r->start && r->empty_cycles >= MS_EMPTY_CYCLES
		&& (best == NULL || r->chunks > best->chunks))
		best = r;
	if (best == NULL)
	    break;
	ms_region_unmap(best);
    }
}

bool
ms_lazy_sweep_one(void)
{
    if (!lazy_sweep_active)
	return false;

    for (;;) {
	ms_region_t *r = sweep_region;
	size_t limit, start, end, start_w, end_w;

	if (r == NULL) {
	    lazy_sweep_active = false;
	    if (trace_gc > 1) {
		size_t live = 0, allocated = 0, empties = 0;

		for (r = ms_regions; r; r = r->next) {
		    live += r->live_words;
		    allocated += (size_t)(r->top - r->start);
		    if (r->top == r->start)
			empties++;
		}
		fprintf(stderr, "; MS-GC: sweep done: %zu live, %zu free on"
			" the lists, %zu allocated in %zu regions, %zu empty.\n",
			live, free_list_total_words, allocated, ms_nregions,
			empties);
	    }
	    ms_unmap_surplus();
	    return false;
	}
	limit = BM_IDX(r, r->sweep_top);
	if (sweep_cursor == 0) {
	    if (limit == 0) {
		/* Nothing from before the cycle: empty then, and still
		   if nothing has been allocated into it since. */
		if (r->top == r->start)
		    r->empty_cycles++;
		else
		    r->empty_cycles = 0;
		sweep_region = r->next;
		continue;
	    }
	    /* Not while the weak pointer table is being cleared: a reset
	       moves sweep_top, which the readers' liveness test uses
	       (ms_weak_alive); the region is swept block by block then,
	       into one entry. */
	    if (!gc_weak_clearing && ms_region_all_dead(r)) {
		ms_region_reset(r);
		sweep_region = r->next;
		return true;
	    }
	    sweep_dead = 0;
	}

	/* One block of R: walk objstart bits in [start, end). */
	start = sweep_cursor;
	end = start + SWEEP_BLOCK_SIZE;
	if (end > limit)
	    end = limit;
	sweep_cursor = end;
	start_w = start / 64;
	end_w = (end + 63) / 64;

	for (size_t w = start_w; w < end_w; w++) {
	    uint64_t os_word = r->objstart[w];
	    if (os_word == 0) continue;

	    while (os_word != 0) {
		int bit = CTZ64(os_word);
		os_word &= os_word - 1;

		size_t idx = w * 64 + bit;
		if (idx < start || idx >= end) continue;

		ref_t *obj = r->start + idx;

		/* An object extends to the next objstart bit (not to
		   what its type says: a naked cell's word[0] can look
		   like a type).  So a run of dead objects extends from
		   its first to the next live one, and only that end is
		   looked for. */
		if (BM_TEST(r->mark, idx)) {
		    /* Live.  The mark bit stays: the next cycle clears
		       them all, and the weak pointer table's readers
		       test them while it is being cleared, which the
		       sweep overlaps. */
		    if (sweep_run) {
			size_t run_len = (size_t)(obj - sweep_run);
			sweep_dead += run_len;
			ms_sweep_run_end(&sweep_run, &run_len);
		    }
		} else if (sweep_run) {
		    BM_CLR(r->objstart, idx);	/* into the run */
		} else {
		    sweep_run = obj;
		}
	    }
	}
	if (end >= limit) {
	    /* The region's end: close the run at sweep_top. */
	    if (sweep_run) {
		size_t run_len = (size_t)(r->sweep_top - sweep_run);
		sweep_dead += run_len;
		ms_sweep_run_end(&sweep_run, &run_len);
	    }
	    r->live_words = limit - sweep_dead;
	    r->empty_cycles = 0;
	    sweep_region = r->next;
	    sweep_cursor = 0;
	}
	tlab_free_none = false;
	return true;
    }
}


void
ms_lazy_sweep_finish(void)
{
    while (ms_lazy_sweep_one())
	;
}


/* ------------------------------------------------------------------ */
/*  Weak pointer handling                                              */
/* ------------------------------------------------------------------ */

/* Whether a weakly held object survived the mark whose marks stand:
   outside the heap, allocated since the snapshot (above its region's
   sweep_top: born black or implicitly live, and not necessarily
   marked), or marked.  Read by the mutators too (ms_weak_read in
   data.h) while the table is being cleared after the remark. */
bool
ms_weak_alive(ref_t ref)
{
    ref_t *p = ANY_TO_PTR(ref);
    ms_region_t *r = ms_region_of(p);

    if (r == NULL)
	return true;
    if (p >= r->sweep_top)
	return true;
    /* For locatives, find the object start. */
    if (TAG_IS(ref, LOC_TAG)) {
	p = find_object_start(r, p);
	if (p == NULL)
	    return false;
    }
    return BM_TEST(r->mark, BM_IDX(r, p)) != 0;
}

/* The weak pointer table after a mark: entries whose objects did not
   survive are cleared.  With threads this runs after the remark, the
   mutators going on (a read of an entry not yet cleared tests the
   mark bit itself, ms_weak_read) and new weak pointers being made
   under wp_lock, which is held throughout. */
static unsigned long
ms_post_gc_wp(void)
{
    unsigned long count = 0;
    long i;

    THREADY(oak_mutex_lock(&wp_lock));
    for (i = 0; i < wp_index; i++) {
	ref_t ref = wp_table[1 + i];

	if ((ref & PTR_MASK) && !ms_weak_alive(ref)) {
	    wp_table[1 + i] = e_nil;
	    count++;
	}
    }
    rebuild_wp_hashtable_locked();
    THREADY(oak_mutex_unlock(&wp_lock));
    return count;
}


/* ------------------------------------------------------------------ */
/*  Main collection entry point                                        */
/* ------------------------------------------------------------------ */

/* A cycle: the mark (stop-the-world in a single-threaded build; with
   threads concurrent, between two pauses, on the GC thread), the weak
   pointers, the budget, and the start of the lazy sweep.  AMOUNT is
   what the request that asked for it needs; with threads the most any
   waiter needs is added to it.  Without threads there is no
   alloc_lock; with threads it is taken here as needed and not held
   on return. */
static void
ms_run_cycle(const char *reason, size_t amount)
{
    long old_used;
    gc_timer_t cycle;
#ifdef THREADS
    gc_timer_t pause;

    gc_stats_begin(&cycle, GC_EV_CYCLE);
    /* Finish any lazy sweep remaining from the previous cycle, a block
       at a time so that the allocator gets in between, and end holding
       the lock.  This must happen before we clear the mark bitmaps. */
    for (;;) {
	oak_mutex_lock(&alloc_lock);
	if (!ms_lazy_sweep_one())
	    break;
	oak_mutex_unlock(&alloc_lock);
    }
#else
    gc_stats_begin(&cycle, GC_EV_MARK_SWEEP);
    ms_lazy_sweep_finish();
#endif

    old_used = (long)ms_allocated_words() - (long)free_list_total_words;

    if (trace_gc == 1)
	fprintf(stderr, "\n;MS-GC");
    if (trace_gc > 1)
	fprintf(stderr, "\n; Mark-sweep GC due to %s.\n", reason);

    /* The cycle's snapshot: every region's sweep_top is its top, and
       its marks are cleared.  Objects allocated above sweep_top during
       the mark are implicitly live and will not be swept.

       With threads, both this snapshot and the TLAB retirement happen
       inside the initial-mark pause below instead of here: the TLAB
       fast path in ALLOCATE_PROT deliberately takes no lock, so doing
       either while the other mutators are still running races them --
       the collector can zero a TLAB tail an instant after a mutator
       has handed out an object in it, leaving a live object whose type
       slot reads as zero. */
#ifndef THREADS
    ms_tlab_retire_all();
    ms_snapshot_tops();
#endif

    /* --- Mark phase --- */
    if (trace_gc > 1) {
	fprintf(stderr, "; Marking... [heap=%zu words in %zu regions,"
		" used=%ld, free_list=%zu]",
		ms_mapped, ms_nregions, old_used + (long)free_list_total_words,
		free_list_total_words);
	fflush(stderr);
    }

    ms_clear_marks();
    mark_stack_init();
    marked_words = 0;
#if defined(THREADS) && defined(OAK_GC_VERIFY)
    ms_verify_cycle_begin();
#endif

    /* Disable crash recovery during GC so that any SIGSEGV from bad
       pointers results in an immediate abort rather than longjmping
       back to the interpreter loop (which would re-trigger GC).
       crash_recovery_installed is thread-local, so this only covers
       the collecting thread. */
    int saved_crash_recovery = crash_recovery_installed;
    crash_recovery_installed = 0;

#ifdef THREADS
    {
	/* === STW PAUSE 1: Initial mark (scan roots) === */
	gc_phase = GC_PHASE_INITIAL_MARK;

	/* Release alloc_lock so blocked threads can see gc_pending
	   in their spin loops and park via wait_for_gc(). */
	oak_mutex_unlock(&alloc_lock);

	gc_stats_begin(&pause, GC_EV_INITIAL_MARK);
	(void)stw_request_pause(MS_GC_THREAD_INDEX);
#ifdef OAK_GC_VERIFY
	ms_verify_stw_owner = MS_GC_THREAD_INDEX;
#endif

	if (trace_gc > 1)
	    fprintf(stderr, "\n; STW initial mark...");

	/* The world is stopped, so no mutator can be inside the TLAB
	   fast path.  Retire the TLABs and take the snapshot now. */
	ms_tlab_retire_all();
	ms_snapshot_tops();

	/* The allocation rate, for the pacing: what was handed out since
	   the last snapshot, over the time since; smoothed, half this
	   measure and half the last. */
	{
	    gc_ns_t now = gc_now();
	    double ms = ms_snapshot_ns ? (double)(now - ms_snapshot_ns) / 1e6 : 0;
	    double rate = ms > 0.01 ? (double)ms_since_snapshot / ms : 0;

	    if (rate > 0)
		ms_alloc_rate = ms_alloc_rate > 0 ? (ms_alloc_rate + rate) / 2 : rate;
	    if (trace_gc > 1)
		fprintf(stderr, "\n; %zu words allocated in the %.1f ms since the"
			" last snapshot (%.0f/ms; smoothed %.0f/ms).",
			ms_since_snapshot, ms, rate, ms_alloc_rate);
	    ms_since_snapshot = 0;
	    ms_snapshot_ns = now;
	}

	ms_mark_thread_roots();
#ifdef OAK_GC_VERIFY
	/* Trace the whole snapshot now into a copy, then start the
	   real mark over from the roots. */
	ms_mark_spatic();
	ms_trace();
	ms_verify_take_snapshot();
	ms_mark_thread_roots();
#endif
	if (trace_gc > 1) { fprintf(stderr, " roots"); fflush(stderr); }

	/* Activate SATB barriers before releasing mutators.
	   This ensures every heap write during concurrent mark
	   logs the old value for the remark phase. */
	gc_satb_active = 1;
	gc_phase = GC_PHASE_CONCURRENT_MARK;

	/* === RELEASE: mutators resume during concurrent mark === */
#ifdef OAK_GC_VERIFY
	ms_verify_stw_owner = MS_NO_STW_OWNER;
#endif
	/* The pause ends with the release: timed before it, since once
	   the mutators are let go this thread may not run again for a
	   scheduler's slice (on one core it never does). */
	{
	    double ms = gc_stats_end(&pause, GC_EV_INITIAL_MARK, reason);
	    stw_release(MS_GC_THREAD_INDEX);
	    if (trace_gc > 1) {
		fprintf(stderr, " %.3f ms.\n; Concurrent tracing...", ms);
		fflush(stderr);
	    }
	}

	/* === CONCURRENT MARK: spatic, then everything reachable === */
	ms_mark_spatic_concurrent();
	ms_trace_concurrent();

	if (trace_gc > 1) {
	    fprintf(stderr, " done.\n; STW remark...");
	    fflush(stderr);
	}

	/* === STW PAUSE 2: Remark === */
	gc_phase = GC_PHASE_REMARK;
	gc_stats_begin(&pause, GC_EV_REMARK);
	(void)stw_request_pause(MS_GC_THREAD_INDEX);
#ifdef OAK_GC_VERIFY
	ms_verify_stw_owner = MS_GC_THREAD_INDEX;
#endif

	/* Retire TLABs allocated during concurrent mark. */
	ms_tlab_retire_all();

	/* What the mutators allocated meanwhile, for the pacing: what
	   the allocator handed out since the snapshot.  Smoothed: half
	   this cycle's, half what it was. */
	{
	    size_t during = ms_since_snapshot;

	    ms_alloc_during_mark = ms_alloc_during_mark
		? (ms_alloc_during_mark + during) / 2 : during;
	    if (trace_gc > 1)
		fprintf(stderr, "\n; %zu words allocated in the mark's %.2f ms"
			" (smoothed %zu).", during,
			(double)(gc_now() - ms_snapshot_ns) / 1e6,
			ms_alloc_during_mark);
	}

	/* Drain SATB buffers — these contain old pointer values
	   that were overwritten during concurrent mark and must
	   be traced to maintain the snapshot-at-beginning invariant. */
	{
	    extern int next_index;
	    gc_ns_t t0 = trace_gc > 2 ? gc_now() : 0, t1, t2;
	    size_t entries = 0;

	    for (int i = 0; i < next_index; i++)
		entries += satb_count_array[i];
	    satb_drain_to_mark_stack();
	    t1 = trace_gc > 2 ? gc_now() : 0;

	    /* Re-scan the registers and stacks, C arrays that no barrier
	       covers.  Spatic is not rescanned: stores into it are
	       barriered like any.  What was allocated during the mark
	       is black and is not scanned: every object it can refer to
	       was reachable at the snapshot (and is marked by the
	       snapshot's rules), was allocated after it (black), or was
	       read through a weak pointer (and logged, SATB_READ_WEAK). */
	    ms_mark_thread_roots();
	    t2 = trace_gc > 2 ? gc_now() : 0;

	    /* Process all newly discovered references. */
	    ms_trace();
	    if (trace_gc > 2)
		fprintf(stderr, "\n; Remark: %zu SATB entries %.3f ms, roots"
			" %.3f ms, trace %.3f ms.", entries,
			(t1 - t0) / 1e6, (t2 - t1) / 1e6, (gc_now() - t2) / 1e6);
	}

#ifdef OAK_GC_VERIFY
	ms_verify_marks();
#endif

	if (trace_gc > 1) { fprintf(stderr, " done."); fflush(stderr); }

	/* Deactivate SATB barriers. */
	gc_satb_active = 0;
	satb_flush_all();
    }
#else
    /* Single-threaded: STW mark. */
    ms_mark_roots();
    if (trace_gc > 1) { fprintf(stderr, " roots"); fflush(stderr); }
    ms_trace();
#endif

    crash_recovery_installed = saved_crash_recovery;

    if (trace_gc > 1)
	fprintf(stderr, "\n; Mark complete.\n");

#ifndef THREADS
    /* --- Weak pointers (with threads: after the pause, below) --- */
    if (trace_gc > 1)
	fprintf(stderr, "; Scanning weak pointer table...");
    {
	unsigned long wp_count = ms_post_gc_wp();
	if (trace_gc > 1)
	    fprintf(stderr, " %lu entr%s discarded.\n",
		    wp_count, wp_count != 1 ? "ies" : "y");
    }
#endif

    /* --- Sweep phase --- */
    gc_phase = GC_PHASE_SWEEP;

    /* Free the mark stack before sweeping — it's no longer needed. */
    free(mark_stack);
    mark_stack = NULL;
    mark_stack_size = 0;

#ifdef THREADS
    /* The waiters' needs, and the eager sweep for them below. */
    oak_mutex_lock(&gc_concurrent_lock);
    if (gc_request_amount > amount)
	amount = gc_request_amount;
    gc_request_amount = 0;
    oak_mutex_unlock(&gc_concurrent_lock);
#endif

    /* The budget for the next cycle, from what the mark found. */
    ms_set_budget(amount);

    /* Prepare the lazy sweep: clear the free lists and their free
       bits, flush the caches of raw refs.  Blocks will be swept on
       demand by allocating threads (ms_lazy_sweep_one) once it is
       started, below. */
    ms_prepare_lazy_sweep();

    if (trace_gc > 1)
	fprintf(stderr, "; Lazy sweep prepared (%zu regions).\n", ms_nregions);

#ifdef THREADS
    /* The weak pointers are cleared after the pause: until then a
       read of one tests the mark bit (ms_weak_read, data.h).  The
       sweep starts now all the same, so the mutators find room
       meanwhile: it leaves the mark bits alone, and resets no region
       while the clearing lasts. */
    gc_weak_clearing = 1;
    ms_start_lazy_sweep();
#endif
    gc_phase = GC_PHASE_IDLE;

#ifdef THREADS
    /* Release all threads from the remark STW pause (timed before the
       release, as above). */
#ifdef OAK_GC_VERIFY
    ms_verify_stw_owner = MS_NO_STW_OWNER;
#endif
    {
	double ms = gc_stats_end(&pause, GC_EV_REMARK, reason);
	stw_release(MS_GC_THREAD_INDEX);
	if (trace_gc > 1)
	    fprintf(stderr, "; Remark pause %.3f ms.\n", ms);
    }

    /* --- Weak pointers, concurrently --- */
    if (trace_gc > 1)
	fprintf(stderr, "; Scanning weak pointer table...");
    {
	unsigned long wp_count = ms_post_gc_wp();
	if (trace_gc > 1)
	    fprintf(stderr, " %lu entr%s discarded.\n",
		    wp_count, wp_count != 1 ? "ies" : "y");
    }
    gc_weak_clearing = 0;
#endif

    /* Sweep enough blocks now to satisfy the requested amount, so that
       the retry of whoever asked finds room (or the budget's).  With
       threads this is after the pause, the lock taken a block at a
       time: a big request could have the whole heap swept here. */
    {
#ifdef THREADS
	oak_mutex_lock(&alloc_lock);
	while (!ms_space_for(amount) && ms_lazy_sweep_one()) {
	    oak_mutex_unlock(&alloc_lock);
	    oak_mutex_lock(&alloc_lock);
	}
	oak_mutex_unlock(&alloc_lock);
#else
	ms_start_lazy_sweep();
	while (!ms_space_for(amount) && ms_lazy_sweep_one()) {
	    /* keep sweeping */
	}
#endif
	if (trace_gc > 1 && free_list_total_words > 0)
	    fprintf(stderr, "; Eagerly swept %zu words.\n",
		    free_list_total_words);
    }

    if (trace_gc == 1)
	fprintf(stderr, ":%ld%%",
		old_used > 0
		? (100 * (long)free_list_total_words) / old_used : 0);
    if (trace_gc > 1)
	fprintf(stderr, "; MS-GC complete.  %zu free on the lists,"
		" %zu words mapped in %zu regions, budget %zu,"
		" headroom %zu.\n",
		free_list_total_words, ms_mapped, ms_nregions,
		(size_t)e_next_newspace_size, ms_headroom);
    if (trace_gc == 1)
	fprintf(stderr, "\n");

#ifdef THREADS
    gc_stats_end(&cycle, GC_EV_CYCLE, reason);
#else
    {
	double ms = gc_stats_end(&cycle, GC_EV_MARK_SWEEP, reason);
	if (trace_gc > 1)
	    fprintf(stderr, "; MS-GC pause %.3f ms.\n", ms);
    }
#endif
}


#ifdef THREADS
/* The GC thread: a cycle for every request, when none is held off. */
static void *
ms_gc_thread_main(void *arg)
{
    int *idx = (int *)xmalloc(sizeof(int));

    (void)arg;
    *idx = MS_GC_THREAD_INDEX;
    oak_tls_set(index_key, idx);
    oak_thread_index_var = MS_GC_THREAD_INDEX;
    oak_mutex_lock(&gc_concurrent_lock);
    for (;;) {
	const char *reason;

	while (!gc_requested || gc_hold)
	    oak_cond_wait(&gc_request_cv, &gc_concurrent_lock);
	gc_requested = 0;
	reason = gc_request_reason;
	gc_concurrent_in_progress = 1;
	oak_mutex_unlock(&gc_concurrent_lock);

	ms_run_cycle(reason, 0);

	oak_mutex_lock(&gc_concurrent_lock);
	gc_concurrent_in_progress = 0;
	gc_cycles_done++;
	oak_cond_broadcast(&gc_concurrent_done_cv);
    }
    return NULL;
}
#endif


/* A collection for a request of AMOUNT words: with threads, wait for
   the GC thread's next cycle to end (asking for one if none runs);
   without, run one here.  The caller holds alloc_lock where there is
   one, and has synchronized its state. */
void
ms_collect(bool pre_dump, char *reason, size_t amount)
{
    /* For world dumps, use the copying GC which produces a contiguous
       heap suitable for serialization; it runs its own stop-the-world
       handshake. */
    if (pre_dump) {
	gc(pre_dump, true, reason, amount);
	return;
    }
#ifdef THREADS
    ms_wait_cycle(reason, amount);
#else
    ms_run_cycle(reason, amount);
#endif
}


/* The GC and FULL-GC instructions: a collection asked for by the
   program: a cycle run here, or with threads one of the GC thread's,
   waited for; the copying collector is not used (in a threads build it
   would run under the marker, and in both it would copy the heap). */
void
ms_collect_explicit(const char *reason)
{
#ifdef THREADS
    while (oak_mutex_trylock(&alloc_lock) != 0) {
	if (gc_pending)
	    wait_for_gc();
	else
	    oak_thread_yield();
    }
#endif
    ms_collect(false, (char *)reason, 0);
#ifdef THREADS
    oak_mutex_unlock(&alloc_lock);
#endif
}


/* ------------------------------------------------------------------ */
/*  Initialization, and the dump compaction's hooks                    */
/* ------------------------------------------------------------------ */

/* Everything that referred into the regions: the TLABs, the free
   lists, the sweep, the born list. */
static void
ms_forget_heap(void)
{
    for (int i = 0; i < MS_TLAB_COUNT; i++) {
	tlab_cursor_array[i] = NULL;
	tlab_end_array[i] = NULL;
	tlab_base_array[i] = NULL;
	tlab_bm_array[i] = NULL;
    }
    for (int i = 0; i < NUM_SIZE_CLASSES; i++)
	free_list_heads[i] = NULL;
    free_list_total_words = 0;
    tlab_free_none = true;
    lazy_sweep_active = false;
    sweep_region = NULL;
    sweep_cursor = 0;
    sweep_run = NULL;
    alloc_region = NULL;
}

static void
ms_unmap_all(void)
{
    while (ms_regions)
	ms_region_unmap(ms_regions);
    ms_forget_heap();
}

/* LOAD-WORLD: the heap goes, with the GC thread held off until
   ms_init makes the new one. */
void
ms_destroy(void)
{
#ifdef THREADS
    ms_gc_hold();
#endif
    ms_unmap_all();
}

void
ms_init(void)
{
    ms_unmap_all();
    e_next_newspace_size = original_newspace_size;
    ms_alloc_during_mark = 0;
    ms_set_pacing();
#ifdef THREADS
    ms_gc_release();
#endif
}

void
ms_evacuate_begin(bool full)
{
    size_t need = ms_mapped + (full ? spatic.size : 0), chunks;
    void *mem;

    if (need == 0)
	need = 1;
    chunks = (need + MS_REGION_WORDS - 1) / MS_REGION_WORDS;
    /* The world is stopped: the TLABs' tails are zeroed, and the
       copying collector reads objects by their types. */
    ms_tlab_retire_all();
    mem = ms_map_aligned(chunks * MS_REGION_BYTES);
    if (mem == NULL)
	ms_fatal("cannot map the dump compaction's destination",
		 chunks * MS_REGION_WORDS);
#if MS_ADDR_BITS < OAK_WORD_SIZE
    if ((uintptr_t)mem >> MS_ADDR_BITS)
	ms_fatal("a region was mapped beyond the address range the"
		 " lookup table covers", chunks * MS_REGION_WORDS);
#endif
    new_space.start = (ref_t *)mem;
    new_space.size = chunks * MS_REGION_WORDS;
    new_space.end = new_space.start + new_space.size;
    free_point = new_space.start;
    evac_chunks = chunks;
    deferred_count = 0;
}

void
ms_evacuate_end(void)
{
    ms_region_t *r;
    size_t want;

    /* The old regions go; the destination span is the heap, one
       region, with the objstart bits of what was transported into
       it. */
    ms_unmap_all();
    r = ms_region_new(new_space.start, evac_chunks);
    r->top = r->sweep_top = free_point;
    for (size_t i = 0; i < deferred_count; i++) {
	ref_t *p = deferred_objstarts[i];
	if (p >= r->start && p < r->top)
	    BM_SET(r->objstart, BM_IDX(r, p));
    }
    free(deferred_objstarts);
    deferred_objstarts = NULL;
    deferred_count = 0;
    deferred_cap = 0;
    alloc_region = r;

    /* The budget, from a perfect count of the live data. */
    want = MS_RECLAIM_FACTOR * (size_t)(r->top - r->start);
    if (want < original_newspace_size)
	want = original_newspace_size;
#ifdef MAX_NEW_SPACE_SIZE
    if (want > MAX_NEW_SPACE_SIZE)
	want = MAX_NEW_SPACE_SIZE;
#endif
    e_next_newspace_size = want;
    ms_set_pacing();
    if (trace_gc > 1)
	fprintf(stderr, "; MS-GC: the heap is the compacted span: %zu words"
		" live in %zu mapped, budget %zu.\n",
		(size_t)(r->top - r->start), ms_mapped, want);
}
