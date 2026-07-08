/*
** Filename:  BackwardWalkDriver.h
**
** Purpose:
**   Declares RunBackwardWalk: the Phase 4 milestone of the retrograde
**   calculator -- loops the backward walk over every level from the
**   deepest completed level down to level 0, dispatching each level to
**   Phase 2's terminal bootstrap (deepest level only) or Phase 3's
**   non-terminal step (every level below it), with whole-level-
**   granularity resumability via a sentinel file per level.
**
** Notes:
**   Assumes RingMaster's forward solve for this store is already fully
**   complete (see project_adaptive_counter_width_design memory's scoping
**   constraint, and Phase 7's own "only after RingMaster's own 6x6 solve
**   has completed" note) -- the deepest completed level must not change
**   between calculator runs against the same store. If it did, an
**   earlier run's terminal-bootstrap classification of what was then the
**   deepest level could be invalidated by RingMaster later extending the
**   tree further, which the resumability logic here does not detect or
**   guard against.
*/

#pragma once

/* Includes */
#include "CalculatorTypes.h"
#include "CounterWidthConfig.h"

/* Functions */

/*
** Function: RunBackwardWalk
** @brief    Processes every level from deepestLevel down to 0, skipping
**           any level whose calculator-output sentinel is already
**           present (crash/restart resumability -- see file Notes).
** @param    pConfig      - run configuration (boardSize)
** @param    pState       - calculator state (storeDirectory, countsDirectory,
**                          levelStats -- updated in place per level)
** @param    pWidthConfig - in/out: this board size's persistent per-level
**                          tier-width table, threaded through every
**                          non-terminal level so width propagation
**                          carries across the whole walk
** @param    deepestLevel - the deepest level with a _complete sentinel in
**                          pState->storeDirectory (from FindDeepestCompleteLevel)
*/
void RunBackwardWalk(POthelloRingMasterCalculatorConfig pConfig, POthelloRingMasterCalculatorState pState,
                      CounterWidthConfig* pWidthConfig, int deepestLevel);
