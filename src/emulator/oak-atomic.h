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
 * oak-atomic.h - Portable atomic operations, bit intrinsics, and
 *                branch hints.
 *
 * Strategy: prefer C11 <stdatomic.h>, fall back to GCC __atomic_*
 * builtins, then to MSVC _Interlocked* intrinsics.
 */

#ifndef OAK_ATOMIC_H_INCLUDED
#define OAK_ATOMIC_H_INCLUDED

#include <stdint.h>
#include <stddef.h>


/* ------------------------------------------------------------------ */
/*  Branch prediction hints                                            */
/* ------------------------------------------------------------------ */

#if defined(__GNUC__) || defined(__clang__)
#define OAK_UNLIKELY(x)  __builtin_expect((x), 0)
#define OAK_LIKELY(x)    __builtin_expect((x), 1)
#else
#define OAK_UNLIKELY(x)  (x)
#define OAK_LIKELY(x)    (x)
#endif


/* ------------------------------------------------------------------ */
/*  Count leading/trailing zeros (64-bit)                              */
/* ------------------------------------------------------------------ */

#if defined(__GNUC__) || defined(__clang__)
#define CLZ64(x)  __builtin_clzll(x)
#define CTZ64(x)  __builtin_ctzll(x)
#elif defined(_MSC_VER)
#include <intrin.h>
static inline int CLZ64(uint64_t x) {
    unsigned long idx;
    _BitScanReverse64(&idx, x);
    return 63 - (int)idx;
}
static inline int CTZ64(uint64_t x) {
    unsigned long idx;
    _BitScanForward64(&idx, x);
    return (int)idx;
}
#else
/* Generic fallback */
static inline int CLZ64(uint64_t x) {
    int n = 0;
    if (!(x & 0xFFFFFFFF00000000ULL)) { n += 32; x <<= 32; }
    if (!(x & 0xFFFF000000000000ULL)) { n += 16; x <<= 16; }
    if (!(x & 0xFF00000000000000ULL)) { n +=  8; x <<=  8; }
    if (!(x & 0xF000000000000000ULL)) { n +=  4; x <<=  4; }
    if (!(x & 0xC000000000000000ULL)) { n +=  2; x <<=  2; }
    if (!(x & 0x8000000000000000ULL)) { n +=  1; }
    return n;
}
static inline int CTZ64(uint64_t x) {
    int n = 0;
    if (!(x & 0x00000000FFFFFFFFULL)) { n += 32; x >>= 32; }
    if (!(x & 0x000000000000FFFFULL)) { n += 16; x >>= 16; }
    if (!(x & 0x00000000000000FFULL)) { n +=  8; x >>=  8; }
    if (!(x & 0x000000000000000FULL)) { n +=  4; x >>=  4; }
    if (!(x & 0x0000000000000003ULL)) { n +=  2; x >>=  2; }
    if (!(x & 0x0000000000000001ULL)) { n +=  1; }
    return n;
}
#endif


/* ------------------------------------------------------------------ */
/*  Atomic operations                                                  */
/* ------------------------------------------------------------------ */

/*
 * Three tiers:
 *   1. C11 <stdatomic.h> (GCC 4.9+, Clang 3.1+, MSVC 17.5+)
 *   2. GCC/Clang __atomic_* builtins
 *   3. MSVC _Interlocked* intrinsics
 *
 * Operations provided:
 *   OAK_ATOMIC_FETCH_OR_U64(ptr, val)   - atomic OR on uint64_t, relaxed
 *   OAK_ATOMIC_FETCH_AND_U64(ptr, val)  - atomic AND on uint64_t, relaxed
 *   OAK_ATOMIC_LOAD_REF(ptr)            - atomic load with acquire semantics
 *   OAK_ATOMIC_STORE_REF(ptr, val)      - atomic store with release semantics
 *   OAK_ATOMIC_CAS_SIZE(ptr, expp, val) - compare-and-swap on a size_t,
 *                                         acquire on success; on failure
 *                                         *expp gets the value seen
 *   OAK_ATOMIC_FENCE_ACQUIRE(), OAK_ATOMIC_FENCE_RELEASE()
 */

#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L \
     && !defined(__STDC_NO_ATOMICS__)) \
    || defined(HAVE_STDATOMIC_H)

#include <stdatomic.h>

#define OAK_ATOMIC_FETCH_OR_U64(ptr, val) \
    atomic_fetch_or_explicit((_Atomic uint64_t *)(ptr), (val), \
			     memory_order_relaxed)

#define OAK_ATOMIC_FETCH_AND_U64(ptr, val) \
    atomic_fetch_and_explicit((_Atomic uint64_t *)(ptr), (val), \
			      memory_order_relaxed)

#define OAK_ATOMIC_LOAD_REF(ptr) \
    atomic_load_explicit((_Atomic size_t *)(ptr), memory_order_acquire)

#define OAK_ATOMIC_STORE_REF(ptr, val) \
    atomic_store_explicit((_Atomic size_t *)(ptr), (val), \
			  memory_order_release)

#define OAK_ATOMIC_CAS_SIZE(ptr, expp, val) \
    atomic_compare_exchange_strong_explicit((_Atomic size_t *)(ptr), \
					    (size_t *)(expp), (val), \
					    memory_order_acquire, \
					    memory_order_relaxed)

#define OAK_ATOMIC_FENCE_ACQUIRE() atomic_thread_fence(memory_order_acquire)
#define OAK_ATOMIC_FENCE_RELEASE() atomic_thread_fence(memory_order_release)

#elif defined(__GNUC__) || defined(__clang__)

#define OAK_ATOMIC_FETCH_OR_U64(ptr, val) \
    __atomic_fetch_or((ptr), (val), __ATOMIC_RELAXED)

#define OAK_ATOMIC_FETCH_AND_U64(ptr, val) \
    __atomic_fetch_and((ptr), (val), __ATOMIC_RELAXED)

#define OAK_ATOMIC_LOAD_REF(ptr) \
    __atomic_load_n((ptr), __ATOMIC_ACQUIRE)

#define OAK_ATOMIC_STORE_REF(ptr, val) \
    __atomic_store_n((ptr), (val), __ATOMIC_RELEASE)

#define OAK_ATOMIC_CAS_SIZE(ptr, expp, val) \
    __atomic_compare_exchange_n((ptr), (expp), (val), 0, \
				__ATOMIC_ACQUIRE, __ATOMIC_RELAXED)

#define OAK_ATOMIC_FENCE_ACQUIRE() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define OAK_ATOMIC_FENCE_RELEASE() __atomic_thread_fence(__ATOMIC_RELEASE)

#elif defined(_MSC_VER)

#include <intrin.h>

#define OAK_ATOMIC_FETCH_OR_U64(ptr, val) \
    _InterlockedOr64((volatile long long *)(ptr), (long long)(val))

#define OAK_ATOMIC_FETCH_AND_U64(ptr, val) \
    _InterlockedAnd64((volatile long long *)(ptr), (long long)(val))

static inline size_t oak_atomic_load_ref_(volatile size_t *p) {
    size_t v = *p;
    _ReadBarrier();
    return v;
}
#define OAK_ATOMIC_LOAD_REF(ptr) \
    oak_atomic_load_ref_((volatile size_t *)(ptr))

static inline void oak_atomic_store_ref_(volatile size_t *p, size_t v) {
    _WriteBarrier();
    *p = v;
}
#define OAK_ATOMIC_STORE_REF(ptr, val) \
    oak_atomic_store_ref_((volatile size_t *)(ptr), (val))

static inline int oak_atomic_cas_size_(volatile size_t *p, size_t *e, size_t v) {
    size_t old = (size_t)_InterlockedCompareExchange64(
	(volatile long long *)p, (long long)v, (long long)*e);
    if (old == *e)
	return 1;
    *e = old;
    return 0;
}
#define OAK_ATOMIC_CAS_SIZE(ptr, expp, val) \
    oak_atomic_cas_size_((volatile size_t *)(ptr), (expp), (val))

/* x86 and x64 order loads and stores enough that keeping the compiler
   from reordering suffices. */
#define OAK_ATOMIC_FENCE_ACQUIRE() _ReadWriteBarrier()
#define OAK_ATOMIC_FENCE_RELEASE() _ReadWriteBarrier()

#else
#error "No atomic operations available.  Need C11, GCC, Clang, or MSVC."
#endif


/* ------------------------------------------------------------------ */
/*  A pause in a spin loop                                             */
/* ------------------------------------------------------------------ */

/* OAK_CPU_RELAX(): tell the processor the thread is spinning, so that
   it yields the core's resources to its sibling and does not
   mispredict the exit. */
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#if defined(_MSC_VER)
#include <intrin.h>
#define OAK_CPU_RELAX() _mm_pause()
#else
#define OAK_CPU_RELAX() __builtin_ia32_pause()
#endif
#elif defined(__aarch64__) || defined(__arm__)
#define OAK_CPU_RELAX() __asm__ __volatile__("yield" ::: "memory")
#else
#define OAK_CPU_RELAX() ((void)0)
#endif

/* OAK_CACHE_ALIGNED: on a variable or a struct type, so that what
   one thread writes shares no cache line with what another does.
   Two lines: the adjacent-line prefetcher pairs them. */
#define OAK_CACHE_LINE 128
#if defined(_MSC_VER)
#define OAK_CACHE_ALIGNED __declspec(align(128))
#elif defined(__GNUC__) || defined(__clang__)
#define OAK_CACHE_ALIGNED __attribute__((aligned(128)))
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define OAK_CACHE_ALIGNED _Alignas(128)
#else
#define OAK_CACHE_ALIGNED
#endif

#endif /* OAK_ATOMIC_H_INCLUDED */
