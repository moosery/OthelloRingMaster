/*
** Filename:  CalculatorLevelTable.cpp
**
** Purpose:
**   Implements the shared per-level table formatting declared in
**   CalculatorLevelTable.h.
*/

/* Includes */
#include "CalculatorLevelTable.h"
#include "CounterWidthConfig.h"   /* COUNTER_WIDTH_NIBBLE */
#include <stdio.h>
#include <string.h>

/* Structures and Types */

/*
** Type:    ColumnSpec
** @brief   One table column's header label and fixed field width -- the
**          single source of truth CalcLevelTableHeaderLines/
**          CalcLevelTableFormatRow both build from, so header, separator,
**          and every data row can never drift out of alignment with each
**          other.
*/
struct ColumnSpec
{
    const char* label;
    int         width;
};

/* Constants */

static const ColumnSpec kColumns[] = {
    { "Lv",          4 },
    { "Boards",     12 },
    { "BlkWins",    11 },
    { "WhtWins",    11 },
    { "Ties",       11 },
    { "Terminal",   10 },
    { "Width",       7 },
    { "Dur(s)",      9 },
    { "Brd/s",      10 },
    { "ns/brd",     10 },
    { "CompletedAt", 19 },
};
static const int kNumColumns = sizeof(kColumns) / sizeof(kColumns[0]);

/* Functions */

/*
** Function: CalcFormatDurationHMS
** @brief    See CalculatorLevelTable.h.
*/
void CalcFormatDurationHMS(int64_t nanos, char* out, size_t outSz)
{
    int64_t s = nanos / 1000000000LL;
    snprintf(out, outSz, "%lld:%02lld:%02lld", s / 3600, (s % 3600) / 60, s % 60);
}

/*
** Function: CalcWidthShortLabel
** @brief    See CalculatorLevelTable.h.
*/
void CalcWidthShortLabel(int byteWidth, char* out, size_t outSz)
{
    if (byteWidth == COUNTER_WIDTH_NIBBLE)
        snprintf(out, outSz, "nibble");
    else
        snprintf(out, outSz, "%d B", byteWidth);
}

/*
** Function: CalcLevelTableHeaderLines
** @brief    See CalculatorLevelTable.h.
*/
void CalcLevelTableHeaderLines(char* outHeader, size_t headerSize, char* outSeparator, size_t sepSize)
{
    size_t hn = 0, sn = 0;
    for (int i = 0; i < kNumColumns; i++)
    {
        const ColumnSpec& c = kColumns[i];

        int written = snprintf(outHeader + hn, (hn < headerSize) ? headerSize - hn : 0,
                               "%*s%s", c.width, c.label, (i + 1 < kNumColumns) ? "  " : "");
        hn += (written > 0) ? (size_t)written : 0;

        for (int d = 0; d < c.width && sn + 1 < sepSize; d++)
            outSeparator[sn++] = '-';
        if (i + 1 < kNumColumns && sn + 2 < sepSize)
        {
            outSeparator[sn++] = ' ';
            outSeparator[sn++] = ' ';
        }
    }
    if (sn < sepSize) outSeparator[sn] = '\0';
}

/*
** Function: CalcLevelTableFormatRow
** @brief    See CalculatorLevelTable.h.
*/
void CalcLevelTableFormatRow(int level, const CalculatorLevelStats* ls, char* out, size_t outSize)
{
    uint64_t boards      = ls->boardsProcessedBlack + ls->boardsProcessedWhite;
    double   durSeconds  = (double)ls->totalNanos / 1e9;
    double   boardsPerSec = (durSeconds > 0.0) ? (double)boards / durSeconds : 0.0;
    uint64_t nsPerBoard  = (boards > 0) ? (uint64_t)(ls->totalNanos / (int64_t)boards) : 0;

    char widthLabel[16];
    CalcWidthShortLabel(ls->counterByteWidth, widthLabel, sizeof(widthLabel));

    snprintf(out, outSize,
             "%*d  %*llu  %*llu  %*llu  %*llu  %*llu  %*s  %*.3f  %*.1f  %*llu  %-*s",
             kColumns[0].width,  level,
             kColumns[1].width,  (unsigned long long)boards,
             kColumns[2].width,  (unsigned long long)ls->combinedTotals.blackWins,
             kColumns[3].width,  (unsigned long long)ls->combinedTotals.whiteWins,
             kColumns[4].width,  (unsigned long long)ls->combinedTotals.ties,
             kColumns[5].width,  (unsigned long long)ls->terminalBoards,
             kColumns[6].width,  widthLabel,
             kColumns[7].width,  durSeconds,
             kColumns[8].width,  boardsPerSec,
             kColumns[9].width,  (unsigned long long)nsPerBoard,
             kColumns[10].width, ls->completedAt);
}
