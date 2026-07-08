/*
** Filename:  CalculatorInitLogger.h
**
** Purpose:
**   Declares CalculatorInitLogger, which builds the dated log file path and
**   opens the process-wide logger before any other startup work happens.
**
** Notes:
**   Mirrors the forward solver's own InitLogger.h/.cpp exactly, just bound
**   to OthelloRingMasterCalculatorConfig/State instead of
**   OthelloRingMasterConfig/State -- kept as its own copy rather than a
**   shared function since the two config/state types are unrelated
**   structs (see CalculatorTypes.h's own Notes on why it doesn't include
**   OthelloTypes.h).
*/

#pragma once

/* Includes */
#include "CalculatorTypes.h"

/* Functions */

/*
** Function: CalculatorInitLogger
** @brief    Builds the dated log file path under pConfig->cacheDirName and
**           opens the process-wide logger.
** @param    pConfig - run configuration (boardSize, cacheDirName)
** @param    pState  - out: logFileName is filled with the opened log path
*/
void CalculatorInitLogger(POthelloRingMasterCalculatorConfig pConfig, POthelloRingMasterCalculatorState pState);
