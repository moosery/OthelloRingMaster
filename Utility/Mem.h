/*
** Filename:  Mem.h
**
** Purpose:
**   Declares a heap allocator wrapper (MemMalloc/MemFree) meant to be used
**   throughout the solution in place of raw malloc/free, plus MemSize/
**   MemStatsPrint/MemCheck for reporting and corruption-checking. Whether
**   allocations are actually tracked (a linked list of live allocations
**   with an overwrite-guard string after each block, to catch buffer
**   overruns and report per-tag allocation counts) or just thin-wrapped
**   straight to malloc/free is a compile-time choice made in Mem.cpp
**   (the NOTRACK/MEMDEBUG defines there).
*/

#pragma once

/* Includes */
#include <memory.h>
#include <stdio.h>

/* Functions */

/*
** Function: MemMalloc
** @brief    Allocates sizeToAlloc bytes, zero-initialized, tagged with pStr
**           for reporting/debugging.
** @param    pStr        - name tag identifying this allocation's call site/purpose
** @param    sizeToAlloc - number of bytes requested
** @return   Pointer to the allocated memory, or nullptr on failure.
*/
void* MemMalloc(const char* pStr, size_t sizeToAlloc);

/*
** Function: MemFree
** @brief    Frees memory previously returned by MemMalloc. Safe to call with nullptr.
** @param    pPtr - the memory to free
*/
void MemFree(void* pPtr);

/*
** Function: MemSize
** @brief    Returns the total number of bytes currently allocated via MemMalloc.
** @return   Total bytes allocated and not yet freed.
*/
size_t MemSize();

/*
** Function: MemStatsPrint
** @brief    Prints a breakdown of live allocations by tag, plus the total
**           bytes allocated.
** @param    fpOut - stream to print to
*/
void MemStatsPrint(FILE* fpOut);

/*
** Function: MemCheck
** @brief    Walks every live allocation and verifies its control string and
**           overwrite-guard string are intact, reporting the first
**           corruption found (if any).
** @param    fpOut  - stream to print to
** @param    pszStr - caller-supplied tag included in any corruption message, to identify the call site
*/
void MemCheck(FILE* fpOut, const char* pszStr);
