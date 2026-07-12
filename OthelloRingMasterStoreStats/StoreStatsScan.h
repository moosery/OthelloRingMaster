/*
** Filename:  StoreStatsScan.h
**
** Purpose:
**   Declares the scan/aggregate logic this tool is built around: given a
**   finished (or in-progress) OthelloRingMaster store directory, find how
**   deep it goes and aggregate one level's on-disk ring-store files (both
**   colors) into a single LevelStoreStats.
*/

#pragma once

/* Includes */
#include "StoreStatsTypes.h"

/* Functions */

/*
** Function: StoreStatsFindDeepestCompleteLevel
** @brief    Walks storeDir's "_complete" sentinels from level 0 upward,
**           stopping at the first missing one, then steps back past any
**           trailing sentinel that marks "confirmed nothing past here"
**           rather than a level with real ring files -- same two-part
**           pattern as OthelloRingMasterCalculator/StoreLevelScan.cpp's
**           FindDeepestCompleteLevel, reimplemented locally here to avoid
**           pulling in that project's CUDA dependency for one small walk.
** @param    storeDir  - store directory to scan
** @param    boardSize - exact board size (4, 6, or 8)
** @return   Highest level with real ring-store files, or -1 if none.
*/
int StoreStatsFindDeepestCompleteLevel(const char* storeDir, int boardSize);

/*
** Function: StoreStatsLevelIsComplete
** @brief    True if level's "_complete" sentinel exists -- the same check
**           StoreStatsFindDeepestCompleteLevel's walk relies on, exposed
**           standalone so a caller asking about one specific level (e.g.
**           --ring34-bitstats --level N) can refuse to touch a level the
**           solver hasn't finished writing yet, rather than opening a
**           ring-store file mid-write and misreporting it as corrupt.
** @param    storeDir  - store directory to check
** @param    boardSize - exact board size (4, 6, or 8)
** @param    level     - level to check
** @return   true if the "_complete" sentinel exists for level.
*/
bool StoreStatsLevelIsComplete(const char* storeDir, int boardSize, int level);

/*
** Function: StoreStatsScanLevel
** @brief    Aggregates one level's combined-color stats from its ring-store
**           files (CellsInUse always, Ring_1/Ring_2 only when
**           RingNestedIndexHasRing1/HasRing2(boardSize), Ring_3_4 always;
**           up to 8 files total across both colors). A color's files being
**           entirely absent (e.g. level 0 has no white files in real 6x6
**           data) contributes zero to that color/level -- not an error.
**           Fatals if a file the store's sentinel already confirms exists
**           fails to open or yield a valid trailer (genuine corruption).
** @param    storeDir  - store directory to scan
** @param    boardSize - exact board size (4, 6, or 8)
** @param    level     - level to aggregate
** @param    pOut      - out: filled LevelStoreStats (pOut->level set to level)
*/
void StoreStatsScanLevel(const char* storeDir, int boardSize, int level, LevelStoreStats* pOut);
