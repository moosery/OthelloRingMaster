/*
** Filename:  Mem.cpp
**
** Purpose:
**   Implements MemMalloc/MemFree/MemSize/MemStatsPrint/MemCheck/MemCheckBlock
**   (declared in Mem.h). Compile-time modes, selected by the defines below:
**     - LOOKFOROVERWRITE defined (the default; requires NOTRACK): every block
**       is laid out as [32-byte header][user bytes][16-byte trailer]. The
**       header's magic word and the trailer are both derived from the block's
**       own address, so a stray write, a block copied to another address, or
**       a pointer that never came from MemMalloc all fail the check. MemFree
**       (and MemCheckBlock) verify both and stop the process on a mismatch,
**       naming the block's tag, size and address; a freed block's header is
**       stamped so freeing it again is reported as a double free. No list and
**       no lock, so it costs a few dozen bytes per block and nothing on the
**       hot paths.
**     - NOTRACK defined without LOOKFOROVERWRITE: a thin, zero-overhead
**       wrapper straight to malloc/free (still zero-initializing on alloc).
**     - NOTRACK undefined: every allocation is wrapped in a MEMORY_NODE,
**       linked into a doubly-linked list, and followed by a known
**       "overwrite check" string, so MemCheck can later detect a buffer
**       overrun and MemStatsPrint can report live allocation counts by tag.
**   MEMDEBUG additionally prints every individual alloc/free when tracking
**   is enabled.
*/

/* Includes */
#include <stdio.h>
#include "Mem.h"
#include <malloc.h>
#include <string.h>
#include <shared_mutex>
#include <windows.h>
#include <memoryapi.h>
#include <stdint.h>
#include "Error.h"

using namespace std;

/* Macros and Defines */
#define NOTRACK 1
#define LOOKFOROVERWRITE 1   /* guard header + trailer on every block; see the file Purpose */
//#define MEMDEBUG 1

#define NAMESIZE 31
#define CONTROLSTR "JWS1234"

/* Structures and Types */

/*
** Type:    MEMORY_NODE
** @brief   Tracking-mode header prepended to every MemMalloc allocation:
**          a control string (corruption sentinel), the caller's name tag,
**          the requested size, and doubly-linked-list pointers so every
**          live allocation can be walked by MemStatsPrint/MemCheck.
*/
typedef struct _Memory_Node
{
    char                  controlStr[8];
    char                  memName[NAMESIZE + 1];
    size_t                memSize;
    struct _Memory_Node*  pNextNode;
    struct _Memory_Node*  pPrevNode;
} MEMORY_NODE, * PMEMORY_NODE;

/*
** Type:    STATSINFO
** @brief   One aggregated per-tag row (name + count) built up by
**          MemStatsPrint while walking the live-allocation list.
*/
typedef struct _statsInfo
{
    char    name[NAMESIZE + 1];
    size_t  numAllocated;
} STATSINFO, * PSTATSINFO;

typedef shared_mutex               MyMallocLock;
typedef unique_lock<MyMallocLock>  MyMallocWriteLock;
typedef shared_lock<MyMallocLock>  MyMallocReadLock;

/* Globals */
PMEMORY_NODE pFirstNode      = NULL;   /* head of the live-allocation list (tracking mode only) */
PMEMORY_NODE pLastNode       = NULL;   /* tail of the live-allocation list (tracking mode only) */
MyMallocLock myMallocLock;             /* guards pFirstNode/pLastNode/totalAllocated            */
size_t       totalAllocated = 0;       /* running total of bytes allocated and not yet freed    */

#ifdef LOOKFOROVERWRITE
#ifndef NOTRACK
#error LOOKFOROVERWRITE replaces the tracking list and requires NOTRACK
#endif

/* Guard-mode block layout: [MemGuardHeader][user bytes][MEM_GUARD_TRAILER_BYTES].
** The header is 32 bytes so the user pointer keeps malloc's 16-byte alignment.
*/
struct MemGuardHeader
{
    uint64_t magic;      /* MEM_GUARD_LIVE_KEY ^ user address while live; MEM_GUARD_FREED once freed */
    uint64_t size;       /* requested user size                                                       */
    char     tag[16];    /* first 15 characters of the caller's tag (not NUL-guaranteed when damaged) */
};
static_assert(sizeof(MemGuardHeader) == 32, "guard header must stay 32 bytes to preserve 16-byte alignment");

const uint64_t MEM_GUARD_LIVE_KEY       = 0x4D454D4C49564531ull;   /* "MEMLIVE1" */
const uint64_t MEM_GUARD_FREED          = 0x4D454D4652454544ull;   /* "MEMFREED" */
const size_t   MEM_GUARD_TRAILER_BYTES  = 16;

/*
** Function: memGuardTrailer
** @brief    Computes the two-word trailer expected just past a block's user
**           bytes. Derived from the block's own address and size, so it can't
**           be matched by an ordinary data pattern (zeros, a repeated fill)
**           and differs for every block.
** @param    pUser - the block's user pointer
** @param    size  - the block's user size
** @param    out   - out: the two expected trailer words
*/
static void memGuardTrailer(const void* pUser, uint64_t size, uint64_t out[2])
{
    uint64_t a = ((uint64_t)(uintptr_t)pUser * 0x9E3779B97F4A7C15ull) ^ size;
    out[0] = a;
    out[1] = ~a ^ 0xA5A5A5A5A5A5A5A5ull;
}

/*
** Function: memGuardFail
** @brief    Stops the process with a full description of a damaged block.
** @param    pszWhere - call site that found the problem
** @param    pszWhat  - what was wrong
** @param    pHdr     - the block's header (contents may themselves be damaged)
** @param    pUser    - the block's user pointer
** @param    detail   - extra number to print (first damaged trailer byte offset, or the bad magic word)
*/
static void memGuardFail(const char* pszWhere, const char* pszWhat, const MemGuardHeader* pHdr,
                         const void* pUser, uint64_t detail)
{
    Fatal(FATAL_MEMORY_CORRUPTED,
          "Memory corruption found by %s: %s -- block at %p, tag '%.16s', recorded size %llu, detail 0x%llX. "
          "(A write ran past this block's end or before its start, the block was freed twice, or the "
          "pointer never came from MemMalloc.)",
          pszWhere, pszWhat, pUser, pHdr->tag, (unsigned long long)pHdr->size, (unsigned long long)detail);
}

/*
** Function: memGuardVerify
** @brief    Checks one block's header and trailer; stops the process on a mismatch.
** @param    pUser    - pointer returned by MemMalloc
** @param    pszWhere - call site, for the failure message
*/
static void memGuardVerify(const void* pUser, const char* pszWhere)
{
    const MemGuardHeader* pHdr = ((const MemGuardHeader*)pUser) - 1;

    if (pHdr->magic != (MEM_GUARD_LIVE_KEY ^ (uint64_t)(uintptr_t)pUser))
    {
        if (pHdr->magic == MEM_GUARD_FREED)
            memGuardFail(pszWhere, "block was already freed (double free or use after free)", pHdr, pUser, pHdr->magic);
        else
            memGuardFail(pszWhere, "guard header damaged, or pointer not from MemMalloc", pHdr, pUser, pHdr->magic);
    }

    uint64_t expect[2];
    memGuardTrailer(pUser, pHdr->size, expect);

    uint8_t actual[MEM_GUARD_TRAILER_BYTES];
    memcpy(actual, (const uint8_t*)pUser + pHdr->size, sizeof(actual));

    if (memcmp(actual, expect, sizeof(actual)) != 0)
    {
        size_t firstBad = 0;
        while (firstBad < sizeof(actual) && actual[firstBad] == ((const uint8_t*)expect)[firstBad])
            firstBad++;
        memGuardFail(pszWhere, "guard trailer overwritten (something wrote past the end of the block)",
                     pHdr, pUser, (uint64_t)firstBad);
    }
}
#endif

/* "Canary" string written just past each tracked allocation's user data;
** MemCheck compares it back to catch a write that ran past the buffer's end.
*/
const char THE_MEMORY_OVERWRITE_CHECK_STR[52] = "Now is the time to see if the data got overwritten!";

/* Functions */

/*
** Function: MemMalloc
** @brief    Allocates sizeToAlloc bytes, zero-initialized, tagged with pStr
**           for reporting/debugging.
** @details  In NOTRACK mode, a direct malloc+memset. In tracking mode,
**           allocates room for a MEMORY_NODE header plus the overwrite-check
**           string after the user's data, fills them in, links the node onto
**           the live-allocation list, and returns a pointer just past the
**           header (i.e. the user's usable memory).
** @param    pStr        - name tag identifying this allocation's call site/purpose
** @param    sizeToAlloc - number of bytes requested
** @return   Pointer to the allocated memory, or nullptr on failure.
*/
void* MemMalloc(const char* pStr, size_t sizeToAlloc)
{
#ifdef LOOKFOROVERWRITE
    if (sizeToAlloc > SIZE_MAX - sizeof(MemGuardHeader) - MEM_GUARD_TRAILER_BYTES)
        return NULL;

    MemGuardHeader* pHdr = (MemGuardHeader*)malloc(sizeof(MemGuardHeader) + sizeToAlloc + MEM_GUARD_TRAILER_BYTES);
    if (pHdr == NULL)
        return NULL;

    uint8_t* pUser = (uint8_t*)(pHdr + 1);

    pHdr->magic = MEM_GUARD_LIVE_KEY ^ (uint64_t)(uintptr_t)pUser;
    pHdr->size  = sizeToAlloc;
    memset(pHdr->tag, 0, sizeof(pHdr->tag));
    if (pStr != NULL)
        strncpy(pHdr->tag, pStr, sizeof(pHdr->tag) - 1);

    memset(pUser, 0, sizeToAlloc);

    uint64_t trailer[2];
    memGuardTrailer(pUser, sizeToAlloc, trailer);
    memcpy(pUser + sizeToAlloc, trailer, sizeof(trailer));

    return pUser;
#elif defined(NOTRACK)
    void* result = (void*)malloc(sizeToAlloc);

    if (result != NULL)
        memset(result, 0, sizeToAlloc);

    return result;
#else
    PMEMORY_NODE  pNewNode;
    size_t        largerSize = sizeToAlloc + sizeof(MEMORY_NODE) + sizeof(THE_MEMORY_OVERWRITE_CHECK_STR);

    pNewNode = (PMEMORY_NODE)malloc(largerSize);

    if (pNewNode == NULL)
    {
        fprintf(stderr, "Could not malloc size of %llu in MemMalloc for structure %s\n", largerSize, pStr);
        return NULL;
    }

#ifdef MEMDEBUG
    printf("Allocated '0x%08p': '%s' for a size of %llu (%llu with overhead)\n", pNewNode, pStr, sizeToAlloc, largerSize);
#endif
    memset(pNewNode, 0, largerSize);
    memcpy(pNewNode->controlStr, CONTROLSTR, 8);

    strncpy(pNewNode->memName, pStr, NAMESIZE);
    pNewNode->memSize = sizeToAlloc;

    /* Stamp the canary string immediately after the user's usable region,
    ** so MemCheck can later detect a write that ran past the end of it.
    */
    char* pOverWriteString = ((char*)pNewNode) + sizeToAlloc + sizeof(MEMORY_NODE);
    memcpy(pOverWriteString, THE_MEMORY_OVERWRITE_CHECK_STR, sizeof(THE_MEMORY_OVERWRITE_CHECK_STR));

    {
        MyMallocWriteLock w_lock(myMallocLock);

        if (pFirstNode == NULL)
        {
            pFirstNode = pNewNode;
            pLastNode  = pNewNode;
        }
        else
        {
            pNewNode->pPrevNode  = pLastNode;
            pLastNode->pNextNode = pNewNode;
            pLastNode            = pNewNode;
        }

        totalAllocated += sizeToAlloc;
    }

    return (&(pNewNode[1]));
#endif
}

/*
** Function: MemFree
** @brief    Frees memory previously returned by MemMalloc. Safe to call with nullptr.
** @details  In tracking mode, validates the control string and canary before
**           unlinking the node from the live-allocation list and freeing it.
** @param    pPtr - the memory to free
*/
void MemFree(void* pPtr)
{
    if (pPtr == NULL)
        return;
#ifdef LOOKFOROVERWRITE
    memGuardVerify(pPtr, "MemFree");

    /* Stamp the header so a second free of this block is recognised as one
    ** (the CRT heap may reuse the memory afterward, but a later check of a
    ** still-stamped header is what catches the common immediate double free).
    */
    MemGuardHeader* pHdr = ((MemGuardHeader*)pPtr) - 1;
    pHdr->magic = MEM_GUARD_FREED;
    free(pHdr);
#elif defined(NOTRACK)
    free(pPtr);
#else
    PMEMORY_NODE pTmp = (PMEMORY_NODE)pPtr;
    pTmp--;

    /* A mismatched control string means this pointer was never handed out
    ** by MemMalloc -- freeing it as a MEMORY_NODE would corrupt the heap.
    */
    if (strcmp(pTmp->controlStr, CONTROLSTR) != 0)
    {
        fprintf(stderr, "Attempt to MemFree a buffer not allocated by MemMalloc!\n");
        return;
    }
#ifdef MEMDEBUG
    printf("Freeing '0x%08p': '%s' size of %llu\n", pTmp, pTmp->memName, pTmp->memSize);
#endif

    /* Check the canary before unlinking -- a corrupted block is worth
    ** reporting even though this function goes on to free it regardless.
    */
    char* pOverWriteString = ((char*)pTmp) + pTmp->memSize + sizeof(MEMORY_NODE);
    if (memcmp(pOverWriteString, THE_MEMORY_OVERWRITE_CHECK_STR, sizeof(THE_MEMORY_OVERWRITE_CHECK_STR)) != 0)
    {
        fprintf(stderr, "The memory block has been corrupted (written past end of buffer)!!\n");
        return;
    }
    {
        MyMallocWriteLock w_lock(myMallocLock);
        if (pTmp == pFirstNode)
        {
            if (pTmp == pLastNode)
            {
                pFirstNode = NULL;
                pLastNode  = NULL;
            }
            else
            {
                pFirstNode = pFirstNode->pNextNode;
                pFirstNode->pPrevNode = NULL;
            }
        }
        else if (pTmp == pLastNode)
        {
            pLastNode = pLastNode->pPrevNode;
            pLastNode->pNextNode = NULL;
        }
        else
        {
            if (pTmp->pNextNode != NULL)
                pTmp->pNextNode->pPrevNode = pTmp->pPrevNode;

            if (pTmp->pPrevNode != NULL)
                pTmp->pPrevNode->pNextNode = pTmp->pNextNode;
        }
        totalAllocated -= pTmp->memSize;
    }

    free(pTmp);
#endif
}

/*
** Function: MemSize
** @brief    Returns the total number of bytes currently allocated via MemMalloc.
** @return   Total bytes allocated and not yet freed.
*/
size_t MemSize()
{
    return totalAllocated;
}

/*
** Function: MemStatsPrint
** @brief    Prints a breakdown of live allocations by tag, plus the total
**           bytes allocated.
** @details  Walks the live-allocation list once, aggregating counts into a
**           fixed-size (32-tag) table by name, then prints that table.
**           A no-op report in NOTRACK mode, since no list is maintained.
** @param    fpOut - stream to print to
*/
void MemStatsPrint(FILE* fpOut)
{
#ifdef NOTRACK
    printf("Memory is not being tracked!\n");
#else
    STATSINFO theStats[32];

    memset(theStats, 0, sizeof(theStats));

    MyMallocReadLock w_lock(myMallocLock);
    {
        PMEMORY_NODE pNode = pFirstNode;

        while (pNode != NULL)
        {
//#define SUPERDETAIL
#ifdef SUPERDETAIL
            printf("Address: 0x%p  User Address: 0x%p  Name: %s\n", pNode, &(pNode[1]), pNode->memName);
#endif
            int idx = 0;
            for (idx = 0; idx < 32; idx++)
            {
                if (theStats[idx].name[0] == '\0')
                    break;
                if (strcmp(theStats[idx].name, pNode->memName) == 0)
                    break;
            }

            /* Only the first 32 distinct tags get their own row -- anything
            ** beyond that is silently uncounted rather than overflowing theStats.
            */
            if (idx < 32)
            {
                if (theStats[idx].name[0] == '\0')
                    memcpy(theStats[idx].name, pNode->memName, sizeof(theStats[idx].name));

                (theStats[idx].numAllocated)++;
            }
            pNode = pNode->pNextNode;
        }
    }

    fprintf(fpOut, "Memory Allocated:\n");
    for (int idx = 0; idx < 32; idx++)
    {
        if (theStats[idx].name[0] == '\0')
            break;

        fprintf(fpOut, "  '%s': %llu\n", theStats[idx].name, theStats[idx].numAllocated);
    }

    fprintf(fpOut, "Total Size Allocated: %llu\n", totalAllocated);
#endif
}

/*
** Function: MemCheck
** @brief    Walks every live allocation and verifies its control string and
**           overwrite-guard string are intact, reporting the first
**           corruption found (if any).
** @param    fpOut  - stream to print to
** @param    pszStr - caller-supplied tag included in any corruption message, to identify the call site
*/
void MemCheck(FILE* fpOut, const char* pszStr)
{
#ifdef NOTRACK
    printf("Memory is not being tracked!\n");
#else
    MyMallocReadLock w_lock(myMallocLock);
    {
        PMEMORY_NODE pNode = pFirstNode;

        while (pNode != NULL)
        {
            if (strcmp(pNode->controlStr, CONTROLSTR) != 0)
            {
                fprintf(fpOut, "There is an invalid control string in a memory tracking node (%s)!\n", pszStr);
                return;
            }

            if (memcmp(THE_MEMORY_OVERWRITE_CHECK_STR, ((char*)pNode) + pNode->memSize + sizeof(MEMORY_NODE), sizeof(THE_MEMORY_OVERWRITE_CHECK_STR)) != 0)
            {
                fprintf(fpOut, "The memory node for '%s' has been the victim of an overrun (%s)!\n", pNode->memName, pszStr);
                return;
            }

            pNode = pNode->pNextNode;
        }
    }
#endif
}

/*
** Function: MemCheckBlock
** @brief    See Mem.h.
*/
void MemCheckBlock(const void* pPtr, const char* pszWhere)
{
#ifdef LOOKFOROVERWRITE
    if (pPtr != NULL)
        memGuardVerify(pPtr, pszWhere);
#else
    (void)pPtr;
    (void)pszWhere;
#endif
}
