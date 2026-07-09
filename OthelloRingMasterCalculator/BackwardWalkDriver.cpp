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
** Function: IsLevelComplete
** @brief    Checks whether level's calculator-output sentinel is present.
** @param    countsDir - counts directory
** @param    boardSize - board size
** @param    level     - level to check
** @return   true if level was already fully processed by a prior run.
*/
static bool IsLevelComplete(const char* countsDir, int boardSize, int level)
{
    char sentPath[MAX_FULL_PATH_NAME];
    CalcSentinelNameComplete(sentPath, sizeof(sentPath), countsDir, boardSize, level);
    return GetFileAttributesA(sentPath) != INVALID_FILE_ATTRIBUTES;
}

/*
** Function: MarkLevelComplete
** @brief    Writes level's calculator-output sentinel (zero-byte marker).
** @param    countsDir - counts directory
** @param    boardSize - board size
** @param    level     - level to mark complete
*/
static void MarkLevelComplete(const char* countsDir, int boardSize, int level)
{
    char sentPath[MAX_FULL_PATH_NAME];
    CalcSentinelNameComplete(sentPath, sizeof(sentPath), countsDir, boardSize, level);

    HANDLE h = CreateFileA(sentPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
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

    /* Tracks whether level 0 was actually (re)processed by THIS run, as
    ** opposed to skipped because a prior run already completed it --
    ** pState->levelStats[0] is only populated when ProcessNonTerminalLevel
    ** genuinely runs for level 0, never when IsLevelComplete skips it, so
    ** the final-tally print below must not assume it's always valid.
    */
    bool level0ProcessedThisRun = false;

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

        if (IsLevelComplete(pState->countsDirectory, boardSize, level))
        {
            LoggerLog("RunBackwardWalk: level %d already complete, skipping\n", level);
            continue;
        }

        if (level == deepestLevel)
            ProcessTerminalLevel(pConfig, pState, level);
        else
            ProcessNonTerminalLevel(pConfig, pState, pWidthConfig, level);

        MarkLevelComplete(pState->countsDirectory, boardSize, level);

        if (level == 0)
            level0ProcessedThisRun = true;
    }

    LoggerLog("RunBackwardWalk: complete -- levels %d down to 0 all processed\n", deepestLevel);

    /* Level 0 always has exactly one (black-to-move, non-terminal) board --
    ** the true starting position -- so its own accumulated total (see
    ** WinTieLossTripleAccumulateNibble/Wide) is not an approximation of
    ** any kind: it IS the fully validated final answer for the whole game
    ** tree.
    */
    if (level0ProcessedThisRun)
    {
        const WinTieLossTriple& finalTotals = pState->levelStats[0].combinedTotals;
        LoggerLog("RunBackwardWalk: FINAL RESULT -- blackWins=%llu whiteWins=%llu ties=%llu\n",
                  (unsigned long long)finalTotals.blackWins,
                  (unsigned long long)finalTotals.whiteWins,
                  (unsigned long long)finalTotals.ties);
    }
    else if (!pState->terminateThreads)
    {
        LoggerLog("RunBackwardWalk: level 0 was already complete from a prior run -- its final "
                  "result was logged then, not this run (delete its sentinel/counts file and "
                  "re-run to recompute and log it again).\n");
    }
}
