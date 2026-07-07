/*
** Filename:  Mem.cpp
**
** Purpose:
**   Implements MemMalloc/MemFree/MemSize/MemStatsPrint/MemCheck (declared in
**   Mem.h). Two compile-time modes, selected by the NOTRACK define below:
**     - NOTRACK defined (the default): a thin, zero-overhead wrapper
**       straight to malloc/free (still zero-initializing on alloc).
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

using namespace std;

/* Macros and Defines */
#define NOTRACK 1
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
#ifdef NOTRACK
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
#ifdef NOTRACK
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
