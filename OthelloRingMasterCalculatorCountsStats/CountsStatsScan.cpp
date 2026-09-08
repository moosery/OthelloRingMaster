/*
** Filename:  CountsStatsScan.cpp
**
** Purpose:
**   Implements CountsStatsFindDeepestCompleteLevel and CountsStatsScanLevel
**   declared in CountsStatsScan.h.
*/

/* Includes */
#include "CountsStatsScan.h"
#include "CalculatorFileName.h"
#include "CalculatorTypes.h"
#include "RSFFileName.h"
#include "Lz4Stream.h"
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
** Function: accumulateCountsFile
** @brief    Folds one color's .counts file into pStats: real on-disk bytes
**           into compressedBytes, and the real decompressed byte count
**           (read by fully decompressing the LZ4 stream to end-of-stream --
**           never derived from recordCount*width math, since that would
**           just be re-deriving what decompression already tells us for
**           free) into decompressedBytes. A color genuinely absent at this
**           level (boardsProcessed == 0 for it) is skipped entirely, not
**           an error. A color the sentinel says WAS processed but whose
**           .counts file is missing or fails to decompress is fatal
**           (genuine corruption).
** @param    path            - .counts file path
** @param    boardsProcessed - this color's boardsProcessed count from the
**                             sentinel -- 0 means skip, nonzero means the
**                             file must exist and decompress cleanly
** @param    pStats          - stats accumulator being built for this level
*/
static void accumulateCountsFile(const char* path, uint64_t boardsProcessed, LevelCountsStats* pStats)
{
    if (boardsProcessed == 0)
        return;   /* legitimately absent -- this color has nothing at this level */

    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        Fatal(FATAL_FILE_OPEN, "CountsStatsScan: '%s' should exist (sentinel says %llu boards processed) but is missing",
              path, (unsigned long long)boardsProcessed);

    pStats->compressedBytes += fileOnDiskBytes(path);

    Lz4StreamReader* pReader = Lz4StreamReaderOpen(path);
    if (!pReader)
        Fatal(FATAL_FILE_OPEN, "CountsStatsScan: '%s' exists but could not be opened as an LZ4 stream (corrupt or truncated)", path);

    static const size_t kChunkSize = 256 * 1024;
    uint8_t buf[kChunkSize];
    uint64_t decompressed = 0;
    size_t   got;
    while ((got = Lz4StreamReaderRead(pReader, buf, kChunkSize)) > 0)
        decompressed += (uint64_t)got;

    Lz4StreamReaderClose(&pReader);
    pStats->decompressedBytes += decompressed;
}

/* Functions */

/*
** Function: CountsStatsFindDeepestCompleteLevel
** @brief    See CountsStatsScan.h.
*/
int CountsStatsFindDeepestCompleteLevel(const char* countsDir, int boardSize)
{
    int deepest = -1;
    for (int level = 0; level < CALC_MAX_LEVELS; level++)
    {
        char sentPath[MAX_FULL_PATH_NAME];
        CalcSentinelNameComplete(sentPath, sizeof(sentPath), countsDir, boardSize, level);

        if (GetFileAttributesA(sentPath) == INVALID_FILE_ATTRIBUTES)
            break;

        deepest = level;
    }
    return deepest;
}

/*
** Function: CountsStatsScanLevel
** @brief    See CountsStatsScan.h.
*/
bool CountsStatsScanLevel(const char* countsDir, int boardSize, int level, LevelCountsStats* pOut)
{
    *pOut = LevelCountsStats{};
    pOut->level = level;

    char sentPath[MAX_FULL_PATH_NAME];
    CalcSentinelNameComplete(sentPath, sizeof(sentPath), countsDir, boardSize, level);

    CalculatorLevelStats ls = {};
    if (!ReadCalcSentinelStats(sentPath, &ls))
        return false;   /* legacy/manual sentinel with no stats payload -- caller treats this row as unavailable */

    pOut->blackBoards      = ls.boardsProcessedBlack;
    pOut->whiteBoards      = ls.boardsProcessedWhite;
    pOut->totalBoards      = ls.boardsProcessedBlack + ls.boardsProcessedWhite;
    pOut->counterByteWidth = ls.counterByteWidth;

    char blackPath[MAX_FULL_PATH_NAME];
    char whitePath[MAX_FULL_PATH_NAME];
    CalcNameCountsFile(blackPath, sizeof(blackPath), countsDir, boardSize, level, RSF_PLAYER_BLACK);
    CalcNameCountsFile(whitePath, sizeof(whitePath), countsDir, boardSize, level, RSF_PLAYER_WHITE);

    accumulateCountsFile(blackPath, ls.boardsProcessedBlack, pOut);
    accumulateCountsFile(whitePath, ls.boardsProcessedWhite, pOut);

    return true;
}
