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
#include "CalculatorTypes.h"   /* CalculatorLevelStats */
#include <windows.h>

/* Constants */
#define CALC_SENTINEL_STATS_MAGIC 0x43414C4353544154ULL   /* "CALCSTAT" in ASCII byte order */

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

/*
** Function: CalcSentinelNameComplete
** @brief    Builds the "this level's counts are fully written" sentinel
**           path in the counts directory -- the calculator's own
**           equivalent of RSFFileName.h's SentinelNameComplete, but for
**           this project's own output rather than RingMaster's store.
**           A level with no sentinel is (re)processed from scratch on the
**           next run, exactly like a crashed/interrupted level's output
**           files getting naturally overwritten by the next attempt --
**           no separate "in progress" sentinel is needed here since
**           there's no multi-writer merge step to interrupt mid-way.
** @param    out       - buffer to receive the built path
** @param    outSize   - size of out
** @param    dir       - counts directory
** @param    boardSize - board size (e.g. 6 for 6x6)
** @param    level     - level number
*/
static inline void CalcSentinelNameComplete(char* out, size_t outSize,
                                             const char* dir, int boardSize, int level)
{
    snprintf(out, outSize, "%s\\Level_%04d_%dx%d_calc_complete", dir, level, boardSize, boardSize);
}

/*
** Function: WriteCalcSentinelStats
** @brief    Writes a calc_complete sentinel with a magic header followed
**           by the full CalculatorLevelStats payload, so a future run can
**           restore this level's stats (for the FINAL RESULT line and any
**           completed-level history display) without re-processing it --
**           mirrors RingMaster's own WriteSentinelStats (OthelloRingMaster.cpp)
**           exactly, using this project's own magic value.
** @param    path - sentinel file path
** @param    ls   - the level's stats to persist
*/
static inline void WriteCalcSentinelStats(const char* path, const CalculatorLevelStats* ls)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    uint64_t magic = CALC_SENTINEL_STATS_MAGIC;
    DWORD nw;
    WriteFile(h, &magic, (DWORD)sizeof(magic), &nw, NULL);
    WriteFile(h, ls,     (DWORD)sizeof(*ls),    &nw, NULL);
    CloseHandle(h);
}

/*
** Function: ReadCalcSentinelStats
** @brief    Reads CalculatorLevelStats from a calc_complete sentinel file.
** @param    path - sentinel file path
** @param    out  - out: filled with the sentinel's stats payload
** @return   false if the file is zero-byte (legacy/manually created) or
**           doesn't contain a valid stats payload.
*/
static inline bool ReadCalcSentinelStats(const char* path, CalculatorLevelStats* out)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    uint64_t magic = 0;
    DWORD    nr    = 0;
    bool ok = ReadFile(h, &magic, (DWORD)sizeof(magic), &nr, NULL)
              && nr == sizeof(magic)
              && magic == CALC_SENTINEL_STATS_MAGIC
              && ReadFile(h, out, (DWORD)sizeof(*out), &nr, NULL)
              && nr == sizeof(*out);
    CloseHandle(h);
    return ok;
}
