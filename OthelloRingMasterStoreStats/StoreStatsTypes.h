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
};
