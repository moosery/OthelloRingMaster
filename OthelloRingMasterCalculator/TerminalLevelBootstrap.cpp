/*
** Filename:  TerminalLevelBootstrap.cpp
**
** Purpose:
**   Implements ProcessTerminalLevel declared in TerminalLevelBootstrap.h.
*/

/* Includes */
#include "TerminalLevelBootstrap.h"
#include "CalculatorFileName.h"
#include "CalculatorScratchCounts.h"
#include "OutcomeTriple.h"
#include "TerminalClassify.h"
#include "RSFFileName.h"
#include "OthelloBasics.h"
#include "RingNestedIndex.h"
#include <windows.h>

/* Structures and Types */

/*
** Type:    TerminalPlayerResult
** @brief   One color's outcome from ProcessTerminalLevelForPlayer: board/
**          outcome bookkeeping plus the scratch segments holding the
**          result, not yet joined to the permanent counts directory.
*/
struct TerminalPlayerResult
{
    uint64_t         boardsProcessed = 0;
    WinTieLossTriple totals          = {};

    SegmentList                           scratchSegments;
    std::vector<std::pair<char, int64_t>> scratchPlan;
};

/* Internal Helpers */

/*
** Function: ProcessTerminalLevelForPlayer
** @brief    Classifies and writes out every board at level for one player
**           (whichever color is to move at this level) to SCRATCH -- the
**           caller joins scratch to the permanent counts directory once
**           both colors are done, same as the non-terminal step, for
**           consistency across every level regardless of kind.
** @param    pConfig - run configuration (boardSize)
** @param    pState  - calculator state (storeDirectory, driveInfo, driveLedger, levelStats)
** @param    level   - the level to process
** @param    player  - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @return   This color's result (board count, exact win/tie/loss totals,
**           and -- if this color has any boards -- the scratch segments
**           holding the result).
*/
static TerminalPlayerResult ProcessTerminalLevelForPlayer(POthelloRingMasterCalculatorConfig pConfig, POthelloRingMasterCalculatorState pState,
                                                            int level, int player)
{
    TerminalPlayerResult result;

    int  boardSize = (int)pConfig->boardSize;
    bool hasRing1  = RingNestedIndexHasRing1(boardSize);
    bool hasRing2  = RingNestedIndexHasRing2(boardSize);

    char cellsInUsePath[MAX_FULL_PATH_NAME];
    char ring1PathBuf[MAX_FULL_PATH_NAME];
    char ring2PathBuf[MAX_FULL_PATH_NAME];
    char ring34Path[MAX_FULL_PATH_NAME];
    RSFNameCellsInUseFile(cellsInUsePath, sizeof(cellsInUsePath), pState->storeDirectory, boardSize, level, player, 0);
    if (hasRing1)
        RSFNameRing1File(ring1PathBuf, sizeof(ring1PathBuf), pState->storeDirectory, boardSize, level, player, 0);
    if (hasRing2)
        RSFNameRing2File(ring2PathBuf, sizeof(ring2PathBuf), pState->storeDirectory, boardSize, level, player, 0);
    RSFNameRing34File(ring34Path, sizeof(ring34Path), pState->storeDirectory, boardSize, level, player, 0);

    const char* ring1Path     = hasRing1 ? ring1PathBuf : nullptr;
    const char* ring2Path     = hasRing2 ? ring2PathBuf : nullptr;
    int         expectedCount = 2 + (hasRing1 ? 1 : 0) + (hasRing2 ? 1 : 0);

    /* A genuinely absent player at this level (0 files) is a normal shape
    ** of the game tree, not a problem -- see InitSolver.cpp's checkLevelFile,
    ** which treats "both players absent" as the only absence that means
    ** anything. Anything found but not fully readable is a real problem
    ** and must never be silently skipped.
    */
    int foundCount = RingNestedIndexFileCount(cellsInUsePath, ring1Path, ring2Path, ring34Path);
    if (foundCount == 0)
    {
        LoggerLog("ProcessTerminalLevel: level %d has no %s-to-move boards, skipping\n", level, RSFPlayerStr(player));
        return result;
    }

    if (foundCount != expectedCount)
        Fatal(FATAL_MERGE_LOGIC_ERROR,
              "ProcessTerminalLevel: level %d %s-to-move nested-index files are corrupt/partial (found %d of %d expected files)",
              level, RSFPlayerStr(player), foundCount, expectedCount);

    /* A streaming count-only pre-pass -- the only reason this is needed at
    ** all is the status listener's "% done" denominator (pStats->totalBoards*),
    ** which must be known before processing starts. Never holds a level
    ** resident; it just walks the same lockstep stream twice.
    */
    CalculatorLevelStats* pStats = &pState->levelStats[level];
    uint64_t              totalBoards = 0;
    if (!RingNestedIndexStreamAll(cellsInUsePath, ring1Path, ring2Path, ring34Path,
                                  [&totalBoards](const BOARD_KEY&) { totalBoards++; }))
        Fatal(FATAL_MERGE_LOGIC_ERROR,
              "ProcessTerminalLevel: level %d %s-to-move nested-index files failed to stream (count pass)",
              level, RSFPlayerStr(player));
    if (player == RSF_PLAYER_BLACK) pStats->totalBoardsBlack = totalBoards;
    else                            pStats->totalBoardsWhite = totalBoards;

    char baseName[MAX_FULL_PATH_NAME];
    snprintf(baseName, sizeof(baseName), "L%04d_%s_out", level, RSFPlayerStr(player));

    ScratchCountsWriter writer;
    writer.Init(pState, pConfig->storeDrive, pConfig->countsDrive, COUNTER_WIDTH_NIBBLE,
                pConfig->scratchDirNameNoDrive, baseName);

    bool streamOk = RingNestedIndexStreamAll(cellsInUsePath, ring1Path, ring2Path, ring34Path,
                                             [&](const BOARD_KEY& key)
    {
        /* Terminal classification needs no legal-move check here -- the
        ** deepest completed level's boards are terminal by construction
        ** (RingMaster only ever produces a further level when at least one
        ** board in this one still has children; the last level it produced
        ** is exactly the one where none did). Final piece count alone
        ** decides the outcome.
        */
        int outcome = ClassifyTerminalOutcome(key);

        NibbleOutcomeTriple triple;
        NibbleOutcomeTripleSetOneHot(&triple, outcome);
        writer.WriteNibbleTriple(triple);

        WinTieLossTripleAccumulateNibble(&result.totals, &triple, level);

        result.boardsProcessed++;

        /* Live progress for the status listener -- updated per board since
        ** classification/write here is cheap; a plain (non-atomic) field
        ** read concurrently by the stats thread, same established pattern
        ** as OthelloRingMaster's own LevelStats.boardsReadFromStore.
        */
        if (player == RSF_PLAYER_BLACK) pStats->boardsProcessedBlack = result.boardsProcessed;
        else                            pStats->boardsProcessedWhite = result.boardsProcessed;
    });
    if (!streamOk)
        Fatal(FATAL_MERGE_LOGIC_ERROR,
              "ProcessTerminalLevel: level %d %s-to-move nested-index files failed to stream (process pass)",
              level, RSFPlayerStr(player));

    if (result.boardsProcessed != totalBoards)
        Fatal(FATAL_MERGE_LOGIC_ERROR,
              "ProcessTerminalLevel: level %d %s-to-move: process pass saw %llu boards, count pass saw %llu",
              level, RSFPlayerStr(player), (unsigned long long)result.boardsProcessed, (unsigned long long)totalBoards);

    writer.Finish();
    result.scratchSegments = writer.store.segments;
    result.scratchPlan     = writer.store.plan;

    if (player == RSF_PLAYER_BLACK) pStats->blackToMoveTotals = result.totals;
    else                            pStats->whiteToMoveTotals = result.totals;

    return result;
}

/*
** Function: ProcessTerminalLevel
** @brief    See TerminalLevelBootstrap.h.
*/
void ProcessTerminalLevel(POthelloRingMasterCalculatorConfig pConfig, POthelloRingMasterCalculatorState pState, int level)
{
    if (!CreateFullPath(pState->countsDirectory))
        Fatal(FATAL_CREATE_DIR_FAILED, "ProcessTerminalLevel: cannot create counts directory '%s'", pState->countsDirectory);

    ClockTick startTick;
    ClockStart(&startTick);

    pState->currentLevel = (uint8_t)level;

    pState->currentPlayer = RSF_PLAYER_BLACK;
    TerminalPlayerResult blackResult = ProcessTerminalLevelForPlayer(pConfig, pState, level, RSF_PLAYER_BLACK);

    pState->currentPlayer = RSF_PLAYER_WHITE;
    TerminalPlayerResult whiteResult = ProcessTerminalLevelForPlayer(pConfig, pState, level, RSF_PLAYER_WHITE);

    /* Join scratch to the permanent counts directory -- "read the first
    ** drive, write it out, read the next drive, etc.," no sort needed.
    ** A color with no boards at this level gets no file at all, matching
    ** the established absent-color behavior.
    */
    int boardSize = (int)pConfig->boardSize;
    for (int player = RSF_PLAYER_WHITE; player <= RSF_PLAYER_BLACK; player++)
    {
        TerminalPlayerResult& r = (player == RSF_PLAYER_BLACK) ? blackResult : whiteResult;
        if (r.scratchSegments.empty()) continue;

        char finalPath[MAX_FULL_PATH_NAME];
        CalcNameCountsFile(finalPath, sizeof(finalPath), pState->countsDirectory, boardSize, level, player);
        JoinScratchCountsToFinal(r.scratchSegments, /*scratchByteWidth=*/1, COUNTER_WIDTH_NIBBLE, finalPath);
        DeleteSegments(pState, r.scratchSegments, r.scratchPlan);
    }

    CalculatorLevelStats* pStats = &pState->levelStats[level];
    pStats->combinedTotals.blackWins = pStats->blackToMoveTotals.blackWins + pStats->whiteToMoveTotals.blackWins;
    pStats->combinedTotals.whiteWins = pStats->blackToMoveTotals.whiteWins + pStats->whiteToMoveTotals.whiteWins;
    pStats->combinedTotals.ties      = pStats->blackToMoveTotals.ties      + pStats->whiteToMoveTotals.ties;

    /* Every board at a terminal level is terminal by construction (see
    ** ProcessTerminalLevelForPlayer's own Notes), and this level never
    ** widens beyond nibble (Phase 2's classification is always a trivial
    ** one-hot, never a wide sum).
    */
    pStats->terminalBoards   = pStats->boardsProcessedBlack + pStats->boardsProcessedWhite;
    pStats->counterByteWidth = COUNTER_WIDTH_NIBBLE;

    pStats->startTick  = startTick;
    pStats->totalNanos = ClockNanosSinceStart(&startTick);

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    snprintf(pStats->completedAt, sizeof(pStats->completedAt), "%04d-%02d-%02d %02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}
