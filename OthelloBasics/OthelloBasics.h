/*
** Filename:  OthelloBasics.h
**
** Purpose:
**   Declares the CPU-safe surface for Othello board keys: BOARD_KEY (just
**   the two bitboard fields), numeric comparison, allocation, the
**   hardcoded starting position, and human-readable printing. Per the
**   strict CPU-organizes/GPU-solves boundary, nothing in this header ever
**   interprets a board key's bits by cell position -- no row-major indexing,
**   no move generation, no canonicalization. All of that is GPU-exclusive;
**   see OthelloBasicsForCUDA.h.
**
**   Player-to-move and board size are external context (which file/batch a
**   key belongs to), not a per-record field -- matching the on-disk
**   convention an earlier disk-key format already used (now merged into
**   this single BOARD_KEY, so the whole solution has one board-key type
**   instead of two near-identical ones).
*/

#pragma once

/* Includes */
#include "Error.h"
#include "RingPermutation.h"
#include <stdio.h>

/* Structures and Types */

/*
** Type:    BOARD_KEY
** @brief   16-byte on-disk/in-memory board key: the two bitboard fields,
**          nothing else. No next-player bit, no padding -- next-player is
**          tracked externally (by whichever file/batch a key belongs to),
**          not per-record.
*/
typedef struct _BoardKey
{
    unsigned long long ullCellsInUse;
    unsigned long long ullCellColors;
} BOARD_KEY, * PBOARD_KEY;
static_assert(sizeof(BOARD_KEY) == 16, "BOARD_KEY must be 16 bytes");

/* Macros and Defines */
#define BLACK ('B')
#define WHITE ('W')

/* Functions */

/*
** Function: BoardKeyCompare
** @brief    Three-way numeric comparison of two BOARD_KEY values (qsort-style).
** @param    arg1 - first BOARD_KEY (as const void*)
** @param    arg2 - second BOARD_KEY (as const void*)
** @return   <0/0/>0 as *arg1 is less/equal/greater than *arg2.
*/
int BoardKeyCompare(const void* arg1, const void* arg2);

/*
** Function: BoardKeyCompareBinSearchLE
** @brief    BinSearchLE-compatible wrapper around BoardKeyCompare.
** @param    arg1 - first BOARD_KEY (as const void*)
** @param    arg2 - second BOARD_KEY (as const void*)
** @param    size - unused; present to match BinSearchLE's comparator signature
** @return   <0/0/>0 as *arg1 is less/equal/greater than *arg2.
*/
int BoardKeyCompareBinSearchLE(const void* arg1, const void* arg2, const size_t size);

/*
** Function: BoardKeyAllocate
** @brief    Allocates and zero-initializes a new BOARD_KEY.
** @return   Newly allocated PBOARD_KEY. Free with MemFree(). nullptr on failure.
*/
PBOARD_KEY BoardKeyAllocate();

/*
** Function: BoardKeyAllocateClone
** @brief    Allocates a new BOARD_KEY and copies pOrigKey's fields into it.
** @param    pOrigKey - the key to clone
** @return   Newly allocated PBOARD_KEY. Free with MemFree(). nullptr on failure.
*/
PBOARD_KEY BoardKeyAllocateClone(PBOARD_KEY pOrigKey);

/*
** Function: BoardKeyAllocateFirstBoard
** @brief    Allocates the Othello starting position -- the one board that
**           exists before any move is played, priming the engine.
** @details  The starting position's occupied cells are always the center
**           2x2 (rows/cols 3-4, 0-indexed), which is identical across every
**           board size since the production encoding always centers a
**           board's active region within the 8x8 word. Its ring-ordered bit
**           pattern is therefore a single precomputed constant, not a
**           runtime computation -- there is no row-major bit manipulation
**           here, hardcoded or otherwise.
** @param    boardSize - board size being solved (4, 6, or 8); validated, does not affect the returned bits
** @return   Newly allocated PBOARD_KEY holding the starting position. Free with MemFree(). nullptr if boardSize is invalid.
*/
PBOARD_KEY BoardKeyAllocateFirstBoard(int boardSize);

/*
** Function: BoardKeyPrint
** @brief    Prints one or more BOARD_KEYs side by side in human-readable form.
** @param    fpOut     - stream to print to
** @param    boardSize - board size (4, 6, or 8), to determine which cells are active/displayed
** @param    keyCount  - number of PBOARD_KEY arguments that follow (capped internally)
** @param    ...       - keyCount PBOARD_KEY values to print
*/
void BoardKeyPrint(FILE* fpOut, int boardSize, int keyCount, ...);

/*
** Function: GetMaxMovesForBoardSize
** @brief    Returns the maximum possible legal-move count for a given board
**           size, used to size GPU batch/accumulator capacity.
** @details  A pure lookup table by board size -- no bit layout involved, so
**           this stays CPU-visible even though move generation itself is
**           GPU-exclusive.
** @param    boardSize - board size (4, 6, or 8)
** @return   Maximum legal moves for one board of that size. Fatals on an invalid board size.
*/
inline int GetMaxMovesForBoardSize(int boardSize)
{
    int returnSize = 0;

    /* Board size determines the max legal-move count; anything else is a
    ** caller error, not a recoverable runtime condition.
    */
    switch (boardSize)
    {
        case 4:
            returnSize = 6;
            break;
        case 6:
            returnSize = 19;
            break;
        case 8:
            returnSize = 28;
            break;
        default:
            Fatal(FATAL_INVALID_BOARD_SIZE, "GetMaxMovesForBoardSize: invalid board size specified (%d)", boardSize);
            returnSize = 0;
            break;
    }

    return returnSize;
}

/* Othello BOARD_KEY return codes */
constexpr auto RC_BOARD_INVALID_SIZE      = RC_BOARD_BASE + 0;
constexpr auto RC_BOARD_ALLOCATE_FAILURE  = RC_BOARD_BASE + 1;
