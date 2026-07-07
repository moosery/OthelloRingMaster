/*
** Filename:  BinSearchLE.h
**
** Purpose:
**   Declares BinSearchLE, a "less-than-or-equal" binary search: rather than
**   only reporting an exact match, it locates the boundary entry nearest the
**   search key in the direction the data is sorted (the last entry <= the
**   key when ascending, or the last entry >= the key when descending), and
**   can back up over a run of duplicate-valued entries to land on the first
**   of the run. The comparison is applied to a sub-region of each entry
**   (offsetInEntryOfDataToSendToComparisonRoutine / sizeOfDataToCompare)
**   rather than requiring the whole entry to be the key, since callers
**   typically search fixed-size records by one field within them.
*/

#pragma once

/* Macros and Defines */
#define BINSEARCH_DATASORTED_ASCENDING  'A'
#define BINSEARCH_DATASORTED_DESCENDING 'D'

#define BINSEARCH_NOT_FOUND 0xFFFFFFFFFFFFFFFF

/* Functions */

/*
** Function: BinSearchLE
** @brief    Finds the boundary entry nearest pDataToFind in the direction
**           the data is sorted, optionally backed up over duplicates.
** @param    cSortDirection         - BINSEARCH_DATASORTED_ASCENDING or BINSEARCH_DATASORTED_DESCENDING
** @param    duplicates             - if true, back up over a run of equal-valued entries to the first of the run
** @param    pEqualFound            - out: set true if the returned entry compares equal to pDataToFind
** @param    pDataToSearch          - base address of the sorted entry array
** @param    numEntries             - number of entries in pDataToSearch
** @param    entrySize              - size in bytes of one entry
** @param    offsetInEntryOfDataToSendToComparisonRoutine - byte offset within each entry of the field to compare
** @param    sizeOfDataToCompare    - size in bytes of the field being compared
** @param    pDataToFind            - the key to search for (same layout as the compared field)
** @param    pCmpRtn                - 3-way comparator over two fields of size cmpSize
** @return   Index of the boundary entry, or BINSEARCH_NOT_FOUND if numEntries is 0 or no boundary entry exists on that side.
*/
size_t BinSearchLE(char cSortDirection, bool duplicates, bool* pEqualFound, void* pDataToSearch, size_t numEntries, size_t entrySize, size_t offsetInEntryOfDataToSendToComparisonRoutine, size_t sizeOfDataToCompare, void* pDataToFind, int (*pCmpRtn) (const void* pItem1, const void* pItem2, const size_t cmpSize));
