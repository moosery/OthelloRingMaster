/*
** Filename:  CreateSeedFile.cpp
**
** Purpose:
**   Implements CreateSeedFile declared in CreateSeedFile.h.
**
** Notes:
**   Writes the seed board in the ring nested-index format (see
**   OthelloBasics/RingNestedIndex.h) rather than a flat RSF file, so
**   level 0's input matches every later level's output format (see
**   MergeFiles.cpp's ConvertLevelOutputToNestedIndex, and
**   LevelSolverThread.cpp's FeedNestedIndexLevel, which is what actually
**   reads it back).
*/

/* Includes */
#include "CreateSeedFile.h"
#include "RSFFileName.h"
#include "OthelloBasics.h"
#include "RingNestedIndex.h"
#include "Logger.h"
#include "Mem.h"
#include <windows.h>

/* Functions */

/*
** Function: CreateSeedFile
** @brief    Writes the standard Othello starting position as level 0's
**           single-board nested-index store.
** @param    pConfig - run configuration (boardSize)
** @param    pState  - solver state (storeDirectory)
*/
void CreateSeedFile(POthelloRingMasterConfig pConfig, POthelloRingMasterState pState)
{
    int boardSize = (int)pConfig->boardSize;
    bool hasRing1 = RingNestedIndexHasRing1(boardSize);
    bool hasRing2 = RingNestedIndexHasRing2(boardSize);

    /* Level 0 seed is always a single black-turn board. Ring_1/Ring_2 paths
    ** are only built (and the corresponding files only ever created) when
    ** this board size actually uses that level -- see RingNestedIndex.h Notes.
    */
    char cellsInUsePath[MAX_FULL_PATH_NAME];
    char ring1PathBuf[MAX_FULL_PATH_NAME];
    char ring2PathBuf[MAX_FULL_PATH_NAME];
    char ring34Path[MAX_FULL_PATH_NAME];
    RSFNameCellsInUseFile(cellsInUsePath, sizeof(cellsInUsePath), pState->storeDirectory, boardSize, 0, RSF_PLAYER_BLACK, 0);
    if (hasRing1)
        RSFNameRing1File(ring1PathBuf, sizeof(ring1PathBuf), pState->storeDirectory, boardSize, 0, RSF_PLAYER_BLACK, 0);
    if (hasRing2)
        RSFNameRing2File(ring2PathBuf, sizeof(ring2PathBuf), pState->storeDirectory, boardSize, 0, RSF_PLAYER_BLACK, 0);
    RSFNameRing34File(ring34Path, sizeof(ring34Path), pState->storeDirectory, boardSize, 0, RSF_PLAYER_BLACK, 0);

    const char* ring1Path = hasRing1 ? ring1PathBuf : nullptr;
    const char* ring2Path = hasRing2 ? ring2PathBuf : nullptr;

    /* Already exists and is valid -- resume scenario, nothing to write
    ** except ensure the complete sentinel is present (idempotent).
    */
    RingNestedIndexReader existing;
    if (existing.Load(cellsInUsePath, ring1Path, ring2Path, ring34Path) && existing.GetBoardCount() > 0)
    {
        LoggerLog("CreateSeedFile: seed already exists (%llu board(s)), skipping\n",
                  (unsigned long long)existing.GetBoardCount());
    }
    else
    {
        /* The one necessary CPU-side exception to the CPU-organizes/
        ** GPU-solves boundary: BoardKeyAllocateFirstBoard returns a
        ** precomputed ring-ordered constant, not a runtime permutation --
        ** the CPU never executes any row-major-to-ring transform here.
        */
        PBOARD_KEY pRoot = BoardKeyAllocateFirstBoard(boardSize);
        if (!pRoot)
            Fatal(FATAL_ALLOCATION_FAILED, "CreateSeedFile: BoardKeyAllocateFirstBoard failed");

        RSFWriter*       pCellsInUseWriter = RSFWriterOpenZL(cellsInUsePath);
        Lz4StreamWriter* pRing1Writer      = hasRing1 ? Lz4StreamWriterOpen(ring1Path) : nullptr;
        Lz4StreamWriter* pRing2Writer      = hasRing2 ? Lz4StreamWriterOpen(ring2Path) : nullptr;
        Lz4StreamWriter* pRing34Writer     = Lz4StreamWriterOpen(ring34Path);

        RingNestedIndexBuilder builder;
        builder.Init(pCellsInUseWriter, pRing1Writer, pRing2Writer, pRing34Writer);
        builder.Process(*pRoot);
        builder.Finish();

        RSFWriterClose(pCellsInUseWriter);
        if (pRing1Writer) Lz4StreamWriterClose(pRing1Writer);
        if (pRing2Writer) Lz4StreamWriterClose(pRing2Writer);
        Lz4StreamWriterClose(pRing34Writer);

        MemFree(pRoot);

        LoggerLog("CreateSeedFile: wrote level-0 seed under '%s' (1 board)\n", pState->storeDirectory);
    }

    /* Level 0 has no end-of-level merge, so write its complete sentinel here. */
    char sentPath[MAX_FULL_PATH_NAME];
    SentinelNameComplete(sentPath, sizeof(sentPath), pState->storeDirectory, boardSize, 0);
    HANDLE hs = CreateFileA(sentPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (hs != INVALID_HANDLE_VALUE) CloseHandle(hs);
}
