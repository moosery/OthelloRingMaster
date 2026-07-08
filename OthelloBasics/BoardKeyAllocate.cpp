/*
** Filename:  BoardKeyAllocate.cpp
**
** Purpose:
**   Implements BoardKeyAllocate/BoardKeyAllocateClone/BoardKeyAllocateFirstBoard
**   (declared in OthelloBasics.h).
*/

/* Includes */
#include "OthelloBasics.h"
#include "Mem.h"

/* Constants */

/* The Othello starting position's ring-ordered bit pattern -- precomputed
** once, offline, from the row-major starting position (center 2x2: (3,3)
** and (4,4) White, (3,4) and (4,3) Black) via the same ring permutation
** RingPermutation.h builds at runtime. Identical for every board size,
** since the production encoding always centers a board's active region
** within the 8x8 word, so the center 2x2 never moves. This is the one
** necessary exception to "the CPU never deals with row-major bit
** structure" -- the permutation was run once, by us, not by running code.
*/
static const unsigned long long kStartingCellsInUseRingOrdered = 0x000000000000000FULL;
static const unsigned long long kStartingCellColorsRingOrdered = 0x0000000000000005ULL;

/* Functions */

/*
** Function: BoardKeyAllocate
** @brief    Allocates and zero-initializes a new BOARD_KEY.
** @return   Newly allocated PBOARD_KEY. Free with MemFree(). nullptr on failure.
*/
PBOARD_KEY BoardKeyAllocate()
{
    PBOARD_KEY pKey = (PBOARD_KEY)MemMalloc("BOARD_KEY.boardKeyAllocate", sizeof(BOARD_KEY));

    if (pKey == NULL)
        Error(RC_BOARD_ALLOCATE_FAILURE, "BoardKeyAllocate: Failed to allocate memory for a BOARD_KEY structure.");

    return pKey;
}

/*
** Function: BoardKeyAllocateClone
** @brief    Allocates a new BOARD_KEY and copies pOrigKey's fields into it.
** @param    pOrigKey - the key to clone
** @return   Newly allocated PBOARD_KEY. Free with MemFree(). nullptr on failure.
*/
PBOARD_KEY BoardKeyAllocateClone(PBOARD_KEY pOrigKey)
{
    PBOARD_KEY pKey = BoardKeyAllocate();

    if (pKey != NULL)
        *pKey = *pOrigKey;

    return pKey;
}

/*
** Function: BoardKeyAllocateFirstBoard
** @brief    Allocates the Othello starting position -- the one board that
**           exists before any move is played, priming the engine.
** @param    boardSize - board size being solved (4, 6, or 8); validated, does not affect the returned bits
** @return   Newly allocated PBOARD_KEY holding the starting position. Free with MemFree(). nullptr if boardSize is invalid.
*/
PBOARD_KEY BoardKeyAllocateFirstBoard(int boardSize)
{
    /* Only these three board sizes are supported; anything else is a caller error. */
    switch (boardSize)
    {
        case 4:
        case 6:
        case 8:
            break;
        default:
            Error(RC_BOARD_INVALID_SIZE, "BoardKeyAllocateFirstBoard: invalid size specified (%d)\n", boardSize);
            return NULL;
    }

    PBOARD_KEY pKey = BoardKeyAllocate();
    if (pKey == NULL)
        return NULL;

    pKey->ullCellsInUse = kStartingCellsInUseRingOrdered;
    pKey->ullCellColors = kStartingCellColorsRingOrdered;

    return pKey;
}
