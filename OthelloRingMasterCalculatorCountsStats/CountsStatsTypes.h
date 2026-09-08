/*
** Filename:  CountsStatsTypes.h
**
** Purpose:
**   Declares LevelCountsStats, the one per-level aggregate this tool builds
**   and emits -- both colors' .counts files folded into a single set of
**   totals for that level, plus the board-processing numbers already
**   recorded in the calculator's own calc_complete sentinel.
*/

#pragma once

/* Includes */
#include <cstdint>

/* Structures and Types */

/*
** Type:    LevelCountsStats
** @brief   One level's combined-color totals: board counts (split by
**          color, plus their sum), real on-disk compressed bytes, and the
**          real decompressed byte count (obtained by actually decompressing
**          each color's LZ4 stream, not derived/guessed), plus the tier
**          width the calculator itself recorded for this level.
*/
struct LevelCountsStats
{
    int      level               = 0;
    uint64_t totalBoards          = 0;   /* blackBoards + whiteBoards                          */
    uint64_t blackBoards          = 0;   /* boardsProcessedBlack, from the calc_complete sentinel */
    uint64_t whiteBoards          = 0;   /* boardsProcessedWhite, from the calc_complete sentinel */
    uint64_t compressedBytes      = 0;   /* real on-disk size, both colors' .counts files combined */
    uint64_t decompressedBytes    = 0;   /* real decompressed byte count, both colors combined -- from actually
                                          ** decompressing each LZ4 stream, never derived/estimated          */

    /* This level's confirmed tier width, from the sentinel -- 0 means
    ** COUNTER_WIDTH_NIBBLE (see CounterWidthConfig.h), any other value is a
    ** real byte width (1, 2, 4, 8, ...). -1 means the sentinel didn't have
    ** a valid stats payload (legacy/manually-created sentinel) -- treat the
    ** whole row's numbers as unavailable in that case, not zero.
    */
    int      counterByteWidth     = -1;
};
