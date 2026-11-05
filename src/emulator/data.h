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


#ifndef _DATA_H_INCLUDED
#define _DATA_H_INCLUDED

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include "config.h"
#include "threads.h"

/* bool with pointer passed to getopt */

typedef int bool_int;

/* reference type */

typedef size_t ref_t;

/* instruction type */

typedef uint16_t instr_t;

/* space type */

typedef struct {
  ref_t *start;
  ref_t *end;
  size_t size;		/* in size reference_t */
} space_t;


extern space_t new_space, old_space, spatic;
extern ref_t *free_point;

/* Size of first newspace, in K words.  For the mark-sweep collector
   it is the heap's budget (gc-ms.c): regions are mapped as needed up
   to it, and it is set after every cycle to three times the live
   data, never below this.  Every cycle scans spatic and the roots and
   marks the live data, so it starts four times bigger: the same
   program then cycles a quarter as often. */
#ifdef USE_MARK_SWEEP
#define DEFAULT_NEWSPACE 512
#else
#define DEFAULT_NEWSPACE 128
#endif

/* The following is for stack debugging */
#define PATTERN 0x0a0b0c0d


#define CONTEXT_FRAME_SIZE 3	/* not a tunable parameter */


/* Garbage collection */

#define GC_EXAMINE_BUFFER_SIZE 16
#ifdef THREADS
extern ref_t gc_examine_buffer_array[MAX_THREAD_COUNT][GC_EXAMINE_BUFFER_SIZE];
extern ref_t *gc_examine_ptr_array[MAX_THREAD_COUNT];
#define gc_examine_buffer	gc_examine_buffer_array[my_index]
#define gc_examine_ptr		gc_examine_ptr_array[my_index]
#else
extern ref_t gc_examine_buffer[GC_EXAMINE_BUFFER_SIZE];
extern ref_t *gc_examine_ptr;
#endif

/* Virtual Machine registers */

typedef struct {
  ref_t     *e_bp;
  ref_t     *e_env;
  ref_t     e_current_method;
  ref_t     e_code_segment;
  instr_t *e_pc;
  unsigned  e_nargs;
  ref_t     e_process;
#ifdef THREADS
  /* The alarm clock (ENABLE-ALARMS...) is per thread.  The interpreter
     keeps it in locals and synchronizes them here at every
     UNLOCALIZE/LOCALIZE, which is how natively compiled code, which
     reads and sets oak_timer_* through these, shares it. */
  unsigned  timer_counter;
  unsigned  timer_increment;
#endif
} register_set_t;


#ifdef THREADS
extern ref_t
  e_t, e_nil, e_fixnum_type, e_loc_type, e_cons_type, e_env_type,
 *e_subtype_table, e_object_type, e_segment_type, e_boot_code,
 *e_arged_tag_trap_table, *e_argless_tag_trap_table,
  e_uninitialized, e_method_type, e_operation_type;
#else
extern ref_t
 *e_bp, *e_env, e_t, e_nil, e_fixnum_type, e_loc_type, e_cons_type, e_env_type,
 *e_subtype_table, e_object_type, e_segment_type, e_boot_code, e_code_segment,
 *e_arged_tag_trap_table, *e_argless_tag_trap_table, e_current_method,
  e_uninitialized, e_method_type, e_operation_type, e_process;

extern instr_t *e_pc;

extern unsigned e_nargs;
#endif

#define e_false e_nil

extern size_t e_next_newspace_size, original_newspace_size;

extern char *world_file_name;
extern char *dump_file_name;
extern int dump_base;

extern bool dump_flag;

/* The alarm clock, shared between the interpreter and native code;
   per thread with THREADS (register_set_t). */
#ifndef THREADS
extern unsigned oak_timer_counter, oak_timer_increment;
#endif

/* threads.c, in every configuration: the live OS threads (1 without
   threads), and the parking of the thread stream primitives. */
extern int oak_live_thread_count(void);
extern int oak_thread_park(long ms);
extern int oak_thread_unpark(int idx);
extern void oak_thread_sleep_ms(long ms);
#define OAK_TIMER_LIMIT 1000
extern bool gc_before_dump;

extern int trace_gc;
extern bool_int trace_traps;
extern bool_int batch_mode;

#ifndef FAST

extern bool_int trace_insts;
extern bool_int trace_segs;
extern bool_int trace_valcon;
extern bool_int trace_cxtcon;
extern bool_int trace_stks;
extern bool_int trace_meth;

#ifdef OP_TYPE_METH_CACHE
extern bool_int trace_mcache;
#endif

#endif

extern bool_int trace_files;


/* miscellanous */

#ifndef ISATTY
#if defined(_MSC_VER)
#define ISATTY(stream) (_isatty(_fileno(stream)))
#else
#define ISATTY(stream) (isatty(fileno(stream)))
#endif
#endif


#define READ_MODE "r"
#define WRITE_MODE "w"
#define APPEND_MODE "a"
#define READ_BINARY_MODE "rb"
#define WRITE_BINARY_MODE "wb"



/* Tag Scheme */

#define SIGN_16BIT_ARG(x)	((int16_t)(x))

#define TAGSIZE 2

/* Architecture description, as used in file headers and in the names
   of prebuilt/ subdirectories.  The bytecode architecture name is
   "bc<instructions-per-ref>-<word-size>", e.g. "bc2-64", and the world
   architecture name adds endianness, e.g. "bc2-el64".  Keep these in
   sync with src/world/architecture.oak and oak-cold-linker.c. */

#if SIZEOF_VOID_P == 8
#define OAK_WORD_SIZE  64
#else
#define OAK_WORD_SIZE  32
#endif

#ifdef WORDS_BIGENDIAN
#define OAK_ENDIAN_NAME "big"
#define OAK_ENDIAN_ABBREV "eb"
#else
#define OAK_ENDIAN_NAME "little"
#define OAK_ENDIAN_ABBREV "el"
#endif

#define OAK_STRINGIFY_(x) #x
#define OAK_STRINGIFY(x) OAK_STRINGIFY_(x)

#define OAK_BYTECODE_ARCH_NAME \
  "bc" OAK_STRINGIFY(INSTRS_PER_REF) "-" OAK_STRINGIFY(OAK_WORD_SIZE)
#define OAK_WORLD_ARCH_NAME \
  "bc" OAK_STRINGIFY(INSTRS_PER_REF) "-" OAK_ENDIAN_ABBREV OAK_STRINGIFY(OAK_WORD_SIZE)

/* REF_SHIFT = log2(sizeof(ref_t)).  On 32-bit this equals TAGSIZE (2),
   but on 64-bit it is 3.  Used for converting word indices to byte
   offsets when constructing or deconstructing zero-based tagged refs
   in world image I/O. */
#if OAK_WORD_SIZE == 64
#define REF_SHIFT 3
#else
#define REF_SHIFT 2
#endif

/* Number of 16-bit instructions packed per ref (logical; INSTRS_PER_REF,
   from configure: 2, or 4 with 64-bit refs), and number of instr_t
   units per ref (physical stride).  With bc2 on 64-bit there is a
   gap: 2 instructions occupy the first 32 bits (in memory order) of
   each 64-bit ref, with the other 32 bits empty -- the low half of
   the ref value on little-endian machines and the high half on
   big-endian ones.  With bc4-64 and bc2-32 the ref is full. */
#ifndef INSTRS_PER_REF
#define INSTRS_PER_REF  2
#endif
#define INSTR_STRIDE    (OAK_WORD_SIZE / 16)
#if INSTRS_PER_REF > INSTR_STRIDE
#error "more instructions per ref than fit in a ref"
#endif

/* Characters packed into each word of a string: as many bytes as the
   fixnum holds.  Part of the world format (see worldio.c). */
#define CHARS_PER_WORD ((OAK_WORD_SIZE - 2) / 8)

#define TAG_MASK	3
#define TAG_MASKL	3l
#define SUBTAG_MASK	0xff
#define SUBTAG_MASKL	0xffl

#define INT_TAG		0
#define IMM_TAG		1
#define LOC_TAG 	2
#define PTR_TAG		3

#define PTR_MASK	2

#define CHAR_SUBTAG	 IMM_TAG

#define TAG_IS(X,TAG)		(((X)&TAG_MASK)==(TAG))
#define SUBTAG_IS(X,SUBTAG)	(((X)&SUBTAG_MASK)==(SUBTAG))


/* #define OR_TAG */

#define REF_TO_INT(r)   ((ssize_t)(r)>>TAGSIZE)


#define REF_TO_PTR(r)	((ref_t*)((r)-PTR_TAG))
/*
   #define REF_TO_PTR(r)   ((ref_t*)((r)&~3ul))
 */
/* This maybe used in slot calculations, where tag corrections
   can be done by the address calculation unit */
#define REF_TO_PTR_ADDR(r) ((ref_t*)((r) - PTR_TAG))


#define LOC_TO_PTR(r)	((ref_t*)((r) - LOC_TAG))
#define ANY_TO_PTR(r)	((ref_t*)((r) & ~TAG_MASKL))

#ifndef OR_TAG
#define PTR_TO_LOC(p)	((ref_t)((ref_t)(p) + LOC_TAG))
#define PTR_TO_REF(p)	((ref_t)((ref_t)(p) + PTR_TAG))
#else
#define PTR_TO_LOC(p)   ((ref_t)((ref_t)(p) | LOC_TAG))
#define PTR_TO_REF(p)   ((ref_t)((ref_t)(p) | PTR_TAG))
#endif

/* Put q's tag onto p */
#define PTR_TO_TAGGED(p,q) ((ref_t)((ref_t)(p) + ((q) & TAG_MASK)))

#define REF_TO_CHAR(r)	((char)((r)>>8))
#ifndef OR_TAG
#define CHAR_TO_REF(c)	(((ref_t)(c)<<8) + CHAR_SUBTAG)
#else
#define CHAR_TO_REF(c)  (((ref_t)(c)<<8) | IMM_TAG)
#endif

/* The shift is done in an unsigned type: left-shifting a negative
   signed value is undefined behavior in C99/C11, and fixnums are
   routinely negative.  The unsigned result has the two's complement
   bit pattern we want.  (REF_TO_INT's arithmetic right shift of a
   negative value is merely implementation-defined, and every compiler
   we target sign-extends, so it is left alone.) */

#ifndef OR_TAG
#define INT_TO_REF(i)	((ref_t)((((size_t)(ssize_t)(i))<<TAGSIZE) + INT_TAG))
#else
#define INT_TO_REF(i)   ((ref_t)((((size_t)(ssize_t)(i))<<TAGSIZE) | INT_TAG))
#endif

#define BOOL_TO_REF(x)   ( (x) ? e_t : e_false )

/* MIN_REF is the most negative fixnum.  There is no corresponding
   positive fixnum, an asymmetry inherent in a twos complement
   representation. */

#define MIN_REF     ((ref_t)((ref_t)0x1<<(OAK_WORD_SIZE-1)))
#define MAX_REF     ((ref_t)-((ssize_t)MIN_REF+1))

/* Check if high three bits are equal. */

/*
#define OVERFLOWN_INT(i,code)					\
{ register int highcrap						\
	= ((uint32_t)(i)) >> (OAK_WORD_SIZE-(TAGSIZE+1));		\
if ((highcrap != 0x0) && (highcrap != 0x7)) {code;} }
*/

/* Check if high three bits are equal (works for any word size). */

#define OVERFLOWN_INT(i,code)					\
{ unsigned int highcrap						\
	= ((size_t)(i)) >> (OAK_WORD_SIZE-(TAGSIZE+1));		\
if ((highcrap != 0x0) && (highcrap != 0x7)) {code;} }

/*
 * Offsets for wired types.  Offset includes type and
 * optional length fields when present.
 */

/* CONS-PAIR: */
#define CONS_PAIR_CAR_OFF	1
#define CONS_PAIR_CDR_OFF	2

/* TYPE: */
#define TYPE_LEN_OFF		1
#define TYPE_VAR_LEN_P_OFF	2
#define TYPE_SUPER_LIST_OFF	3
#define TYPE_IVAR_LIST_OFF	4
#define TYPE_IVAR_COUNT_OFF	5
#define TYPE_TYPE_BP_ALIST_OFF	6
#define TYPE_OP_METHOD_ALIST_OFF 7
#define TYPE_WIRED_P_OFF	8

/* METHOD: */
#define METHOD_CODE_OFF		1
#define METHOD_ENV_OFF		2

/* CODE-VECTOR: */
#define CODE_IVAR_MAP_OFF	2
#define CODE_CODE_START_OFF	3

/* OPERATION: */
#define OPERATION_LAMBDA_OFF		1
#define OPERATION_CACHE_TYPE_OFF	2
#define OPERATION_CACHE_METH_OFF	3
#define OPERATION_CACHE_TYPE_OFF_OFF	4
#define OPERATION_LENGTH		5

/* ESCAPE-OBJECT */
#define ESCAPE_OBJECT_VAL_OFF	1
#define ESCAPE_OBJECT_CXT_OFF	2

/* Continuation Objects */
#define CONTINUATION_VAL_SEGS	1
#define CONTINUATION_VAL_OFF	2
#define CONTINUATION_CXT_SEGS	3
#define CONTINUATION_CXT_OFF	4

#define SPACE_PTR(s,p)	((s).start<=(p) && (p)<(s).end)
#define NEW_PTR(r)      SPACE_PTR(new_space,(r))
#define SPATIC_PTR(r)	SPACE_PTR(spatic,(r))
/* With the mark-sweep collector the copying collector runs only to
   compact the heap for a dump, and old space is then every region
   (gc-ms.h); new_space is its destination either way. */
#ifdef USE_MARK_SWEEP
#define OLD_PTR(r) (ms_old_ptr(r)||(full_gc&&SPACE_PTR(spatic,(r))))
#else
#define OLD_PTR(r) (SPACE_PTR(old_space,(r))||(full_gc&&SPACE_PTR(spatic,(r))))
#endif




/* Leaving r unsigned lets us checks for negative and too big in one shot.
   The value read goes through the concurrent mark's weak-read barrier
   (satb_weak_read, below; a no-op without the mark-sweep collector), in
   the interpreter and in generated C alike. */
#define wp_to_ref(r)					\
  ( (size_t)REF_TO_INT(r) >= (size_t) wp_index ?	\
   e_nil : satb_weak_read(wp_table[1+(size_t)REF_TO_INT((r))]) )


/* This is used to allocate some storage.  It calls gc when necessary. */

#define ALLOCATE(p, words, reason)			\
  ALLOCATE_PROT(p, words, reason,; ,; )

/* This is used to allocate some storage */

#define ALLOCATE_SS(p, words, reason)			\
  ALLOCATE_PROT(p, words, reason,			\
		{ value_stack.sp = local_value_sp;	\
          context_stack.sp = local_context_sp;		\
		  e_pc = local_e_pc; },			\
		{ local_e_pc = e_pc;			\
          local_context_sp = context_stack.sp;		\
		  local_value_sp = value_stack.sp; })


/* This allocates some storage, assuming that v must be protected from gc. */

#define ALLOCATE1(p, words, reason, v)			\
  ALLOCATE_PROT(p, words, reason,			\
		{ GC_MEMORY(v);				\
		  value_stack.sp = local_value_sp;	\
          context_stack.sp = local_context_sp;		\
		  e_pc = local_e_pc; },			\
		{ local_e_pc = e_pc;			\
          local_context_sp = context_stack.sp;		\
		  local_value_sp = value_stack.sp;	\
		  GC_RECALL(v); })


#if defined(USE_MARK_SWEEP)

/*
 * Mark-sweep allocator, with a TLAB per thread (one in all without
 * threads): the fast path bumps the thread's TLAB pointer, no lock;
 * everything else is ms_allocate_slow (gc-ms.c), out of line, so that
 * the interpreter's many allocation sites stay small.  BEFORE and
 * AFTER synchronize the caller's state for a collection, which the
 * slow path may run.
 */

#include "gc-ms.h"

#ifdef OAK_GC_VERIFY
/* Verify mode: the coordinator's index while the world is stopped. */
extern volatile int ms_verify_stw_owner;
extern void ms_verify_stw_alloc(int idx, const char *how);
#define VERIFY_STW_ALLOC(idx, how)					\
  if (ms_verify_stw_owner != MS_NO_STW_OWNER					\
      && ms_verify_stw_owner != (idx))					\
    ms_verify_stw_alloc((idx), (how));
extern void ms_verify_log_alloc(ref_t *p, int path, size_t words);
#define VERIFY_LOG_ALLOC(p, path, n) ms_verify_log_alloc((p), (path), (n));
#else
#define VERIFY_STW_ALLOC(idx, how)
#define VERIFY_LOG_ALLOC(p, path, n)
#endif

#define ALLOCATE_PROT(p, words, reason, before, after)			\
{									\
  int _tlab_idx = MS_TLAB_INDEX;					\
  VERIFY_STW_ALLOC(_tlab_idx, reason)					\
  if ((words) <= TLAB_SIZE &&						\
      tlab_cursor_array[_tlab_idx] &&					\
      tlab_cursor_array[_tlab_idx] + (words)				\
        <= tlab_end_array[_tlab_idx]) {					\
    /* TLAB fast path — no lock needed. */				\
    (p) = tlab_cursor_array[_tlab_idx];					\
    tlab_cursor_array[_tlab_idx] += (words);				\
    MS_TLAB_OBJSTART(_tlab_idx, p);					\
    VERIFY_LOG_ALLOC((p), 1, (words))					\
  } else {								\
    before;								\
    (p) = ms_allocate_slow((words), (reason));				\
    after;								\
  }									\
}

#else /* !USE_MARK_SWEEP */

#define ALLOCATE_PROT(p, words, reason, before, after)	\
{							\
  THREADY(						\
      while (oak_mutex_trylock(&alloc_lock) != 0) {	\
	      if (OAK_LOAD_RELAXED(&gc_pending)) {		\
		      before; wait_for_gc(); after;	\
	      }						\
      }							\
  )							\
  /* Compare word counts, not pointers: free_point+words can wrap  \
     around the address space, which is undefined behavior and lets \
     an absurd length slip through. */			\
  if ((size_t)(words) >= (size_t)(new_space.end - free_point))	\
    {							\
      before;						\
      gc(false, false, (reason), (words));		\
      after;						\
    }							\
  (p) = free_point;					\
  free_point += (words);				\
  THREADY( oak_mutex_unlock(&alloc_lock); )		\
}

#endif /* USE_MARK_SWEEP */

/* SATB write barrier — no-op when mark-sweep GC is not enabled. */
#ifndef USE_MARK_SWEEP
#define SATB_BARRIER(addr, v) ((void)0)
#define SATB_READ_WEAK(v) ((void)0)
#define SATB_READ_RAW(v) ((void)0)
#define SATB_STORE(addr, v) (*(addr) = (v))
#endif

/* A value read through a weak pointer (wp_to_ref): logged during a
   concurrent mark (gc-ms.h, SATB_READ_WEAK), since an object the
   snapshot held only weakly is otherwise invisible to the mark once
   it is stored into a black object; and while the table is being
   cleared after a mark, nil unless its object survived (the clearing
   runs with the mutators going on). */
static inline ref_t
satb_weak_read(ref_t v)
{
  SATB_READ_WEAK(v);
#ifdef USE_MARK_SWEEP
  if (OAK_UNLIKELY(OAK_LOAD_RELAXED(&gc_weak_clearing)) && (v & PTR_MASK)
      && !ms_weak_alive(v))
    return e_nil;
#endif
  return v;
}

/* These get slots out of Oaklisp objects, and may be used as lvalues. */

#define SLOT(p,s)	(*((p)+(s)))
#define REF_SLOT(r,s)	SLOT(REF_TO_PTR(r),s)


/* This is for the warmup code */

#define CODE_SEG_FIRST_INSTR(seg) \
  ( (instr_t *)(REF_TO_PTR((seg)) + CODE_CODE_START_OFF) )


#ifdef THREADS

#define reg_set register_array[my_index]
#define value_stack (*value_stack_array[my_index])
#define context_stack (*cntxt_stack_array[my_index])
#define value_stack_address value_stack_array[my_index]
#define context_stack_address cntxt_stack_array[my_index]
#define e_code_segment     ( (reg_set->e_code_segment) )
#define e_current_method   ( (reg_set->e_current_method) )
#define e_pc               ( (reg_set->e_pc) )
#define e_bp               ( (reg_set->e_bp) )
#define e_env              ( (reg_set->e_env) )
#define e_nargs            ( (reg_set->e_nargs) )
#define e_process          ( (reg_set->e_process) )
#define oak_timer_counter   (reg_set->timer_counter)
#define oak_timer_increment (reg_set->timer_increment)
/* What a new OS thread runs with its start operation (threads.c):
   loop in the emulator, the native call driver in a stand-alone
   executable, each entry point setting it. */
extern void (*oak_thread_entry)(ref_t start_operation);

#else

extern register_set_t *reg_set;
#define value_stack_address &value_stack
#define context_stack_address &context_stack

#endif

extern int create_thread(ref_t start_method);

extern register_set_t* register_array[];

#endif
