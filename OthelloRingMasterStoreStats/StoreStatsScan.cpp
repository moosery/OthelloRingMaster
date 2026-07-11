/*
** Filename:  StoreStatsScan.cpp
**
** Purpose:
**   Implements StoreStatsFindDeepestCompleteLevel and StoreStatsScanLevel
**   declared in StoreStatsScan.h.
*/

/* Includes */
#include "StoreStatsScan.h"
#include "RSFFileName.h"
#include "RingNestedIndex.h"
#include "RingStoreFile.h"
#include "OthelloTypes.h"
#include "Error.h"
#include "FileAndDirUtils.h"
#include <windows.h>

/* Internal Helpers */

/*
** Function: fileOnDiskBytes
** @brief    Returns path's real on-disk byte size, or 0 if it doesn't exist.
** @param    path - file path to size
** @return   File size in bytes (0 if absent).
*/
static uint64_t fileOnDiskBytes(const char* path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
        return 0;
    return ((uint64_t)fad.nFileSizeHigh << 32) | (uint64_t)fad.nFileSizeLow;
}

/*
** Function: accumulateRingFile
** @brief    Folds one ring-store file's contribution into pStats: on-disk
**           bytes into compressedBytes, recordCount*width into
**           uncompressedBytes, and (for Ring_3_4 only) recordCount into
**           the caller-selected color's board total. A cleanly-absent file
**           (path doesn't exist) contributes nothing and is not an error --
**           only a sentinel-confirmed-present file that fails to open/read
**           a valid trailer is fatal (genuine corruption).
** @param    path        - ring-store file path
** @param    isShaped    - true for Ring_1/Ring_2/Ring_3_4 (RSFOpenShaped), false for CellsInUse (RSFOpen)
** @param    shape       - record shape, meaningful only when isShaped is true
** @param    recordWidth - uncompressed bytes per record for this file's shape
** @param    pBoardTotal - out: recordCount added here if this file is Ring_3_4 (nullptr otherwise)
** @param    pStats      - stats accumulator being built for this level
*/
static void accumulateRingFile(const char* path, bool isShaped, RSFRecordShape shape,
                                int recordWidth, uint64_t* pBoardTotal, LevelStoreStats* pStats)
{
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        return;   /* legitimately absent (e.g. an early level's other-color file) */

    RSFReader* pReader = isShaped ? RSFOpenShaped(path, shape) : RSFOpen(path);
    if (!pReader)
        Fatal(FATAL_FILE_OPEN, "StoreStatsScan: '%s' exists but its trailer could not be read (corrupt or truncated)", path);

    const RSFTrailer* pTrailer = RSFReaderTrailer(pReader);
    uint64_t recordCount = pTrailer->recordCount;
    RSFClose(&pReader);

    pStats->compressedBytes   += fileOnDiskBytes(path);
    pStats->uncompressedBytes += recordCount * (uint64_t)recordWidth;

    if (pBoardTotal)
        *pBoardTotal += recordCount;
}

/*
** Function: levelHasRingFiles
** @brief    True if either color has at least one ring-store file present
**           for level -- mirrors StoreLevelScan.cpp's LevelHasBoardData, so
**           a trailing "_complete" sentinel that only marks "confirmed
**           nothing past here" (zero real files) doesn't count as a real level.
** @param    storeDir  - store directory
** @param    boardSize - exact board size
** @param    level     - level to check
** @return   true if RingNestedIndexFileCount is nonzero for either color.
*/
static bool levelHasRingFiles(const char* storeDir, int boardSize, int level)
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

/*
** Function: readLevelGenerationStats
** @brief    Reads the LevelStats embedded in level's own "_complete"
**           sentinel (magic + raw struct, written by OthelloRingMaster.cpp's
**           WriteSentinelStats). Per that same convention, this sentinel
**           holds the stats for the solve step that PRODUCED this level
**           (i.e. "level-1 -> level"), not this level's own board
**           population -- see ScanForResumeLevel in InitSolver.cpp
**           (`pState->levelStats[level - 1] = restored` for sentinel
**           "level"), mirrored here read-only.
** @param    storeDir           - store directory
** @param    boardSize          - exact board size
** @param    level              - level whose sentinel to read
** @param    pBoardsGenerated   - out: raw GPU-generated boards, valid only if this returns true
** @param    pDupsRemoved       - out: gpuDupsRemoved + mrgDupsRemoved, valid only if this returns true
** @return   false if the sentinel is zero-byte (legacy/manual, e.g. level 0)
**           or otherwise has no valid stats payload -- not an error.
*/
static bool readLevelGenerationStats(const char* storeDir, int boardSize, int level,
                                      uint64_t* pBoardsGenerated, uint64_t* pDupsRemoved)
{
    char sentPath[MAX_FULL_PATH_NAME];
    SentinelNameComplete(sentPath, sizeof(sentPath), storeDir, boardSize, level);

    HANDLE h = CreateFileA(sentPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    uint64_t   magic = 0;
    LevelStats stats  = {};
    DWORD      nr     = 0;
    bool ok = ReadFile(h, &magic, (DWORD)sizeof(magic), &nr, NULL)
              && nr == sizeof(magic)
              && magic == RSF_SENTINEL_STATS_MAGIC
              && ReadFile(h, &stats, (DWORD)sizeof(stats), &nr, NULL)
              && nr == sizeof(stats);
    CloseHandle(h);

    if (!ok)
        return false;

    *pBoardsGenerated = stats.boardsGenerated;
    *pDupsRemoved     = stats.gpuDupsRemoved + stats.mrgDupsRemoved;
    return true;
}

/* Functions */

/*
** Function: StoreStatsFindDeepestCompleteLevel
** @brief    See StoreStatsScan.h.
*/
int StoreStatsFindDeepestCompleteLevel(const char* storeDir, int boardSize)
{
    constexpr int STORESTATS_MAX_LEVELS = 256;

    int deepest = -1;
    for (int level = 0; level < STORESTATS_MAX_LEVELS; level++)
    {
        char sentPath[MAX_FULL_PATH_NAME];
        SentinelNameComplete(sentPath, sizeof(sentPath), storeDir, boardSize, level);

        if (GetFileAttributesA(sentPath) == INVALID_FILE_ATTRIBUTES)
            break;

        deepest = level;
    }

    while (deepest >= 0 && !levelHasRingFiles(storeDir, boardSize, deepest))
        deepest--;

    return deepest;
}

/*
** Function: StoreStatsScanLevel
** @brief    See StoreStatsScan.h.
*/
void StoreStatsScanLevel(const char* storeDir, int boardSize, int level, LevelStoreStats* pOut)
{
    *pOut = LevelStoreStats{};
    pOut->level = level;

    bool hasRing1 = RingNestedIndexHasRing1(boardSize);
    bool hasRing2 = RingNestedIndexHasRing2(boardSize);

    for (int player = RSF_PLAYER_WHITE; player <= RSF_PLAYER_BLACK; player++)
    {
        uint64_t* pBoardTotal = (player == RSF_PLAYER_WHITE) ? &pOut->whiteBoards : &pOut->blackBoards;

        char cellsInUsePath[MAX_FULL_PATH_NAME];
        RSFNameCellsInUseFile(cellsInUsePath, sizeof(cellsInUsePath), storeDir, boardSize, level, player, 0);
        accumulateRingFile(cellsInUsePath, false, RSF_SHAPE_PAIR64, 16, nullptr, pOut);

        if (hasRing1)
        {
            char ring1Path[MAX_FULL_PATH_NAME];
            RSFNameRing1File(ring1Path, sizeof(ring1Path), storeDir, boardSize, level, player, 0);
            accumulateRingFile(ring1Path, true, RSF_SHAPE_RING_LEVEL, 12, nullptr, pOut);
        }

        if (hasRing2)
        {
            char ring2Path[MAX_FULL_PATH_NAME];
            RSFNameRing2File(ring2Path, sizeof(ring2Path), storeDir, boardSize, level, player, 0);
            accumulateRingFile(ring2Path, true, RSF_SHAPE_RING_LEVEL, 12, nullptr, pOut);
        }

        char ring34Path[MAX_FULL_PATH_NAME];
        RSFNameRing34File(ring34Path, sizeof(ring34Path), storeDir, boardSize, level, player, 0);
        accumulateRingFile(ring34Path, true, RSF_SHAPE_LEAF16, 2, pBoardTotal, pOut);
    }

    pOut->totalBoards = pOut->whiteBoards + pOut->blackBoards;

    pOut->hasGenerationStats = readLevelGenerationStats(storeDir, boardSize, level,
                                                          &pOut->boardsGenerated, &pOut->dupsRemoved);
}
