/*
** Filename:  StoreLevelScan.cpp
**
** Purpose:
**   Implements FindDeepestCompleteLevel declared in StoreLevelScan.h.
*/

/* Includes */
#include "StoreLevelScan.h"
#include "RSFFileName.h"
#include "RingNestedIndex.h"
#include <windows.h>

/* Internal Helpers */

/*
** Function: LevelHasBoardData
** @brief    Checks whether level actually has ring-index files for at
**           least one color -- RingMaster writes a level+1 _complete
**           sentinel purely to record "confirmed nothing past here" the
**           moment its own solve loop produces zero boards for a level,
**           so a sentinel's presence alone does NOT mean that level has
**           real data (see file Notes).
** @param    storeDir  - RingMaster's finished store directory
** @param    boardSize - exact board size
** @param    level     - level to check
** @return   true if either color has nested-index files present.
*/
static bool LevelHasBoardData(const char* storeDir, int boardSize, int level)
{
    bool hasRing1 = RingNestedIndexHasRing1(boardSize);
    bool hasRing2 = RingNestedIndexHasRing2(boardSize);

    for (int player = RSF_PLAYER_WHITE; player <= RSF_PLAYER_BLACK; player++)
    {
        char cellsInUsePath[MAX_FULL_PATH_NAME];
        char ring1PathBuf[MAX_FULL_PATH_NAME];
        char ring2PathBuf[MAX_FULL_PATH_NAME];
        char ring34Path[MAX_FULL_PATH_NAME];
        RSFNameCellsInUseFile(cellsInUsePath, sizeof(cellsInUsePath), storeDir, boardSize, level, player, 0);
        if (hasRing1)
            RSFNameRing1File(ring1PathBuf, sizeof(ring1PathBuf), storeDir, boardSize, level, player, 0);
        if (hasRing2)
            RSFNameRing2File(ring2PathBuf, sizeof(ring2PathBuf), storeDir, boardSize, level, player, 0);
        RSFNameRing34File(ring34Path, sizeof(ring34Path), storeDir, boardSize, level, player, 0);

        const char* ring1Path = hasRing1 ? ring1PathBuf : nullptr;
        const char* ring2Path = hasRing2 ? ring2PathBuf : nullptr;

        if (RingNestedIndexFileCount(cellsInUsePath, ring1Path, ring2Path, ring34Path) > 0)
            return true;
    }

    return false;
}

/* Functions */

/*
** Function: FindDeepestCompleteLevel
** @brief    See StoreLevelScan.h.
*/
int FindDeepestCompleteLevel(const char* storeDir, int boardSize)
{
    int deepest = -1;

    for (int level = 0; level < CALC_MAX_LEVELS; level++)
    {
        char sentPath[MAX_FULL_PATH_NAME];
        SentinelNameComplete(sentPath, sizeof(sentPath), storeDir, boardSize, level);

        if (GetFileAttributesA(sentPath) == INVALID_FILE_ATTRIBUTES)
            break;

        deepest = level;
    }

    /* The deepest sentinel found can be RingMaster's "confirmed nothing
    ** past here" marker rather than a real data level (see LevelHasBoardData) --
    ** step back to the deepest level that actually has board files.
    */
    while (deepest >= 0 && !LevelHasBoardData(storeDir, boardSize, deepest))
        deepest--;

    return deepest;
}
