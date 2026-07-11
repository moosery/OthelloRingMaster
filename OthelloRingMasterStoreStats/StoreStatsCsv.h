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
** @param    fpOut - destination stream (stdout or the --output file)
** @param    stats - one level's already-aggregated totals
*/
void StoreStatsWriteCsvRow(FILE* fpOut, const LevelStoreStats* stats);
