/*
** Filename:  InitLogger.h
**
** Purpose:
**   Declares InitLogger, which builds the dated log file path and opens the
**   process-wide logger before any other startup work happens.
**
** Notes:
**   Promoted from OthelloLevelBlaster's InitLogger.h; logic unchanged, only
**   the config/state types are renamed (OthelloLevelBlasterConfig/State ->
**   OthelloRingMasterConfig/State).
*/

#pragma once

/* Includes */
#include "OthelloTypes.h"

/* Functions */

/*
** Function: InitLogger
** @brief    Builds the dated log file path under pConfig->cacheDirName and
**           opens the process-wide logger.
** @param    pConfig - run configuration (boardSize, cacheDirName)
** @param    pState  - out: logFileName is filled with the opened log path
*/
void InitLogger(POthelloRingMasterConfig pConfig, POthelloRingMasterState pState);
