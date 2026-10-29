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
 * gc-stats.c - Pauses and stalls of the garbage collectors (see
 * gc-stats.h).  The collectors bracket each event with
 * gc_stats_begin/gc_stats_end; the samples are kept per kind (a
 * uniform reservoir once there are more than fit) under a lock, since
 * stalls are recorded by every thread, and the report is printed at
 * exit.
 */

#define _REENTRANT

#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif
#include "config.h"
#include "data.h"
#include "gc-stats.h"
#ifdef THREADS
#include "threads.h"
#endif

int gc_stats_on = 0;

/* Whether durations are measured at all: for the samples, or for the
   trace lines. */
static int gc_timing = 0;

static gc_ns_t gc_stats_t0;

/* Depth of global pauses on the coordinating thread (pauses are
   serialized by gc_lock, and begun only once it is held). */
static int pause_depth = 0;

#define GC_STATS_SAMPLES 4096

/* Histogram buckets, in ms: < 0.1, < 1, < 10, < 100, the rest. */
#define GC_STATS_BUCKETS 5

typedef struct {
  uint64_t count;
  gc_ns_t total;
  gc_ns_t max;
  uint32_t kept;
  uint64_t hist[GC_STATS_BUCKETS];
  gc_ns_t samples[GC_STATS_SAMPLES];
} gc_stat_t;

static gc_stat_t stats[GC_EV_COUNT];

static const char *const gc_event_name[GC_EV_COUNT] = {
  "copying collection",
  "mark-sweep collection",
  "initial-mark pause",
  "remark pause",
  "  threads stopping",
  "concurrent cycle, its thread",
  "safepoint wait",
  "allocation wait",
  "all global pauses",
};

#ifdef THREADS
static oak_mutex_t gc_stats_lock = OAK_MUTEX_INITIALIZER;
#endif

gc_ns_t
gc_now(void)
{
#ifdef _WIN32
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (gc_ns_t)(c.QuadPart / f.QuadPart) * 1000000000ull
    + (gc_ns_t)(c.QuadPart % f.QuadPart) * 1000000000ull / (gc_ns_t)f.QuadPart;
#elif defined(CLOCK_MONOTONIC)
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (gc_ns_t)ts.tv_sec * 1000000000ull + (gc_ns_t)ts.tv_nsec;
#else
  struct timeval tv;
  gettimeofday(&tv, 0);
  return (gc_ns_t)tv.tv_sec * 1000000000ull + (gc_ns_t)tv.tv_usec * 1000ull;
#endif
}

static int
is_pause(enum gc_event ev)
{
  return ev == GC_EV_COPY || ev == GC_EV_MARK_SWEEP
    || ev == GC_EV_INITIAL_MARK || ev == GC_EV_REMARK;
}

static void
gc_stats_add(gc_stat_t *s, gc_ns_t ns)
{
  double ms = (double)ns / 1e6;
  int b = ms < 0.1 ? 0 : ms < 1 ? 1 : ms < 10 ? 2 : ms < 100 ? 3 : 4;

  s->count++;
  s->total += ns;
  if (ns > s->max)
    s->max = ns;
  s->hist[b]++;
  if (s->kept < GC_STATS_SAMPLES)
    s->samples[s->kept++] = ns;
  else {
    /* Reservoir: every sample equally likely to be kept. */
    uint64_t j = (uint64_t)rand() % s->count;
    if (j < GC_STATS_SAMPLES)
      s->samples[j] = ns;
  }
}

void
gc_stats_begin(gc_timer_t *t, enum gc_event ev)
{
  t->nested = 0;
  if (!gc_timing) {
    t->t0 = 0;
    return;
  }
  t->t0 = gc_now();
  if (is_pause(ev))
    t->nested = pause_depth++ > 0;
}

double
gc_stats_end(gc_timer_t *t, enum gc_event ev, const char *reason)
{
  gc_ns_t ns;

  if (!gc_timing)
    return 0;
  ns = gc_now() - t->t0;
  if (is_pause(ev))
    pause_depth--;
  if (gc_stats_on) {
#ifdef THREADS
    oak_mutex_lock(&gc_stats_lock);
#endif
    gc_stats_add(&stats[ev], ns);
    if (is_pause(ev) && !t->nested)
      gc_stats_add(&stats[GC_EV_PAUSES], ns);
#ifdef THREADS
    oak_mutex_unlock(&gc_stats_lock);
#endif
    if (trace_gc > 2)
      fprintf(stderr, "\n; GC event: %s%s %.3f ms (%s)\n",
	      ev == GC_EV_HANDSHAKE ? "threads stopping" : gc_event_name[ev],
	      t->nested ? ", nested," : "", (double)ns / 1e6,
	      reason ? reason : "");
  }
  return (double)ns / 1e6;
}

static int
cmp_ns(const void *a, const void *b)
{
  gc_ns_t x = *(const gc_ns_t *)a, y = *(const gc_ns_t *)b;
  return x < y ? -1 : x > y;
}

/* The P-th percentile of the kept samples, sorted. */
static double
percentile(const gc_ns_t *sorted, uint32_t n, int p)
{
  uint32_t i = (uint32_t)(((uint64_t)n * p + 99) / 100);
  if (i == 0)
    i = 1;
  return (double)sorted[i - 1] / 1e6;
}

void
gc_stats_report(FILE *f)
{
  gc_ns_t run = gc_now() - gc_stats_t0;
  gc_stat_t *all = &stats[GC_EV_PAUSES];
  double p99 = 0;
  int ev;
  gc_ns_t *sorted = malloc(GC_STATS_SAMPLES * sizeof(gc_ns_t));

#ifdef THREADS
  oak_mutex_lock(&gc_stats_lock);
#endif
  fprintf(f, "\n; GC pauses and stalls, ms: count, total, mean, max, p50, p90, p99,"
	  " and how many under 0.1, 1, 10, 100 ms and over\n");
  for (ev = 0; ev < GC_EV_COUNT; ev++) {
    gc_stat_t *s = &stats[ev];
    double p50, p90, pp99;

    if (s->count == 0)
      continue;
    if (sorted) {
      memcpy(sorted, s->samples, s->kept * sizeof(gc_ns_t));
      qsort(sorted, s->kept, sizeof(gc_ns_t), cmp_ns);
      p50 = percentile(sorted, s->kept, 50);
      p90 = percentile(sorted, s->kept, 90);
      pp99 = percentile(sorted, s->kept, 99);
    }
    else
      p50 = p90 = pp99 = (double)s->max / 1e6;
    if (ev == GC_EV_PAUSES)
      p99 = pp99;
    fprintf(f, "; %-30s %7llu %9.2f %8.3f %8.3f %8.3f %8.3f %8.3f  %6llu %5llu %5llu %5llu %5llu\n",
	    gc_event_name[ev], (unsigned long long)s->count,
	    (double)s->total / 1e6, (double)s->total / 1e6 / (double)s->count,
	    (double)s->max / 1e6, p50, p90, pp99,
	    (unsigned long long)s->hist[0], (unsigned long long)s->hist[1],
	    (unsigned long long)s->hist[2], (unsigned long long)s->hist[3],
	    (unsigned long long)s->hist[4]);
  }
  fprintf(f, "; GC: %llu global pauses, total %.2f ms, max %.3f ms, p99 %.3f ms,"
	  " %.1f%% of %.0f ms\n",
	  (unsigned long long)all->count, (double)all->total / 1e6,
	  (double)all->max / 1e6, p99,
	  run > 0 ? 100.0 * (double)all->total / (double)run : 0.0,
	  (double)run / 1e6);
  fflush(f);
#ifdef THREADS
  oak_mutex_unlock(&gc_stats_lock);
#endif
  free(sorted);
}

static void
gc_stats_at_exit(void)
{
  gc_stats_report(stderr);
}

void
gc_stats_init(void)
{
  if (trace_gc > 2)
    gc_stats_on = 1;
  gc_timing = gc_stats_on || trace_gc > 1;
  if (!gc_timing)
    return;
#if defined(THREADS) && defined(OAK_NEEDS_DYNAMIC_MUTEX_INIT)
  oak_mutex_init(&gc_stats_lock);
#endif
  gc_stats_t0 = gc_now();
  if (gc_stats_on)
    atexit(gc_stats_at_exit);
}
