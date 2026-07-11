/*
** Filename:  StoreStatsCsv.h
**
** Purpose:
**   Declares the CSV emission logic -- kept separate from the scan/
**   aggregate logic in StoreStatsScan.cpp so each file has one job.
*/

#pragma once

/* Includes */
#include "StoreStatsTypes.h"
#include <cstdio>

/* Functions */

/*
** Function: StoreStatsWriteCsvHeader
** @brief    Writes the fixed CSV column header row to fpOut.
** @param    fpOut - destination stream (stdout or the --output file)
*/
void StoreStatsWriteCsvHeader(FILE* fpOut);

/*
** Function: StoreStatsWriteCsvRow
** @brief    Computes Ratio/ReductionPercent/BitsPerBoard from stats and
**           writes one CSV row to fpOut. Guards divide-by-zero for a level
**           with zero boards or zero uncompressed/compressed bytes by
**           leaving the affected field blank rather than emitting inf/nan.
**           BoardsGenerated/DupsRemoved are left blank when stats->
**           hasGenerationStats is false (e.g. level 0's sentinel has no
**           stats payload); cumulativeBoardsGenerated is still written in
**           that case (the caller's running sum simply doesn't advance for
**           an unknown level).
** @param    fpOut                      - destination stream (stdout or the --output file)
** @param    stats                      - one level's already-aggregated totals
** @param    cumulativeBoardsGenerated  - running total of boardsGenerated through this level, inclusive
*/
void StoreStatsWriteCsvRow(FILE* fpOut, const LevelStoreStats* stats, uint64_t cumulativeBoardsGenerated);
