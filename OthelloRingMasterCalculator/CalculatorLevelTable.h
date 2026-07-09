/*
** Filename:  CalculatorLevelTable.h
**
** Purpose:
**   Declares the shared per-level table formatting used both by
**   BackwardWalkDriver.cpp (printed to the log/screen as each level
**   completes, plus a full reprint at the end of the walk -- mirroring
**   OthelloRingMaster.cpp's own scrolling table) and
**   CalculatorStatsListener.cpp (the on-demand STATUS response's history
**   table), so both places show the exact same columns in the exact same
**   format.
**
** Notes:
**   Column widths are declared as one table (CalcLevelTableColumns) so
**   the header, separator, and every data row always stay aligned with
**   each other by construction -- no hand-counted dashes to keep in sync.
*/

#pragma once

/* Includes */
#include "CalculatorTypes.h"

/* Functions */

/*
** Function: CalcWidthShortLabel
** @brief    Formats a tier width compactly for a table column: "nibble"
**           or "N B" (bytes) -- shorter than CounterWidthConfig.cpp's own
**           WidthLabel ("N bytes"), which is sized for a sentence, not a
**           fixed-width column.
** @param    byteWidth - COUNTER_WIDTH_NIBBLE or a byte width
** @param    out       - buffer to receive the label
** @param    outSz     - size in bytes of out
*/
void CalcWidthShortLabel(int byteWidth, char* out, size_t outSz);

/*
** Function: CalcLevelTableHeaderLines
** @brief    Builds the two-line column header (labels, then a dashes
**           separator) both aligned to the same column widths every data
**           row uses. Neither line has a trailing newline.
** @param    outHeader    - buffer to receive the label line
** @param    headerSize   - size in bytes of outHeader
** @param    outSeparator - buffer to receive the dashes line
** @param    sepSize      - size in bytes of outSeparator
*/
void CalcLevelTableHeaderLines(char* outHeader, size_t headerSize, char* outSeparator, size_t sepSize);

/*
** Function: CalcLevelTableFormatRow
** @brief    Formats one level's stats as one table row: level, boards
**           processed, black/white/tie totals, terminal board count,
**           counter width, duration, boards/sec, ns/board, and the
**           completed-at timestamp. No trailing newline.
** @param    level   - the level number this row is for
** @param    ls      - that level's stats (must be complete -- totalNanos > 0)
** @param    out     - buffer to receive the formatted row
** @param    outSize - size in bytes of out
*/
void CalcLevelTableFormatRow(int level, const CalculatorLevelStats* ls, char* out, size_t outSize);
