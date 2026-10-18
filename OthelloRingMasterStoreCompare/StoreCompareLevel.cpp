/*
** Filename:  StoreCompareLevel.cpp
**
** Purpose:
**   Implements StoreCompareLevelPlayer -- see StoreCompareLevel.h.
*/

/* Includes */
#include "StoreCompareLevel.h"
#include "RingNestedIndex.h"
#include "RSFFileName.h"
#include "FileAndDirUtils.h"
#include <cstdio>

/*
** Function: OpenSide
** @brief    Builds one store's four nested-index file paths for this
**           level/player and opens a PullReader against them.
** @param    storeDir  - the store directory (old or new side)
** @param    boardSize - board size (selects which rings apply)
** @param    level     - level number
** @param    player    - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @param    pReader   - out: opened (or partially-opened) reader
** @return   true if Open() succeeded.
*/
static bool OpenSide(const char* storeDir, int boardSize, int level, int player,
                     RingNestedIndexPullReader* pReader)
{
    char cellsPath[MAX_FULL_PATH_NAME], ring1Path[MAX_FULL_PATH_NAME],
         ring2Path[MAX_FULL_PATH_NAME], ring34Path[MAX_FULL_PATH_NAME];

    RSFNameCellsInUseFile(cellsPath, sizeof(cellsPath), storeDir, boardSize, level, player, 0);
    RSFNameRing34File(ring34Path, sizeof(ring34Path), storeDir, boardSize, level, player, 0);

    bool hasRing1 = RingNestedIndexHasRing1(boardSize);
    bool hasRing2 = RingNestedIndexHasRing2(boardSize);
    if (hasRing1) RSFNameRing1File(ring1Path, sizeof(ring1Path), storeDir, boardSize, level, player, 0);
    if (hasRing2) RSFNameRing2File(ring2Path, sizeof(ring2Path), storeDir, boardSize, level, player, 0);

    return pReader->Open(cellsPath, hasRing1 ? ring1Path : nullptr,
                          hasRing2 ? ring2Path : nullptr, ring34Path);
}

/*
** Function: RecordMismatch
** @brief    Adds one mismatch to the result -- always counted, only
**           sampled up to STORE_COMPARE_MAX_SAMPLE_MISMATCHES.
*/
static void RecordMismatch(StoreCompareResult* pResult, const BOARD_KEY& key, bool onlyInOld)
{
    if (onlyInOld) pResult->onlyInOldCount++;
    else           pResult->onlyInNewCount++;

    if (pResult->sample.size() < STORE_COMPARE_MAX_SAMPLE_MISMATCHES)
        pResult->sample.push_back({ key, onlyInOld });
}

StoreCompareResult StoreCompareLevelPlayer(const char* oldStoreDir, const char* newStoreDir,
                                            int boardSize, int level, int player)
{
    StoreCompareResult result;

    RingNestedIndexPullReader oldReader, newReader;
    bool oldOpen = OpenSide(oldStoreDir, boardSize, level, player, &oldReader);
    bool newOpen = OpenSide(newStoreDir, boardSize, level, player, &newReader);

    if (!oldOpen) result.oldOpenFailed = true;
    if (!newOpen) result.newOpenFailed = true;
    if (!oldOpen || !newOpen)
    {
        oldReader.Close();
        newReader.Close();
        return result;
    }

    /* Classic sorted two-pointer merge-compare: both sides are already in
    ** global sorted order (a guarantee of the merge that wrote them), so
    ** this never needs to buffer, sort, or hash anything -- one record
    ** from each side is all that's ever resident, matching PullReader's
    ** own O(1)-memory guarantee regardless of level size.
    */
    BOARD_KEY oldKey, newKey;
    bool haveOld = oldReader.Peek(&oldKey);
    bool haveNew = newReader.Peek(&newKey);

    while (haveOld && haveNew)
    {
        int cmp = BoardKeyCompare(&oldKey, &newKey);
        if (cmp == 0)
        {
            result.matched++;
            haveOld = oldReader.Advance() ? oldReader.Peek(&oldKey) : false;
            haveNew = newReader.Advance() ? newReader.Peek(&newKey) : false;
        }
        else if (cmp < 0)
        {
            /* old's current key is smaller -- it has a board new hasn't reached yet (or doesn't have) */
            RecordMismatch(&result, oldKey, true);
            haveOld = oldReader.Advance() ? oldReader.Peek(&oldKey) : false;
        }
        else
        {
            RecordMismatch(&result, newKey, false);
            haveNew = newReader.Advance() ? newReader.Peek(&newKey) : false;
        }
    }
    /* One side ran out first -- everything remaining on the other side is
    ** unmatched by definition; drain it the same way to pick up every
    ** remaining mismatch rather than stopping at the first side's EOF.
    */
    while (haveOld)
    {
        RecordMismatch(&result, oldKey, true);
        haveOld = oldReader.Advance() ? oldReader.Peek(&oldKey) : false;
    }
    while (haveNew)
    {
        RecordMismatch(&result, newKey, false);
        haveNew = newReader.Advance() ? newReader.Peek(&newKey) : false;
    }

    result.oldCorrupted = oldReader.IsCorrupted();
    result.newCorrupted = newReader.IsCorrupted();

    oldReader.Close();
    newReader.Close();
    return result;
}
