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


#ifndef _THREADS_H_INCLUDED
#define _THREADS_H_INCLUDED

#include "config.h"
#include "oak-thread.h"

#ifdef THREADS
extern int next_index;
extern int oak_live_thread_count(void);
extern oak_tls_key_t index_key;
extern oak_mutex_t index_lock;
extern oak_mutex_t alloc_lock;
extern oak_mutex_t test_and_set_locative_lock;
extern oak_mutex_t wp_lock;
extern oak_mutex_t dump_lock;

/* A spawned thread runs its thunk with an empty context stack, so when
   the thunk returns from its outermost frame there is no context to pop.
   The stack machinery notices the underflow and unwinds back to
   init_thread through these.  */
extern int oak_thread_can_exit(void);
extern void oak_thread_exit_unwind(void);

/* The calling thread's index for code that has no my_index local of
   its own: natively compiled code and its helpers (oak-native.h
   defines my_index as this).  A thread-local variable of the
   executable; a module loaded with dlopen reaches it in the
   initial-exec model, which is valid for a variable the executable
   itself defines and costs a load, not a call. */
#if defined(_MSC_VER) && !defined(__MINGW32__)
extern __declspec(thread) int oak_thread_index_var;
#else
extern __thread int oak_thread_index_var __attribute__((tls_model("initial-exec")));
#endif

/* Thread 0's slot, thread-local index, stacks and registers: what
   every entry point (oaklisp.c, embed-native.c) does first. */
extern void oak_thread0_init(void);

/* Around a long blocking call (the C compiler) made with this thread's
   stacks synchronized: the thread counts as at a safepoint meanwhile,
   so a collection does not wait for it, and it leaves that state under
   gc_lock, like a parked thread. */
extern void oak_thread_blocking_begin(void);
extern void oak_thread_blocking_end(void);
#endif



#ifdef THREADS
#define THREADY(x) x
#else
#define THREADY(x)
#endif

/* On Windows (MSVC), statically declared mutexes need explicit
   initialization.  Call once from main() before any threading.
   On POSIX this is a no-op. */
#ifdef THREADS
void oak_threads_system_init(void);
#else
#define oak_threads_system_init() ((void)0)
#endif

#endif /*_THREADS_H_INCLUDED*/
