/*
** Filename:  BinSearchFile.cpp
**
** Purpose:
**   Implements BinarySearchFile (declared in BinarySearch.h): a binary
**   search over a sorted array of fixed-size records stored in an open
**   file, seeking and reading one record at a time so the whole array never
**   needs to be memory-resident.
*/

/* Includes */
#include "BinarySearch.h"
#include "Error.h"

/* Functions */

/*
** Function: BinarySearchFile
** @brief    Binary-searches a sorted array of fixed-size records stored in
**           an open file.
** @details  Classic three-way binary search over [leftIdx, rightIdx]: each
**           iteration seeks to the candidate record's byte offset, reads it
**           into pDataBuffer, and narrows the range based on the comparator
**           result. Any seek or read failure is treated as fatal, since a
**           partial/misaligned read would otherwise silently corrupt the
**           search.
** @param    fpOut                 - open file positioned over the sorted record array
** @param    pKey                  - the key to search for
** @param    pDataBuffer           - scratch buffer of at least sizeOfElementInArray bytes for the record just read
** @param    numElements           - number of records in the file
** @param    sizeOfElementInArray  - size in bytes of one record
** @param    pComp                 - 3-way comparator: <0/0/>0 as *pEntry is less/equal/greater than *pKey
** @param    pContext              - opaque context passed through to pComp
** @param    startIdx              - first element of the searched sub-range, in whole-file terms (0 by default)
** @return   Index of the matching record if found; otherwise -(insertion point + 1).
*/
long long BinarySearchFile(FILE* fpOut, void* pKey, void* pDataBuffer, long long numElements, long long sizeOfElementInArray, int (*pComp)(void* pContext, const void* pEntry, const void* pKey), void* pContext, long long startIdx)
{
    long long  leftIdx     = 0;                 /* lower bound of the still-uneliminated sub-range index (inclusive) */
    long long  rightIdx    = numElements - 1;   /* upper bound of the still-uneliminated sub-range index (inclusive) */
    long long  midIdx      = 0;                 /* sub-range-relative index of the record read/compared this iteration */
    long long  seekOffset;                      /* byte offset of (startIdx + midIdx) in the whole file              */

    while (leftIdx <= rightIdx)
    {
        midIdx = leftIdx + ((rightIdx - leftIdx) >> 1);

        seekOffset = ((startIdx + midIdx) * sizeOfElementInArray);

        /* A failed seek means the search can no longer trust file position --
        ** fatal rather than silently comparing against stale/wrong data.
        */
        if (_fseeki64(fpOut, seekOffset, SEEK_SET) != 0)
        {
            Fatal(FATAL_SEEK_FAILED, "Could not seek to value of %zd\n", seekOffset);
        }

        /* A short/failed read would leave pDataBuffer holding a stale or
        ** partial record -- fatal rather than comparing against garbage.
        */
        if (fread(pDataBuffer, sizeOfElementInArray, 1, fpOut) != 1)
        {
            Fatal(FATAL_READ_FAILED, "Could not read record at seek value of %zd\n", seekOffset);
        }

        int cmpVal = pComp(pContext, (void*)pDataBuffer, pKey);

        if (cmpVal < 0)
        {
            leftIdx = midIdx + 1;
        }
        else if (cmpVal > 0)
        {
            rightIdx = midIdx - 1;
        }
        else
        {
            return startIdx + midIdx;
        }
    }

    return -(startIdx + leftIdx + 1);
}
