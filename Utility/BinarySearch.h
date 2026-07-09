/*
** Filename:  BinarySearch.h
**
** Purpose:
**   Declares two binary-search variants over a sorted, fixed-size-record
**   array, both driven by a caller-supplied 3-way comparator so element
**   layout and key interpretation stay opaque to the search itself:
**     - BinarySearch     searches an array already resident in memory.
**     - BinarySearchFile searches directly against an open file via
**       seek+read, for record sets too large to hold in memory at once.
**   Both follow the "found index, or -(insertion point + 1)" return
**   convention (as in Java's Arrays.binarySearch) so a negative result still
**   tells the caller exactly where the key would need to be inserted.
*/

#pragma once

/* Includes */
#include <stdio.h>

/* Functions */

/*
** Function: BinarySearch
** @brief    Binary-searches a sorted in-memory array of fixed-size records.
** @param    dataArray             - base address of the sorted record array
** @param    pKey                  - the key to search for
** @param    numElements           - number of records in dataArray
** @param    sizeOfElementInArray  - size in bytes of one record
** @param    pComp                 - 3-way comparator: <0/0/>0 as *pEntry is less/equal/greater than *pKey
** @param    pContext              - opaque context passed through to pComp
** @return   Index of the matching record if found; otherwise -(insertion point + 1).
*/
long long BinarySearch(void* dataArray, void* pKey, long long numElements, long long sizeOfElementInArray, int (*pComp)(void* pContext, const void* pEntry, const void* pKey), void* pContext);

/*
** Function: BinarySearchFile
** @brief    Binary-searches a sorted array of fixed-size records stored in
**           an open file, seeking and reading one record at a time rather
**           than requiring the whole array in memory.
** @param    fpOut                 - open file positioned over the sorted record array
** @param    pKey                  - the key to search for
** @param    pDataBuffer           - scratch buffer of at least sizeOfElementInArray bytes for the record just read
** @param    numElements           - number of records in the file
** @param    sizeOfElementInArray  - size in bytes of one record
** @param    pComp                 - 3-way comparator: <0/0/>0 as *pEntry is less/equal/greater than *pKey
** @param    pContext              - opaque context passed through to pComp
** @param    startIdx              - index of the first element of the searched sub-range within the
**                                    file's own logical record array (0 = search from the start, the
**                                    prior default/only behavior); every seek and the returned index
**                                    are offset by this, so results stay in whole-file terms even when
**                                    restricting the search to [startIdx, startIdx+numElements)
** @return   Index of the matching record if found; otherwise -(insertion point + 1).
*/
long long BinarySearchFile(FILE* fpOut, void* pKey, void* pDataBuffer, long long numElements, long long sizeOfElementInArray, int (*pComp)(void* pContext, const void* pEntry, const void* pKey), void* pContext, long long startIdx = 0);
