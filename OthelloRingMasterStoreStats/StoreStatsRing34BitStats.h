/*
** Filename:  StoreStatsRing34BitStats.h
**
** Purpose:
**   Declares a diagnostic mode that streams a single level's real Ring_3_4
**   records (decompressing for real -- this needs actual delta+varint+LZ4
**   decode, not just a trailer read) and tallies per-bit-position occupancy
**   plus a popcount histogram, to answer whether Ring_3_4's 16-bit field
**   still has real structural sparsity (unoccupied board cells contributing
**   fixed-zero bits) at a given depth, or whether it's already close to
**   fully occupied by then.
*/

#pragma once

/* Includes */
#include <cstdint>
#include <cstdio>

/* Structures and Types */

/*
** Type:    Ring34BitStats
** @brief   Accumulated tally over however many Ring_3_4 records were
**          actually read (both colors combined).
*/
struct Ring34BitStats
{
    uint64_t totalRecords          = 0;
    uint64_t bitSetCount[16]       = {};   /* how many records had bit i set, i=0..15 */
    uint64_t popcountHistogram[17] = {};   /* count of records with popcount == k, k=0..16 */
};

/* Functions */

/*
** Function: StoreStatsCollectRing34BitStats
** @brief    Streams level's Ring_3_4 files (both colors) via RSFOpenShaped/
**           RSFReadShaped (real decompression, O(1) memory -- never loads a
**           level wholesale) and accumulates per-bit and popcount tallies.
**           Stops early once sampleLimit records have been read (0 = read
**           every record in both files, unbounded). Prints a progress line
**           to stderr every time another 5% of the run's total target is
**           crossed (a cheap trailer-only pre-pass sizes that denominator
**           before the real decompressing pass starts), so a large or
**           unbounded read doesn't sit silent for a long time.
** @param    storeDir     - store directory to read from
** @param    boardSize    - exact board size
** @param    level        - level whose Ring_3_4 files to read
** @param    sampleLimit  - stop after this many total records (0 = unlimited)
** @param    pOut         - out: accumulated tally
*/
void StoreStatsCollectRing34BitStats(const char* storeDir, int boardSize, int level,
                                      uint64_t sampleLimit, Ring34BitStats* pOut);

/*
** Function: StoreStatsPrintRing34BitStats
** @brief    Prints a human-readable report of stats to fpOut: per-bit-position
**           occupancy percentage, the popcount histogram, and the average
**           popcount (i.e. average number of the 16 board cells this field
**           covers that are actually occupied at this level).
** @param    fpOut - destination stream (stdout or the --output file)
** @param    level - level the stats were collected for (for the report header)
** @param    stats - the accumulated tally to report
*/
void StoreStatsPrintRing34BitStats(FILE* fpOut, int level, const Ring34BitStats* stats);
