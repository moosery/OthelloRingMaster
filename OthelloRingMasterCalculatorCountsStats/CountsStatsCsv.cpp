/*
** Filename:  CountsStatsCsv.cpp
**
** Purpose:
**   Implements CountsStatsWriteCsvHeader and CountsStatsWriteCsvRow declared
**   in CountsStatsCsv.h.
*/

/* Includes */
#include "CountsStatsCsv.h"
#include "CounterWidthConfig.h"   /* COUNTER_WIDTH_NIBBLE */

/* Functions */

/*
** Function: CountsStatsWriteCsvHeader
** @brief    See CountsStatsCsv.h.
*/
void CountsStatsWriteCsvHeader(FILE* fpOut)
{
    fprintf(fpOut, "Level,TotalBoards,BlackBoards,WhiteBoards,CounterTier,"
                   "CompressedBytes,DecompressedBytes,Ratio,ReductionPercent,BitsPerBoard\n");
}

/*
** Function: CountsStatsWriteCsvRow
** @brief    See CountsStatsCsv.h.
*/
void CountsStatsWriteCsvRow(FILE* fpOut, const LevelCountsStats* stats)
{
    fprintf(fpOut, "%d,", stats->level);

    if (stats->counterByteWidth < 0)
    {
        /* No valid sentinel payload for this level -- every remaining
        ** column is genuinely unavailable, not zero.
        */
        fprintf(fpOut, ",,,,,,,,\n");
        return;
    }

    fprintf(fpOut, "%llu,%llu,%llu,",
            (unsigned long long)stats->totalBoards,
            (unsigned long long)stats->blackBoards,
            (unsigned long long)stats->whiteBoards);

    if (stats->counterByteWidth == COUNTER_WIDTH_NIBBLE)
        fprintf(fpOut, "nibble,");
    else
        fprintf(fpOut, "%d,", stats->counterByteWidth);

    fprintf(fpOut, "%llu,%llu,",
            (unsigned long long)stats->compressedBytes,
            (unsigned long long)stats->decompressedBytes);

    if (stats->compressedBytes > 0 && stats->decompressedBytes > 0)
    {
        double ratio            = (double)stats->decompressedBytes / (double)stats->compressedBytes;
        double reductionPercent = (1.0 - (double)stats->compressedBytes / (double)stats->decompressedBytes) * 100.0;
        fprintf(fpOut, "%.4f,%.2f,", ratio, reductionPercent);
    }
    else
    {
        fprintf(fpOut, ",,");
    }

    if (stats->totalBoards > 0)
        fprintf(fpOut, "%.4f\n", stats->compressedBytes * 8.0 / (double)stats->totalBoards);
    else
        fprintf(fpOut, "\n");
}
