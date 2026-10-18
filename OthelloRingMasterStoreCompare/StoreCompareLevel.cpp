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
#include "RingStoreFile.h"
#include <cstdio>

/* Constants */

/* How often StoreCompareLevelPlayer calls onProgress, as a fraction of the
** level/player's total board count -- every 10% by default, per the user's
** request. Exact percent isn't guaranteed (one loop iteration can cross more
** than one decile on a sparse/skewed mismatch run), only "at least once per
** decile crossed."
*/
constexpr uint64_t PROGRESS_REPORT_DECILE = 10;

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
                                            int boardSize, int level, int player,
                                            const StoreCompareProgressFn& onProgress)
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

    /* Ring_3_4 holds exactly one record per board (the leaf ring, see
    ** RingNestedIndex.h), so its trailer's recordCount -- read once at Open(),
    ** no decompression -- is this level/player's exact total board count on
    ** that side. Take the larger of the two as the progress denominator: when
    ** the sides match it doesn't matter which; when they diverge, the larger
    ** total is the one that actually bounds how long this call can run.
    */
    uint64_t oldTotal = RSFReaderTrailer(oldReader.pRing34)->recordCount;
    uint64_t newTotal = RSFReaderTrailer(newReader.pRing34)->recordCount;
    uint64_t total = oldTotal > newTotal ? oldTotal : newTotal;
    uint64_t processed = 0, lastReportedDecile = 0;
    if (onProgress) onProgress(0, total);   /* lets the caller start its own elapsed-time clock before any real progress exists */

    /*
    ** Function: ReportProgress (local lambda)
    ** @brief    Advances processed by one and calls onProgress at most once
    **           per PROGRESS_REPORT_DECILE crossed. "processed" is an
    **           approximation of true position when the two sides diverge --
    **           it counts loop iterations (each advances at least one side by
    **           one record), not a position both sides agree on -- but it's
    **           monotonic and bounded by total either way, which is all a
    **           percent-complete estimate needs.
    */
    auto reportProgress = [&]()
    {
        processed++;
        if (total == 0 || !onProgress) return;
        uint64_t decile = (processed * PROGRESS_REPORT_DECILE) / total;
        if (decile > lastReportedDecile)
        {
            lastReportedDecile = decile;
            onProgress(processed, total);
        }
    };

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
        reportProgress();
    }
    /* One side ran out first -- everything remaining on the other side is
    ** unmatched by definition; drain it the same way to pick up every
    ** remaining mismatch rather than stopping at the first side's EOF.
    */
    while (haveOld)
    {
        RecordMismatch(&result, oldKey, true);
        haveOld = oldReader.Advance() ? oldReader.Peek(&oldKey) : false;
        reportProgress();
    }
    while (haveNew)
    {
        RecordMismatch(&result, newKey, false);
        haveNew = newReader.Advance() ? newReader.Peek(&newKey) : false;
        reportProgress();
    }

    result.oldCorrupted = oldReader.IsCorrupted();
    result.newCorrupted = newReader.IsCorrupted();

    oldReader.Close();
    newReader.Close();
    return result;
}
