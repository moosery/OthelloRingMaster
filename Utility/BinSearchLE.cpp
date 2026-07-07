/*
** Filename:  BinSearchLE.cpp
**
** Purpose:
**   Implements BinSearchLE (declared in BinSearchLE.h): a "less-than-or-
**   equal" binary search that locates the boundary entry nearest a search
**   key in the direction the data is sorted, optionally backing up over a
**   run of duplicate-valued entries to the first of that run.
*/

/* Includes */
#include "BinSearchLE.h"

/* Functions */

/*
** Function: BinSearchLE
** @brief    Finds the boundary entry nearest pDataToFind in the direction
**           the data is sorted, optionally backed up over duplicates.
** @details  Runs a standard binary search narrowing [low, high), with the
**           comparison direction flipped depending on ascending vs.
**           descending sort order, so the loop always converges on the
**           correct boundary side. After convergence, checks whether the
**           boundary entry is an exact match (pEqualFound), then -- if the
**           caller asked for duplicate handling -- walks backward over any
**           further entries that compare equal, so the first entry of a
**           run of duplicates is returned rather than an arbitrary one
**           within the run.
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
size_t BinSearchLE(char cSortDirection, bool duplicates, bool* pEqualFound, void* pDataToSearch, size_t numEntries, size_t entrySize, size_t offsetInEntryOfDataToSendToComparisonRoutine, size_t sizeOfDataToCompare, void* pDataToFind, int (*pCmpRtn) (const void* pItem1, const void* pItem2, const size_t cmpSize))
{
    char*   pDataToSearchAsChar = (char*)pDataToSearch;                                         /* pDataToSearch reinterpreted as bytes for pointer arithmetic    */
    char*   pDataToFindAsChar   = (char*)pDataToFind;                                           /* pDataToFind reinterpreted as bytes for pointer arithmetic      */
    size_t  low                = 0;                                                             /* lower bound of the still-uneliminated range (inclusive)        */
    size_t  high               = numEntries;                                                    /* upper bound of the still-uneliminated range (exclusive)        */
    size_t  result             = BINSEARCH_NOT_FOUND;                                           /* converged index; BINSEARCH_NOT_FOUND until a boundary is found */
    int     direction          = (cSortDirection == BINSEARCH_DATASORTED_ASCENDING ? 1 : -1);   /* +1 if ascending, -1 if descending                              */
    *pEqualFound = false;

    /* Nothing to search in an empty array. */
    if (numEntries == 0)
        return result;

    /* Ascending data: the boundary we want is the last entry <= the key, so
    ** the loop pushes low past every entry that compares >= the key.
    */
    if (direction == 1)
    {
        while (low < high)
        {
            size_t mid       = low + ((high - low) >> 1);
            int    cmpResult = pCmpRtn(pDataToFindAsChar, pDataToSearchAsChar + mid * entrySize + offsetInEntryOfDataToSendToComparisonRoutine, sizeOfDataToCompare);
            if (cmpResult >= 0)
                low = mid + 1;
            else
                high = mid;
        }
        result = (low > 0) ? low - 1 : BINSEARCH_NOT_FOUND;
    }
    /* Descending data: the boundary we want is the last entry >= the key, so
    ** the comparison sense is mirrored relative to the ascending case.
    */
    else
    {
        while (low < high)
        {
            size_t mid       = low + ((high - low) >> 1);
            int    cmpResult = pCmpRtn(pDataToFindAsChar, pDataToSearchAsChar + mid * entrySize + offsetInEntryOfDataToSendToComparisonRoutine, sizeOfDataToCompare);
            if (cmpResult < 0)
                low = mid + 1;
            else
                high = mid;
        }
        result = (low < numEntries) ? low : BINSEARCH_NOT_FOUND;
    }

    /* Report whether the boundary entry landed exactly on the key. */
    if (result != BINSEARCH_NOT_FOUND)
    {
        if (pCmpRtn(pDataToFindAsChar, pDataToSearchAsChar + result * entrySize + offsetInEntryOfDataToSendToComparisonRoutine, sizeOfDataToCompare) == 0)
            *pEqualFound = true;
    }

    /* Back up over duplicates, if requested, so the first entry of a run of
    ** equal-valued entries is returned rather than wherever the binary
    ** search happened to converge within that run. Skipped when the
    ** boundary is already at index 0 on the ascending side, since there is
    ** nothing further to back up over.
    */
    if (result != BINSEARCH_NOT_FOUND && duplicates && !(direction == 1 && result == 0))
    {
        char*   rowOrigVal  = pDataToSearchAsChar + result * entrySize + offsetInEntryOfDataToSendToComparisonRoutine;
        size_t  pos         = result - direction;
        char*   pRec        = pDataToSearchAsChar + pos * entrySize;

        while (pRec >= pDataToSearchAsChar && pos < numEntries)
        {
            if (pCmpRtn(rowOrigVal, pRec + offsetInEntryOfDataToSendToComparisonRoutine, sizeOfDataToCompare) != 0)
                break;
            result = pos;
            pos -= direction;
            pRec = pDataToSearchAsChar + pos * entrySize;
        }
    }

    return result;
}
