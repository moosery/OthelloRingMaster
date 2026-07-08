/*
** Filename:  BoardKeyPrint.cpp
**
** Purpose:
**   Implements BoardKeyPrint (declared in OthelloBasics.h): human-readable
**   printing of one or more BOARD_KEYs, side by side.
**
** Notes:
**   BOARD_KEY's bits are ring-ordered, not row-major -- printing never
**   converts a key to row-major to display it. Instead, for each visual
**   (row,col) cell, the inverse ring permutation table (RingPermutation.h)
**   is consulted to find which ring-bit position that cell lives at, and
**   that bit is tested directly on the still-ring-ordered value. The table
**   is plain lookup data; no row-major value is ever materialized here,
**   consistent with the CPU-organizes/GPU-solves boundary.
*/

/* Includes */
#include "OthelloBasics.h"
#include <stdarg.h>

/* Constants */
#define MAXBOARDPRINT 4

/* Functions */

/*
** Function: getBoardActiveBounds
** @brief    Returns the [startIdx, endIdx) row/col range of active cells for
**           a given board size. Pure arithmetic on boardSize -- not a
**           row-major bit operation.
** @param    boardSize  - board size (4, 6, or 8)
** @param    pStartIdx  - out: first active row/col index
** @param    pEndIdx    - out: one past the last active row/col index
*/
static void getBoardActiveBounds(int boardSize, int* pStartIdx, int* pEndIdx)
{
    *pStartIdx = (8 - boardSize) / 2;
    *pEndIdx   = 8 - *pStartIdx;
}

/*
** Function: testRingBit
** @brief    Tests bit ringPos (0 = MSB) of a ring-ordered 64-bit value.
** @param    value   - the ring-ordered value to test
** @param    ringPos - ring-bit position to test (0 = MSB, 63 = LSB)
** @return   true if that bit is set.
*/
static inline bool testRingBit(unsigned long long value, int ringPos)
{
    return ((value >> (63 - ringPos)) & 1ULL) != 0;
}

/*
** Function: getInverseRingTable
** @brief    Returns the (row*8+col) -> ring-position lookup table, built once.
** @return   Pointer to a 64-entry table, valid for the process lifetime.
*/
static const int* getInverseRingTable()
{
    static std::vector<int> inverseTable = BuildInverseRingPermutation(8, 0);
    return inverseTable.data();
}

/*
** Function: printRowSeparator
** @brief    Prints one "+---+---+...+" style separator row.
** @param    fpOut    - stream to print to
** @param    startch  - character to print in place of the leading '+' (e.g. the next-player marker)
** @param    startIdx - first active column index
** @param    endIdx   - one past the last active column index
*/
static void printRowSeparator(FILE* fpOut, char startch, int startIdx, int endIdx)
{
    fprintf(fpOut, "%c", startch);

    for (int col = startIdx; col < endIdx; col++)
        fprintf(fpOut, "---+");
}

/*
** Function: BoardKeyPrint
** @brief    Prints one or more BOARD_KEYs side by side in human-readable form.
** @param    fpOut     - stream to print to
** @param    boardSize - board size (4, 6, or 8), to determine which cells are active/displayed
** @param    keyCount  - number of PBOARD_KEY arguments that follow (capped internally)
** @param    ...       - keyCount PBOARD_KEY values to print
*/
void BoardKeyPrint(FILE* fpOut, int boardSize, int keyCount, ...)
{
    PBOARD_KEY  pKeyArray[MAXBOARDPRINT];
    int         keyArraySize = (keyCount <= MAXBOARDPRINT ? keyCount : MAXBOARDPRINT);

    if (keyArraySize < 1)
        return;

    va_list args;
    va_start(args, keyCount);
    for (int i = 0; i < keyArraySize; i++)
        pKeyArray[i] = va_arg(args, PBOARD_KEY);
    va_end(args);

    int  startIdx, endIdx;
    getBoardActiveBounds(boardSize, &startIdx, &endIdx);
    const int* pInverseRing = getInverseRingTable();

    for (int i = 0; i < keyArraySize; i++)
    {
        if (i > 0)
            fprintf(fpOut, "   ");
        fprintf(fpOut, "ullCellsInUse=0x%016llX", pKeyArray[i]->ullCellsInUse);
    }
    fprintf(fpOut, "\n");

    for (int i = 0; i < keyArraySize; i++)
    {
        if (i > 0)
            fprintf(fpOut, "   ");
        fprintf(fpOut, "ullCellColors=0x%016llX", pKeyArray[i]->ullCellColors);
    }
    fprintf(fpOut, "\n");

    for (int row = startIdx; row < endIdx; row++)
    {
        /* Loop through all keys to print the separators */
        for (int keyIdx = 0; keyIdx < keyArraySize; keyIdx++)
        {
            if (keyIdx > 0)
                fprintf(fpOut, "   ");

            printRowSeparator(fpOut, '+', startIdx, endIdx);
        }
        fprintf(fpOut, "\n");

        /* Loop through all keys to print this row's cell values */
        for (int keyIdx = 0; keyIdx < keyArraySize; keyIdx++)
        {
            if (keyIdx > 0)
                fprintf(fpOut, "   ");

            fprintf(fpOut, "|");
            for (int col = startIdx; col < endIdx; col++)
            {
                int   ringPos = pInverseRing[row * 8 + col];
                char  color;

                if (testRingBit(pKeyArray[keyIdx]->ullCellsInUse, ringPos))
                    color = testRingBit(pKeyArray[keyIdx]->ullCellColors, ringPos) ? BLACK : WHITE;
                else
                    color = ' ';

                fprintf(fpOut, " %c |", color);
            }
        }
        fprintf(fpOut, "\n");
    }

    for (int keyIdx = 0; keyIdx < keyArraySize; keyIdx++)
    {
        if (keyIdx > 0)
            fprintf(fpOut, "   ");

        printRowSeparator(fpOut, '+', startIdx, endIdx);
    }
    fprintf(fpOut, "\n");
}
