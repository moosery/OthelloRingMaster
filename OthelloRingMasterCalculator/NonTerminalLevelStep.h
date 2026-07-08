/*
** Filename:  NonTerminalLevelStep.h
**
** Purpose:
**   Declares ProcessNonTerminalLevel: the Phase 3 milestone of the
**   retrograde calculator -- the real backward step for one level whose
**   boards are not (all) terminal. Given that level+1 has already been
**   fully processed (its counts files exist on disk), regenerates each
**   board's children via the GPU (RetrogradeKernels.h), looks each child
**   up in level+1's staged lookup source (CalculatorLookupSource.h's
**   LookupChildTriple, against drive-spanning segmented scratch -- see
**   SegmentedStore.h), sums them (overflow-checked, abort-and-retry-wider
**   on overflow -- see
**   project_adaptive_counter_width_design memory), and writes the result
**   back out in the same order this level's own boards are stored in.
**
** Notes:
**   Scoped to exactly one level per call, matching Phase 3's charter
**   ("Deliverable: given a fully-processed level N+1, process level N").
**   Looping this over every level from the deepest-1 down to 0 is
**   Phase 4's job, not this file's.
*/

#pragma once

/* Includes */
#include "CalculatorTypes.h"
#include "CounterWidthConfig.h"

/* Functions */

/*
** Function: ProcessNonTerminalLevel
** @brief    Processes level (both black-to-move and white-to-move),
**           assuming level+1 is already fully processed. Widens level's
**           own tier and reprocesses both colors from scratch on
**           overflow; persists pWidthConfig after every width change and
**           after final completion.
** @param    pConfig      - run configuration (boardSize)
** @param    pState       - calculator state (storeDirectory, countsDirectory,
**                          levelStats -- levelStats[level] and currentLevel/
**                          currentPlayer are updated in place)
** @param    pWidthConfig - in/out: this board size's persistent per-level
**                          tier-width table (level+1's width is read to
**                          interpret its counts file; level's own width
**                          is read/bumped/saved as this level is processed)
** @param    level        - the level to process (level+1 must already have
**                          its counts files written under pState->countsDirectory)
*/
void ProcessNonTerminalLevel(POthelloRingMasterCalculatorConfig pConfig, POthelloRingMasterCalculatorState pState,
                              CounterWidthConfig* pWidthConfig, int level);
