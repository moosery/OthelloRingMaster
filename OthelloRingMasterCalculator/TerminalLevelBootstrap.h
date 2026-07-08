/*
** Filename:  TerminalLevelBootstrap.h
**
** Purpose:
**   Declares ProcessTerminalLevel: the Phase 2 milestone of the retrograde
**   calculator -- classify every board at the deepest completed level
**   directly from its final piece count and write the one-hot result out,
**   with no children and no lookups (see
**   project_adaptive_counter_width_design memory's "per-board
**   classification" section for why the deepest level's boards are always
**   terminal and always fit the nibble tier).
*/

#pragma once

/* Includes */
#include "CalculatorTypes.h"

/* Functions */

/*
** Function: ProcessTerminalLevel
** @brief    Classifies and writes out every board at level, for both
**           black-to-move and white-to-move, then records the combined
**           per-level stats into pState->levelStats[level].
** @param    pConfig - run configuration (boardSize)
** @param    pState  - calculator state (storeDirectory, countsDirectory,
**                     levelStats -- levelStats[level] and currentLevel/
**                     currentPlayer are updated in place)
** @param    level   - the level to process (must already have a
**                     _complete sentinel in pState->storeDirectory)
*/
void ProcessTerminalLevel(POthelloRingMasterCalculatorConfig pConfig, POthelloRingMasterCalculatorState pState, int level);
