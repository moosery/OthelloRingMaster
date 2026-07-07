/*
** Filename:  ArenaMem.cpp
**
** Purpose:
**   Implements the bump-pointer arena allocator declared in ArenaMem.h.
**   ArenaMemMalloc advances an atomic offset into a single preallocated
**   block; anything past the end of that block falls back to a system-heap
**   allocation chained onto pOverflowChainHead so it can still be freed by
**   ArenaMemReset/ArenaMemDestroy.
*/

/* Includes */
#include "ArenaMem.h"

/* Functions */

/*
** Function: ArenaMemCreate
** @brief    Allocates and zero-initializes a new arena of the given size.
** @param    totalSize - number of bytes to preallocate for the arena
** @return   A newly allocated PArenaMem, or nullptr if allocation failed.
*/
PArenaMem ArenaMemCreate(size_t totalSize)
{
    PArenaMem pArena = (PArenaMem)MemMalloc("ArenaMemCreate", sizeof(ArenaMem));

    /* No arena to configure if the struct itself couldn't be allocated. */
    if (!pArena)
        return nullptr;

    pArena->pBase = MemMalloc("ArenaMemCreate", totalSize);

    /* Roll back the struct allocation if the backing block failed. */
    if (!pArena->pBase)
    {
        MemFree(pArena);
        return nullptr;
    }

    pArena->totalSize          = totalSize;
    pArena->usedSize           = 0;
    pArena->pOverflowChainHead = nullptr;
    memset(pArena->pBase, 0, totalSize);
    return pArena;
}

/*
** Function: ArenaMemDestroy
** @brief    Frees an arena's preallocated block, its overflow chain, and the
**           arena struct itself. Safe to call with nullptr.
** @param    pArena - the arena to destroy
*/
void ArenaMemDestroy(PArenaMem pArena)
{
    if (!pArena)
        return;

    /* Walk and free every overflow node before freeing the arena itself. */
    PArenaMemOverflowChainNode p = pArena->pOverflowChainHead.exchange(nullptr);
    while (p)
    {
        PArenaMemOverflowChainNode next = p->pNext;
        MemFree(p);
        p = next;
    }
    MemFree(pArena->pBase);
    MemFree(pArena);
}

/*
** Function: ArenaMemReset
** @brief    Frees the overflow chain and rewinds usedSize to 0 so the arena's
**           preallocated block can be reused from the start. Safe to call
**           with nullptr.
** @param    pArena - the arena to reset
*/
void ArenaMemReset(PArenaMem pArena)
{
    if (!pArena)
        return;

    /* Overflow nodes are heap allocations outside the arena block, so they
    ** must be freed explicitly -- resetting usedSize alone would leak them.
    */
    PArenaMemOverflowChainNode p = pArena->pOverflowChainHead.exchange(nullptr);
    while (p)
    {
        PArenaMemOverflowChainNode next = p->pNext;
        MemFree(p);
        p = next;
    }

    pArena->usedSize = 0;
}

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
void* ArenaMemMalloc(PArenaMem pArena, const char* pStr, size_t sizeToAlloc)
{
    if (!pArena || !pStr || sizeToAlloc == 0)
        return nullptr;

    /* Align to 8 bytes so every returned pointer is naturally aligned. */
    size_t aligned = (sizeToAlloc + 7) & ~7;

    size_t offset = pArena->usedSize.fetch_add(aligned);

    /* Common case: the bump still fits inside the preallocated block. */
    if (offset + aligned <= pArena->totalSize)
        return (char*)pArena->pBase + offset;

    /* Overflow: arena is full. Allocate from the system heap and chain the
    ** node so ArenaMemReset can free it. There is no rollback of usedSize --
    ** rolling back would create a race where another thread could receive
    ** the same slot.
    */
    size_t                      nodeSize = sizeof(ArenaMemOverflowChainNode) - 1 + aligned;
    PArenaMemOverflowChainNode  pNode    = (PArenaMemOverflowChainNode)MemMalloc(pStr, nodeSize);
    if (!pNode)
        return nullptr;

    /* Push the new node onto the lock-free overflow chain. */
    do
    {
        pNode->pNext = pArena->pOverflowChainHead.load();
    } while (!pArena->pOverflowChainHead.compare_exchange_weak(pNode->pNext, pNode));

    return pNode->data;
}

/*
** Function: ArenaMemSize
** @brief    Returns the number of bytes handed out from the arena so far.
** @param    pArena - the arena to query
** @return   Bytes used, or 0 if pArena is nullptr.
*/
size_t ArenaMemSize(PArenaMem pArena)
{
    if (!pArena)
        return 0;
    return pArena->usedSize.load();
}

/*
** Function: ArenaMemHasOverflowed
** @brief    Reports whether the arena has ever fallen back to a system-heap
**           overflow allocation.
** @param    pArena - the arena to query
** @return   true if at least one overflow allocation is chained.
*/
bool ArenaMemHasOverflowed(PArenaMem pArena)
{
    if (!pArena)
        return false;
    return pArena->pOverflowChainHead.load(std::memory_order_relaxed) != nullptr;
}

/*
** Function: ArenaMemStatsPrint
** @brief    Prints total size, used size (percentage), and overflow-node
**           count for an arena.
** @param    pArena - the arena to report on
** @param    fpOut  - stream to print to
*/
void ArenaMemStatsPrint(PArenaMem pArena, FILE* fpOut)
{
    if (!pArena || !fpOut)
        return;

    size_t used     = pArena->usedSize.load();
    size_t capped   = (used < pArena->totalSize) ? used : pArena->totalSize;   /* clamp for display; usedSize can exceed totalSize once overflow occurs */
    int    overflow = 0;

    /* Count overflow nodes purely for reporting -- not on any allocation hot path. */
    for (PArenaMemOverflowChainNode p = pArena->pOverflowChainHead.load(); p; p = p->pNext)
        overflow++;

    fprintf(fpOut, "ArenaMem Stats:\n");
    fprintf(fpOut, "  Total Size:      %zu bytes\n", pArena->totalSize);
    fprintf(fpOut, "  Used Size:       %zu bytes (%.1f%%)\n",
            capped, pArena->totalSize ? 100.0 * capped / pArena->totalSize : 0.0);
    fprintf(fpOut, "  Overflow Nodes:  %d\n", overflow);
}
