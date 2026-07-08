/*
** Filename:  BoardKeyCompare.cpp
**
** Purpose:
**   Implements BoardKeyCompare/BoardKeyCompareBinSearchLE (declared in
**   OthelloBasics.h): a plain three-way numeric comparison over BOARD_KEY's
**   two fields, matching the ordering the GPU radix sort already uses.
*/

/* Includes */
#include "OthelloBasics.h"

/* Functions */

/*
** Function: BoardKeyCompare
** @brief    Three-way numeric comparison of two BOARD_KEY values (qsort-style).
** @param    arg1 - first BOARD_KEY (as const void*)
** @param    arg2 - second BOARD_KEY (as const void*)
** @return   <0/0/>0 as *arg1 is less/equal/greater than *arg2.
*/
int BoardKeyCompare(const void* arg1, const void* arg2)
{
    const BOARD_KEY* a = (const BOARD_KEY*)arg1;
    const BOARD_KEY* b = (const BOARD_KEY*)arg2;

    if (a->ullCellsInUse != b->ullCellsInUse)
        return (a->ullCellsInUse < b->ullCellsInUse) ? -1 : 1;
    if (a->ullCellColors != b->ullCellColors)
        return (a->ullCellColors < b->ullCellColors) ? -1 : 1;
    return 0;
}

/*
** Function: BoardKeyCompareBinSearchLE
** @brief    BinSearchLE-compatible wrapper around BoardKeyCompare.
** @param    arg1 - first BOARD_KEY (as const void*)
** @param    arg2 - second BOARD_KEY (as const void*)
** @param    size - unused; present to match BinSearchLE's comparator signature
** @return   <0/0/>0 as *arg1 is less/equal/greater than *arg2.
*/
int BoardKeyCompareBinSearchLE(const void* arg1, const void* arg2, const size_t size)
{
    return BoardKeyCompare(arg1, arg2);
}
