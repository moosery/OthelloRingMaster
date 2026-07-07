/*
** Filename:  BinSearch.cpp
**
** Purpose:
**   Implements BinarySearch (declared in BinarySearch.h): a binary search
**   over an in-memory array of fixed-size records using a caller-supplied
**   comparator.
*/

/* Includes */
#include "BinarySearch.h"

/* Functions */

/*
** Function: BinarySearch
** @brief    Binary-searches a sorted in-memory array of fixed-size records.
** @details  Standard lower-bound binary search: narrows [leftIdx, rightIdx)
**           until they meet, then checks whether the converged index is an
**           exact match. Returning the converged index (negated/offset)
**           even on a miss lets the caller insert the key at the right spot
**           without a second search.
** @param    dataArray             - base address of the sorted record array
** @param    pKey                  - the key to search for
** @param    numElements           - number of records in dataArray
** @param    sizeOfElementInArray  - size in bytes of one record
** @param    pComp                 - 3-way comparator: <0/0/>0 as *pEntry is less/equal/greater than *pKey
** @param    pContext              - opaque context passed through to pComp
** @return   Index of the matching record if found; otherwise -(insertion point + 1).
*/
long long BinarySearch(void* dataArray, void* pKey, long long numElements, long long sizeOfElementInArray, int (*pComp)(void* pContext, const void* pEntry, const void* pKey), void* pContext)
{
    long long  leftIdx    = 0;                  /* lower bound of the still-uneliminated range (inclusive)      */
    long long  rightIdx   = numElements;        /* upper bound of the still-uneliminated range (exclusive)      */
    char*      pFirstByte = (char*)dataArray;   /* dataArray reinterpreted as bytes so pointer math is in bytes */

    /* Narrow the range until it collapses to the insertion point. */
    while (leftIdx < rightIdx)
    {
        long long  midIdx          = leftIdx + ((rightIdx - leftIdx) >> 1);
        char*      pLocToCompare   = pFirstByte + (midIdx * sizeOfElementInArray);
        int        cmpVal          = pComp(pContext, (void*)pLocToCompare, pKey);

        if (cmpVal < 0)
            leftIdx = midIdx + 1;
        else
            rightIdx = midIdx;
    }

    /* leftIdx now points at the insertion point; check if it's an exact match. */
    if (leftIdx < numElements)
    {
        char* pLocToCompare = pFirstByte + (leftIdx * sizeOfElementInArray);
        if (pComp(pContext, (void*)pLocToCompare, pKey) == 0)
            return leftIdx;
    }

    return -(leftIdx + 1);
}
