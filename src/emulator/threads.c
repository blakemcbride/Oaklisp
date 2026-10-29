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


#define _REENTRANT

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "threads.h"
#include "xmalloc.h"
#include "stacks.h"
#include "loop.h"
#include "gc.h"
#include "gc-stats.h"
#ifdef USE_MARK_SWEEP
#include "gc-ms.h"
#endif

#ifdef THREADS
int next_index = 0;
oak_tls_key_t index_key;
oak_mutex_t gc_lock = OAK_MUTEX_INITIALIZER;
oak_mutex_t alloc_lock = OAK_MUTEX_INITIALIZER;
oak_mutex_t index_lock = OAK_MUTEX_INITIALIZER;
/* DUMP-WORLD and LOAD-WORLD, in the interpreter and in native code. */
oak_mutex_t dump_lock = OAK_MUTEX_INITIALIZER;
bool gc_pending = false;
int gc_ready[MAX_THREAD_COUNT];
/* Slots of threads that have exited.  next_index never decreases, so
   without this a dead thread's gc_ready flag would stay 0 forever and
   stall every subsequent stop-the-world handshake. */
int gc_thread_dead[MAX_THREAD_COUNT];
/* Slots whose thread has exited and whose stacks, registers and TLAB
   have all been released.  get_next_index hands these back out, so the
   number of threads a process can create over its lifetime is bounded
   by how many run at once rather than by MAX_THREAD_COUNT.  Guarded by
   index_lock.  Slot 0 is the main thread and is never recycled. */
static int slot_reusable[MAX_THREAD_COUNT];
register_set_t* register_array[MAX_THREAD_COUNT];
oakstack *value_stack_array[MAX_THREAD_COUNT];
oakstack *cntxt_stack_array[MAX_THREAD_COUNT];
#endif

#ifdef THREADS
static instr_t tail_recurse_instruction = (22 << 2);

/* Unwind targets for threads whose thunk runs to completion.  A spawned
   thread starts with an empty context stack, so the RETURN that leaves
   the thunk's outermost frame has no context to pop; stack_unflush
   detects that and unwinds back to init_thread instead of walking a
   segment list that isn't there.  Slot 0, the main thread, is never
   armed: when the boot code returns there is nowhere to unwind to. */
static jmp_buf thread_exit_point[MAX_THREAD_COUNT];
static volatile int thread_exit_armed[MAX_THREAD_COUNT];

/* True when the calling thread can be unwound out of loop(). */
int
oak_thread_can_exit(void)
{
  int *my_index_p = (int *)oak_tls_get(index_key);

  if (my_index_p == NULL)
    return 0;
  if (*my_index_p <= 0 || *my_index_p >= MAX_THREAD_COUNT)
    return 0;
  return thread_exit_armed[*my_index_p];
}

/* Leave the interpreter and return to init_thread.  Does not return. */
void
oak_thread_exit_unwind(void)
{
  int my_index = *((int *)oak_tls_get(index_key));

  /* This thread will not execute another instruction, so retire it here
     rather than making a collection that is already waiting on our
     handshake flag wait for the thread-local destructor to run.
     free_registers repeats this when the thread actually exits. */
  gc_thread_dead[my_index] = 1;
  gc_ready[my_index] = 1;

  thread_exit_armed[my_index] = 0;
  longjmp(thread_exit_point[my_index], 1);
}

void
oak_threads_system_init(void)
{
#ifdef OAK_NEEDS_DYNAMIC_MUTEX_INIT
    oak_mutex_init(&gc_lock);
    oak_mutex_init(&alloc_lock);
    oak_mutex_init(&index_lock);
    {
      extern oak_mutex_t wp_lock;
      oak_mutex_init(&wp_lock);
      oak_mutex_init(&dump_lock);
      oak_mutex_init(&park_init_lock);
    }
#endif
}

/* Thread 0: the mutexes, the thread-local key and index, the slot,
   its stacks (sized here, before the command line can resize them;
   init_stacks allocates them) and a zeroed register set -- e_process
   in particular has to start as the fixnum 0, which is how the world
   knows no process object exists yet. */
void
oak_thread0_init(void)
{
  int my_index;
  int *my_index_p;

  oak_threads_system_init();
  oak_tls_create(&index_key, free_registers);
  my_index_p = (int *)malloc(sizeof (int));
  if (my_index_p == NULL) {
    fprintf(stderr, "Out of memory starting the main thread.\n");
    exit(EXIT_FAILURE);
  }
  *my_index_p = get_next_index();
  oak_tls_set(index_key, (void*)my_index_p);
  my_index = *my_index_p;
  oak_thread_index_var = my_index;
  gc_ready[my_index] = 0;
  value_stack_array[my_index] = (oakstack*)malloc(sizeof (oakstack));
  cntxt_stack_array[my_index] = (oakstack*)malloc(sizeof (oakstack));
  register_array[my_index] = (register_set_t*)malloc(sizeof(register_set_t));
  if (value_stack_array[my_index] == NULL || cntxt_stack_array[my_index] == NULL
      || register_array[my_index] == NULL) {
    fprintf(stderr, "Out of memory starting the main thread.\n");
    exit(EXIT_FAILURE);
  }
  memset(value_stack_array[my_index], 0, sizeof (oakstack));
  memset(cntxt_stack_array[my_index], 0, sizeof (oakstack));
  memset(register_array[my_index], 0, sizeof(register_set_t));
  value_stack.size = 1024;
  value_stack.filltarget = 1024/2;
  context_stack.size = 512;
  context_stack.filltarget = 512/2;
  gc_examine_ptr = gc_examine_buffer;
}

/* A long blocking call, made with the stacks synchronized, counts as
   time parked: say so for its length, and leave that state under
   gc_lock so a collection that counted us stays stopped (as
   oak_thread_park does). */
void
oak_thread_blocking_begin(void)
{
  gc_ready[oak_thread_index_var] = 1;
}

void
oak_thread_blocking_end(void)
{
  oak_mutex_lock(&gc_lock);
  gc_ready[oak_thread_index_var] = 0;
  oak_mutex_unlock(&gc_lock);
}
#endif

typedef struct {
  ref_t start_operation;
  int parent_index;
  int my_index;
} start_info_t;

#ifdef THREADS
/* This thread's index, for code without a my_index local (threads.h). */
#if defined(_MSC_VER) && !defined(__MINGW32__)
__declspec(thread) int oak_thread_index_var;
#else
__thread int oak_thread_index_var __attribute__((tls_model("initial-exec")));
#endif

/* What a new thread runs: loop (oaklisp.c) or the native call driver
   (embed-native.c), whichever entry point is linked in. */
void (*oak_thread_entry)(ref_t start_operation) = NULL;
#endif

#ifdef THREADS
static void *init_thread(void *info_p);
static void release_thread_slot(int i);
#endif

int create_thread(ref_t start_operation)
{
#ifdef THREADS
  oak_thread_t new_thread;
  int index;
  start_info_t *info_p = (start_info_t *)malloc(sizeof(start_info_t));

  if (info_p == NULL) {
    fprintf (stderr, "Out of memory.  No thread created\n");
    return 0;
  }
  index = get_next_index();
  if (index == -1) {
    fprintf (stderr,
	     "Max thread count of %d has been exceeded.  No thread created\n",
	     MAX_THREAD_COUNT);
    free(info_p);
    return 0;
  }
  gc_ready[index] = 0;
  gc_thread_dead[index] = 0;
  info_p->start_operation = start_operation;
  info_p->parent_index = *((int *)oak_tls_get(index_key));
  info_p->my_index = index;
  if (oak_thread_create(&new_thread, init_thread, (void *)info_p)) {
    free(info_p);
    release_thread_slot(index);
    return 0;
  }
  else
    return 1;
#else
  return 0;
#endif
}

#ifdef THREADS
static void *init_thread (void *info_p)
{
  volatile int my_index;
  int *my_index_p;
  start_info_t info;
  info = *((start_info_t *)info_p);
  free(info_p);
  /* Retrieve the next index in the thread arrays and lock it so
     another starting thread cannot get the same index */

  my_index = info.my_index;
  my_index_p = (int *)malloc(sizeof(int));
  if (my_index_p == NULL) {
    fprintf(stderr, "Out of memory starting thread %d.\n", (int)my_index);
    /* Nothing is in thread-local storage yet, so free_registers will
       not run for this thread; hand the slot back by hand. */
    release_thread_slot(my_index);
    return 0;
  }
  *my_index_p = info.my_index;
  oak_tls_set(index_key, (void *)my_index_p);
  oak_thread_index_var = my_index;
  /* From here on the thread-local destructor, free_registers, releases
     whatever has been allocated and hands the slot back, so an
     allocation failure only has to return. */

  /* Increment also releases the gc lock on next_index so another
     starting thread can get the lock, or a thread that is gc'ing can
     get the lock */

  /* Shouldn't get interrupted for gc until after stacks are
     created.  This is below here in the vm not checking intterupts
     until after we get to the loop */

  value_stack_array[my_index] = (oakstack*)malloc (sizeof (oakstack));
  cntxt_stack_array[my_index] = (oakstack*)malloc(sizeof (oakstack));
  if (value_stack_array[my_index] == NULL || cntxt_stack_array[my_index] == NULL) {
    fprintf(stderr, "Out of memory starting thread %d.\n", (int)my_index);
    return 0;
  }
  /* free_registers frees ->bp, so it must not be left uninitialized. */
  memset(value_stack_array[my_index], 0, sizeof (oakstack));
  memset(cntxt_stack_array[my_index], 0, sizeof (oakstack));

  value_stack_array[my_index]->size = value_stack_array[0]->size;
  value_stack_array[my_index]->filltarget = value_stack_array[0]->filltarget;
  cntxt_stack_array[my_index]->size = cntxt_stack_array[0]->size;
  cntxt_stack_array[my_index]->filltarget = cntxt_stack_array[0]->filltarget;

  init_stacks ();
  register_array[my_index] = (register_set_t*)malloc(sizeof (register_set_t));
  if (register_array[my_index] == NULL) {
    fprintf(stderr, "Out of memory starting thread %d.\n", (int)my_index);
    return 0;
  }

  memcpy(register_array[my_index], register_array[info.parent_index],
	 sizeof(register_set_t));
  /* Alarms start off in a new thread, whatever its parent's were. */
  register_array[my_index]->timer_counter = 0;
  register_array[my_index]->timer_increment = 0;

  gc_examine_ptr = gc_examine_buffer;

#ifdef USE_MARK_SWEEP
  tlab_cursor_array[my_index] = NULL;
  tlab_end_array[my_index] = NULL;
#endif

  /* At this point, it should be OK if the garbage collector gets run. */
  e_pc = &tail_recurse_instruction;
  e_nargs = 0;

  /* Big virtual machine interpreter loop.  It never returns: a thunk
     that runs to completion returns from its outermost frame, which
     underflows this thread's context stack, and stack_unflush unwinds
     back to the setjmp below instead of faulting. */
  if (setjmp(thread_exit_point[my_index]) == 0)
    {
      thread_exit_armed[my_index] = 1;
      (*oak_thread_entry)(info.start_operation);
    }

  thread_exit_armed[my_index] = 0;

  /* The thunk returned: the thread is done (MAKE-THREAD's threads end
     this way; the scheduler's never return). */
  return 0;
}
#endif

#ifdef THREADS
/* Parking (stream primitives 19 and 20): a thread blocks on its own
   condition variable until another thread unparks it.  A permit makes
   an unpark before the park count, so no wake-up is lost.  A parked
   thread is at a safepoint: the caller has unlocalized its stacks, and
   gc_ready says so for the length of the block; it leaves the parked
   state under gc_lock, as wait_for_gc does, so a stop-the-world that
   counted it stays stopped.  The slots are set up on first use, under
   a lock of their own. */
static oak_mutex_t park_init_lock = OAK_MUTEX_INITIALIZER;
static int park_inited;
static oak_mutex_t park_lock[MAX_THREAD_COUNT];
static oak_cond_t park_cv[MAX_THREAD_COUNT];
static int park_permit[MAX_THREAD_COUNT];

static void
park_init(void)
{
  if (!park_inited) {
    oak_mutex_lock(&park_init_lock);
    if (!park_inited) {
      int i;
      for (i = 0; i < MAX_THREAD_COUNT; i++) {
	oak_mutex_init(&park_lock[i]);
	oak_cond_init(&park_cv[i]);
	park_permit[i] = 0;
      }
      park_inited = 1;
    }
    oak_mutex_unlock(&park_init_lock);
  }
}
#endif

/* Block until unparked, or for MS milliseconds if MS >= 0: 1 if
   unparked, 0 on timeout (and always 0 at once without threads). */
int
oak_thread_park(long ms)
{
#ifdef THREADS
  int idx = *(int *)oak_tls_get(index_key);
  int got;

  park_init();
  gc_ready[idx] = 1;
  oak_mutex_lock(&park_lock[idx]);
  if (ms < 0) {
    while (!park_permit[idx])
      oak_cond_wait(&park_cv[idx], &park_lock[idx]);
  } else if (!park_permit[idx]) {
    oak_cond_timedwait_ms(&park_cv[idx], &park_lock[idx], ms);
  }
  got = park_permit[idx];
  park_permit[idx] = 0;
  oak_mutex_unlock(&park_lock[idx]);
  oak_mutex_lock(&gc_lock);
  gc_ready[idx] = 0;
  oak_mutex_unlock(&gc_lock);
  return got;
#else
  (void)ms;
  return 0;
#endif
}

/* Give thread IDX a permit, waking it if parked: 1, or 0 for no such
   thread. */
int
oak_thread_unpark(int idx)
{
#ifdef THREADS
  if (idx < 0 || idx >= next_index)
    return 0;
  park_init();
  oak_mutex_lock(&park_lock[idx]);
  park_permit[idx] = 1;
  oak_cond_broadcast(&park_cv[idx]);
  oak_mutex_unlock(&park_lock[idx]);
  return 1;
#else
  (void)idx;
  return 0;
#endif
}

/* Sleep MS milliseconds, at a safepoint. */
void
oak_thread_sleep_ms(long ms)
{
#ifdef THREADS
  int idx = *(int *)oak_tls_get(index_key);

  gc_ready[idx] = 1;
  oak_sleep_ms(ms);
  oak_mutex_lock(&gc_lock);
  gc_ready[idx] = 0;
  oak_mutex_unlock(&gc_lock);
#else
  oak_sleep_ms(ms);
#endif
}

/* The OS threads running Oaklisp code: 1 without threads. */
int oak_live_thread_count(void)
{
#ifdef THREADS
  int n = 0, i, count = next_index;
  for (i = 0; i < count; i++)
    if (!gc_thread_dead[i])
      n++;
  return n;
#else
  return 1;
#endif
}

void set_gc_flag (bool flag)
{
#ifdef THREADS
  if (flag == true) {
    oak_mutex_lock(&gc_lock);
    gc_pending = flag;
  }
  else {
    gc_pending = flag;
    oak_mutex_unlock(&gc_lock);
  }
#endif
}

/* Increment uses the gc lock since we must be sure that a new thread
   does not get started and begin processing while the gc is already
   running.  The get_next_index additionally ensures that no two
   threads get the same index when starting */

int get_next_index ()
{
  int ret = -1;
#ifdef THREADS
  int i;
  oak_mutex_lock(&index_lock);
  /* Prefer the slot of a thread that has already exited: reusing keeps
     next_index down to the high-water mark of concurrent threads,
     which is what the stop-the-world handshake and the root scan walk. */
  for (i = 1; i < next_index; i++)
    if (slot_reusable[i]) {
      slot_reusable[i] = 0;
      ret = i;
      break;
    }
  if (ret == -1 && next_index < MAX_THREAD_COUNT) {
    ret = next_index;
    next_index++;
  }
  oak_mutex_unlock(&index_lock);
#endif
  return (ret);
}

#ifdef THREADS
/* Hand a thread slot back for reuse.  Only call once the slot holds no
   state belonging to its late occupant. */
static void release_thread_slot (int i)
{
  if (i <= 0 || i >= MAX_THREAD_COUNT)
    return;			/* slot 0 is the main thread */
  gc_thread_dead[i] = 1;
  gc_ready[i] = 1;
  oak_mutex_lock(&index_lock);
  slot_reusable[i] = 1;
  oak_mutex_unlock(&index_lock);
}
#endif

/* Thread-local storage destructor for index_key: runs when a thread
   exits.  Retires the thread's slot so the garbage collector stops
   waiting for it and stops scanning its stacks as roots. */

void free_registers (void *arg)
{
#ifdef THREADS
  int *my_index_p = (int *)arg;
  int i;

  if (my_index_p == NULL)
    return;

  i = *my_index_p;
  if (i < 0 || i >= MAX_THREAD_COUNT)
    return;

  /* Announce the slot is gone before doing anything that can block, so
     a collection that is already waiting on our handshake flag is free
     to finish. */
  gc_thread_dead[i] = 1;
  gc_ready[i] = 1;

  /* gc_lock is held for the duration of a collection, so taking it
     here guarantees nobody is scanning our stacks right now.  Anyone
     who starts afterwards will skip the slot. */
  oak_mutex_lock(&gc_lock);

  if (value_stack_array[i] != NULL)
    {
      if (value_stack_array[i]->bp != NULL)
	free(value_stack_array[i]->bp - 1);
      free(value_stack_array[i]);
      value_stack_array[i] = NULL;
    }
  if (cntxt_stack_array[i] != NULL)
    {
      if (cntxt_stack_array[i]->bp != NULL)
	free(cntxt_stack_array[i]->bp - 1);
      free(cntxt_stack_array[i]);
      cntxt_stack_array[i] = NULL;
    }
  if (register_array[i] != NULL)
    {
      free(register_array[i]);
      register_array[i] = NULL;
    }

#ifdef USE_MARK_SWEEP
  /* Retire the TLAB here rather than leaving it for the collector: once
     the slot is recycled the next occupant overwrites these pointers,
     and the unused tail would then have no objstart bit for the sweep
     to find.  Holding gc_lock means no stop-the-world pause -- and so
     no ms_tlab_retire_all -- is running. */
  ms_tlab_retire(tlab_cursor_array[i], tlab_end_array[i]);
  tlab_cursor_array[i] = NULL;
  tlab_end_array[i] = NULL;
#endif

  oak_mutex_unlock(&gc_lock);

  release_thread_slot(i);

  free(my_index_p);
#else
  (void)arg;
#endif
}

void wait_for_gc()
{
#ifdef THREADS
  int *my_index_p;
  int  my_index;
  gc_timer_t stall;
  my_index_p = oak_tls_get(index_key);
  my_index = *(my_index_p);
  gc_stats_begin(&stall, GC_EV_SAFEPOINT);
  gc_ready[my_index] = 1;
  oak_mutex_lock(&gc_lock);
  gc_ready[my_index] = 0;
  oak_mutex_unlock(&gc_lock);
  gc_stats_end(&stall, GC_EV_SAFEPOINT, NULL);
#endif
}
