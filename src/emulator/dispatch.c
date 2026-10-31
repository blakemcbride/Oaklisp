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


/* Method search, shared by the interpreter and natively compiled
   code; moved out of loop.c so that a stand-alone executable links
   without the interpreter.  See dispatch.h. */

#define _REENTRANT

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "data.h"
#include "xmalloc.h"
#include "dispatch.h"

#ifdef FAST
#define maybe_put(x, s)
#else
static inline void
maybe_put(bool v, char *s)
{
  if (v)
    {
      printf("%s", s);
      fflush(stdout);
    }
}
#endif


/* The ASSQ instruction takes its list off the value stack, so both the
   spine and every element of it are ordinary Oaklisp values that the
   program chose.  car() and cdr() dereference without looking at the
   tag, so this checks each cell the way CONSINSTR checks the cons
   instructions -- a pointer, and a pair -- before following it.  A
   false return means the instruction should trap; *RESULT is set only
   on success. */

bool
assq_checked(ref_t elt, ref_t lis, ref_t notfound, ref_t *result)
{
  while (lis != e_nil) {
    ref_t this;

    if (!TAG_IS(lis, PTR_TAG) || REF_SLOT(lis, 0) != e_cons_type)
      return false;
    this = car(lis);
    if (!TAG_IS(this, PTR_TAG) || REF_SLOT(this, 0) != e_cons_type)
      return false;
    if (car(this) == elt) {
      *result = this;
      return true;
    }
    lis = cdr(lis);
  }
  *result = notfound;
  return true;
}



/* Depth of the supertype search stack that lives on the C stack.  6 is
   enough for the current build, but a user-defined type graph can be
   arbitrarily deep, so past this point the table moves to the heap
   instead of running off the end of the frame. */
#define N_LATERS 64

void
find_method_type_pair(ref_t op,
		      ref_t obj_type,
		      ref_t *method_ptr,
		      ref_t *type_ptr)
{
  ref_t alist;
  ref_t car_cache;
  ref_t *locl = NULL;
#ifdef OP_METH_ALIST_MTF
  ref_t thelist;
  ref_t *loclist;
#endif
  /* stack of lists of types that remain to be searched */
  ref_t inline_lists[N_LATERS];
  ref_t *later_lists = inline_lists;
  size_t later_size = N_LATERS;
  size_t llp = 0;		/* index of first empty slot in table */

  while (1)			/* forever */
    {
      /* First look for it in the local method alist of obj_type: */


#ifdef OP_METH_ALIST_MTF
      alist = thelist =
	*(loclist = &REF_SLOT(obj_type, TYPE_OP_METHOD_ALIST_OFF));
#else
      alist = REF_SLOT(obj_type, TYPE_OP_METHOD_ALIST_OFF);
#endif
      while (alist != e_nil)
	{
	  if (car((car_cache = car(alist))) == op)
	    {
	      maybe_put(trace_meth, "x\n");
#ifdef OP_METH_ALIST_MTF
	      if (locl != NULL)
		{
		  *locl = cdr(alist);
		  *loclist = alist;
		  *pcdr(alist) = thelist;
		}
#endif
	      *method_ptr = cdr(car_cache);
	      *type_ptr = obj_type;
	      if (later_lists != inline_lists)
		free(later_lists);
	      return;
	    }
	  alist = *(locl = pcdr(alist));
	  maybe_put(trace_meth, "-");
	}

      /* Not found in local alist, so stack the entire supertype list
         and then fetch the top guy available on the stack. */

      if (llp == later_size)
	{
	  /* Grow the table; the first growth moves it off the C stack. */
	  size_t new_size = 2 * later_size;
	  ref_t *bigger;

	  if (later_lists == inline_lists)
	    {
	      bigger = (ref_t *) xmalloc(new_size * sizeof(ref_t));
	      memcpy(bigger, inline_lists, later_size * sizeof(ref_t));
	    }
	  else
	    {
	      bigger = (ref_t *) realloc(later_lists,
					 new_size * sizeof(ref_t));
	      if (bigger == NULL)
		{
		  fprintf(stderr, "\nERROR: out of memory while searching"
			  " a type hierarchy %lu levels deep.\n",
			  (unsigned long)later_size);
		  exit(EXIT_FAILURE);
		}
	    }
	  later_lists = bigger;
	  later_size = new_size;
	}
      later_lists[llp++] = REF_SLOT(obj_type, TYPE_SUPER_LIST_OFF);

      while (later_lists[llp - 1] == e_nil)
	{
	  if (llp == 1)
	    {
	      if (later_lists != inline_lists)
		free(later_lists);
	      return;
	    }
	  llp -= 1;
	}

      locl = NULL;
      obj_type = car(later_lists[llp - 1]);
      later_lists[llp - 1] = cdr(later_lists[llp - 1]);
    }
}




#ifdef THREADS
size_t mcache_seq[MCACHE_STRIPES];
#endif

/* Record a search's result in OP's one-entry cache: method and offset
   first, then the type, which commits it for readers.  With THREADS
   only with the stripe's count taken from even to odd; if another
   fill has it, leave the cache be. */
void
oak_mcache_fill(ref_t op, ref_t y_type, ref_t method, ref_t offset)
{
#ifdef OP_TYPE_METH_CACHE
#ifdef THREADS
  size_t *seqp = MCACHE_SEQ(op);
  size_t seq = OAK_ATOMIC_LOAD_REF(seqp);

  if (!(seq & 1) && OAK_ATOMIC_CAS_SIZE(seqp, &seq, seq + 1))
    {
      OAK_ATOMIC_FENCE_RELEASE();
      SATB_BARRIER(&REF_SLOT(op, OPERATION_CACHE_TYPE_OFF), y_type);
      OAK_ATOMIC_STORE_REF(&REF_SLOT(op, OPERATION_CACHE_TYPE_OFF), y_type);
      SATB_BARRIER(&REF_SLOT(op, OPERATION_CACHE_METH_OFF), method);
      OAK_ATOMIC_STORE_REF(&REF_SLOT(op, OPERATION_CACHE_METH_OFF), method);
      OAK_ATOMIC_STORE_REF(&REF_SLOT(op, OPERATION_CACHE_TYPE_OFF_OFF),
			   offset);
      OAK_ATOMIC_STORE_REF(seqp, seq + 2);
    }
#else
  SATB_BARRIER(&REF_SLOT(op, OPERATION_CACHE_TYPE_OFF), y_type);
  REF_SLOT(op, OPERATION_CACHE_TYPE_OFF) = y_type;
  SATB_BARRIER(&REF_SLOT(op, OPERATION_CACHE_METH_OFF), method);
  REF_SLOT(op, OPERATION_CACHE_METH_OFF) = method;
  REF_SLOT(op, OPERATION_CACHE_TYPE_OFF_OFF) = offset;
#endif
#else
  (void)op; (void)y_type; (void)method; (void)offset;
#endif
}

/* The global method cache: what find_method_type_pair answered for an
   operation and a type -- the method and the type it was found on --
   for the searches an operation's one-entry cache misses.  That cache
   holds one type, so a call that sees several types in turn misses it
   every time, and searched the type graph every time.

   Entries hold raw refs and are no roots: oak_gmc_flush, an epoch
   bump, empties the cache at every collection that frees or moves
   objects and at every world load.  ADD-METHOD (%INSTALL-METHOD-WITH-
   ENV) changes an operation's methods only after parking its one-entry
   cache (the type slot set to 0), so a search for an operation found
   parked flushes the cache first; the fill that follows unparks it.
   With THREADS an entry is read and written under its own sequence
   count, as the one-entry caches are. */

#define GMC_SIZE 4096

typedef struct
{
  size_t seq;			/* odd while being written (THREADS) */
  size_t epoch;
  ref_t op, type, method, meth_type;
} gmc_entry;

static gmc_entry gmc[GMC_SIZE];
static size_t gmc_epoch = 1;	/* never 0, so the empty entries are stale */

#define GMC_INDEX(op, type)						\
  ((((size_t)(op) >> 3) ^ (((size_t)(type) >> 3) * 31)) & (GMC_SIZE - 1))

void
oak_gmc_flush(void)
{
#ifdef THREADS
  size_t e = OAK_ATOMIC_LOAD_REF(&gmc_epoch);

  while (!OAK_ATOMIC_CAS_SIZE(&gmc_epoch, &e, e + 1))
    ;
#else
  gmc_epoch++;
#endif
}

static bool
gmc_read(ref_t op, ref_t type, ref_t *method, ref_t *meth_type)
{
  gmc_entry *g = &gmc[GMC_INDEX(op, type)];
#ifdef THREADS
  size_t s1 = OAK_ATOMIC_LOAD_REF(&g->seq);
  ref_t o, t, m, mt;
  size_t e;

  if (s1 & 1)
    return false;
  e = OAK_ATOMIC_LOAD_REF(&g->epoch);
  o = OAK_ATOMIC_LOAD_REF(&g->op);
  t = OAK_ATOMIC_LOAD_REF(&g->type);
  m = OAK_ATOMIC_LOAD_REF(&g->method);
  mt = OAK_ATOMIC_LOAD_REF(&g->meth_type);
  OAK_ATOMIC_FENCE_ACQUIRE();
  if (OAK_ATOMIC_LOAD_REF(&g->seq) != s1
      || e != OAK_ATOMIC_LOAD_REF(&gmc_epoch) || o != op || t != type)
    return false;
  *method = m;
  *meth_type = mt;
  return true;
#else
  if (g->epoch != gmc_epoch || g->op != op || g->type != type)
    return false;
  *method = g->method;
  *meth_type = g->meth_type;
  return true;
#endif
}

static void
gmc_write(ref_t op, ref_t type, ref_t method, ref_t meth_type)
{
  gmc_entry *g = &gmc[GMC_INDEX(op, type)];
#ifdef THREADS
  size_t s = OAK_ATOMIC_LOAD_REF(&g->seq);

  /* Another writer has it: leave the entry be. */
  if ((s & 1) || !OAK_ATOMIC_CAS_SIZE(&g->seq, &s, s + 1))
    return;
  OAK_ATOMIC_FENCE_RELEASE();
  OAK_ATOMIC_STORE_REF(&g->epoch, OAK_ATOMIC_LOAD_REF(&gmc_epoch));
  OAK_ATOMIC_STORE_REF(&g->op, op);
  OAK_ATOMIC_STORE_REF(&g->type, type);
  OAK_ATOMIC_STORE_REF(&g->method, method);
  OAK_ATOMIC_STORE_REF(&g->meth_type, meth_type);
  OAK_ATOMIC_STORE_REF(&g->seq, s + 2);
#else
  g->epoch = gmc_epoch;
  g->op = op;
  g->type = type;
  g->method = method;
  g->meth_type = meth_type;
#endif
}

/* True when the global cache answered.  A caller then leaves the
   operation's one-entry cache as it is: refilling it on every such hit
   is what made a call seeing several types in turn pay for a fill
   every time, and the type it holds keeps hitting there. */
bool
oak_find_method(ref_t op, ref_t obj_type, ref_t *method_ptr,
		ref_t *type_ptr)
{
#ifdef THREADS
  ref_t cached = OAK_ATOMIC_LOAD_REF(&REF_SLOT(op, OPERATION_CACHE_TYPE_OFF));
#else
  ref_t cached = REF_SLOT(op, OPERATION_CACHE_TYPE_OFF);
#endif

  if (cached == INT_TO_REF(0))
    oak_gmc_flush();		/* parked: methods changed since its last fill */
  else if (gmc_read(op, obj_type, method_ptr, type_ptr))
    {
      /* A raw ref from no root: during a mark, logged for the marker
	 (the entry may predate the snapshot). */
      SATB_READ_RAW(*method_ptr);
      return true;
    }
  find_method_type_pair(op, obj_type, method_ptr, type_ptr);
  if (*method_ptr != e_nil)
    gmc_write(op, obj_type, *method_ptr, *type_ptr);
  return false;
}

/* For ^SUPER: the method of OP for TYPE as oak_find_method finds it.
   ^SUPER fills no cache of its own, so an operation found parked
   would stay parked and flush the global cache at every ^SUPER;
   filling its one-entry cache for TYPE unparks it. */
void
oak_find_super_method(ref_t op, ref_t type, ref_t *method_ptr,
		      ref_t *type_ptr)
{
  bool parked = REF_SLOT(op, OPERATION_CACHE_TYPE_OFF) == INT_TO_REF(0);

  oak_find_method(op, type, method_ptr, type_ptr);
  if (parked && *method_ptr != e_nil)
    oak_mcache_fill(op, type, *method_ptr,
		    lookup_bp_offset(type, *type_ptr));
}

/* For natively compiled code (oak-native.h): find the method as
   FUNCALL does, filling the operation's method cache. */
ref_t
oak_native_find_method(ref_t op, ref_t y_type, ref_t *offset)
{
  /* Left alone when there is no method. */
  ref_t method = e_nil, meth_type;
  bool cached = oak_find_method(op, y_type, &method, &meth_type);

  if (method == e_nil)
    return e_nil;
  *offset = lookup_bp_offset(y_type, meth_type);
  if (!cached)
    oak_mcache_fill(op, y_type, method, *offset);
  return method;
}

