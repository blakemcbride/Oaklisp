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


/* The instructions natively compiled code cannot inline, and the call
   driver that replaces the interpreter in stand-alone executables.
   Every helper mirrors its loop.c case; see native-ops.h for the
   calling conventions. */

#define _REENTRANT

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include "config.h"
#include "data.h"
#include "stacks.h"
#include "gc.h"
#include "weak.h"
#include "worldio.h"
#include "xmalloc.h"
#include "lineedit.h"
#include "timers.h"
#include "cmdline.h"
#include "dispatch.h"
#include "native.h"
#include "native-ops.h"
#ifdef USE_MARK_SWEEP
#include "gc-ms.h"
#endif
#include "threads.h"

#include "oak-native.h"

#define NEW_STORAGE e_uninitialized

/* OAK_RT_TRACE=1 in the environment logs the control helpers. */
static int rt_trace = -1;

static int
rt_tracing(void)
{
  if (rt_trace < 0)
    rt_trace = getenv("OAK_RT_TRACE") != NULL;
  return rt_trace;
}

#define RT_LOG(...)							\
  do { if (rt_tracing())						\
	 { fprintf(stderr, __VA_ARGS__);				\
	   fprintf(stderr, " [v %ld+%d c %ld+%d]\n",			\
		   (long)(value_stack.sp - value_stack.bp + 1),		\
		   value_stack.pushed_count,				\
		   (long)(context_stack.sp - context_stack.bp + 1),	\
		   context_stack.pushed_count); } } while (0)

/* Stack heights, as VALUE_STACK_HEIGHT sees them with everything
   synchronized. */

static inline long
val_height(void)
{
  return (value_stack.sp - value_stack.bp + 1) + value_stack.pushed_count;
}

static inline long
cxt_height(void)
{
  return (context_stack.sp - context_stack.bp + 1)
    + context_stack.pushed_count;
}

static inline void
bash_val_height(long h)
{
  long to_pop = val_height() - h;

  if (to_pop > 0)
    stack_pop_n(&value_stack, to_pop);
}

static inline void
bash_cxt_height(long h)
{
  long to_pop = cxt_height() - h;

  if (to_pop > 0)
    stack_pop_n(&context_stack, to_pop);
}

/* Allocation with the operands already on the (synchronized) stack,
   where the collector finds them. */

static ref_t *
rt_allocate(size_t words, char *reason)
{
  ref_t *p;

  ALLOCATE(p, words, reason);
  return p;
}

ref_t *
oak_rt_reverse_cons(ref_t * sp)
{
  ref_t *p;
  ref_t x;

  value_stack.sp = sp;
  ALLOCATE(p, 3, "space crunch in native REVERSE-CONS");
  sp = value_stack.sp;
  x = *sp--;
  p[CONS_PAIR_CDR_OFF] = x;
  p[CONS_PAIR_CAR_OFF] = *sp;
  p[0] = e_cons_type;
  *sp = PTR_TO_REF(p);
  return sp;
}

bool
oak_rt_allocate(ref_t ** spp)
{
  ref_t *sp = *spp;
  ref_t y = sp[-1];
  ref_t *p, *fill, *end;
  size_t alloc_len;

  if (!TAG_IS(y, INT_TAG) || REF_TO_INT(y) < 1)
    return false;
  alloc_len = (size_t)REF_TO_INT(y);
  value_stack.sp = sp;
  p = rt_allocate(alloc_len, "space crunch in native ALLOCATE");
  sp = value_stack.sp;
  *p = sp[0];			/* the type */
  sp -= 1;
  sp[0] = PTR_TO_REF(p);
  for (fill = p + 1, end = p + alloc_len; fill < end;)
    *fill++ = NEW_STORAGE;
  *spp = sp;
  return true;
}

bool
oak_rt_vlen_allocate(ref_t ** spp)
{
  ref_t *sp = *spp;
  ref_t y = sp[-1];
  ref_t *p, *fill, *end;
  size_t alloc_len;

  if (!TAG_IS(y, INT_TAG) || REF_TO_INT(y) < 2)
    return false;
  alloc_len = (size_t)REF_TO_INT(y);
  value_stack.sp = sp;
  p = rt_allocate(alloc_len, "space crunch in native VLEN-ALLOCATE");
  sp = value_stack.sp;
  p[0] = sp[0];			/* the type */
  p[1] = sp[-1];		/* the length */
  sp -= 1;
  sp[0] = PTR_TO_REF(p);
  for (fill = p + 2, end = p + alloc_len; fill < end;)
    *fill++ = NEW_STORAGE;
  *spp = sp;
  return true;
}

bool
oak_rt_crunch(ref_t ** spp)
{
  ref_t *sp = *spp;
  ref_t x = sp[0], y = sp[-1];
  int tag;
  ref_t z;

  if ((((x) | (y)) & TAG_MASK) != 0)
    return false;
  tag = (int)(REF_TO_INT(y) & TAG_MASK);
  if (tag & PTR_MASK)
    {
      ssize_t i = REF_TO_INT(x);

      if (i < 0)
	return false;
      if (i < (ssize_t)spatic.size)
	z = PTR_TO_LOC(spatic.start + i);
#ifdef USE_MARK_SWEEP
      else
	{
	  ref_t *hp = ms_crunch((size_t)(i - (ssize_t)spatic.size));

	  if (hp == NULL)
	    return false;
	  z = PTR_TO_LOC(hp);
	}
#else
      else if (i < (ssize_t)(spatic.size + new_space.size))
	z = PTR_TO_LOC(new_space.start + (i - spatic.size));
      else
	return false;
#endif
    }
  else
    z = x;
  sp -= 1;
  sp[0] = z | tag;
  *spp = sp;
  return true;
}

void
oak_rt_get_data(ref_t * sp)
{
  ref_t x = sp[0];

  if (x & PTR_MASK)
    {
      ref_t *p = (x & 1) ? REF_TO_PTR(x) : LOC_TO_PTR(x);
#ifdef USE_MARK_SWEEP
      ssize_t d = SPATIC_PTR(p) ? -1 : ms_get_data(p);

      sp[0] =
	INT_TO_REF(SPATIC_PTR(p) ? p - spatic.start
		   : d >= 0 ? d + (ssize_t)spatic.size
		   : (printf("GET-DATA of "),
		      printref(stdout, x),
		      printf("\n"), -(ssize_t)p - 1));
#else
      sp[0] =
	INT_TO_REF(SPATIC_PTR(p) ? p - spatic.start
		   : NEW_PTR(p) ? (p - new_space.start) + spatic.size
		   : (printf("GET-DATA of "),
		      printref(stdout, x),
		      printf("\n"), -(ssize_t)p - 1));
#endif
    }
  else
    sp[0] = (x & ~TAG_MASKL) | INT_TAG;
}

bool
oak_rt_assq(ref_t ** spp)
{
  ref_t *sp = *spp;
  ref_t result;

  if (!assq_checked(sp[0], sp[-1], e_false, &result))
    return false;
  sp -= 1;
  sp[0] = result;
  *spp = sp;
  return true;
}

bool
oak_rt_ash(ref_t ** spp)
{
  ref_t *sp = *spp;
  ref_t x = sp[0], y = sp[-1];
  const unsigned fixnum_width = OAK_WORD_SIZE - TAGSIZE;
  ssize_t a, b;

  if ((((x) | (y)) & TAG_MASK) != 0)
    return false;
  /* The argument order on the stack is the reverse of the emulator's
     POPVAL order: x is the number, y the shift count. */
  a = REF_TO_INT(x);
  b = REF_TO_INT(y);
  if (b < 0)
    {
      size_t count = (size_t)(-b);

      sp -= 1;
      sp[0] = (count >= fixnum_width)
	? INT_TO_REF(a < 0 ? -1 : 0) : INT_TO_REF(a >> count);
    }
  else if (a == 0 || b == 0)
    {
      sp -= 1;
      sp[0] = INT_TO_REF(a);
    }
  else
    {
      ssize_t r;

      if ((size_t)b >= fixnum_width)
	return false;
      r = (ssize_t)((size_t)a << b);
      if ((r >> b) != a)
	return false;
      {
	unsigned highcrap
	  = ((size_t)(r)) >> (OAK_WORD_SIZE - (TAGSIZE + 1));

	if ((highcrap != 0x0) && (highcrap != 0x7))
	  return false;
      }
      sp -= 1;
      sp[0] = INT_TO_REF(r);
    }
  *spp = sp;
  return true;
}

ref_t *
oak_rt_rot(ref_t * sp)
{
  ref_t x = sp[0], y = sp[-1];
  const ssize_t width = OAK_WORD_SIZE - TAGSIZE;
  size_t a = (size_t)x;
  ssize_t b = REF_TO_INT(y);

  b %= width;
  if (b < 0)
    b += width;
  sp -= 1;
  sp[0] = (b == 0)
    ? (ref_t) (a & ~TAG_MASKL)
    : (ref_t) ((a << b | a >> (width - b)) & ~TAG_MASKL);
  return sp;
}

static ref_t *
push_room(ref_t * sp)
{
  if (sp + 1 >= value_stack.bp + value_stack.size)
    {
      value_stack.sp = sp;
      stack_flush(&value_stack, value_stack.filltarget);
      sp = value_stack.sp;
    }
  return sp;
}

ref_t *
oak_rt_getc(ref_t * sp)
{
  int c = oak_getc(stdin);

  sp = push_room(sp);
  *++sp = (c == EOF ? e_nil : CHAR_TO_REF(c));
  return sp;
}

bool
oak_rt_putc(ref_t * sp)
{
  ref_t x = sp[0];

  if (!SUBTAG_IS(x, CHAR_SUBTAG))
    return false;
  putc(REF_TO_CHAR(x), stdout);
  oak_note_putc(REF_TO_CHAR(x), stdout);
  fflush(stdout);
  return true;
}

void
oak_rt_load_length(ref_t * sp)
{
  ref_t x = sp[0];

  sp[0] =
    (TAG_IS(x, PTR_TAG) ?
     (REF_SLOT(REF_SLOT(x, 0), TYPE_VAR_LEN_P_OFF) == e_false ?
      REF_SLOT(REF_SLOT(x, 0), TYPE_LEN_OFF) : REF_SLOT(x, 1)) :
     INT_TO_REF(0));
}

int
oak_rt_stream_primitive(unsigned n, ref_t ** spp)
{
  ref_t *sp = *spp;
  ref_t x, y;

  switch (n)
    {
    case 0:
      sp = push_room(sp);
      *++sp = file_to_ref(stdin);
      break;
    case 1:
      sp = push_room(sp);
      *++sp = file_to_ref(stdout);
      break;
    case 2:
      sp = push_room(sp);
      *++sp = file_to_ref(stderr);
      break;

    case 3:
    case 4:
    case 5:
      x = sp[0];
      if (!TAG_IS(x, LOC_TAG))
	return 2;
      {
	char *s = oak_c_string((ref_t *) LOC_TO_PTR(x), REF_TO_INT(sp[-1]));
	FILE *fd;

	if (trace_files)
	  printf("About to open '%s'.\n", s);
	fd = fopen(s, n == 3 ? READ_MODE : n == 4 ? WRITE_MODE : APPEND_MODE);
	free(s);
	sp -= 1;
	sp[0] = (fd == NULL) ? e_false : file_to_ref(fd);
      }
      break;

    case 6:
      sp[0] = BOOL_TO_REF(fclose(ref_to_file(sp[0])) != EOF);
      break;
    case 7:
      sp[0] = BOOL_TO_REF(fflush(ref_to_file(sp[0])) != EOF);
      break;

    case 8:
      y = sp[-1];
      if (!SUBTAG_IS(y, CHAR_SUBTAG))
	return 2;
      x = sp[0];
      sp -= 1;
      sp[0] = BOOL_TO_REF(putc(REF_TO_CHAR(y), ref_to_file(x)) != EOF);
      oak_note_putc(REF_TO_CHAR(y), ref_to_file(x));
      break;

    case 9:
      {
	int c = oak_getc(ref_to_file(sp[0]));

	if (c == EOF)
	  {
	    if (ISATTY(ref_to_file(sp[0])))
	      {
		if (trace_files)
		  printf("Clearing EOF.\n");
		clearerr(ref_to_file(sp[0]));
	      }
	    sp[0] = e_nil;
	  }
	else
	  sp[0] = CHAR_TO_REF(c);
      }
      break;

    case 10:
      sp[0] = ISATTY(ref_to_file(sp[0])) ? e_t : e_nil;
      break;

    case 11:
      {
	long pos = ftell(ref_to_file(sp[0]));

	sp[0] = (pos < 0) ? e_nil : INT_TO_REF(pos);
      }
      break;

    case 12:
      {
	FILE *fd = ref_to_file(sp[0]);
	long i = (long)REF_TO_INT(sp[-1]);

	sp -= 1;
	sp[0] = fseek(fd, i, 0) == 0 ? e_t : e_nil;
      }
      break;

    case 13:
      x = sp[0];
      if (!TAG_IS(x, LOC_TAG))
	return 2;
      {
	char *s = oak_c_string((ref_t *) LOC_TO_PTR(x), REF_TO_INT(sp[-1]));

	sp -= 1;
	sp[0] = chdir(s) == 0 ? e_t : e_nil;
	free(s);
      }
      break;

    case 14:
      {
	char *from, *to;

	if (!TAG_IS(sp[0], LOC_TAG) || !TAG_IS(sp[-2], LOC_TAG))
	  return 4;
	from = oak_c_string((ref_t *) LOC_TO_PTR(sp[0]), REF_TO_INT(sp[-1]));
	to = oak_c_string((ref_t *) LOC_TO_PTR(sp[-2]), REF_TO_INT(sp[-3]));
	if (trace_files)
	  printf("About to rename '%s' to '%s'.\n", from, to);
	sp -= 3;
	sp[0] = rename(from, to) == 0 ? e_t : e_nil;
	free(from);
	free(to);
      }
      break;

    case 15:
      x = sp[0];
      if (!TAG_IS(x, LOC_TAG))
	return 2;
      {
	char *s = oak_c_string((ref_t *) LOC_TO_PTR(x), REF_TO_INT(sp[-1]));

	sp -= 1;
	sp[0] = oak_native_load(s) ? e_t : e_nil;
	free(s);
      }
      break;

    case 16:
      {
	char *c_file, *so_file;

	if (!TAG_IS(sp[0], LOC_TAG) || !TAG_IS(sp[-2], LOC_TAG))
	  return 4;
	c_file = oak_c_string((ref_t *) LOC_TO_PTR(sp[0]),
			      REF_TO_INT(sp[-1]));
	so_file = oak_c_string((ref_t *) LOC_TO_PTR(sp[-2]),
			       REF_TO_INT(sp[-3]));
	sp -= 3;
	/* The C compiler takes a while: with THREADS this thread
	   counts as parked meanwhile (the stack is synchronized for
	   a collection in another thread). */
	{
	  bool built;

	  value_stack.sp = sp;
	  THREADY(oak_thread_blocking_begin());
	  built = oak_native_cc(c_file, so_file);
	  THREADY(oak_thread_blocking_end());
	  sp = value_stack.sp;
	  sp[0] = built ? e_t : e_nil;
	}
	free(c_file);
	free(so_file);
      }
      break;

    case 17:
      x = sp[0];
      if (!TAG_IS(x, INT_TAG))
	return 1;
      {
	ssize_t i = REF_TO_INT(x);

	if (oak_embedded_program == NULL)
	  sp[0] = e_nil;
	else if (i == -1)
	  sp[0] = e_t;
	else if (i < 0 || (size_t)i >= oak_embedded_program_size)
	  sp[0] = e_nil;
	else
	  sp[0] = CHAR_TO_REF(oak_embedded_program[i]);
      }
      break;

    case 18:
      {
	char *first, *second;
	struct stat st1, st2;

	if (!TAG_IS(sp[0], LOC_TAG) || !TAG_IS(sp[-2], LOC_TAG))
	  return 4;
	first = oak_c_string((ref_t *) LOC_TO_PTR(sp[0]), REF_TO_INT(sp[-1]));
	second = oak_c_string((ref_t *) LOC_TO_PTR(sp[-2]), REF_TO_INT(sp[-3]));
	sp -= 3;
	sp[0] = (stat(first, &st1) == 0
		 && stat(second, &st2) == 0
		 && st1.st_mtime > st2.st_mtime) ? e_t : e_nil;
	free(first);
	free(second);
      }
      break;

      /* Parking and sleeping block with the stack synchronized: the
	 thread is at a safepoint meanwhile (threads.c). */
    case 19:
      x = sp[0];
      if (x != e_nil && !TAG_IS(x, INT_TAG))
	return 1;
      value_stack.sp = sp;
      y = oak_thread_park(x == e_nil ? -1L : (long)REF_TO_INT(x))
	? e_t : e_nil;
      sp = value_stack.sp;
      sp[0] = y;
      break;
    case 20:
      x = sp[0];
      if (!TAG_IS(x, INT_TAG))
	return 1;
      sp[0] = BOOL_TO_REF(oak_thread_unpark((int)REF_TO_INT(x)));
      break;
    case 21:
      if (!TAG_IS(sp[0], INT_TAG))
	return 1;
      value_stack.sp = sp;
      oak_thread_sleep_ms((long)REF_TO_INT(sp[0]));
      sp = value_stack.sp;
      sp[0] = e_t;
      break;

    default:
      fprintf(stderr, "\nError (native code): bad stream primitive %u.\n", n);
      maybe_dump_world(333);
      exit(EXIT_FAILURE);
    }
  *spp = sp;
  return -1;
}

bool
oak_rt_throw(void)
{
  ref_t x = value_stack.sp[0];

  RT_LOG("throw");
  ref_t y;
  long new_val_height, new_cxt_height;

  if (!TAG_IS(x, PTR_TAG))
    return false;
  new_val_height = (long)REF_TO_INT(REF_SLOT(x, ESCAPE_OBJECT_VAL_OFF));
  new_cxt_height = (long)REF_TO_INT(REF_SLOT(x, ESCAPE_OBJECT_CXT_OFF));
  /* The heights come out of an ordinary heap object; see loop.c. */
  if (new_val_height < 0 || new_val_height > val_height() - 1
      || new_cxt_height < 0 || new_cxt_height > cxt_height())
    return false;
  value_stack.sp -= 1;		/* the escape object */
  y = value_stack.sp[0];	/* the value thrown */
  bash_val_height(new_val_height);
  bash_cxt_height(new_cxt_height);
  if (value_stack.sp + 1 >= value_stack.bp + value_stack.size)
    stack_flush(&value_stack, value_stack.filltarget);
  *++value_stack.sp = y;
  oak_native_pop_context();
  return true;
}

void
oak_rt_fill_continuation(void)
{
  ref_t x;

  RT_LOG("fill-continuation");
  if (!TAG_IS(value_stack.sp[0], PTR_TAG) && rt_tracing())
    fprintf(stderr, "fill-continuation: TOP NOT A POINTER: %#zx\n",
	    (size_t)value_stack.sp[0]);

  if (1 > (long)(value_stack.sp - value_stack.bp))
    stack_unflush(&value_stack, 1);
  stack_flush(&value_stack, 2);
  stack_flush(&context_stack, 0);
  x = value_stack.sp[0];
  ON_STORE(&REF_SLOT(x, CONTINUATION_VAL_SEGS), value_stack.segment);
  ON_STORE(&REF_SLOT(x, CONTINUATION_VAL_OFF),
	   INT_TO_REF(value_stack.pushed_count));
  ON_STORE(&REF_SLOT(x, CONTINUATION_CXT_SEGS), context_stack.segment);
  ON_STORE(&REF_SLOT(x, CONTINUATION_CXT_OFF),
	   INT_TO_REF(context_stack.pushed_count));
}

bool
oak_rt_continue(void)
{
  ref_t x = value_stack.sp[0];

  RT_LOG("continue");
  ref_t y;

  if (!TAG_IS(x, PTR_TAG))
    return false;
  value_stack.sp -= 1;
  y = value_stack.sp[0];
  value_stack.segment = REF_SLOT(x, CONTINUATION_VAL_SEGS);
  value_stack.pushed_count = REF_TO_INT(REF_SLOT(x, CONTINUATION_VAL_OFF));
  value_stack.sp = value_stack.bp;
  value_stack.sp[0] = y;
  context_stack.segment = REF_SLOT(x, CONTINUATION_CXT_SEGS);
  context_stack.pushed_count =
    REF_TO_INT(REF_SLOT(x, CONTINUATION_CXT_OFF));
  context_stack.sp = context_stack.bp - 1;
  oak_native_pop_context();
  return true;
}

void
oak_rt_gc(int full)
{
#ifdef USE_MARK_SWEEP
  (void)full;
  ms_collect_explicit("explicit call");
#else
  gc(false, full != 0, "explicit call", 0);
#endif
  if (value_stack.sp + 1 >= value_stack.bp + value_stack.size)
    stack_flush(&value_stack, value_stack.filltarget);
  *++value_stack.sp = e_false;
}

void
oak_rt_halt(int code)
{
  maybe_dump_world(code);
  exit(code);
}

bool
oak_rt_dump_world(ref_t ** spp)
{
  ref_t *sp = *spp;
  ref_t x = sp[0];
  char *s, *old_dump_file_name;

  if (!TAG_IS(x, LOC_TAG))
    return false;
  s = oak_c_string((ref_t *) LOC_TO_PTR(x), REF_TO_INT(sp[-1]));
  THREADY(oak_mutex_lock(&dump_lock));
  old_dump_file_name = dump_file_name;
  dump_file_name = s;
  sp -= 1;
  value_stack.sp = sp;
  gc(false, false, "impending world dump", 0);
  dump_world(false);
  sp = value_stack.sp;
  dump_file_name = old_dump_file_name;
  THREADY(oak_mutex_unlock(&dump_lock));
  free(s);
  sp[0] = e_t;
  *spp = sp;
  return true;
}

bool
oak_rt_load_world(ref_t ** spp)
{
  ref_t *sp = *spp;
  ref_t x = sp[0];
  char *s;

  if (!TAG_IS(x, LOC_TAG))
    return false;
  s = oak_c_string((ref_t *) LOC_TO_PTR(x), REF_TO_INT(sp[-1]));

  THREADY(oak_mutex_lock(&dump_lock));
  free_space(&spatic);
#ifdef USE_MARK_SWEEP
  ms_destroy();
#else
  free_space(&new_space);
#endif
  read_world(s);
  free(s);

#ifdef USE_MARK_SWEEP
  ms_init();
#else
  new_space.size = e_next_newspace_size = original_newspace_size;
  alloc_space(&new_space, new_space.size);
  free_point = new_space.start;
#endif
  clear_program_args();

  value_stack.sp = value_stack.bp;
  *value_stack.bp = INT_TO_REF(1234);
  value_stack.segment = 0;
  value_stack.pushed_count = 0;
  context_stack.sp = context_stack.bp;
  *context_stack.bp = INT_TO_REF(1234);
  context_stack.segment = 0;
  context_stack.pushed_count = 0;

  e_current_method = e_boot_code;
  e_env = REF_TO_PTR(REF_SLOT(e_current_method, METHOD_ENV_OFF));
  e_code_segment = REF_SLOT(e_current_method, METHOD_CODE_OFF);
  e_pc = CODE_SEG_FIRST_INSTR(e_code_segment);
  e_bp = e_env;
  e_nargs = 0;

  *++value_stack.sp = INT_TO_REF(4321);
  *++value_stack.sp = INT_TO_REF(54321);
  *spp = value_stack.sp;
  THREADY(oak_mutex_unlock(&dump_lock));
  return true;
}

int
oak_rt_super_call(oak_rt_fn * fn)
{
  ref_t *sp = value_stack.sp;

  RT_LOG("super");
  ref_t the_type, x, y, y_type, meth_type, offset, seg;
  oak_native_fn f;

  the_type = sp[0];
  if (!TAG_IS(the_type, PTR_TAG))
    return 0;
  x = sp[-1];			/* the operation */
  if (!TAG_IS(x, PTR_TAG))
    return 0;
  sp -= 1;			/* pop the type; the operation stays */
  value_stack.sp = sp;
  if (1 > (long)(sp - value_stack.bp))
    {
      stack_unflush(&value_stack, 1);
      sp = value_stack.sp;
    }
  y = sp[-1];			/* self */
  y_type = get_type(y);

  e_current_method = e_nil;
  oak_find_super_method(x, the_type, &e_current_method, &meth_type);
  if (e_current_method == e_nil)
    {
      /* No handler: put the type back and let the caller trap. */
      if (sp + 1 >= value_stack.bp + value_stack.size)
	{
	  stack_flush(&value_stack, value_stack.filltarget);
	  sp = value_stack.sp;
	}
      *++sp = the_type;
      value_stack.sp = sp;
      return 0;
    }
  offset = lookup_bp_offset(y_type, meth_type);
  e_bp = REF_TO_PTR(y) + REF_TO_INT(offset);

  x = e_current_method;
  e_env = REF_TO_PTR(REF_SLOT(x, METHOD_ENV_OFF));
  seg = REF_SLOT(x, METHOD_CODE_OFF);
  e_code_segment = seg;
  e_pc = CODE_SEG_FIRST_INSTR(seg);
  if ((f = oak_native_known(seg)) == NULL)
    return 1;
  e_pc += 1;
  *fn = f;
  return 2;
}

ref_t *
oak_rt_enable_alarms(ref_t * sp)
{
  oak_timer_increment = 1;
  sp = push_room(sp);
  *++sp = e_nil;
  return sp;
}

ref_t *
oak_rt_disable_alarms(ref_t * sp)
{
  oak_timer_increment = 0;
  oak_timer_counter = 0;
  sp = push_room(sp);
  *++sp = e_nil;
  return sp;
}

ref_t *
oak_rt_reset_alarm_counter(ref_t * sp)
{
  oak_timer_counter = 0;
  sp = push_room(sp);
  *++sp = e_nil;
  return sp;
}

/* TEST-AND-SET-LOCATIVE, as loop.c does it: with THREADS one
   compare-and-swap. */
ref_t
oak_rt_test_and_set(ref_t * p, ref_t old, ref_t new)
{
#ifdef THREADS
  SATB_BARRIER(p, new);
  return OAK_ATOMIC_CAS_SIZE(p, &old, new) ? e_t : e_false;
#else
  if (*p != old)
    return e_false;
  SATB_STORE(p, new);
  return e_t;
#endif
}

ref_t
oak_rt_make_thread(ref_t op)
{
#ifdef THREADS
  return BOOL_TO_REF(create_thread(op));
#else
  (void)op;
  return e_nil;
#endif
}

void
oak_rt_thread_yield(void)
{
#ifdef THREADS
  oak_thread_yield();
#endif
}

/* The call driver: the execution engine of a stand-alone executable,
   where every method is native.  It does what the emulator's FUNCALL
   and NATIVE instructions do, and nothing else: there is no bytecode
   interpreter behind it. */

static void
run_signal_trap(void)
{
  /* The emulator reports a pending signal by pretending a NOOP
     failed (or instruction 127 for the alarm clock): the handler gets
     the old NARGS and must put things back.  e_pc is at the NATIVE
     instruction to resume. */
  ref_t *c;
  int alarm = 0;

  if (signal_poll_flag THREADY(&& my_index == 0))
    signal_poll_flag = 0;
  else
    {
      alarm = 127;
      oak_timer_counter = 0;
    }
  if (context_stack.sp + CONTEXT_FRAME_SIZE
      >= context_stack.bp + context_stack.size)
    stack_flush(&context_stack, context_stack.filltarget);
  c = context_stack.sp;
  c[1] = INT_TO_REF((ref_t) ((char *)e_pc - (char *)e_code_segment));
  c[2] = e_current_method;
  c[3] = PTR_TO_LOC(e_bp);
  context_stack.sp = c + CONTEXT_FRAME_SIZE;

  if (value_stack.sp + 2 >= value_stack.bp + value_stack.size)
    stack_flush(&value_stack, value_stack.filltarget);
  *++value_stack.sp = INT_TO_REF((ssize_t)e_nargs);
  *++value_stack.sp = e_argless_tag_trap_table[alarm];
  e_nargs = 1;
}

static void
run_funcall_trap(void)
{
  /* FUNCALL with something the dispatch cannot call: hand everything
     to the FUNCALL trap operation, as the emulator's funcall_tail
     does for a failing FUNCALL-TAIL (no context frame). */
  unsigned old_nargs = e_nargs;

  if (value_stack.sp + 2 >= value_stack.bp + value_stack.size)
    stack_flush(&value_stack, value_stack.filltarget);
  *++value_stack.sp = INT_TO_REF((ssize_t)old_nargs);
  *++value_stack.sp = e_arged_tag_trap_table[22];
  e_nargs = old_nargs + 2;
}

/* Something to deliver before running on: a signal (thread 0's) or
   the alarm clock.  A collection another thread asked for is waited
   out first, here, where this thread's stacks are synchronized. */
static int
run_pending(void)
{
#ifdef THREADS
  if (OAK_LOAD_RELAXED(&gc_pending))
    wait_for_gc();
#endif
  return (signal_poll_flag THREADY(&& my_index == 0))
    || (oak_timer_increment && oak_timer_counter > OAK_TIMER_LIMIT);
}

static void run_loop(oak_native_fn fn, unsigned k);

void
oak_native_run(void)
{
  oak_native_fn fn;

  /* Enter the boot method. */
  fn = oak_native_known(e_code_segment);
  if (fn == NULL)
    {
      fprintf(stderr, "Fatal error (native run): the boot code is not "
	      "native.\n");
      exit(EXIT_FAILURE);
    }
  e_pc = CODE_SEG_FIRST_INSTR(e_code_segment) + 1;
  run_loop(fn, 0);
}

/* A new OS thread (threads.c): call START_OPERATION with no arguments
   -- what loop() does for one, from its tail-recursion instruction --
   and drive it until its thunk returns, which underflows this thread's
   context stack and unwinds out of here. */
void
oak_native_run_thread(ref_t start_operation)
{
  oak_native_fn fn = NULL;

  if (value_stack.sp + 2 >= value_stack.bp + value_stack.size)
    stack_flush(&value_stack, value_stack.filltarget);
  *++value_stack.sp = INT_TO_REF(4321);
  *++value_stack.sp = start_operation;
  e_nargs = 0;
  e_code_segment = e_nil;
  e_pc = NULL;
  /* As the driver does for a function that returned OAK_NATIVE_FUNCALL:
     a null function with that result enters the call path. */
  run_loop(fn, 0);
}

static void
run_loop(oak_native_fn fn, unsigned k)
{
  int r;

  for (;;)
    {
      r = fn == NULL ? OAK_NATIVE_FUNCALL : fn(k);

      if (r == OAK_NATIVE_FUNCALL)
	{
	  /* The ordinary call path refused: a pending signal, or
	     something that needs the trap operation. */
	  switch (oak_native_call_1(&fn, 1))
	    {
	    case 2:
	      break;
	    case 1:
	      goto bytecode;
	    default:
	      /* Hand everything to the FUNCALL trap operation. */
	      run_funcall_trap();
	      switch (oak_native_call_1(&fn, 1))
		{
		case 2:
		  break;
		case 1:
		  goto bytecode;
		default:
		  fprintf(stderr, "Fatal error (native run): the trap "
			  "operation cannot be called.\n");
		  exit(EXIT_FAILURE);
		}
	    }
	  if (run_pending())
	    {
	      /* Deliver the signal (or alarm) at the callee's entry,
		 the way the emulator's NATIVE case polls before
		 running: a frame resuming at its NATIVE 0, then the
		 handler. */
	      e_pc = CODE_SEG_FIRST_INSTR(e_code_segment);
	      run_signal_trap();
	      switch (oak_native_call_1(&fn, 1))
		{
		case 2:
		  k = 0;
		  continue;
		default:
		  goto bytecode;
		}
	    }
	  k = 0;
	  continue;
	}

      /* OAK_NATIVE_CONTINUE: run from e_pc, which must sit on a
	 NATIVE instruction of a native code vector. */
      if (run_pending())
	{
	  /* e_pc is at the instruction to resume (ON_POLL); deliver
	     the signal first, as the emulator's intr_trap does. */
	  run_signal_trap();
	  switch (oak_native_call_1(&fn, 1))
	    {
	    case 2:
	      k = 0;
	      continue;
	    default:
	      goto bytecode;
	    }
	}
      {
	instr_t i = *e_pc;
	unsigned op = ((i >> 2) & 0x3F) - ON_NATIVE_OPCODE;

	if (op < ON_NATIVE_OPCODES
	    && (fn = oak_native_known(e_code_segment)) != NULL)
	  {
	    k = (op << 8) | (i >> 8);
	    e_pc += 1;
	    continue;
	  }
      }

    bytecode:
      fprintf(stderr, "Fatal error (native run): bytecode reached "
	      "(no interpreter in this executable).\n");
      fprintf(stderr, "  method ");
      printref(stderr, e_current_method);
      fprintf(stderr, "\n");
      exit(EXIT_FAILURE);
    }
}
