/*
** Filename:  StoreStatsTypes.h
**
** Purpose:
**   Declares LevelStoreStats, the one per-level aggregate this tool builds
**   and emits -- both colors' CellsInUse/Ring_1/Ring_2/Ring_3_4 files folded
**   into a single set of totals for that level.
*/

#pragma once

/* Includes */
#include <cstdint>

/* Structures and Types */

/*
** Type:    LevelStoreStats
** @brief   One level's combined-color totals: board counts (split by color,
**          plus their sum), real on-disk compressed bytes, and the
**          uncompressed-equivalent byte count each file's own record shape
**          implies (recordCount * record-width, summed across every
**          applicable ring file and both colors).
*/
struct LevelStoreStats
{
    int      level            = 0;
    uint64_t totalBoards       = 0;   /* whiteBoards + blackBoards                        */
    uint64_t whiteBoards       = 0;   /* white player's own Ring_3_4 recordCount           */
    uint64_t blackBoards       = 0;   /* black player's own Ring_3_4 recordCount           */
    uint64_t compressedBytes   = 0;   /* real on-disk size, all applicable files, both colors */
    uint64_t uncompressedBytes = 0;   /* recordCount * record-width, all applicable files, both colors */

    /*
    ** Raw GPU generation/dedup counts for the solve step that PRODUCED this
    ** level (read from this level's own "_complete" sentinel, which -- per
    ** OthelloRingMaster's own WriteSentinelStats/ScanForResumeLevel
    ** convention -- holds the LevelStats for "level-1 -> level", not this
    ** level's own board population). hasGenerationStats is false for level 0
    ** (its sentinel is zero-byte, no stats payload) and for any level solved
    ** before this stats payload existed.
    **
    ** dupsRemoved is the FULL three-stage total, not just the two stages
    ** LevelStats has an explicit counter for:
    **   1. gpuDupsRemoved      -- GPU intra-flush dedup (has its own counter)
    **   2. boardsReceivedFromGpu - boardsWrittenToDisk
    **                           -- merge-writer pool cross-segment dedup
    **                              (MergePoolToWriter, MergeFiles.cpp) --
    **                              LevelStats has NO explicit counter for
    **                              this stage; derived here from two fields
    **                              that ARE both tracked (boardsReceivedFromGpu
    **                              is tallied before this stage runs,
    **                              boardsWrittenToDisk reflects its result)
    **   3. mrgDupsRemoved      -- final end-of-level k-way merge dedup (has its own counter)
    ** Without stage 2, BoardsGenerated - DupsRemoved does not equal
    ** TotalBoards for any level whose merge-writer pool held more than one
    ** GPU flush's worth of data before spilling to disk (first happens at
    ** real 6x6 scale around level 15-16, per real-run investigation).
    */
    bool     hasGenerationStats = false;
    uint64_t boardsGenerated    = 0;   /* raw boards the GPU generated (pre-dedup) to produce this level */
    uint64_t dupsRemoved        = 0;   /* full 3-stage total removed while producing this level, see above */
};
