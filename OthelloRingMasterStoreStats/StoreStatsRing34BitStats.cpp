/*
** Filename:  StoreStatsRing34BitStats.cpp
**
** Purpose:
**   Implements StoreStatsCollectRing34BitStats and StoreStatsPrintRing34BitStats
**   declared in StoreStatsRing34BitStats.h.
*/

/* Includes */
#include "StoreStatsRing34BitStats.h"
#include "RSFFileName.h"
#include "RingNestedIndex.h"
#include "RingStoreFile.h"
#include <windows.h>

/* Constants */
#define RING34_READ_BATCH 65536   /* records per RSFReadShaped call */

/* Internal Helpers */

/*
** Function: ring34FileRecordCount
** @brief    Returns path's Ring_3_4 record count straight from its trailer
**           (no decompression), or 0 if the file is absent -- used only to
**           size the progress-percentage denominator before the real
**           (decompressing) read pass starts.
** @param    path - Ring_3_4 file path for one color
** @return   The file's recordCount, or 0 if it doesn't exist.
*/
static uint64_t ring34FileRecordCount(const char* path)
{
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        return 0;

    RSFReader* pReader = RSFOpenShaped(path, RSF_SHAPE_LEAF16);
    if (!pReader)
        Fatal(FATAL_FILE_OPEN, "StoreStatsRing34BitStats: '%s' exists but its trailer could not be read (corrupt or truncated)", path);

    uint64_t count = RSFReaderTrailer(pReader)->recordCount;
    RSFClose(&pReader);
    return count;
}

/*
** Function: accumulateRing34Records
** @brief    Opens one color's Ring_3_4 file (if present) and streams records
**           into pStats until either the file is exhausted or targetTotal
**           (the run's overall stop point, both colors combined) is reached.
**           Prints a progress line to stderr to stderr every time another 5%
**           of targetTotal is crossed, tracked via *pLastPercentBucket
**           (shared across both colors' calls) so a huge unlimited read
**           doesn't sit silent for a long time. A cleanly-absent file is not
**           an error (mirrors StoreStatsScan.cpp's own convention).
** @param    path              - Ring_3_4 file path for one color
** @param    sampleLimit       - stop once pStats->totalRecords reaches this (0 = unlimited)
** @param    targetTotal       - denominator for percentage progress (both colors combined)
** @param    pLastPercentBucket - in/out: highest 5%-multiple already printed (start at -1)
** @param    pStats            - accumulator being built
*/
static void accumulateRing34Records(const char* path, uint64_t sampleLimit, uint64_t targetTotal,
                                     int* pLastPercentBucket, Ring34BitStats* pStats)
{
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        return;   /* legitimately absent (e.g. an early level's other-color file) */

    RSFReader* pReader = RSFOpenShaped(path, RSF_SHAPE_LEAF16);
    if (!pReader)
        Fatal(FATAL_FILE_OPEN, "StoreStatsRing34BitStats: '%s' exists but could not be opened (corrupt or truncated)", path);

    Ring34Rec batch[RING34_READ_BATCH];
    int n;
    while ((n = RSFReadShaped(pReader, batch, RING34_READ_BATCH)) > 0)
    {
        for (int i = 0; i < n; i++)
        {
            uint16_t pattern = batch[i].pattern;
            int      popcount = 0;
            for (int bit = 0; bit < 16; bit++)
            {
                if (pattern & (1u << bit))
                {
                    pStats->bitSetCount[bit]++;
                    popcount++;
                }
            }
            pStats->popcountHistogram[popcount]++;
            pStats->totalRecords++;

            if (targetTotal > 0)
            {
                int bucket = (int)(pStats->totalRecords * 100 / targetTotal / 5);
                if (bucket > *pLastPercentBucket)
                {
                    *pLastPercentBucket = bucket;
                    fprintf(stderr, "  %d%% (%llu / %llu records)\n",
                            bucket * 5, (unsigned long long)pStats->totalRecords, (unsigned long long)targetTotal);
                }
            }

            if (sampleLimit > 0 && pStats->totalRecords >= sampleLimit)
            {
                RSFClose(&pReader);
                return;
            }
        }
    }

    RSFClose(&pReader);
}

/* Functions */

/*
** Function: StoreStatsCollectRing34BitStats
** @brief    See StoreStatsRing34BitStats.h.
*/
void StoreStatsCollectRing34BitStats(const char* storeDir, int boardSize, int level,
                                      uint64_t sampleLimit, Ring34BitStats* pOut)
{
    *pOut = Ring34BitStats{};

    char whitePath[MAX_FULL_PATH_NAME], blackPath[MAX_FULL_PATH_NAME];
    RSFNameRing34File(whitePath, sizeof(whitePath), storeDir, boardSize, level, RSF_PLAYER_WHITE, 0);
    RSFNameRing34File(blackPath, sizeof(blackPath), storeDir, boardSize, level, RSF_PLAYER_BLACK, 0);

    /* Cheap trailer-only pre-pass, just to size the progress-percentage
    ** denominator before the real (decompressing) pass below.
    */
    uint64_t available   = ring34FileRecordCount(whitePath) + ring34FileRecordCount(blackPath);
    uint64_t targetTotal = (sampleLimit > 0 && sampleLimit < available) ? sampleLimit : available;

    int lastPercentBucket = -1;
    const char* paths[2] = { whitePath, blackPath };
    for (int i = 0; i < 2; i++)
    {
        if (sampleLimit > 0 && pOut->totalRecords >= sampleLimit)
            break;
        accumulateRing34Records(paths[i], sampleLimit, targetTotal, &lastPercentBucket, pOut);
    }
}

/*
** Function: StoreStatsPrintRing34BitStats
** @brief    See StoreStatsRing34BitStats.h.
*/
void StoreStatsPrintRing34BitStats(FILE* fpOut, int level, const Ring34BitStats* stats)
{
    fprintf(fpOut, "Ring_3_4 bit-occupancy report -- level %d, %llu records sampled\n",
            level, (unsigned long long)stats->totalRecords);
    fprintf(fpOut, "(records read from the start of each color's file -- see tool notes on sampling bias)\n\n");

    if (stats->totalRecords == 0)
    {
        fprintf(fpOut, "No records read (level absent or empty).\n");
        return;
    }

    fprintf(fpOut, "BitPosition,SetCount,SetPercent\n");
    for (int bit = 0; bit < 16; bit++)
    {
        double pct = (double)stats->bitSetCount[bit] / (double)stats->totalRecords * 100.0;
        fprintf(fpOut, "%d,%llu,%.4f\n", bit, (unsigned long long)stats->bitSetCount[bit], pct);
    }

    fprintf(fpOut, "\nPopcount,Count,Percent\n");
    uint64_t popcountSum = 0;
    for (int k = 0; k <= 16; k++)
    {
        double pct = (double)stats->popcountHistogram[k] / (double)stats->totalRecords * 100.0;
        fprintf(fpOut, "%d,%llu,%.4f\n", k, (unsigned long long)stats->popcountHistogram[k], pct);
        popcountSum += (uint64_t)k * stats->popcountHistogram[k];
    }

    double avgPopcount = (double)popcountSum / (double)stats->totalRecords;
    fprintf(fpOut, "\nAveragePopcount,%.4f\n", avgPopcount);
    fprintf(fpOut, "AverageBitsIdle,%.4f\n", 16.0 - avgPopcount);
}
