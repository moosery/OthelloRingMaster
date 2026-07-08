/*
** Filename:  CalculatorTypes.h
**
** Purpose:
**   Declares the core config/state/stats structures for the retrograde
**   win/tie/loss calculator: OthelloRingMasterCalculatorConfig (fixed run
**   configuration), OthelloRingMasterCalculatorState (live/mutable
**   calculator state -- current level/color progress, per-level stats
**   history), WinTieLossTriple (the one recurring (black, white, tie)
**   shape everything here is built from), and CalculatorLevelStats (the
**   per-level bookkeeping each history-table row holds).
**
** Notes:
**   Deliberately much smaller than OthelloTypes.h's
**   OthelloRingMasterConfig/State -- the calculator has none of the
**   forward solver's multi-drive/multi-writer machinery (no NVMe writer
**   pool, no cascading merge, no drive ledger for a dozen drives); it only
**   ever reads one already-finished level at a time from RingMaster's
**   existing store and writes one counts file back out. Reuses everything
**   Utility already provides (MAX_FULL_PATH_NAME, ClockTick, ThreadPool)
**   rather than redefining any of it.
*/

#pragma once

/* Includes */
#include "Utility.h"

/* Macros and Defines */
#define CALCULATOR_VERSION "0.18.0"   /* tracks the shared solution-wide version in OthelloTypes.h, not an independent counter */

#define CALC_MAX_LEVELS 256   /* covers up to 16x16 board (252 levels) -- same bound OthelloTypes.h uses, kept local rather than shared across projects */

/* Structures and Types */

/*
** Type:    WinTieLossTriple
** @brief   The one recurring shape everything in this calculator is built
**          from: a census of outcomes (however many boards/continuations
**          this triple is summarizing) split by final result.
*/
typedef struct __WinTieLossTriple
{
    uint64_t blackWins;
    uint64_t whiteWins;
    uint64_t ties;
} WinTieLossTriple, * PWinTieLossTriple;

/*
** Type:    CalculatorLevelStats
** @brief   Everything tracked about one level's retrograde pass: boards
**          processed per color, each color's own outcome aggregate, the
**          combined total, and timing. One history-table row per
**          already-completed level.
*/
typedef struct __CalculatorLevelStats
{
    uint64_t  boardsProcessedBlack;
    uint64_t  boardsProcessedWhite;

    /*
    ** blackToMoveTotals/whiteToMoveTotals: the aggregate outcome census
    ** across all black-to-move (respectively white-to-move) boards
    ** processed at this level. combinedTotals is their elementwise sum --
    ** "total end games reached so far" through this level (see
    ** project_adaptive_counter_width_design memory for why all three are
    ** kept, not just the combined figure).
    */
    WinTieLossTriple  blackToMoveTotals;
    WinTieLossTriple  whiteToMoveTotals;
    WinTieLossTriple  combinedTotals;

    ClockTick  startTick;
    int64_t    totalNanos;
    char       completedAt[24];   /* "YYYY-MM-DD HH:MM:SS" stamped when the level finishes */
} CalculatorLevelStats, * PCalculatorLevelStats;

/*
** Type:    OthelloRingMasterCalculatorConfig
** @brief   Fixed run configuration, set once from command-line args at
**          startup and never mutated afterward.
*/
typedef struct __OthelloRingMasterCalculatorConfig
{
    uint8_t   boardSize;
    char      storeDrive;                            /* drive holding RingMaster's finished store to read from */
    char      storeDirNameNoDrive[MAX_FULL_PATH_NAME]; /* sub-path on storeDrive, matching RingMaster's own addressing */
    char      cacheDirName[MAX_FULL_PATH_NAME];        /* logs + the single per-level width-config file live here */
    uint16_t  statsPort;                              /* default 17632 -- distinct from RingMaster's 17532 */
} OthelloRingMasterCalculatorConfig, * POthelloRingMasterCalculatorConfig;

/*
** Type:    OthelloRingMasterCalculatorState
** @brief   All live, mutable calculator state: current level/color being
**          processed, per-level stats history, and the stats thread pool.
*/
typedef struct __OthelloRingMasterCalculatorState
{
    uint8_t              currentLevel;
    uint8_t              currentPlayer;              /* RSF_PLAYER_BLACK or RSF_PLAYER_WHITE -- which color is in progress right now */
    volatile bool         terminateThreads;
    volatile bool         terminateStatsListener;
    const char* volatile  currentPhase;               /* points to a string literal; set by main thread at each phase transition */

    char  storeDirectory[MAX_FULL_PATH_NAME];   /* resolved from storeDrive + storeDirNameNoDrive -- RingMaster's finished output */
    char  cacheDirectory[MAX_FULL_PATH_NAME];
    char  logFileName[MAX_FULL_PATH_NAME];

    /* Per-level stats history -- one entry per level, indexed by level number. */
    CalculatorLevelStats  levelStats[CALC_MAX_LEVELS];

    ThreadPool* pStatsThreadPool;
} OthelloRingMasterCalculatorState, * POthelloRingMasterCalculatorState;
