/*
** Filename:  ArenaMem.h
**
** Purpose:
**   Declares a bump-pointer arena allocator (ArenaMem) used to hand out many
**   small allocations from one preallocated block without per-allocation
**   locking overhead. Allocations are never individually freed -- the arena
**   is freed or reset as a whole. Thread-safe: usedSize is advanced with an
**   atomic fetch-add, and any request that would overflow totalSize instead
**   falls back to a system-heap allocation chained onto pOverflowChainHead,
**   so ArenaMemReset/ArenaMemDestroy can still reclaim it.
**
** Notes:
**   ArenaMemFree and ArenaMemCheck are no-op macros -- individual
**   allocations are never freed one at a time; only ArenaMemReset/Destroy
**   reclaim memory. They exist so call sites can be written as though a
**   normal alloc/free/check discipline is in effect, in case that ever
**   changes.
*/

#pragma once

/* Includes */
#include <memory.h>
#include <stdio.h>
#include "Mem.h"
#include <atomic>

/* Macros and Defines */
#define ArenaMemFree(pArena, pPtr) ((void)0)
#define ArenaMemCheck(pArena, fpOut, pszStr) ((void)0)

/* Structures and Types */

/*
** Type:    ArenaMemOverflowChainNode
** @brief   One system-heap allocation made after the arena filled up.
**          Chained so ArenaMemReset/ArenaMemDestroy can free every overflow
**          allocation.
*/
typedef struct ArenaMemOverflowChainNode
{
    struct ArenaMemOverflowChainNode* pNext;
    char                              data[1];   /* variable-length data starts here; alloc = sizeof(node)-1+size */
} ArenaMemOverflowChainNode, * PArenaMemOverflowChainNode;

/*
** Type:    ArenaMem
** @brief   One bump-pointer arena: a preallocated block (pBase/totalSize), an
**          atomically-advancing high-water mark (usedSize), and a chain of
**          overflow allocations for anything that didn't fit.
*/
typedef struct ArenaMem
{
    void*                                     pBase;               /* start of the preallocated block                   */
    size_t                                    totalSize;           /* size in bytes of the preallocated block           */
    std::atomic_size_t                        usedSize;            /* bytes handed out so far (bump pointer)            */
    std::atomic<PArenaMemOverflowChainNode>   pOverflowChainHead;  /* head of the overflow allocation chain, or nullptr */
} ArenaMem, * PArenaMem;

/* Functions */

/*
** Function: ArenaMemCreate
** @brief    Allocates and zero-initializes a new arena of the given size.
** @param    totalSize - number of bytes to preallocate for the arena
** @return   A newly allocated PArenaMem, or nullptr if allocation failed.
*/
PArenaMem ArenaMemCreate(size_t totalSize);

/*
** Function: ArenaMemDestroy
** @brief    Frees an arena's preallocated block, its overflow chain, and the
**           arena struct itself. Safe to call with nullptr.
** @param    pArena - the arena to destroy
*/
void ArenaMemDestroy(PArenaMem pArena);

/*
** Function: ArenaMemReset
** @brief    Frees the overflow chain and rewinds usedSize to 0 so the arena's
**           preallocated block can be reused from the start. Safe to call
**           with nullptr.
** @param    pArena - the arena to reset
*/
void ArenaMemReset(PArenaMem pArena);

/*
** Function: ArenaMemMalloc
** @brief    Hands out sizeToAlloc bytes from the arena, 8-byte aligned.
**           Falls back to a chained system-heap allocation if the arena is
**           full.
** @param    pArena       - the arena to allocate from
** @param    pStr         - name tag passed through to MemMalloc on overflow
** @param    sizeToAlloc  - number of bytes requested
** @return   Pointer to the allocated memory, or nullptr on failure.
*/
void* ArenaMemMalloc(PArenaMem pArena, const char* pStr, size_t sizeToAlloc);

/*
** Function: ArenaMemSize
** @brief    Returns the number of bytes handed out from the arena so far.
** @param    pArena - the arena to query
** @return   Bytes used, or 0 if pArena is nullptr.
*/
size_t ArenaMemSize(PArenaMem pArena);

/*
** Function: ArenaMemHasOverflowed
** @brief    Reports whether the arena has ever fallen back to a system-heap
**           overflow allocation.
** @param    pArena - the arena to query
** @return   true if at least one overflow allocation is chained.
*/
bool ArenaMemHasOverflowed(PArenaMem pArena);

/*
** Function: ArenaMemStatsPrint
** @brief    Prints total size, used size (percentage), and overflow-node
**           count for an arena.
** @param    pArena - the arena to report on
** @param    fpOut  - stream to print to
*/
void ArenaMemStatsPrint(PArenaMem pArena, FILE* fpOut);
