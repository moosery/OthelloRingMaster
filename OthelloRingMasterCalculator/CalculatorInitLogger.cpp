/*
** Filename:  CalculatorInitLogger.cpp
**
** Purpose:
**   Implements CalculatorInitLogger declared in CalculatorInitLogger.h.
*/

/* Includes */
#include "CalculatorInitLogger.h"

/* Functions */

/*
** Function: CalculatorInitLogger
** @brief    See CalculatorInitLogger.h.
*/
void CalculatorInitLogger(POthelloRingMasterCalculatorConfig pConfig, POthelloRingMasterCalculatorState pState)
{
    char dateStr[32];
    GetCurrentDateTimeString(dateStr, sizeof(dateStr));

    if (!CreateFullPath(pConfig->cacheDirName))
        Fatal(FATAL_CREATE_DIR_FAILED, "CalculatorInitLogger: cannot create cache directory '%s'", pConfig->cacheDirName);

    snprintf(pState->logFileName, MAX_FULL_PATH_NAME, "%s\\log_%dx%d_%s.txt",
             pConfig->cacheDirName, pConfig->boardSize, pConfig->boardSize, dateStr);
    LoggerInit(pState->logFileName);

    /* From here on a native crash (or an uncaught exception, abort, or C
    ** runtime invalid-parameter failure) appends a readable record to
    ** this same log -- without it, a crash leaves a log that just stops.
    */
    CrashHandlerInstall(pState->logFileName);

    LoggerLog("OthelloRingMasterCalculator! (Version %s)\n", CALCULATOR_VERSION);
}
