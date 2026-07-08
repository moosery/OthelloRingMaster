/*
** Filename:  InitLogger.cpp
**
** Purpose:
**   Implements InitLogger declared in InitLogger.h.
*/

/* Includes */
#include "InitLogger.h"

/* Functions */

/*
** Function: InitLogger
** @brief    Builds the dated log file path under pConfig->cacheDirName and
**           opens the process-wide logger.
** @param    pConfig - run configuration (boardSize, cacheDirName)
** @param    pState  - out: logFileName is filled with the opened log path
*/
void InitLogger(POthelloRingMasterConfig pConfig, POthelloRingMasterState pState)
{
    char dateStr[32];
    GetCurrentDateTimeString(dateStr, sizeof(dateStr));

    /* Cache directory must exist before the log file can be created in it. */
    if (!CreateFullPath(pConfig->cacheDirName))
        Fatal(FATAL_CREATE_DIR_FAILED, "Cannot create cache directory '%s'", pConfig->cacheDirName);

    snprintf(pState->logFileName, MAX_FULL_PATH_NAME, "%s\\log_%dx%d_%s.txt",
             pConfig->cacheDirName, pConfig->boardSize, pConfig->boardSize, dateStr);
    LoggerInit(pState->logFileName);
    LoggerLog("OthelloRingMaster! (Version %s)\n", VERSION);
}
