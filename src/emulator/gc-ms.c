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
 * Design:
 *   - Heap is the existing contiguous new_space.
 *   - Two bitmaps: object-start (1 bit per ref_t word) and mark.
 *   - Allocation: segregated free lists by size class, with
 *     bump-pointer (free_point) fallback for virgin space.
 *   - Collection: stop-the-world mark from roots, sweep to rebuild
 *     free lists.
 *   - The copying GC (gc.c) is retained for pre-dump compaction.
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


/* ------------------------------------------------------------------ */
/*  Bitmaps                                                            */
/* ------------------------------------------------------------------ */

uint64_t *objstart_bitmap;          /* 1 bit per ref_t word in new_space */
static uint64_t *mark_bitmap;       /* 1 bit per ref_t word in new_space */
static size_t bitmap_words;         /* number of uint64_t in each bitmap */

#define BM_IDX(p)    ((size_t)((p) - new_space.start))
#define BM_SET(bm, i)   ((bm)[(i)/64] |=  (1ULL << ((i) % 64)))
#define BM_CLR(bm, i)   ((bm)[(i)/64] &= ~(1ULL << ((i) % 64)))
#define BM_TEST(bm, i)  ((bm)[(i)/64] &   (1ULL << ((i) % 64)))

/* Objstart bits are set by the locked slow paths and the collector in
   words they may share, so those updates are atomic with threads; the
   TLAB fast path on every thread sets them with no lock, in words
   that are the TLAB's alone (TLAB_ALIGN), with a plain store. */
#ifdef THREADS
#define OBJSTART_SET(i) \
    ((void)OAK_ATOMIC_FETCH_OR_U64(&objstart_bitmap[(i)/64], 1ULL << ((i) % 64)))
#else
#define OBJSTART_SET(i) BM_SET(objstart_bitmap, (i))
#endif


/* ------------------------------------------------------------------ */
/*  Size classes                                                       */
/* ------------------------------------------------------------------ */

/*
 * Class   Max words
 *   0       1
 *   1       2
 *   2       3    (cons pairs)
 *   3       4
 *   4       6
 *   5       8    (lambda = 8 words)
 *   6      12
 *   7      16
 *   8      24
 *   9      32
 *  10      48
 *  11      64
 *  12      96
 *  13     128
 *  14     192
 *  15     256
 *  16     512
 *  17    1024
 *  18    inf   (large objects)
 */

static const size_t class_max[NUM_SIZE_CLASSES] = {
    1, 2, 3, 4, 6, 8, 12, 16, 24, 32,
    48, 64, 96, 128, 192, 256, 512, 1024,
    (size_t)-1   /* large: no upper bound */
};

int
ms_size_class(size_t n)
{
    /* Find smallest class whose max >= n.
       Linear scan is fine for 19 classes. */
    for (int i = 0; i < NUM_SIZE_CLASSES; i++)
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
 *   The entry size is the original object's size (known from objstart bitmap).
 *
 * free_list_heads[sc] points to the first entry in class sc, or NULL.
 */

static ref_t *free_list_heads[NUM_SIZE_CLASSES];

/* Total words on free lists (for statistics / GC triggering). */
static size_t free_list_total_words;

/* Words of the objects the mark traced this cycle: the live data, less
   what was born black meanwhile.  The heap grows when they crowd it
   (ms_crowded, the copying collector's own rule). */
static size_t marked_words;
#define MS_RECLAIM_FACTOR 3

/* The lazy sweep's state (defined with it, below). */
static size_t lazy_sweep_cursor;
static bool   lazy_sweep_active;

/* A run of dead words onto its free list (the sweeps, below). */
static void ms_sweep_run_end(ref_t **run_start, size_t *run_len);

/* Words from OBJ to the next objstart bit, or to word LIMIT. */
static size_t
ms_objstart_extent_to(ref_t *obj, size_t limit)
{
    size_t idx = BM_IDX(obj);
    size_t next = idx + 1;

    while (next < limit && !BM_TEST(objstart_bitmap, next))
	next++;
    return (next > limit ? limit : next) - idx;
}
#define TLAB_MIN_FREE 257	/* a free run this big can be a TLAB */
static bool tlab_free_none;


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
	    /* Validate actual_size to guard against corrupt free list entries. */
	    if (actual_size == 0 || actual_size > new_space.size ||
		p + actual_size > new_space.end) {
		prev = (ref_t **)p;
		p = (ref_t *)(*p);
		continue;
	    }
	    if (actual_size >= n_words) {
		*prev = (ref_t *)(*p);
		OBJSTART_SET(BM_IDX(p));
		free_list_total_words -= actual_size;

		/* Born-black: during concurrent mark, objects allocated
		   from the free list (below sweep_limit) must have their
		   mark bit set so sweep doesn't reclaim them. */
		if (OAK_UNLIKELY(
			gc_phase >= GC_PHASE_CONCURRENT_MARK))
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
       however much else is free.  An entry can run on past the lazy
       sweep's cursor into a block not yet swept, and a rest there
       would get an objstart bit the sweep would then put on a list a
       second time, so the rest has to lie behind the cursor (or no
       sweep be under way).  Each cycle starts the lists afresh. */
    for (int c = sc + 1; c < NUM_SIZE_CLASSES; c++) {
	ref_t **prev = &free_list_heads[c];
	ref_t *p = *prev;

	while (p) {
	    size_t actual_size = (size_t)p[1];

	    if (actual_size == 0 || actual_size > new_space.size ||
		p + actual_size > new_space.end) {
		prev = (ref_t **)p;
		p = (ref_t *)(*p);
		continue;
	    }
	    if (actual_size > n_words
		&& (!lazy_sweep_active
		    || BM_IDX(p + actual_size) <= lazy_sweep_cursor)) {
		ref_t *rest = p + n_words;
		size_t rest_size = actual_size - n_words;
		int rc = ms_size_class(rest_size);

		*prev = (ref_t *)(*p);
		OBJSTART_SET(BM_IDX(p));
		OBJSTART_SET(BM_IDX(rest));
		rest[0] = (ref_t)free_list_heads[rc];
		if (rest_size >= 2)
		    rest[1] = (ref_t)rest_size;
		free_list_heads[rc] = rest;
		free_list_total_words -= n_words;
		if (rest_size >= TLAB_MIN_FREE)
		    tlab_free_none = false;

		if (OAK_UNLIKELY(gc_phase >= GC_PHASE_CONCURRENT_MARK))
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


/* After a cycle: is the live data crowding the heap?  The copying
   collector's rule (gc.c, RECLAIM_FACTOR): grow when it takes more
   than a third.  Without this the heap never grew unless a request
   went unmet, and a program with a few hundred K words live cycled
   many times a second, each cycle scanning spatic and sweeping the
   whole heap.  True sets the next size (a copying collection into it
   follows, which also compacts). */
static bool
ms_crowded(size_t amount)
{
    size_t want = MS_RECLAIM_FACTOR * marked_words + amount;

#ifdef MAX_NEW_SPACE_SIZE
    if (want > MAX_NEW_SPACE_SIZE)
	want = MAX_NEW_SPACE_SIZE;
#endif
    if (want <= new_space.size)
	return false;
    if (trace_gc > 1)
	fprintf(stderr, "; MS-GC: %zu words live in a heap of %zu: "
		"growing to %zu.\n", marked_words, new_space.size, want);
    original_newspace_size = want;
    e_next_newspace_size = want;
    return true;
}

/* Whether a request of N words can be met without collecting: from
   the bump region, or by a free-list entry (of its class or a larger
   one, see the split above) big enough for it.  Total free words
   say nothing about that: the lists could hold any amount in pieces
   all smaller than N. */
static bool
ms_space_for(size_t n_words)
{
    int sc = ms_size_class(n_words);

    /* The same tests as the allocator's, exactly: a request this says
       fits must then be met, or the caller would collect for ever. */
    if (n_words < (size_t)(new_space.end - free_point))
	return true;
    for (int c = sc; c < NUM_SIZE_CLASSES; c++) {
	ref_t *p = free_list_heads[c];
	while (p) {
	    size_t actual_size = (c == 0) ? 1 : (size_t)p[1];
	    if (actual_size == 0 || actual_size > new_space.size ||
		p + actual_size > new_space.end) {
		p = (ref_t *)(*p);
		continue;
	    }
	    if (c == sc
		? actual_size >= n_words
		: (actual_size > n_words
		   && (!lazy_sweep_active
		       || BM_IDX(p + actual_size) <= lazy_sweep_cursor)))
		return true;
	    p = (ref_t *)(*p);
	}
    }
    return false;
}

void
ms_bump_alloc_notify(ref_t *p, size_t n_words)
{
    OBJSTART_SET(BM_IDX(p));
    (void)n_words;
}


/* The TLAB fast path's objstart bit is ms_bump_alloc_notify_tlab in
   gc-ms.h: a plain store, the bitmap word being the TLAB's own
   (TLAB_ALIGN). */


/* ------------------------------------------------------------------ */
/*  TLABs (thread-local allocation buffers)                            */
/* ------------------------------------------------------------------ */

/* With threads a TLAB per thread; without, the one at index 0 is the
   allocator's bump cursor, so that ALLOCATE's fast path is the same
   few instructions in both builds (data.h). */
#include "threads.h"

ref_t *tlab_cursor_array[MS_TLAB_COUNT];
ref_t *tlab_end_array[MS_TLAB_COUNT];

/* A TLAB begins and ends on a bitmap word's boundary (64 words), so
   its objstart bits are in words no other thread touches, and the
   fast path sets them with a plain store (ms_bump_alloc_notify) --
   the atomic OBJSTART_SET is for the shared words the slow paths and
   the collector write.  The words skipped to align a start are zeroed
   and given an objstart bit: a dead filler for the next sweep. */
#define TLAB_ALIGN 64

static ref_t *
ms_tlab_align_up(ref_t *p)
{
    size_t off = (size_t)(p - new_space.start) % TLAB_ALIGN;

    return off == 0 ? p : p + (TLAB_ALIGN - off);
}

static void
ms_tlab_filler(ref_t *p, size_t words)
{
    if (words == 0)
	return;
    memset(p, 0, words * sizeof(ref_t));
    OBJSTART_SET(BM_IDX(p));
}

/*
 * Carve a TLAB from the global bump pointer.
 * Must be called with alloc_lock held.
 * Returns the start of the new TLAB, or NULL if insufficient space.
 * Sets *out_end to one past the last word of the TLAB.
 */
static ref_t *
ms_tlab_refill(size_t min_words, ref_t **out_end)
{
    size_t tlab_words = TLAB_SIZE;
    ref_t *start = ms_tlab_align_up(free_point);
    size_t gap = (size_t)(start - free_point);

    if (tlab_words < min_words)
	tlab_words = (min_words + TLAB_ALIGN - 1) / TLAB_ALIGN * TLAB_ALIGN;

    if (gap + tlab_words >= (size_t)(new_space.end - free_point))
	return NULL;

    ms_tlab_filler(free_point, gap);
    free_point = start + tlab_words;
    *out_end = free_point;
    return start;
}

/* A TLAB out of a free-list entry: the biggest-class entry of at
   least TLAB_MIN_FREE words behind the lazy sweep's cursor (an entry
   can run on into an unswept block, whose objects the sweep would
   then free), trimmed to aligned edges, the trimmings going back on
   the lists.  Not during a concurrent mark: objects allocated below
   sweep_limit then must be born black, which the fast path does not
   do (and the cycle began by retiring every TLAB; refills come from
   the bump region, above sweep_limit, until it ends). */
/* No free run served last time, and none has been added since: the
   search would find nothing again (cleared by the sweeps and the
   split).  TLAB_MIN_FREE is the least size of every entry in the
   classes searched. */
static bool tlab_free_none = true;

static ref_t *
ms_tlab_from_free(size_t min_words, ref_t **out_end)
{
    size_t need = min_words + 2 * TLAB_ALIGN;

    if (need < TLAB_MIN_FREE)
	need = TLAB_MIN_FREE;
    if (gc_phase >= GC_PHASE_CONCURRENT_MARK || tlab_free_none)
	return NULL;
    for (int c = NUM_SIZE_CLASSES - 1; c >= 0 && class_max[c] >= need; c--) {
	ref_t **prev = &free_list_heads[c];
	ref_t *p = *prev;

	while (p) {
	    size_t actual_size = (size_t)p[1];

	    if (actual_size == 0 || actual_size > new_space.size ||
		p + actual_size > new_space.end) {
		prev = (ref_t **)p;
		p = (ref_t *)(*p);
		continue;
	    }
	    if (actual_size >= need
		&& (!lazy_sweep_active
		    || BM_IDX(p + actual_size) <= lazy_sweep_cursor)) {
		ref_t *start = ms_tlab_align_up(p);
		ref_t *end = p + actual_size;
		size_t tail = (size_t)(end - new_space.start) % TLAB_ALIGN;
		size_t head = (size_t)(start - p);

		*prev = (ref_t *)(*p);
		free_list_total_words -= actual_size;
		end -= tail;
		if (head != 0) {
		    size_t len = head;
		    ms_sweep_run_end(&p, &len);	/* keeps p's objstart bit */
		}
		if (tail != 0) {
		    ref_t *t = end;
		    size_t len = tail;

		    ms_tlab_filler(t, 0);
		    OBJSTART_SET(BM_IDX(t));
		    ms_sweep_run_end(&t, &len);
		}
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


ref_t *
ms_alloc_slow_locked(size_t n_words,
		     ref_t **tlab_cursor_p, ref_t **tlab_end_p)
{
    ref_t *p;

    /* 1. A small request wants a TLAB, so that the ones after it take
       no lock: out of a big free entry, sweeping for one if need be,
       else out of the bump region. */
    if (n_words <= TLAB_SIZE) {
	ref_t *new_end, *new_start;

	for (;;) {
	    new_start = ms_tlab_from_free(n_words, &new_end);
	    if (new_start || !ms_lazy_sweep_one())
		break;
	}
	if (!new_start)
	    new_start = ms_tlab_refill(n_words, &new_end);
	if (new_start) {
	    *tlab_cursor_p = new_start + n_words;
	    *tlab_end_p = new_end;
	    ms_bump_alloc_notify(new_start, n_words);
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

    /* 3. Try global bump pointer (large allocs or TLAB refill failed). */
    if (n_words < (size_t)(new_space.end - free_point)) {
	p = free_point;
	free_point += n_words;
	ms_bump_alloc_notify(p, n_words);
	return p;
    }

    return NULL;  /* GC needed */
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
    p = ms_alloc_slow_locked(words, &tlab_cursor_array[idx],
			     &tlab_end_array[idx]);
    /* ms_collect either runs a cycle as the coordinator -- and when
       that leaves no room for the request, the copying collector,
       growing the heap -- or, with threads, when another thread's
       cycle is under way, waits for it; after a wait the retry can
       still fail, so go round again.  The copying collector is never
       called from here after a wait: run by a thread that is not the
       coordinator while a cycle is in progress, it would skip its
       handshake and move objects under the marker. */
    while (!p) {
	ms_collect(false, reason, words);
	p = ms_alloc_slow_locked(words, &tlab_cursor_array[idx],
				 &tlab_end_array[idx]);
#ifdef THREADS
	if (!p && ms_collect_was_coordinator()) {
#else
	if (!p) {
#endif
	    /* Our own cycle, and still nothing big enough: the copying
	       collector, which grows the heap.  Safe here, unlike after
	       a wait: a cycle needs alloc_lock to start, and this thread
	       holds it, so none is under way. */
	    gc(false, true, reason, words);
	    p = free_point;
	    free_point += words;
	    ms_bump_alloc_notify(p, words);
	}
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
    if (!cursor || cursor >= end)
	return;

    size_t remaining = (size_t)(end - cursor);

    /* Zero the unused region so it doesn't confuse the mark phase
       (zeroed words are fixnum 0, not pointers). */
    memset(cursor, 0, remaining * sizeof(ref_t));

    /* Set an objstart bit so the sweep can find and reclaim it. */
    OBJSTART_SET(BM_IDX(cursor));
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
    }
}


/* ------------------------------------------------------------------ */
/*  SATB write barrier buffers                                         */
/* ------------------------------------------------------------------ */

int gc_satb_active = 0;

/*
 * The buffers grow on demand.  Dropping an entry would break the
 * snapshot-at-the-beginning invariant: an object whose last remaining
 * reference was overwritten during concurrent mark would never be
 * marked and would then be swept while still live.  A mutator can
 * easily overwrite more than SATB_BUFFER_SIZE pointer slots in one
 * mark phase, so the buffer has to be able to hold them all.
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

void
satb_log_old(ref_t *addr)
{
    ref_t old_val = *addr;

    /* Only log pointer-tagged values (skip fixnums and immediates). */
    if (!(old_val & PTR_MASK))
	return;

#ifdef THREADS
    {
	int idx = *(int *)oak_tls_get(index_key);
	satb_room(&satb_buffer_array[idx], satb_count_array[idx],
		  &satb_size_array[idx]);
	satb_buffer_array[idx][satb_count_array[idx]++] = old_val;
    }
#else
    satb_room(&satb_buffer, satb_count, &satb_size);
    satb_buffer[satb_count++] = old_val;
#endif
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
/*  Concurrent marking state (Phase 4)                                 */
/* ------------------------------------------------------------------ */

volatile int gc_phase = GC_PHASE_IDLE;

/* Sweep limit: only sweep objects that existed before the mark phase
   started.  Objects allocated during concurrent mark (above this
   address) are implicitly live and will not be swept. */
static ref_t *sweep_limit;

#ifdef THREADS
volatile int gc_concurrent_in_progress = 0;

/* Whether the calling thread's last ms_collect ran the cycle itself
   (as opposed to waiting for another thread's).  Per thread: a single
   flag let a thread that had only waited see the one another thread's
   cycle had set, and run the copying collector under that cycle's
   mark. */
static bool ms_collect_coordinated[MAX_THREAD_COUNT];

bool
ms_collect_was_coordinator(void)
{
    return ms_collect_coordinated[*(int *)oak_tls_get(index_key)];
}

static oak_mutex_t gc_concurrent_lock = OAK_MUTEX_INITIALIZER;
static oak_cond_t  gc_concurrent_done_cv = OAK_COND_INITIALIZER;

void
gc_safepoint_init(void)
{
#ifdef OAK_NEEDS_DYNAMIC_MUTEX_INIT
    oak_mutex_init(&gc_concurrent_lock);
    oak_cond_init(&gc_concurrent_done_cv);
#endif
}

void
ms_wait_concurrent_gc(void)
{
    oak_mutex_lock(&gc_concurrent_lock);
    while (gc_concurrent_in_progress)
	oak_cond_wait(&gc_concurrent_done_cv, &gc_concurrent_lock);
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
    gc_ready[my_index_val] = 0;
    set_gc_flag(false);   /* gc_pending=false, unlocks gc_lock */
}

#endif /* THREADS */


#if defined(THREADS) && defined(OAK_GC_VERIFY)
/* Verify mode: the objects born black this cycle. */
static uint64_t *ms_verify_born;
/* Verify mode: everything reachable at the snapshot (initial mark). */
static uint64_t *ms_verify_snap;
#endif

/* Set mark bit for born-black allocation during concurrent mark. */
void
ms_mark_alloc(ref_t *p)
{
    if (!mark_bitmap)
	return;
    if (p >= new_space.start && p < new_space.end) {
	size_t idx = BM_IDX(p);
#if defined(THREADS) && defined(OAK_GC_VERIFY)
	if (ms_verify_born)
	    OAK_ATOMIC_FETCH_OR_U64(&ms_verify_born[idx / 64],
				    1ULL << (idx % 64));
#endif
#ifdef THREADS
	OAK_ATOMIC_FETCH_OR_U64(&mark_bitmap[idx / 64],
				1ULL << (idx % 64));
#else
	BM_SET(mark_bitmap, idx);
#endif
    }
}


/* ------------------------------------------------------------------ */
/*  Deferred objstart recording for copying GC fallback                */
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
   pointer and derive a length from the *next* object's memory. */
static size_t
ms_objstart_extent(ref_t *obj)
{
    ref_t *limit = free_point;
    size_t heap_words;
    size_t idx, next;

    if (limit > new_space.end)
	limit = new_space.end;
    heap_words = (size_t)(limit - new_space.start);

    idx = BM_IDX(obj);
    if (idx + 1 >= heap_words)
	return 1;

    next = idx + 1;
    while (next < heap_words && !BM_TEST(objstart_bitmap, next))
	next++;
    return next - idx;
}


/* ------------------------------------------------------------------ */
/*  Find object start from an interior address (for locatives)         */
/* ------------------------------------------------------------------ */

static ref_t *
find_object_start(ref_t *addr)
{
    if (!ms_is_heap_ptr(addr))
	return NULL;

    size_t idx = BM_IDX(addr);
    size_t w = idx / 64;
    int bit = (int)(idx % 64);

    /* Mask: keep bits 0..bit (at or below the target). */
    uint64_t word = objstart_bitmap[w] & ((2ULL << bit) - 1);

    /* Scan backward through bitmap words. */
    while (word == 0) {
	if (w == 0) return NULL;
	w--;
	word = objstart_bitmap[w];
    }

    int highest = 63 - CLZ64(word);
    return new_space.start + w * 64 + highest;
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
 * Mark an object in new_space and push it onto the mark stack
 * if not already marked.  For spatic objects, do nothing (they
 * are permanent and always considered live).
 */
static void
ms_mark_ref(ref_t r)
{
    if (!(r & PTR_MASK))
	return;   /* fixnum or immediate — not a pointer */

    ref_t *p = ANY_TO_PTR(r);

    if (TAG_IS(r, LOC_TAG)) {
	/* Locative: find the containing object and mark that. */
	if (!NEW_PTR(p))
	    return;   /* spatic or outside heap */
	p = find_object_start(p);
	if (!p) return;
    } else {
	/* PTR_TAG reference. */
	if (!NEW_PTR(p))
	    return;   /* spatic — always live */
	/* A pointer can be interior too: MAKE-LAMBDA allocates the
	   operation and its method as one object, and the method is
	   referred to on its own (e_current_method, context frames),
	   often after the operation has become garbage. */
	if (!BM_TEST(objstart_bitmap, BM_IDX(p))) {
	    p = find_object_start(p);
	    if (!p) return;
	}
    }

    size_t idx = BM_IDX(p);
    if (!BM_TEST(mark_bitmap, idx)) {
	BM_SET(mark_bitmap, idx);
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
	size_t len = ms_objstart_extent(obj);

	/* Sanity: clamp length so we don't read past the heap. */
	if (obj + len > new_space.end)
	    len = (size_t)(new_space.end - obj);
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
ms_mark_ref_concurrent(ref_t r)
{
    if (!(r & PTR_MASK))
	return;

    ref_t *p = ANY_TO_PTR(r);

    if (TAG_IS(r, LOC_TAG)) {
	if (!NEW_PTR(p))
	    return;
	p = find_object_start(p);
	if (!p) return;
    } else {
	if (!NEW_PTR(p))
	    return;
	/* Interior pointers, as in ms_mark_ref. */
	if (!BM_TEST(objstart_bitmap, BM_IDX(p))) {
	    p = find_object_start(p);
	    if (!p) return;
	}
    }

    size_t idx = BM_IDX(p);
    uint64_t bit = 1ULL << (idx % 64);
    uint64_t old = OAK_ATOMIC_FETCH_OR_U64(&mark_bitmap[idx / 64], bit);
    if (!(old & bit))
	mark_stack_push(p);
}


/*
 * Concurrent trace: pop objects from the mark stack and scan their
 * fields with atomic marking.  The mark stack is only accessed by
 * the GC coordinator thread.
 */
static void
ms_trace_concurrent(void)
{
    while (mark_stack_top > 0) {
	ref_t *obj = mark_stack_pop();
	size_t len = ms_objstart_extent(obj);

	if (obj + len > new_space.end)
	    len = (size_t)(new_space.end - obj);
	marked_words += len;

	for (size_t i = 0; i < len; i++) {
	    ref_t val = obj[i];
	    if (val & PTR_MASK)
		ms_mark_ref_concurrent(val);
	}
    }
}
#endif /* THREADS */


#if defined(THREADS) && defined(OAK_GC_VERIFY)
/*
 * Verify mode (-DOAK_GC_VERIFY): at the end of the remark pause, with
 * the world still stopped, mark the heap again from scratch and
 * compare.  An object the fresh mark reaches below sweep_limit that
 * the concurrent mark missed would be swept while live: a missing
 * write barrier or born-black.  Each one is reported with the object
 * that refers to it, then kept, so the run can go on.
 */
static unsigned long ms_verify_cycles, ms_verify_missed_total;
static int ms_verify_from_marked;
volatile int ms_verify_stw_owner = -1;

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

static ref_t *
ms_verify_target(ref_t r)
{
    ref_t *p = ANY_TO_PTR(r);

    if (!NEW_PTR(p))
	return NULL;
    if (TAG_IS(r, LOC_TAG) || !BM_TEST(objstart_bitmap, BM_IDX(p)))
	p = find_object_start(p);
    return p;
}

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
    size_t len = ms_objstart_extent(obj);

    fprintf(stderr, "; GC VERIFY: missed %p %s in-snapshot %d (%zu words: %#lx %#lx %#lx)",
	    (void *)obj, ms_verify_type_name(obj[0]),
	    ms_verify_snap ? BM_TEST(ms_verify_snap, BM_IDX(obj)) != 0 : -1,
	    len, (unsigned long)obj[0],
	    (unsigned long)(len > 1 ? obj[1] : 0),
	    (unsigned long)(len > 2 ? obj[2] : 0));
    if (from) {
	fprintf(stderr, " via field %zu of %p %s (%zu words, marked %d,"
		" above sweep_limit %d)",
		field, (void *)from, ms_verify_type_name(from[0]),
		ms_objstart_extent(from), ms_verify_from_marked,
		from >= sweep_limit);
	if (ms_verify_born)
	    fprintf(stderr, " born-black %d",
		    BM_TEST(ms_verify_born, BM_IDX(from)) != 0);
    } else
	fprintf(stderr, " from %s", what);
    fprintf(stderr, "\n");
    ms_verify_find_alloc(obj);
    if (from)
	ms_verify_find_alloc(from);
}

static void
ms_verify_marks(void)
{
    size_t bytes = bitmap_words * sizeof(uint64_t);
    uint64_t *done = (uint64_t *)xmalloc(bytes);
    size_t limit = (size_t)(sweep_limit - new_space.start);
    unsigned long missed = 0, reported = 0;
    ref_t *p;

    memcpy(done, mark_bitmap, bytes);
    memset(mark_bitmap, 0, bytes);

    /* What the remark treats as live: the roots, and everything
       allocated above sweep_limit (scanned, never swept). */
    ms_mark_roots();
    for (p = sweep_limit; p < free_point; p++)
	if (*p & PTR_MASK)
	    ms_mark_ref(*p);

    while (mark_stack_top > 0) {
	ref_t *obj = mark_stack_pop();
	size_t len = ms_objstart_extent(obj);

	if (obj + len > new_space.end)
	    len = (size_t)(new_space.end - obj);
	for (size_t i = 0; i < len; i++) {
	    ref_t val = obj[i];
	    ref_t *t;
	    size_t idx;

	    if (!(val & PTR_MASK) || !(t = ms_verify_target(val)))
		continue;
	    idx = BM_IDX(t);
	    if (idx < limit && !BM_TEST(done, idx) && !BM_TEST(mark_bitmap, idx)
		&& reported < 20) {
		ms_verify_from_marked =
		    obj >= sweep_limit || BM_TEST(done, BM_IDX(obj)) != 0;
		ms_verify_report(NULL, obj, i, t);
		reported++;
	    }
	    ms_mark_ref(val);
	}
    }

    /* Report the rest only if none was reached through a field: those
       were reached straight from the roots. */
    unsigned long via_field = reported;

    for (size_t w = 0; w < bitmap_words; w++) {
	uint64_t lost = mark_bitmap[w] & ~done[w];
	while (lost) {
	    size_t idx = w * 64 + CTZ64(lost);
	    lost &= lost - 1;
	    if (idx >= limit)
		break;
	    if (!via_field && reported < 20) {
		ms_verify_report("the roots", NULL, 0, new_space.start + idx);
		reported++;
	    }
	    missed++;
	}
	mark_bitmap[w] |= done[w];
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


/*
 * Mark roots: VM registers, stacks, spatic space.
 */
static void
ms_mark_roots(void)
{
    ref_t *p;

/* A root is worth a call only if it points into new space: nearly
   every word of spatic and of the stacks is a fixnum or points into
   spatic, and this is the collector's one pass over them. */
#define MS_ROOT(r)							\
    do { ref_t _r = (r);						\
	 if ((_r & PTR_MASK) && NEW_PTR(ANY_TO_PTR(_r)))		\
	     ms_mark_ref(_r); } while (0)

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

    /* Scan spatic space: any pointer from spatic into new_space is a root. */
    for (p = spatic.start; p < spatic.end; p++)
	MS_ROOT(*p);

#undef FORTHREADS
#undef MS_ROOT
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
	free_list_heads[sc] = p;
	free_list_total_words += len;
	if (len >= TLAB_MIN_FREE)
	    tlab_free_none = false;
    }
    *run_start = NULL;
    *run_len = 0;
}

#ifndef THREADS
static size_t
ms_sweep(void)
{
    size_t reclaimed = 0;

    /* Clear all free lists. */
    for (int i = 0; i < NUM_SIZE_CLASSES; i++)
	free_list_heads[i] = NULL;
    free_list_total_words = 0;

    /* Walk the heap using the objstart bitmap.  Only sweep up to
       sweep_limit — objects allocated during concurrent mark (above
       sweep_limit) are implicitly live and must not be swept. */
    ref_t *limit = sweep_limit ? sweep_limit : free_point;
    size_t heap_words = (size_t)(limit - new_space.start);
    size_t bm_words = (heap_words + 63) / 64;
    ref_t *run_start = NULL;
    size_t run_len = 0;

    for (size_t w = 0; w < bm_words; w++) {
	uint64_t os_word = objstart_bitmap[w];
	if (os_word == 0) continue;

	while (os_word != 0) {
	    int bit = CTZ64(os_word);
	    os_word &= os_word - 1;   /* clear lowest set bit */

	    size_t idx = w * 64 + bit;
	    if (idx >= heap_words) break;

	    ref_t *obj = new_space.start + idx;

	    /* An object extends to the next objstart bit (not to what
	       its type says: a naked cell's word[0] can look like a
	       type).  So a run of dead objects extends from its first
	       to the next live one, and only that end is looked for. */
	    if (BM_TEST(mark_bitmap, idx)) {
		/* Live: clear mark for next cycle. */
		BM_CLR(mark_bitmap, idx);
		if (run_start) {
		    run_len = (size_t)(obj - run_start);
		    reclaimed += run_len;
		    ms_sweep_run_end(&run_start, &run_len);
		}
	    } else if (run_start) {
		BM_CLR(objstart_bitmap, idx);	/* into the run */
	    } else {
		run_start = obj;
	    }
	}
    }
    if (run_start) {
	run_len = ms_objstart_extent_to(run_start, heap_words);
	reclaimed += run_len;
	ms_sweep_run_end(&run_start, &run_len);
    }

    return reclaimed;
}
#endif /* !THREADS */


/* ------------------------------------------------------------------ */
/*  Lazy sweep (Phase 5)                                               */
/* ------------------------------------------------------------------ */

static size_t lazy_sweep_cursor;   /* word index into heap */
static size_t lazy_sweep_end;      /* word index the sweep stops at */
static bool   lazy_sweep_active;   /* true when blocks remain to sweep */

void
ms_prepare_lazy_sweep(void)
{
    /* Clear all free lists — they will be rebuilt incrementally
       as blocks are lazily swept. */
    for (int i = 0; i < NUM_SIZE_CLASSES; i++)
	free_list_heads[i] = NULL;
    free_list_total_words = 0;

    lazy_sweep_cursor = 0;
    /* The sweep is bounded by the cycle's snapshot for good: what
       other threads allocate above it while the sweep goes on lazily
       (and what they allocated during the concurrent mark) carries no
       mark bit, so a sweep that ran up to free_point would free it
       live. */
    lazy_sweep_end = (size_t)((sweep_limit ? sweep_limit : free_point)
			      - new_space.start);
    lazy_sweep_active = true;
}


bool
ms_lazy_sweep_one(void)
{
    if (!lazy_sweep_active)
	return false;

    size_t heap_words = lazy_sweep_end;

    if (lazy_sweep_cursor >= heap_words) {
	lazy_sweep_active = false;
	return false;
    }

    size_t start = lazy_sweep_cursor;
    size_t end = start + SWEEP_BLOCK_SIZE;
    if (end > heap_words)
	end = heap_words;
    lazy_sweep_cursor = end;

    /* Walk objstart bits in [start, end). */
    size_t start_w = start / 64;
    size_t end_w = (end + 63) / 64;
    ref_t *run_start = NULL;
    size_t run_len = 0;

    for (size_t w = start_w; w < end_w; w++) {
	uint64_t os_word = objstart_bitmap[w];
	if (os_word == 0) continue;

	while (os_word != 0) {
	    int bit = CTZ64(os_word);
	    os_word &= os_word - 1;

	    size_t idx = w * 64 + bit;
	    if (idx < start || idx >= end) continue;

	    ref_t *obj = new_space.start + idx;

	    /* As in ms_sweep: a run of dead objects, from its first to
	       the next live one.  The last run's end may lie past this
	       block, in one not yet swept: the entry is made anyway
	       (the allocator keeps off such an entry until the sweep
	       has passed it). */
	    if (BM_TEST(mark_bitmap, idx)) {
		BM_CLR(mark_bitmap, idx);
		if (run_start) {
		    run_len = (size_t)(obj - run_start);
		    ms_sweep_run_end(&run_start, &run_len);
		}
	    } else if (run_start) {
		BM_CLR(objstart_bitmap, idx);
	    } else {
		run_start = obj;
	    }
	}
    }
    if (run_start) {
	run_len = ms_objstart_extent_to(run_start, heap_words);
	ms_sweep_run_end(&run_start, &run_len);
    }
    tlab_free_none = false;	/* an entry that straddled the cursor may not now */

    return true;
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

static unsigned long
ms_post_gc_wp(void)
{
    unsigned long count = 0;
    long i;

    for (i = 0; i < wp_index; i++) {
	ref_t r = wp_table[1 + i];
	if (r & PTR_MASK) {
	    ref_t *p = ANY_TO_PTR(r);
	    if (NEW_PTR(p)) {
		size_t idx = BM_IDX(p);
		/* For locatives, find the object start. */
		if (TAG_IS(r, LOC_TAG)) {
		    ref_t *obj = find_object_start(p);
		    if (!obj || !BM_TEST(mark_bitmap, BM_IDX(obj))) {
			wp_table[1 + i] = e_nil;
			count++;
		    }
		} else {
		    if (!BM_TEST(mark_bitmap, idx)) {
			wp_table[1 + i] = e_nil;
			count++;
		    }
		}
	    }
	}
    }

    rebuild_wp_hashtable();
    return count;
}


/* ------------------------------------------------------------------ */
/*  Main collection entry point                                        */
/* ------------------------------------------------------------------ */

void
ms_collect(bool pre_dump, char *reason, size_t amount)
{
    long old_used;
    gc_timer_t cycle;
#ifdef THREADS
    gc_timer_t pause;
#endif

#ifdef THREADS
    /* If a concurrent GC is already running (another thread triggered
       collection), wait for it to complete and return.  The caller's
       ALLOCATE_PROT will retry the allocation after we return. */
    ms_collect_coordinated[*(int *)oak_tls_get(index_key)] = false;
    if (gc_concurrent_in_progress) {
	int parked_index = *(int *)oak_tls_get(index_key);
	gc_timer_t wait;

	gc_stats_begin(&wait, GC_EV_ALLOC_WAIT);
	oak_mutex_unlock(&alloc_lock);
	/* Advertise that we are parked for the whole time we are out of
	   action, including while re-acquiring the allocator lock.  The
	   coordinator's stop-the-world handshake spins until every
	   thread's gc_ready flag is set, so a thread that parks here
	   without setting it deadlocks the collection. */
	gc_ready[parked_index] = 1;
	ms_wait_concurrent_gc();
	/* Leave the parked state the way wait_for_gc does: under
	   gc_lock, which a coordinator holds for the whole of a
	   stop-the-world pause.  Clearing gc_ready without it let a
	   thread woken here run straight through the next cycle's
	   initial mark, which had counted it as parked, so what it
	   allocated meanwhile was neither a root nor born black. */
	oak_mutex_lock(&gc_lock);
	gc_ready[parked_index] = 0;
	oak_mutex_unlock(&gc_lock);
	while (oak_mutex_trylock(&alloc_lock) != 0) {
	    if (gc_pending)
		wait_for_gc();
	    else
		oak_thread_yield();
	}
	gc_stats_end(&wait, GC_EV_ALLOC_WAIT, reason);
	return;
    }
#endif

    /* Finish any lazy sweep remaining from the previous cycle.
       This must happen before we clear the mark bitmap. */
    ms_lazy_sweep_finish();

    /* For world dumps, use the copying GC which produces a contiguous
       heap suitable for serialization.  Note that we have deliberately
       not claimed gc_concurrent_in_progress: that flag tells gc() that
       the mutators are already parked, and here they are not, so it has
       to run its own stop-the-world handshake. */
    if (pre_dump) {
	gc(pre_dump, true, reason, amount);
	return;
    }

#ifdef THREADS
    /* We are the GC coordinator for this cycle. */
    gc_concurrent_in_progress = 1;
    ms_collect_coordinated[*(int *)oak_tls_get(index_key)] = true;
    gc_stats_begin(&cycle, GC_EV_CYCLE);
#else
    gc_stats_begin(&cycle, GC_EV_MARK_SWEEP);
#endif

    old_used = (long)(free_point - new_space.start) -
	(long)free_list_total_words;

    if (trace_gc == 1)
	fprintf(stderr, "\n;MS-GC");
    if (trace_gc > 1)
	fprintf(stderr, "\n; Mark-sweep GC due to %s.\n", reason);

    /* Save the current free_point as the sweep limit.  Objects
       allocated above this point during concurrent mark are
       implicitly live and will not be swept.

       With threads, both this snapshot and the TLAB retirement happen
       inside the initial-mark pause below instead of here: the TLAB
       fast path in ALLOCATE_PROT deliberately takes no lock, so doing
       either while the other mutators are still running races them --
       the collector can zero a TLAB tail an instant after a mutator
       has handed out an object in it, leaving a live object whose type
       slot reads as zero. */
#ifndef THREADS
    ms_tlab_retire_all();
    sweep_limit = free_point;
#endif

    /* --- Mark phase --- */
    if (trace_gc > 1) {
	fprintf(stderr, "; Marking... [heap=%zu, used=%ld, free_list=%zu]",
		new_space.size, (long)(free_point - new_space.start),
		free_list_total_words);
	fflush(stderr);
    }

    memset(mark_bitmap, 0, bitmap_words * sizeof(uint64_t));
    mark_stack_init();
    marked_words = 0;
#if defined(THREADS) && defined(OAK_GC_VERIFY)
    if (!ms_vlog)
	ms_vlog = calloc(VLOG_SIZE, sizeof(*ms_vlog));
    free(ms_verify_born);
    ms_verify_born = (uint64_t *)xmalloc(bitmap_words * sizeof(uint64_t));
    memset(ms_verify_born, 0, bitmap_words * sizeof(uint64_t));
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
	int my_index_val = *(int *)oak_tls_get(index_key);

	/* === STW PAUSE 1: Initial mark (scan roots) === */
	gc_phase = GC_PHASE_INITIAL_MARK;

	/* Release alloc_lock so blocked threads can see gc_pending
	   in their spin loops and park via wait_for_gc(). */
	oak_mutex_unlock(&alloc_lock);

	gc_stats_begin(&pause, GC_EV_INITIAL_MARK);
	(void)stw_request_pause(my_index_val);
#ifdef OAK_GC_VERIFY
	ms_verify_stw_owner = my_index_val;
#endif

	if (trace_gc > 1)
	    fprintf(stderr, "\n; STW initial mark...");

	/* The world is stopped, so no mutator can be inside the TLAB
	   fast path.  Retire the TLABs and take the sweep_limit
	   snapshot now. */
	ms_tlab_retire_all();
	sweep_limit = free_point;

	ms_mark_roots();
#ifdef OAK_GC_VERIFY
	/* Trace the whole snapshot now into a copy, then start the
	   real mark over from the roots. */
	ms_trace();
	free(ms_verify_snap);
	ms_verify_snap = (uint64_t *)xmalloc(bitmap_words * sizeof(uint64_t));
	memcpy(ms_verify_snap, mark_bitmap, bitmap_words * sizeof(uint64_t));
	memset(mark_bitmap, 0, bitmap_words * sizeof(uint64_t));
	ms_mark_roots();
#endif
	if (trace_gc > 1) { fprintf(stderr, " roots"); fflush(stderr); }

	/* Activate SATB barriers before releasing mutators.
	   This ensures every heap write during concurrent mark
	   logs the old value for the remark phase. */
	gc_satb_active = 1;
	gc_phase = GC_PHASE_CONCURRENT_MARK;

	/* === RELEASE: mutators resume during concurrent mark === */
#ifdef OAK_GC_VERIFY
	ms_verify_stw_owner = -1;
#endif
	stw_release(my_index_val);
	{
	    double ms = gc_stats_end(&pause, GC_EV_INITIAL_MARK, reason);
	    if (trace_gc > 1) {
		fprintf(stderr, " %.3f ms.\n; Concurrent tracing...", ms);
		fflush(stderr);
	    }
	}

	/* === CONCURRENT MARK === */
	ms_trace_concurrent();

	if (trace_gc > 1) {
	    fprintf(stderr, " done.\n; STW remark...");
	    fflush(stderr);
	}

	/* === STW PAUSE 2: Remark === */
	gc_phase = GC_PHASE_REMARK;
	gc_stats_begin(&pause, GC_EV_REMARK);
	(void)stw_request_pause(my_index_val);
#ifdef OAK_GC_VERIFY
	ms_verify_stw_owner = my_index_val;
#endif

	/* Retire TLABs allocated during concurrent mark. */
	ms_tlab_retire_all();

	/* Drain SATB buffers — these contain old pointer values
	   that were overwritten during concurrent mark and must
	   be traced to maintain the snapshot-at-beginning invariant. */
	satb_drain_to_mark_stack();

	/* Re-scan roots to catch new stack entries that appeared
	   during concurrent mark.  Stacks are C arrays, not covered
	   by SATB barriers. */
	ms_mark_roots();

	/* Scan objects allocated during concurrent mark.  These
	   are above sweep_limit and won't be swept, but they may
	   contain pointers to old objects that need marking. */
	{
	    ref_t *p;
	    for (p = sweep_limit; p < free_point; p++) {
		ref_t val = *p;
		if (val & PTR_MASK)
		    ms_mark_ref(val);
	    }
	}

	/* Process all newly discovered references. */
	ms_trace();

#ifdef OAK_GC_VERIFY
	ms_verify_marks();
#endif

	if (trace_gc > 1) { fprintf(stderr, " done."); fflush(stderr); }

	/* Deactivate SATB barriers. */
	gc_satb_active = 0;
	satb_flush_all();
    }
#else
    /* Single-threaded: STW mark as before. */
    ms_mark_roots();
    if (trace_gc > 1) { fprintf(stderr, " roots"); fflush(stderr); }
    ms_trace();
#endif

    crash_recovery_installed = saved_crash_recovery;

    if (trace_gc > 1)
	fprintf(stderr, "\n; Mark complete.\n");

    /* --- Weak pointers --- */
    if (trace_gc > 1)
	fprintf(stderr, "; Scanning weak pointer table...");
    {
	unsigned long wp_count = ms_post_gc_wp();
	if (trace_gc > 1)
	    fprintf(stderr, " %lu entr%s discarded.\n",
		    wp_count, wp_count != 1 ? "ies" : "y");
    }

    /* --- Sweep phase --- */
    gc_phase = GC_PHASE_SWEEP;

    /* Free the mark stack before sweeping — it's no longer needed. */
    free(mark_stack);
    mark_stack = NULL;
    mark_stack_size = 0;

#ifdef THREADS
    {
	int my_index_val = *(int *)oak_tls_get(index_key);

	/* Prepare lazy sweep: clear free lists and set the sweep
	   cursor to the start of the heap.  Blocks will be swept
	   on demand by allocating threads (ms_lazy_sweep_one). */
	ms_prepare_lazy_sweep();

	if (trace_gc > 1)
	    fprintf(stderr, "; Lazy sweep prepared (%zu words to sweep).\n",
		    (size_t)((sweep_limit ? sweep_limit : free_point)
			     - new_space.start));

	/* Sweep enough blocks now to satisfy the requested amount.
	   This avoids the caller falling through to the copying GC
	   fallback when free lists are still empty. */
	{
	    size_t swept_so_far = 0;
	    while (!ms_space_for(amount) && ms_lazy_sweep_one()) {
		/* keep sweeping */
	    }
	    swept_so_far = free_list_total_words;

	    if (trace_gc > 1 && swept_so_far > 0)
		fprintf(stderr, "; Eagerly swept %zu words.\n",
			swept_so_far);
	}

	gc_phase = GC_PHASE_IDLE;

	/* Release all threads from the remark STW pause. */
#ifdef OAK_GC_VERIFY
	ms_verify_stw_owner = -1;
#endif
	stw_release(my_index_val);
	{
	    double ms = gc_stats_end(&pause, GC_EV_REMARK, reason);
	    if (trace_gc > 1)
		fprintf(stderr, "; Remark pause %.3f ms.\n", ms);
	}

	/* Re-acquire alloc_lock — ALLOCATE_PROT expects us to hold it. */
	oak_mutex_lock(&alloc_lock);

	/* Check if we need the copying GC fallback. */
	{
	    size_t free_space = free_list_total_words +
		(size_t)(new_space.end - free_point);

	    if (trace_gc == 1)
		fprintf(stderr, ":%ld%%",
			old_used > 0
			? (100 * (long)free_list_total_words) / old_used
			: 0);
	    if (trace_gc > 1)
		fprintf(stderr, "; MS-GC complete.  %zu free (%zu list + %zu bump).\n",
			free_space, free_list_total_words,
			(size_t)(new_space.end - free_point));

	    if (!ms_space_for(amount)) {
		if (trace_gc > 1)
		    fprintf(stderr,
			    "; MS-GC: insufficient space after collection, "
			    "falling back to copying GC.\n");

		/* Finish any remaining lazy sweep before copying GC. */
		ms_lazy_sweep_finish();
		sweep_limit = NULL;

		{
		    long new_used = old_used - (long)free_list_total_words;
		    if (new_used < 0) new_used = 0;
		    size_t live = (size_t)new_used + amount;
		    size_t want = 3 * live;
		    if (want > original_newspace_size) {
			original_newspace_size = want;
			e_next_newspace_size = want;
		    }
		}

		/* The mutators were released by stw_release() above, so
		   the object-relocating copying collector must do its own
		   stop-the-world handshake.  Drop the coordinator flag
		   (without broadcasting: threads parked in
		   ms_wait_concurrent_gc still have their gc_ready flags
		   set, which is exactly what the handshake needs). */
		gc_concurrent_in_progress = 0;
		gc(false, true, "mark-sweep fallback", amount);
		gc_concurrent_in_progress = 1;
	    }
	    else if (ms_crowded(amount)) {
		/* As above, into the bigger space; spatic stays. */
		ms_lazy_sweep_finish();
		sweep_limit = NULL;
		gc_concurrent_in_progress = 0;
		gc(false, false, "mark-sweep growth", amount);
		gc_concurrent_in_progress = 1;
	    }
	}

	sweep_limit = NULL;

	if (trace_gc == 1)
	    fprintf(stderr, "\n");

	/* Signal any threads waiting for the concurrent GC to finish. */
	oak_mutex_lock(&gc_concurrent_lock);
	gc_concurrent_in_progress = 0;
	oak_cond_broadcast(&gc_concurrent_done_cv);
	oak_mutex_unlock(&gc_concurrent_lock);
	gc_stats_end(&cycle, GC_EV_CYCLE, reason);
    }
#else
    /* Single-threaded: full STW sweep. */
    if (trace_gc > 1)
	fprintf(stderr, "; Sweeping...");

    size_t reclaimed = ms_sweep();

    if (trace_gc > 1)
	fprintf(stderr, " %zu words reclaimed.\n", reclaimed);

    gc_phase = GC_PHASE_IDLE;
    sweep_limit = NULL;

    /* --- Post-GC sizing --- */
    {
	long new_used = old_used - (long)reclaimed;
	if (new_used < 0) new_used = 0;
	size_t free_space = free_list_total_words +
	    (size_t)(new_space.end - free_point);

	if (trace_gc == 1)
	    fprintf(stderr, ":%ld%%",
		    old_used > 0 ? (100 * (long)reclaimed) / old_used : 0);
	if (trace_gc > 1)
	    fprintf(stderr, "; MS-GC complete.  %ld used, %zu free (%zu list + %zu bump).\n",
		    (long)new_used, free_space,
		    free_list_total_words,
		    (size_t)(new_space.end - free_point));

	/* If nothing big enough is free, expand the heap. */
	if (!ms_space_for(amount)) {
	    if (trace_gc > 1)
		fprintf(stderr,
			"; MS-GC: insufficient space after collection, "
			"falling back to copying GC.\n");

	    {
		size_t live = (size_t)new_used + amount;
		size_t want = 3 * live;
		if (want > original_newspace_size) {
		    original_newspace_size = want;
		    e_next_newspace_size = want;
		}
	    }

	    gc(false, true, "mark-sweep fallback", amount);
	}
	else if (ms_crowded(amount))
	    gc(false, false, "mark-sweep growth", amount);

	if (trace_gc == 1)
	    fprintf(stderr, "\n");
    }
    {
	double ms = gc_stats_end(&cycle, GC_EV_MARK_SWEEP, reason);
	if (trace_gc > 1)
	    fprintf(stderr, "; MS-GC pause %.3f ms.\n", ms);
    }
#endif
}


#ifdef THREADS
/* The GC and FULL-GC instructions: a collection asked for by the
   program.  ms_collect wants the allocator lock, and either runs the
   cycle or joins the one in progress; calling gc() from here would
   run the copying collector under another thread's marker. */
void
ms_collect_explicit(const char *reason)
{
    while (oak_mutex_trylock(&alloc_lock) != 0) {
	if (gc_pending)
	    wait_for_gc();
	else
	    oak_thread_yield();
    }
    ms_collect(false, (char *)reason, 0);
    oak_mutex_unlock(&alloc_lock);
}
#endif


/* ------------------------------------------------------------------ */
/*  Initialization                                                     */
/* ------------------------------------------------------------------ */

void
ms_init(void)
{
    size_t heap_words = new_space.size;
    bitmap_words = (heap_words + 63) / 64;

    objstart_bitmap = (uint64_t *)xmalloc(bitmap_words * sizeof(uint64_t));
    mark_bitmap = (uint64_t *)xmalloc(bitmap_words * sizeof(uint64_t));

    memset(objstart_bitmap, 0, bitmap_words * sizeof(uint64_t));
    memset(mark_bitmap, 0, bitmap_words * sizeof(uint64_t));

    for (int i = 0; i < NUM_SIZE_CLASSES; i++)
	free_list_heads[i] = NULL;
    free_list_total_words = 0;
}

void
ms_destroy(void)
{
    if (objstart_bitmap) { free(objstart_bitmap); objstart_bitmap = NULL; }
    if (mark_bitmap) { free(mark_bitmap); mark_bitmap = NULL; }
    bitmap_words = 0;
}

void
ms_reinit(void)
{
    ms_destroy();
    ms_init();

    /* The heap has been replaced, so every thread-local allocation
       buffer points into the freed one. */
    for (int i = 0; i < MS_TLAB_COUNT; i++) {
	tlab_cursor_array[i] = NULL;
	tlab_end_array[i] = NULL;
    }

    /* Likewise any sweep that was still in progress. */
    lazy_sweep_active = false;
    lazy_sweep_cursor = 0;
    sweep_limit = NULL;

    /* Apply objstart bits recorded during the copying GC's transport
       phase.  Without this, objects copied by gc() would have no
       objstart bits and be invisible to ms_sweep. */
    for (size_t i = 0; i < deferred_count; i++) {
	ref_t *p = deferred_objstarts[i];
	if (p >= new_space.start && p < new_space.end)
	    OBJSTART_SET(BM_IDX(p));
    }
    if (deferred_objstarts) {
	free(deferred_objstarts);
	deferred_objstarts = NULL;
    }
    deferred_count = 0;
    deferred_cap = 0;
}


bool
ms_is_heap_ptr(const ref_t *p)
{
    return p >= new_space.start && p < new_space.end;
}
