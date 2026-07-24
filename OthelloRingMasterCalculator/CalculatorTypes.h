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
**   Smaller than OthelloTypes.h's OthelloRingMasterConfig/State -- no
**   cascading merge, no MW segment-pool bookkeeping -- but it DOES carry
**   its own drive ledger now (see CalcDriveLedger.h): while processing
**   level N, level N+1's board/counts data is staged as segmented,
**   drive-spread scratch (fastest drives first, falling back to medium
**   then slow) so a lookup never needs the whole level resident in
**   memory, and level N's own output is written the same way before
**   being joined and compressed back to the permanent counts directory.
**   Reuses everything Utility already provides (MAX_FULL_PATH_NAME,
**   ClockTick, ThreadPool, DriveInfo) rather than redefining any of it.
*/

#pragma once

/* Includes */
#include "Utility.h"

/* Macros and Defines */
#define CALCULATOR_VERSION "0.32.7"   /* tracks the shared solution-wide version in OthelloTypes.h, not an independent counter */

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

    /* Total boards to process this level, per color -- set once (from a
    ** streaming RingNestedIndexStreamAll count-only pre-pass, never a
    ** wholesale load) before processing starts, so the live status
    ** display has a denominator for "% done" while a level is still in
    ** progress. 0 until then (and permanently 0 for a color genuinely
    ** absent at this level).
    */
    uint64_t  totalBoardsBlack;
    uint64_t  totalBoardsWhite;

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

    /* Boards at this level (both colors combined) with zero legal moves --
    ** classified directly rather than summed from children. For a terminal
    ** level (Phase 2) this always equals boardsProcessedBlack+White (every
    ** board there is terminal by construction); for a non-terminal level
    ** it's however many of this level's boards happened to have no moves.
    */
    uint64_t  terminalBoards;

    /* This level's confirmed final tier width: COUNTER_WIDTH_NIBBLE or a
    ** byte count (see CounterWidthConfig.h) -- always COUNTER_WIDTH_NIBBLE
    ** for a terminal level (Phase 2 never widens), whatever
    ** CounterWidthConfigGet last confirmed for a non-terminal one.
    */
    int       counterByteWidth;

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
    char      countsDrive;                            /* drive this calculator writes its own counts files to */
    char      countsDirNameNoDrive[MAX_FULL_PATH_NAME]; /* sub-path on countsDrive for counts output */
    char      cacheDirName[MAX_FULL_PATH_NAME];        /* logs + the single per-level width-config file live here */
    /* Separate from cacheDirName deliberately: drive benchmark results
    ** (write/read MB/s per physical drive) are a property of the machine,
    ** not of which program asked, so this defaults to RingMaster's OWN
    ** cache directory -- the calculator reuses RingMaster's already-
    ** benchmarked driveinfo.json instead of re-benchmarking every drive
    ** from scratch. Can't just share cacheDirName wholesale: RingMaster's
    ** and the calculator's log files use the identical "log_WxH_date.txt"
    ** naming pattern, so pointing both programs' whole cache directory at
    ** the same path would risk one silently overwriting the other's log.
    */
    char      driveCacheDirName[MAX_FULL_PATH_NAME];
    uint16_t  statsPort;                              /* default 17632 -- distinct from RingMaster's 17532 */
    char      useDrives[64];                          /* drive letters available for segmented scratch (e.g. "DEFG"); empty = auto-enumerate all fixed local drives */
    char      scratchDirNameNoDrive[MAX_FULL_PATH_NAME]; /* sub-path (on whichever scratch drive) segments are written under */
    bool      force;   /* --force: delete this board size's own sentinels/counts files before starting, ignoring any prior run's completed levels */
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
    uint8_t              deepestLevel;                /* set once at walk start -- the walk's starting point, for status display */
    volatile bool         terminateThreads;
    volatile bool         terminateStatsListener;
    const char* volatile  currentPhase;               /* points to a string literal; set by main thread at each phase transition */

    char  storeDirectory[MAX_FULL_PATH_NAME];   /* resolved from storeDrive + storeDirNameNoDrive -- RingMaster's finished output */
    char  countsDirectory[MAX_FULL_PATH_NAME];  /* resolved from countsDrive + countsDirNameNoDrive -- this calculator's own output */
    char  cacheDirectory[MAX_FULL_PATH_NAME];
    char  logFileName[MAX_FULL_PATH_NAME];

    /* Per-level stats history -- one entry per level, indexed by level number. */
    CalculatorLevelStats  levelStats[CALC_MAX_LEVELS];

    /* Drive-aware segmented scratch storage (see CalcDriveLedger.h,
    ** SegmentedStore.h): driveInfo is probed once at startup;
    ** driveLedger tracks each drive's remaining scratch budget as both
    ** level+1's lookup-source segments and level N's own output segments
    ** draw from the same pool. Same shape as OthelloTypes.h's own
    ** driveLedger[26] (indexed by letter - 'A').
    */
    MachineDriveInfo   driveInfo;
    volatile int64_t   driveLedger[26];

    ThreadPool* pStatsThreadPool;
    ThreadPool* pLookupThreadPool;   /* parallelizes per-parent child lookups -- see NonTerminalLevelStep.cpp */
} OthelloRingMasterCalculatorState, * POthelloRingMasterCalculatorState;
