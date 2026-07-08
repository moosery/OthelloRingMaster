/*
** Filename:  TerminalLevelBootstrap.cpp
**
** Purpose:
**   Implements ProcessTerminalLevel declared in TerminalLevelBootstrap.h.
*/

/* Includes */
#include "TerminalLevelBootstrap.h"
#include "CalculatorFileName.h"
#include "OutcomeTriple.h"
#include "CalculatorCountsFile.h"
#include "RSFFileName.h"
#include "OthelloBasics.h"
#include "RingNestedIndex.h"
#include <bit>
#include <windows.h>

/* Internal Helpers */

/*
** Function: ProcessTerminalLevelForPlayer
** @brief    Classifies and writes out every board at level for one player
**           (whichever color is to move at this level), accumulating that
**           color's own running totals into pState->levelStats[level].
** @param    pConfig - run configuration (boardSize)
** @param    pState  - calculator state (storeDirectory, countsDirectory, levelStats)
** @param    level   - the level to process
** @param    player  - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
*/
static void ProcessTerminalLevelForPlayer(POthelloRingMasterCalculatorConfig pConfig, POthelloRingMasterCalculatorState pState,
                                           int level, int player)
{
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
        return;
    }

    RingNestedIndexReader reader;
    if (foundCount != expectedCount || !reader.Load(cellsInUsePath, ring1Path, ring2Path, ring34Path))
        Fatal(FATAL_MERGE_LOGIC_ERROR,
              "ProcessTerminalLevel: level %d %s-to-move nested-index files are corrupt/partial (found %d of %d expected files)",
              level, RSFPlayerStr(player), foundCount, expectedCount);

    char countsPath[MAX_FULL_PATH_NAME];
    CalcNameCountsFile(countsPath, sizeof(countsPath), pState->countsDirectory, boardSize, level, player);

    NibbleCountsWriter* pWriter = NibbleCountsWriterOpen(countsPath);

    WinTieLossTriple totals          = {};
    uint64_t         boardsProcessed = 0;

    reader.ExpandAll([&](const BOARD_KEY& key)
    {
        /* Terminal classification needs no legal-move check here -- the
        ** deepest completed level's boards are terminal by construction
        ** (RingMaster only ever produces a further level when at least one
        ** board in this one still has children; the last level it produced
        ** is exactly the one where none did). Final piece count alone
        ** decides the outcome.
        */
        int blackCount = std::popcount(key.ullCellsInUse & key.ullCellColors);
        int whiteCount = std::popcount(key.ullCellsInUse) - blackCount;

        int outcome;
        if (blackCount > whiteCount)      { outcome = OUTCOME_BLACK_WIN; totals.blackWins++; }
        else if (whiteCount > blackCount) { outcome = OUTCOME_WHITE_WIN; totals.whiteWins++;  }
        else                              { outcome = OUTCOME_TIE;      totals.ties++;       }

        NibbleOutcomeTriple triple;
        NibbleOutcomeTripleSetOneHot(&triple, outcome);
        NibbleCountsWriterWrite(pWriter, &triple);

        boardsProcessed++;
    });

    NibbleCountsWriterClose(pWriter);

    CalculatorLevelStats* pStats = &pState->levelStats[level];
    if (player == RSF_PLAYER_BLACK)
    {
        pStats->boardsProcessedBlack = boardsProcessed;
        pStats->blackToMoveTotals    = totals;
    }
    else
    {
        pStats->boardsProcessedWhite = boardsProcessed;
        pStats->whiteToMoveTotals    = totals;
    }

    LoggerLog("ProcessTerminalLevel: level %d %s-to-move: %llu boards (blackWins=%llu whiteWins=%llu ties=%llu)\n",
              level, RSFPlayerStr(player), (unsigned long long)boardsProcessed,
              (unsigned long long)totals.blackWins, (unsigned long long)totals.whiteWins, (unsigned long long)totals.ties);
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
    ProcessTerminalLevelForPlayer(pConfig, pState, level, RSF_PLAYER_BLACK);

    pState->currentPlayer = RSF_PLAYER_WHITE;
    ProcessTerminalLevelForPlayer(pConfig, pState, level, RSF_PLAYER_WHITE);

    CalculatorLevelStats* pStats = &pState->levelStats[level];
    pStats->combinedTotals.blackWins = pStats->blackToMoveTotals.blackWins + pStats->whiteToMoveTotals.blackWins;
    pStats->combinedTotals.whiteWins = pStats->blackToMoveTotals.whiteWins + pStats->whiteToMoveTotals.whiteWins;
    pStats->combinedTotals.ties      = pStats->blackToMoveTotals.ties      + pStats->whiteToMoveTotals.ties;

    pStats->startTick  = startTick;
    pStats->totalNanos = ClockNanosSinceStart(&startTick);

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    snprintf(pStats->completedAt, sizeof(pStats->completedAt), "%04d-%02d-%02d %02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    LoggerLog("ProcessTerminalLevel: level %d complete: %llu boards total (blackWins=%llu whiteWins=%llu ties=%llu), %lld ns\n",
              level,
              (unsigned long long)(pStats->boardsProcessedBlack + pStats->boardsProcessedWhite),
              (unsigned long long)pStats->combinedTotals.blackWins,
              (unsigned long long)pStats->combinedTotals.whiteWins,
              (unsigned long long)pStats->combinedTotals.ties,
              (long long)pStats->totalNanos);
}
