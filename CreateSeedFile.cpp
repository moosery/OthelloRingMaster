/*
** Filename:  CreateSeedFile.cpp
**
** Purpose:
**   Implements CreateSeedFile declared in CreateSeedFile.h.
*/

/* Includes */
#include "CreateSeedFile.h"
#include "RSFFileName.h"
#include "OthelloBasics.h"
#include "Logger.h"
#include "Mem.h"
#include <windows.h>

/* Functions */

/*
** Function: CreateSeedFile
** @brief    Writes the standard Othello starting position as a single
**           BOARD_KEY record to level 0's store file.
** @param    pConfig - run configuration (boardSize)
** @param    pState  - solver state (storeDirectory)
*/
void CreateSeedFile(POthelloRingMasterConfig pConfig, POthelloRingMasterState pState)
{
    /* Level 0 seed is always a single black-turn board. */
    char seedPath[MAX_FULL_PATH_NAME];
    RSFNameStoreFile(seedPath, sizeof(seedPath), pState->storeDirectory,
                      (int)pConfig->boardSize, 0, RSF_PLAYER_BLACK, 0);

    /* Already exists and is valid -- resume scenario, nothing to write
    ** except ensure the complete sentinel is present (idempotent).
    */
    RSFReader* r = RSFOpen(seedPath);
    if (r)
    {
        uint64_t count = RSFReaderTrailer(r)->recordCount;
        RSFClose(&r);
        LoggerLog("CreateSeedFile: seed already exists (%llu board(s)), skipping: %s\n",
                  count, seedPath);
    }
    else
    {
        /* The one necessary CPU-side exception to the CPU-organizes/
        ** GPU-solves boundary: BoardKeyAllocateFirstBoard returns a
        ** precomputed ring-ordered constant, not a runtime permutation --
        ** the CPU never executes any row-major-to-ring transform here.
        */
        PBOARD_KEY pRoot = BoardKeyAllocateFirstBoard((int)pConfig->boardSize);
        if (!pRoot)
            Fatal(FATAL_ALLOCATION_FAILED, "CreateSeedFile: BoardKeyAllocateFirstBoard failed");

        UINT64_PAIR rec;
        rec.hi = pRoot->ullCellsInUse;
        rec.lo = pRoot->ullCellColors;
        MemFree(pRoot);

        RSFWrite(seedPath, &rec, 1);
        LoggerLog("CreateSeedFile: wrote level-0 seed -> '%s' (1 board)\n", seedPath);
    }

    /* Level 0 has no end-of-level merge, so write its complete sentinel here. */
    char sentPath[MAX_FULL_PATH_NAME];
    SentinelNameComplete(sentPath, sizeof(sentPath), pState->storeDirectory, (int)pConfig->boardSize, 0);
    HANDLE hs = CreateFileA(sentPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (hs != INVALID_HANDLE_VALUE) CloseHandle(hs);
}
