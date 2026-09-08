/*
** Filename:  CountsStatsCsv.h
**
** Purpose:
**   Declares CountsStatsWriteCsvHeader and CountsStatsWriteCsvRow -- prints
**   one CSV row per level to an already-open FILE*.
*/

#pragma once

/* Includes */
#include "CountsStatsTypes.h"
#include <cstdio>

/* Functions */

/*
** Function: CountsStatsWriteCsvHeader
** @brief    Writes the CSV column header line.
** @param    fpOut - destination stream (stdout or an --output file)
*/
void CountsStatsWriteCsvHeader(FILE* fpOut);

/*
** Function: CountsStatsWriteCsvRow
** @brief    Writes one level's stats as a CSV row. If stats came from a
**           level with no valid sentinel payload (counterByteWidth == -1),
**           every numeric field prints blank rather than a misleading 0,
**           same convention StoreStatsCsv.cpp uses for its own optional
**           columns.
** @param    fpOut  - destination stream
** @param    stats  - the level's aggregated stats
*/
void CountsStatsWriteCsvRow(FILE* fpOut, const LevelCountsStats* stats);
