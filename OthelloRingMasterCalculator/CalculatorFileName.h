/*
** Filename:  CalculatorFileName.h
**
** Purpose:
**   Declares the file-naming convention for this calculator's own output:
**   the per-level, per-player counts file (see CalculatorCountsFile.h).
**
** Notes:
**   RSFFileName.h deliberately does not include this -- "the win/tie/loss
**   stats format itself is a separate, not-yet-started future phase" per
**   its own Notes, written back when this calculator was still Phase 0
**   scaffolding. Now that phase has arrived, so this lives here instead,
**   in the same header-only/static-inline style as RSFFileName.h. A
**   single ".counts" extension covers both the nibble and byte-and-wider
**   tiers -- the tier width isn't encoded in the filename since a reader
**   always already knows it from CounterWidthConfig before opening the file.
*/

#pragma once

/* Includes */
#include "RSFFileName.h"

/* Functions */

/*
** Function: CalcNameCountsFile
** @brief    Builds the counts-file path for one level/player.
** @param    out       - buffer to receive the built path
** @param    outSize   - size of out
** @param    dir       - counts directory
** @param    boardSize - board size (e.g. 6 for 6x6)
** @param    level     - level number
** @param    player    - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
*/
static inline void CalcNameCountsFile(char* out, size_t outSize,
                                       const char* dir, int boardSize,
                                       int level, int player)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_%s.counts",
             dir, level, boardSize, boardSize, RSFPlayerStr(player));
}
