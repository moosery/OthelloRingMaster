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

    for (int level = deepestLevel; level >= 0; level--)
    {
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
    }

    LoggerLog("RunBackwardWalk: complete -- levels %d down to 0 all processed\n", deepestLevel);
}
