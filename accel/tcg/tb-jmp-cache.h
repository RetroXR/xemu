/*
 * The per-CPU TranslationBlock jump cache.
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_TCG_TB_JMP_CACHE_H
#define ACCEL_TCG_TB_JMP_CACHE_H

#include "qemu/rcu.h"
#include "exec/cpu-common.h"

#ifdef XBOX
/* Games have more hot targets of indirect jumps than 64 slots in 64 pages */
#define TB_JMP_CACHE_BITS 14
#else
#define TB_JMP_CACHE_BITS 12
#endif
#define TB_JMP_CACHE_SIZE (1 << TB_JMP_CACHE_BITS)

/*
 * Invalidated in parallel; all accesses to 'tb' must be atomic.
 * A valid entry is read/written by a single CPU, therefore there is
 * no need for qatomic_rcu_read() and pc is always consistent with a
 * non-NULL value of 'tb'.  Strictly speaking pc is only needed for
 * CF_PCREL, but it's used always for simplicity.
 */
#ifdef XBOX
/*
 * A game spends a large part of its time on indirect jumps, returns above
 * all, with more targets than the data caches of the host hold: what a lookup
 * costs is the cache lines it touches. So an entry has everything that is
 * compared and the address of the code, leaving the TranslationBlock alone,
 * and two of them share a line. The second one is the entry that was last
 * displaced from the first: two hot targets with the same hash would
 * otherwise evict each other on every jump.
 *
 * As an entry is not checked against the cflags of its TB, where CF_INVALID
 * would show, it has to go when the TB is invalidated. With CF_PCREL a TB can
 * be at any virtual address, so it keeps track of where it was entered, see
 * tb_jmp_cache_insert().
 */
typedef struct CPUJumpCacheWay {
    uint64_t key[2]; /* pc and cs_base, flags and cflags */
    TranslationBlock *tb;
    const void *tc_ptr;
} CPUJumpCacheWay;

typedef struct CPUJumpCache {
    struct rcu_head rcu;
    struct {
        CPUJumpCacheWay way[2];
    } QEMU_ALIGNED(64) array[TB_JMP_CACHE_SIZE];
} CPUJumpCache;
#else
typedef struct CPUJumpCache {
    struct rcu_head rcu;
    struct {
        TranslationBlock *tb;
        vaddr pc;
    } array[TB_JMP_CACHE_SIZE];
} CPUJumpCache;
#endif

#endif /* ACCEL_TCG_TB_JMP_CACHE_H */
