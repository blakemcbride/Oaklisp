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


/**********************************************************************
 *  Copyright (c) by Barak A. Pearlmutter and Kevin J. Lang, 1987-99. *
 *  Copyright (c) by Alex Stuebinger, 1998-99.                        *
 *  Distributed under the GNU General Public License v2 or later      *
 **********************************************************************/


#ifndef _GC_H_INCLUDED
#define _GC_H_INCLUDED

#include "config.h"
#include "data.h"
#include "oak-atomic.h"

extern bool full_gc;
extern void printref(FILE * fd, ref_t refin);

extern void gc(bool pre_dump, bool full_gc, char *reason,
	       size_t amount);

#define GC_MEMORY(v) \
{*gc_examine_ptr++ = (v);}
		/*
		   assert(gc_examine_ptr < &gc_examine_buffer[GC_EXAMINE_BUFFER_SIZE]);\
		   } */

#define GC_RECALL(v) \
{(v) = *--gc_examine_ptr;}
		/*
		   assert(gc_examine_ptr >= gc_examine_buffer);\
		   } */

#ifdef THREADS
/* Each thread's parked flag and dead flag: written by the thread
   (gc_ready, once its stacks are synchronized: a release) or its
   slot's reaper, read by whoever runs a pause or a round (an
   acquire, before it scans the thread's stacks). */
extern int gc_ready[];
extern int gc_thread_dead[];
#define GC_READY(i)		OAK_LOAD_ACQUIRE(&gc_ready[i])
#define GC_READY_SET(i)		OAK_STORE_RELEASE(&gc_ready[i], 1)
#define GC_READY_CLEAR(i)	OAK_STORE_RELAXED(&gc_ready[i], 0)
#define GC_THREAD_DEAD(i)	OAK_LOAD_RELAXED(&gc_thread_dead[i])
#define GC_THREAD_DEAD_SET(i, x) OAK_STORE_RELAXED(&gc_thread_dead[i], (x))
extern int oak_live_thread_count(void);
extern int oak_thread_park(long ms);
extern int oak_thread_unpark(int idx);
extern void oak_thread_sleep_ms(long ms);
/* Set by whoever runs a pause or a round, polled by every thread at
   its safepoints. */
extern bool gc_pending;
extern bool gc_stw;
extern oak_mutex_t gc_lock;
#endif

/* --gc-threads (gc-ms.c): threads marking in parallel in the
   concurrent collector; 0 is the default.  Accepted in every build. */
extern int gc_threads_wanted;

extern void set_gc_flag (bool flag);
extern int get_next_index();
extern void free_registers(void *arg);
extern void wait_for_gc();

#endif
