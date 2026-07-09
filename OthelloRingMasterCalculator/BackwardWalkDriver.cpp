/*
** Filename:  BackwardWalkDriver.cpp
**
** Purpose:
**   Implements RunBackwardWalk declared in BackwardWalkDriver.h.
*/

/* Includes */
#include "BackwardWalkDriver.h"
#include "CalculatorFileName.h"
#include "TerminalLevelBootstrap.h"
#include "NonTerminalLevelStep.h"
#include <windows.h>

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
            LoggerLog(statsValid
                       ? "RunBackwardWalk: level %d already complete, skipping (stats restored from sentinel)\n"
                       : "RunBackwardWalk: level %d already complete, skipping (legacy sentinel, no stats payload)\n",
                       level);
            if (level == 0) level0StatsValid = statsValid;
            continue;
        }

        if (level == deepestLevel)
            ProcessTerminalLevel(pConfig, pState, level);
        else
            ProcessNonTerminalLevel(pConfig, pState, pWidthConfig, level);

        MarkLevelComplete(pState, boardSize, level);

        if (level == 0) level0StatsValid = true;
    }

    LoggerLog("RunBackwardWalk: complete -- levels %d down to 0 all processed\n", deepestLevel);

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
    {
        const WinTieLossTriple& finalTotals = pState->levelStats[0].combinedTotals;
        LoggerLog("RunBackwardWalk: FINAL RESULT -- blackWins=%llu whiteWins=%llu ties=%llu\n",
                  (unsigned long long)finalTotals.blackWins,
                  (unsigned long long)finalTotals.whiteWins,
                  (unsigned long long)finalTotals.ties);
    }
    else if (!pState->terminateThreads)
    {
        LoggerLog("RunBackwardWalk: level 0's sentinel has no stats payload (written before this "
                  "version) -- delete Level_0000's calc_complete sentinel and re-run to recompute "
                  "and log the FINAL RESULT.\n");
    }
}
