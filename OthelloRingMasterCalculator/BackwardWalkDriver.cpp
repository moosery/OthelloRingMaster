/*
** Filename:  BackwardWalkDriver.cpp
**
** Purpose:
**   Implements RunBackwardWalk declared in BackwardWalkDriver.h.
*/

/* Includes */
#include "BackwardWalkDriver.h"
#include "CalculatorFileName.h"
#include "CalculatorLevelTable.h"
#include "TerminalLevelBootstrap.h"
#include "NonTerminalLevelStep.h"
#include <windows.h>
#include <string.h>

/* Internal Helpers */

/*
** Function: CheckLevelCompleteAndRestore
** @brief    Checks whether level's calculator-output sentinel is present,
**           and if so, restores its persisted CalculatorLevelStats into
**           pState->levelStats[level] -- mirrors RingMaster's own
**           ScanForResumeLevel (InitSolver.cpp), which does the same
**           restoration on its own sentinel hit, so a resumed/rerun
**           calculator invocation can still log/report a previously
**           completed level's real numbers (including level 0's FINAL
**           RESULT) without re-processing it.
** @param    pState          - calculator state (levelStats[level] restored into on hit)
** @param    boardSize       - board size
** @param    level           - level to check
** @param    pOutStatsValid  - out: true if the sentinel actually carried a
**                             restorable stats payload -- false for a
**                             legacy zero-byte sentinel (written before
**                             this version), in which case
**                             pState->levelStats[level] is left untouched
**                             (all zero) and must NOT be reported as real
** @return   true if level was already fully processed by a prior run
**           (the counts data itself is valid either way -- only the
**           STATS payload can be legacy-missing).
*/
static bool CheckLevelCompleteAndRestore(POthelloRingMasterCalculatorState pState, int boardSize, int level,
                                          bool* pOutStatsValid)
{
    char sentPath[MAX_FULL_PATH_NAME];
    CalcSentinelNameComplete(sentPath, sizeof(sentPath), pState->countsDirectory, boardSize, level);

    if (GetFileAttributesA(sentPath) == INVALID_FILE_ATTRIBUTES)
    {
        *pOutStatsValid = false;
        return false;
    }

    CalculatorLevelStats restored = {};
    *pOutStatsValid = ReadCalcSentinelStats(sentPath, &restored);
    if (*pOutStatsValid)
        pState->levelStats[level] = restored;

    return true;
}

/*
** Function: MarkLevelComplete
** @brief    Writes level's calculator-output sentinel with its full stats
**           payload (see WriteCalcSentinelStats), so a future run can
**           restore and report this level's real numbers without
**           re-processing it.
** @param    pState    - calculator state (levelStats[level] is what gets persisted)
** @param    boardSize - board size
** @param    level     - level to mark complete
*/
static void MarkLevelComplete(POthelloRingMasterCalculatorState pState, int boardSize, int level)
{
    char sentPath[MAX_FULL_PATH_NAME];
    CalcSentinelNameComplete(sentPath, sizeof(sentPath), pState->countsDirectory, boardSize, level);
    WriteCalcSentinelStats(sentPath, &pState->levelStats[level]);
}

/*
** Function: LogLevelTableRow
** @brief    Prints one level's table row via CalcLevelTableFormatRow.
** @param    level - the level this row is for
** @param    pState - calculator state (levelStats[level] must be valid)
*/
static void LogLevelTableRow(POthelloRingMasterCalculatorState pState, int level)
{
    char row[256];
    CalcLevelTableFormatRow(level, &pState->levelStats[level], row, sizeof(row));
    LoggerLog("%s\n", row);
}

/*
** Function: PrintFinalResultBox
** @brief    Prints level 0's fully validated final answer as a small
**           ASCII box, black/white/tie plus the total, for a clean,
**           unambiguous end-of-run summary distinct from the scrolling
**           per-level table above it.
** @param    boardSize - board size, shown in the box title
** @param    t         - level 0's combinedTotals (the real final answer)
*/
static void PrintFinalResultBox(int boardSize, const WinTieLossTriple& t)
{
    const int labelW = 14;
    const int valueW = 20;
    const int innerW = 1 + labelW + 1 + valueW + 1;   /* " label " + " value " widths incl. spaces either side */

    char fullBorder[64];
    int  n = 0;
    fullBorder[n++] = '+';
    for (int i = 0; i < innerW; i++) fullBorder[n++] = '-';
    fullBorder[n++] = '+';
    fullBorder[n] = '\0';

    char midBorder[64];
    n = 0;
    midBorder[n++] = '+';
    for (int i = 0; i < labelW + 2; i++) midBorder[n++] = '-';
    midBorder[n++] = '+';
    for (int i = 0; i < valueW + 2; i++) midBorder[n++] = '-';
    midBorder[n++] = '+';
    midBorder[n] = '\0';

    char title[48];
    snprintf(title, sizeof(title), "FINAL RESULT (%dx%d)", boardSize, boardSize);
    int titleLen  = (int)strlen(title);
    int leftPad   = (innerW - titleLen) / 2;
    int rightPad  = (innerW - titleLen) - leftPad;
    if (leftPad < 0) leftPad = 0;
    if (rightPad < 0) rightPad = 0;

    uint64_t total = t.blackWins + t.whiteWins + t.ties;

    LoggerLog("%s\n", fullBorder);
    LoggerLog("|%*s%s%*s|\n", leftPad, "", title, rightPad, "");
    LoggerLog("%s\n", midBorder);
    LoggerLog("| %-*s | %*llu |\n", labelW, "Black Wins", valueW, (unsigned long long)t.blackWins);
    LoggerLog("| %-*s | %*llu |\n", labelW, "White Wins", valueW, (unsigned long long)t.whiteWins);
    LoggerLog("| %-*s | %*llu |\n", labelW, "Ties",       valueW, (unsigned long long)t.ties);
    LoggerLog("%s\n", midBorder);
    LoggerLog("| %-*s | %*llu |\n", labelW, "Total Games", valueW, (unsigned long long)total);
    LoggerLog("%s\n", midBorder);
}

/* Functions */

/*
** Function: RunBackwardWalk
** @brief    See BackwardWalkDriver.h.
*/
void RunBackwardWalk(POthelloRingMasterCalculatorConfig pConfig, POthelloRingMasterCalculatorState pState,
                      CounterWidthConfig* pWidthConfig, int deepestLevel)
{
    int boardSize = (int)pConfig->boardSize;
    pState->deepestLevel = (uint8_t)deepestLevel;

    char header[256], separator[256];
    CalcLevelTableHeaderLines(header, sizeof(header), separator, sizeof(separator));
    LoggerLog("%s\n", header);
    LoggerLog("%s\n", separator);

    bool level0StatsValid = false;

    for (int level = deepestLevel; level >= 0; level--)
    {
        /* A STOP request via the stats listener is checked between levels
        ** only -- matching the project's whole-level-granularity
        ** resumability model, this never interrupts a level partway
        ** through (see CalculatorStatsListener.h's own Notes).
        */
        if (pState->terminateThreads)
        {
            LoggerLog("RunBackwardWalk: stop requested, halting before level %d\n", level);
            break;
        }

        bool statsValid = false;
        if (CheckLevelCompleteAndRestore(pState, boardSize, level, &statsValid))
        {
            if (statsValid)
                LogLevelTableRow(pState, level);
            else
                LoggerLog("RunBackwardWalk: level %d already complete, skipping (legacy sentinel, no stats payload)\n", level);
            if (level == 0) level0StatsValid = statsValid;
            continue;
        }

        if (level == deepestLevel)
            ProcessTerminalLevel(pConfig, pState, level);
        else
            ProcessNonTerminalLevel(pConfig, pState, pWidthConfig, level);

        MarkLevelComplete(pState, boardSize, level);
        LogLevelTableRow(pState, level);

        if (level == 0) level0StatsValid = true;
    }

    LoggerLog("RunBackwardWalk: complete -- levels %d down to 0 all processed\n\n", deepestLevel);

    /* Full reprint of the completed-level history, deepest-first --
    ** mirrors OthelloRingMaster.cpp's own end-of-run table reprint.
    ** Skips any level whose stats aren't available (never processed this
    ** run and no restorable sentinel payload -- totalNanos stays 0 in
    ** that case, the same "not yet completed" signal
    ** CalculatorStatsListener.cpp's own history table already uses).
    */
    LoggerLog("--- Completed level history ---\n");
    LoggerLog("%s\n", header);
    LoggerLog("%s\n", separator);
    for (int level = deepestLevel; level >= 0; level--)
    {
        if (pState->levelStats[level].totalNanos == 0) continue;
        LogLevelTableRow(pState, level);
    }
    LoggerLog("\n");

    /* Level 0 always has exactly one (black-to-move, non-terminal) board --
    ** the true starting position -- so its own accumulated total (see
    ** WinTieLossTripleAccumulateNibble/Wide) is not an approximation of
    ** any kind: it IS the fully validated final answer for the whole game
    ** tree. Available whether level 0 was processed fresh by this run or
    ** restored from a prior run's sentinel -- either way the number is
    ** real. NOT printed for a legacy zero-byte sentinel (statsValid
    ** false): pState->levelStats[0] was left untouched (all zero) in
    ** that case, and printing it would be a silently wrong answer, not a
    ** missing one.
    */
    if (level0StatsValid)
        PrintFinalResultBox(boardSize, pState->levelStats[0].combinedTotals);
    else if (!pState->terminateThreads)
    {
        LoggerLog("RunBackwardWalk: level 0's sentinel has no stats payload (written before this "
                  "version) -- delete Level_0000's calc_complete sentinel and re-run to recompute "
                  "and log the FINAL RESULT.\n");
    }
}
