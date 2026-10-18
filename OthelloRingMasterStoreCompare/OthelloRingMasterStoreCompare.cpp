/*
** Filename:  OthelloRingMasterStoreCompare.cpp
**
** Purpose:
**   Entry point for OthelloRingMasterStoreCompare: a small, read-only
**   command-line tool that confirms two OthelloRingMaster stores hold the
**   EXACT SAME SET of boards, level by level -- not just matching counts
**   (OthelloRingMasterStoreStats already covers that). Built for
**   cross-checking the from-scratch 6x6 rerun on the new AMD machine
**   against the preserved pre-RMA store (Y:\OthelloRingMaster.save), see
**   project_store_set_equality_tool_plan memory for the full background.
**
** Notes:
**   Walks level 0 upward, stopping at the first level not "_complete" on
**   BOTH sides (reported, not an error -- an in-progress rerun is the
**   normal case this tool runs against). Levels are fully independent, so
**   the comparable range is dispatched across a small ThreadPool (default
**   3 concurrent levels) rather than run one at a time -- each level's
**   work is a mix of network I/O (both stores typically live on the same
**   NAS) and CPU decode, so overlapping a few levels keeps both busy
**   without over-saturating one shared NAS link.
*/

/* Includes */
#include "StoreCompareLevel.h"
#include "FileAndDirUtils.h"
#include "RSFFileName.h"
#include "ThreadPool.h"
#include "ClockTick.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
#include <cmath>
#include <memory>
#include <windows.h>

/* Constants */
#define DEFAULT_THREADS 3

/* Structures and Types */

/*
** Type:    StoreCompareConfig
** @brief   Parsed CLI arguments for this run.
*/
struct StoreCompareConfig
{
    int  boardSize                                 = 6;
    char oldStoreDrive                             = 'Y';
    char oldStoreDirNameNoDrive[MAX_FULL_PATH_NAME] = "\\OthelloRingMaster.save\\Store";
    char newStoreDrive                             = 'Y';
    char newStoreDirNameNoDrive[MAX_FULL_PATH_NAME] = "\\OthelloRingMaster\\Store";
    int  startLevel                                = 0;
    int  threads                                   = DEFAULT_THREADS;
};

/* Functions */

/*
** Function: PrintUsage
** @brief    Prints command-line usage help.
*/
static void PrintUsage(const char* prog)
{
    printf("Usage: %s [options]\n\n", prog);
    printf("  --board-size N        Board size: 4, 6, or 8                         [default: 6]\n");
    printf("  --old-store-drive L   Drive letter the OLD store lives on            [default: Y]\n");
    printf("  --old-store-dir PATH  Sub-path on the old store's drive (no drive letter)\n");
    printf("                                                      [default: \\OthelloRingMaster.save\\Store]\n");
    printf("  --new-store-drive L   Drive letter the NEW store lives on            [default: Y]\n");
    printf("  --new-store-dir PATH  Sub-path on the new store's drive (no drive letter)\n");
    printf("                                                      [default: \\OthelloRingMaster\\Store]\n");
    printf("  --start-level N       First level to compare                        [default: 0]\n");
    printf("  --threads N           Levels compared concurrently                  [default: %d]\n", DEFAULT_THREADS);
    printf("  --help                Show this help\n\n");
    printf("Confirms the two stores hold the exact same set of boards, level by level, not\n");
    printf("just matching counts -- a streaming sorted-merge comparison, never loading a\n");
    printf("whole level into memory. Stops (not an error) at the first level that isn't\n");
    printf("\"_complete\" on both sides.\n\n");
}

/*
** Function: ParseArgs
** @brief    Parses command-line arguments into pConfig, applying defaults first.
** @return   true to continue; false if --help was requested or args were invalid.
*/
static bool ParseArgs(int argc, char** argv, StoreCompareConfig* pConfig)
{
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--help")) { PrintUsage(argv[0]); return false; }
        else if (!strcmp(argv[i], "--board-size") && i + 1 < argc) pConfig->boardSize = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--old-store-drive") && i + 1 < argc) pConfig->oldStoreDrive = (char)toupper(argv[++i][0]);
        else if (!strcmp(argv[i], "--old-store-dir") && i + 1 < argc)
            strncpy_s(pConfig->oldStoreDirNameNoDrive, sizeof(pConfig->oldStoreDirNameNoDrive), argv[++i], _TRUNCATE);
        else if (!strcmp(argv[i], "--new-store-drive") && i + 1 < argc) pConfig->newStoreDrive = (char)toupper(argv[++i][0]);
        else if (!strcmp(argv[i], "--new-store-dir") && i + 1 < argc)
            strncpy_s(pConfig->newStoreDirNameNoDrive, sizeof(pConfig->newStoreDirNameNoDrive), argv[++i], _TRUNCATE);
        else if (!strcmp(argv[i], "--start-level") && i + 1 < argc) pConfig->startLevel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) pConfig->threads = atoi(argv[++i]);
        else
        {
            fprintf(stderr, "Unknown or incomplete argument: %s\n\n", argv[i]);
            PrintUsage(argv[0]);
            return false;
        }
    }
    if (pConfig->boardSize != 4 && pConfig->boardSize != 6 && pConfig->boardSize != 8)
    {
        fprintf(stderr, "--board-size must be 4, 6, or 8\n");
        return false;
    }
    if (pConfig->threads < 1) pConfig->threads = 1;
    return true;
}

/*
** Function: MakeProgressCallback
** @brief    Builds a StoreCompareProgressFn that prints "N% complete, ETA ..."
**           under printMutex every time StoreCompareLevelPlayer crosses a new
**           decile (see PROGRESS_REPORT_DECILE in StoreCompareLevel.cpp).
** @details  The very first call always has processed==0 (StoreCompareLevelPlayer
**           guarantees this) -- that's the signal this starts its own elapsed-
**           time clock here rather than needing a separate "start" call. ETA is
**           a straight-line projection (elapsed / percent-done * percent-remaining):
**           a rough estimate, not a promise -- real throughput on a shared NAS
**           link can speed up or slow down as sibling levels' jobs finish.
** @param    level       - level number, for the printed label
** @param    colorLabel  - "black" or "white", for the printed label
** @param    pPrintMutex - serializes output across concurrently-running level jobs
*/
static StoreCompareProgressFn MakeProgressCallback(int level, const char* colorLabel, std::mutex* pPrintMutex)
{
    auto pTick = std::make_shared<ClockTick>();
    return [level, colorLabel, pPrintMutex, pTick](uint64_t processed, uint64_t total)
    {
        if (processed == 0)
        {
            ClockStart(pTick.get());
            return;   /* first call is just the clock-start signal -- nothing to report yet */
        }
        if (total == 0) return;

        double elapsedSec = (double)ClockMillisSinceStart(pTick.get()) / 1000.0;
        double percent = 100.0 * (double)processed / (double)total;
        double etaSec = (percent > 0.0) ? elapsedSec * (100.0 - percent) / percent : 0.0;

        std::lock_guard<std::mutex> lock(*pPrintMutex);
        printf("Level %3d (%s): %3.0f%% complete (%llu / %llu boards), ETA %.0fm %02.0fs\n",
               level, colorLabel, percent, (unsigned long long)processed, (unsigned long long)total,
               etaSec / 60.0, fmod(etaSec, 60.0));
    };
}

/*
** Function: LevelCompleteOnBothSides
** @brief    Checks the "_complete" sentinel for one level on both stores.
** @return   true only if both exist.
*/
static bool LevelCompleteOnBothSides(const char* oldStoreDir, const char* newStoreDir,
                                      int boardSize, int level, bool* pOldComplete, bool* pNewComplete)
{
    char oldSentinel[MAX_FULL_PATH_NAME], newSentinel[MAX_FULL_PATH_NAME];
    SentinelNameComplete(oldSentinel, sizeof(oldSentinel), oldStoreDir, boardSize, level);
    SentinelNameComplete(newSentinel, sizeof(newSentinel), newStoreDir, boardSize, level);

    *pOldComplete = GetFileAttributesA(oldSentinel) != INVALID_FILE_ATTRIBUTES;
    *pNewComplete = GetFileAttributesA(newSentinel) != INVALID_FILE_ATTRIBUTES;
    return *pOldComplete && *pNewComplete;
}

int main(int argc, char** argv)
{
    StoreCompareConfig config;
    if (!ParseArgs(argc, argv, &config)) return 1;

    char oldStoreDir[MAX_FULL_PATH_NAME], newStoreDir[MAX_FULL_PATH_NAME];
    /* The configured dir is the parent area; the real per-level ring-store files live in a
    ** fixed "storeDir" subdirectory within it (storeMergeDir, the other sibling, holds the
    ** end-of-level merge's own cross-drive temp/output, not the finished per-level files) --
    ** same convention OthelloRingMasterStoreStats's own main() uses.
    */
    snprintf(oldStoreDir, sizeof(oldStoreDir), "%c:%s\\storeDir", config.oldStoreDrive, config.oldStoreDirNameNoDrive);
    snprintf(newStoreDir, sizeof(newStoreDir), "%c:%s\\storeDir", config.newStoreDrive, config.newStoreDirNameNoDrive);

    printf("Comparing boards %dx%d, level %d upward:\n  OLD: %s\n  NEW: %s\n\n",
           config.boardSize, config.boardSize, config.startLevel, oldStoreDir, newStoreDir);

    /* Phase 1: cheap sequential scan (file-attribute checks only, no data
    ** read) to find the comparable range -- stop at the first level not
    ** complete on both sides. Doing this up front, rather than letting
    ** thread-pool jobs discover it themselves, keeps the "where did we
    ** stop and why" report a single clean line instead of a race between
    ** whichever worker gets there first.
    */
    std::vector<int> levelsToCompare;
    int level = config.startLevel;
    for (;; level++)
    {
        bool oldComplete, newComplete;
        if (!LevelCompleteOnBothSides(oldStoreDir, newStoreDir, config.boardSize, level, &oldComplete, &newComplete))
        {
            if (!oldComplete && !newComplete)
                printf("Stopping before level %d: not complete on either side.\n", level);
            else if (!oldComplete)
                printf("Stopping before level %d: not complete on the OLD store.\n", level);
            else
                printf("Stopping before level %d: not complete on the NEW store.\n", level);
            break;
        }
        levelsToCompare.push_back(level);
    }

    if (levelsToCompare.empty())
    {
        printf("\nNothing to compare.\n");
        return 0;
    }
    printf("Comparing levels %d..%d (%zu levels), %d concurrent.\n\n",
           levelsToCompare.front(), levelsToCompare.back(), levelsToCompare.size(), config.threads);

    /* Phase 2: dispatch each level's comparison (both colors) to the pool. */
    std::mutex printMutex;
    std::vector<bool> anyMismatch(levelsToCompare.size(), false);
    std::vector<bool> anyCorruption(levelsToCompare.size(), false);

    ThreadPool pool((uint32_t)config.threads, "StoreCompare");
    pool.Start();

    for (size_t idx = 0; idx < levelsToCompare.size(); idx++)
    {
        int lvl = levelsToCompare[idx];
        bool queued = pool.QueueJob([&, lvl, idx](uint32_t /*workerIndex*/)
        {
            StoreCompareResult black = StoreCompareLevelPlayer(oldStoreDir, newStoreDir, config.boardSize, lvl, RSF_PLAYER_BLACK,
                                                                MakeProgressCallback(lvl, "black", &printMutex));
            StoreCompareResult white = StoreCompareLevelPlayer(oldStoreDir, newStoreDir, config.boardSize, lvl, RSF_PLAYER_WHITE,
                                                                MakeProgressCallback(lvl, "white", &printMutex));

            bool clean = black.IsClean() && white.IsClean();
            anyMismatch[idx] = !clean;
            anyCorruption[idx] = black.oldCorrupted || black.newCorrupted || white.oldCorrupted || white.newCorrupted;

            std::lock_guard<std::mutex> lock(printMutex);
            if (clean)
            {
                printf("Level %3d: MATCH       (black %llu boards, white %llu boards)\n",
                       lvl, (unsigned long long)black.matched, (unsigned long long)white.matched);
            }
            else
            {
                printf("Level %3d: MISMATCH    black: matched=%llu onlyOld=%llu onlyNew=%llu%s%s\n",
                       lvl, (unsigned long long)black.matched, (unsigned long long)black.onlyInOldCount,
                       (unsigned long long)black.onlyInNewCount,
                       black.oldOpenFailed ? " OLD-OPEN-FAILED" : "",
                       black.newOpenFailed ? " NEW-OPEN-FAILED" : "");
                printf("               white: matched=%llu onlyOld=%llu onlyNew=%llu%s%s\n",
                       (unsigned long long)white.matched, (unsigned long long)white.onlyInOldCount,
                       (unsigned long long)white.onlyInNewCount,
                       white.oldOpenFailed ? " OLD-OPEN-FAILED" : "",
                       white.newOpenFailed ? " NEW-OPEN-FAILED" : "");
                for (const auto& m : black.sample)
                    printf("             black sample: %016llx %016llx  (only in %s)\n",
                           (unsigned long long)m.key.ullCellsInUse, (unsigned long long)m.key.ullCellColors,
                           m.onlyInOld ? "OLD" : "NEW");
                for (const auto& m : white.sample)
                    printf("             white sample: %016llx %016llx  (only in %s)\n",
                           (unsigned long long)m.key.ullCellsInUse, (unsigned long long)m.key.ullCellColors,
                           m.onlyInOld ? "OLD" : "NEW");
            }
            if (black.oldCorrupted || black.newCorrupted)
                printf("           black: CORRUPTED (old=%d new=%d) -- a file hit EOF where more data was expected\n",
                       black.oldCorrupted, black.newCorrupted);
            if (white.oldCorrupted || white.newCorrupted)
                printf("           white: CORRUPTED (old=%d new=%d) -- a file hit EOF where more data was expected\n",
                       white.oldCorrupted, white.newCorrupted);
        });
        if (!queued)
        {
            fprintf(stderr, "Failed to queue level %d -- pool unexpectedly shutting down\n", lvl);
            break;
        }
    }

    WaitForPoolIdle(&pool);
    pool.Stop();

    size_t cleanCount = 0, mismatchCount = 0, corruptCount = 0;
    for (size_t i = 0; i < levelsToCompare.size(); i++)
    {
        if (anyCorruption[i]) corruptCount++;
        else if (anyMismatch[i]) mismatchCount++;
        else cleanCount++;
    }

    printf("\n=== Summary ===\n");
    printf("Levels compared : %zu\n", levelsToCompare.size());
    printf("Clean matches   : %zu\n", cleanCount);
    printf("Mismatches      : %zu\n", mismatchCount);
    printf("Corrupted reads : %zu\n", corruptCount);

    return (mismatchCount > 0 || corruptCount > 0) ? 1 : 0;
}
