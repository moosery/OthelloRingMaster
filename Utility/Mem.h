/*
** Filename:  Mem.h
**
** Purpose:
**   Declares a heap allocator wrapper (MemMalloc/MemFree) meant to be used
**   throughout the solution in place of raw malloc/free, plus MemSize/
**   MemStatsPrint/MemCheck for reporting and corruption-checking. How much
**   checking the allocator does is a compile-time choice made in Mem.cpp:
**     - LOOKFOROVERWRITE: every block gets a guard header and a guard trailer;
**       a block found damaged (or freed twice) at MemFree, or at an explicit
**       MemCheckBlock call, stops the process and names the block. No list,
**       no lock -- cheap enough to leave on for a production run.
**     - NOTRACK: a thin wrapper straight to malloc/free.
**     - neither: a linked list of live allocations with an overwrite-guard
**       string after each block, reporting per-tag counts (slow; global lock).
**   (MEMDEBUG additionally prints every alloc/free in the list mode.)
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
[[nodiscard]] void* MemMalloc(const char* pStr, size_t sizeToAlloc);

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

/*
** Function: MemCheckBlock
** @brief    Verifies one MemMalloc block's guard header and trailer right now,
**           stopping the process (naming the block) if either was overwritten.
**           A no-op unless Mem.cpp is built with LOOKFOROVERWRITE.
** @details  Call it immediately after code that writes into a block through a
**           computed length -- a decompress, a read into a buffer, a memcpy of
**           a variable size -- so an overrun is caught at the statement that
**           caused it instead of at some later free. pPtr must be exactly a
**           pointer returned by MemMalloc (never an offset into one).
** @param    pPtr     - a pointer returned by MemMalloc
** @param    pszWhere - short description of the call site, for the failure message
*/
void MemCheckBlock(const void* pPtr, const char* pszWhere);
