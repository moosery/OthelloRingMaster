/*
** Filename:  CountsStatsScan.h
**
** Purpose:
**   Declares the scan/aggregate logic this tool is built around: given a
**   finished (or in-progress) OthelloRingMasterCalculator counts directory,
**   find how deep it goes and aggregate one level's on-disk .counts files
**   (both colors) into a single LevelCountsStats.
*/

#pragma once

/* Includes */
#include "CountsStatsTypes.h"

/* Functions */

/*
** Function: CountsStatsFindDeepestCompleteLevel
** @brief    Walks countsDir's "calc_complete" sentinels from level 0
**           upward, stopping at the first missing one.
** @param    countsDir - counts directory to scan
** @param    boardSize - exact board size (4, 6, or 8)
** @return   Highest level with a calc_complete sentinel, or -1 if none.
*/
int CountsStatsFindDeepestCompleteLevel(const char* countsDir, int boardSize);

/*
** Function: CountsStatsScanLevel
** @brief    Aggregates one level's combined-color stats: real on-disk
**           .counts file sizes (compressedBytes), their real decompressed
**           byte counts (decompressedBytes -- obtained by actually
**           decompressing each color's LZ4 stream to end of stream, never
**           derived from record-width math), and the board counts/tier
**           width already recorded in the level's own calc_complete
**           sentinel. Fatals if a .counts file the sentinel already implies
**           should exist fails to open or decompress cleanly (genuine
**           corruption) -- a color simply absent at this level (its
**           boardsProcessedX is 0) is not an error.
** @param    countsDir - counts directory to scan
** @param    boardSize - exact board size (4, 6, or 8)
** @param    level     - level to aggregate
** @param    pOut      - out: filled LevelCountsStats (pOut->level set to level)
** @return   true if level's sentinel had a valid stats payload (pOut is
**           fully populated); false if not (pOut->counterByteWidth is -1,
**           every other field left at 0 -- caller should treat this row's
**           numbers as unavailable, not zero).
*/
bool CountsStatsScanLevel(const char* countsDir, int boardSize, int level, LevelCountsStats* pOut);
