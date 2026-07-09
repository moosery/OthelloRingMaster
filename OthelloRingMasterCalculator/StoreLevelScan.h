/*
** Filename:  StoreLevelScan.h
**
** Purpose:
**   Declares FindDeepestCompleteLevel: a read-only scan of RingMaster's
**   finished store directory for the highest level number whose
**   _complete sentinel is present.
**
** Notes:
**   Deliberately much simpler than InitSolver.cpp's own
**   ScanForResumeLevel -- that scan also repairs/purges partial output
**   because the forward solver owns storeDir and must resume writing to
**   it. This calculator only ever reads storeDir, never writes to it, so
**   there is nothing to repair here: a missing sentinel just means "stop
**   scanning," full stop.
**
**   The deepest sentinel found is NOT always a level with real board
**   data: RingMaster (OthelloRingMaster.cpp) writes a level+1 _complete
**   sentinel the moment its own solve loop confirms level produced zero
**   new boards -- a "nothing past here" marker, not a claim that level+1
**   itself has data. FindDeepestCompleteLevel steps back past any such
**   empty level(s) to the deepest one that actually has ring-index
**   files, since that's the real terminal level for the calculator to
**   process.
*/

#pragma once

/* Includes */
#include "CalculatorTypes.h"

/* Functions */

/*
** Function: FindDeepestCompleteLevel
** @brief    Scans storeDir from level 0 upward for _complete sentinels and
**           returns the highest level number found.
** @param    storeDir  - RingMaster's finished store directory to scan
** @param    boardSize - exact board size (sentinel names embed it)
** @return   The highest complete level found, or -1 if even level 0 has no
**           _complete sentinel (nothing to process yet).
*/
int FindDeepestCompleteLevel(const char* storeDir, int boardSize);
