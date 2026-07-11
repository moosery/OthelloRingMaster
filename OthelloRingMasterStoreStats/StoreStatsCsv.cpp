/*
** Filename:  StoreStatsCsv.cpp
**
** Purpose:
**   Implements StoreStatsWriteCsvHeader and StoreStatsWriteCsvRow declared
**   in StoreStatsCsv.h.
*/

/* Includes */
#include "StoreStatsCsv.h"

/* Functions */

/*
** Function: StoreStatsWriteCsvHeader
** @brief    See StoreStatsCsv.h.
*/
void StoreStatsWriteCsvHeader(FILE* fpOut)
{
    fprintf(fpOut, "Level,TotalBoards,WhiteBoards,BlackBoards,CompressedBytes,UncompressedBytes,"
                   "Ratio,ReductionPercent,BitsPerBoard,BoardsGenerated,DupsRemoved,CumulativeBoardsGenerated\n");
}

/*
** Function: StoreStatsWriteCsvRow
** @brief    See StoreStatsCsv.h.
*/
void StoreStatsWriteCsvRow(FILE* fpOut, const LevelStoreStats* stats, uint64_t cumulativeBoardsGenerated)
{
    fprintf(fpOut, "%d,%llu,%llu,%llu,%llu,%llu,",
            stats->level,
            (unsigned long long)stats->totalBoards,
            (unsigned long long)stats->whiteBoards,
            (unsigned long long)stats->blackBoards,
            (unsigned long long)stats->compressedBytes,
            (unsigned long long)stats->uncompressedBytes);

    if (stats->compressedBytes > 0 && stats->uncompressedBytes > 0)
    {
        double ratio            = (double)stats->uncompressedBytes / (double)stats->compressedBytes;
        double reductionPercent = (1.0 - (double)stats->compressedBytes / (double)stats->uncompressedBytes) * 100.0;
        fprintf(fpOut, "%.4f,%.2f,", ratio, reductionPercent);
    }
    else
    {
        fprintf(fpOut, ",,");
    }

    if (stats->totalBoards > 0)
        fprintf(fpOut, "%.4f,", stats->compressedBytes * 8.0 / (double)stats->totalBoards);
    else
        fprintf(fpOut, ",");

    if (stats->hasGenerationStats)
        fprintf(fpOut, "%llu,%llu,", (unsigned long long)stats->boardsGenerated, (unsigned long long)stats->dupsRemoved);
    else
        fprintf(fpOut, ",,");

    fprintf(fpOut, "%llu\n", (unsigned long long)cumulativeBoardsGenerated);
}
