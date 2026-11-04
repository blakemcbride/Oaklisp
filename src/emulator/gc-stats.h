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
 * gc-stats.h - Pauses and stalls of the garbage collectors, timed by
 * the monotonic clock (--gc-stats, --trace-gc 3; GC.md).
 *
 * A global pause stops every thread: a copying collection, a
 * single-threaded mark-sweep collection, and the two stop-the-world
 * pauses of a concurrent mark-sweep cycle.  A stall holds up one
 * thread: a thread parks at a safepoint while the GC thread holds a
 * pause, and a thread that runs out of memory waits for a cycle to
 * end.  The concurrent cycle itself is timed too, as the GC thread
 * sees it; it stalls nobody.  Each event is timed where it happens,
 * and the report at exit gives count, total, mean, maximum,
 * percentiles and a histogram per kind.
 */

#ifndef GC_STATS_H_INCLUDED
#define GC_STATS_H_INCLUDED

#include <stdint.h>
#include <stdio.h>

enum gc_event {
  /* Global pauses. */
  GC_EV_COPY,		/* a copying collection, start to end */
  GC_EV_MARK_SWEEP,	/* a mark-sweep collection with one thread */
  GC_EV_VERIFY,		/* verify mode's pause at the end of a mark */
  /* A part of a pause. */
  GC_EV_HANDSHAKE,	/* asking the threads to stop until all have */
  /* The GC thread's own time: stalls nobody. */
  GC_EV_CYCLE,		/* a concurrent cycle, as the GC thread sees it */
  GC_EV_ROUND,		/* a handshake round, request to last thread in */
  GC_EV_SWEEP,		/* the background sweep after a cycle */
  /* Stalls of one thread. */
  GC_EV_SCAN,		/* a thread scanning its own stacks at a handshake */
  GC_EV_SAFEPOINT,	/* parked at a safepoint for a pause */
  GC_EV_ALLOC_WAIT,	/* out of memory, waiting for another's cycle */
  /* Every global pause not nested in another; every stop of one
     thread (those pauses, and the stalls). */
  GC_EV_PAUSES,
  GC_EV_STOPS,
  GC_EV_COUNT
};

/* Set by --gc-stats, or --trace-gc 3: samples are kept and the
   report printed at exit.  Durations are also measured, for the
   trace lines, at --trace-gc 2. */
extern int gc_stats_on;

typedef uint64_t gc_ns_t;

/* The monotonic clock, in nanoseconds (always read: the collector's
   pacing uses it too). */
extern gc_ns_t gc_now(void);

typedef struct {
  gc_ns_t t0;
  int nested;		/* a pause begun inside another pause */
} gc_timer_t;

/* Bracket an event: begin as it starts, end when it is over.  End
   returns its length in milliseconds (0 when nothing is timed). */
extern void gc_stats_begin(gc_timer_t *t, enum gc_event ev);
extern double gc_stats_end(gc_timer_t *t, enum gc_event ev, const char *reason);

/* After the options are parsed: start the clock, arrange the report. */
extern void gc_stats_init(void);
extern void gc_stats_report(FILE *f);

#endif
