/*
** Filename:  CalculatorStatsListener.h
**
** Purpose:
**   Declares CalculatorContext (the config/state bundle passed through the
**   stats thread job, mirroring LevelSolverThread.h's SolveContext) and
**   SubmitCalculatorStatsListenerJob, which submits a long-running job
**   that listens for STATUS and STOP commands on pConfig->statsPort and
**   returns immediately.
**
** Notes:
**   Mirrors StatsListener.h/.cpp's query-on-demand shape exactly (same
**   protocol the already-existing OthelloRingMasterCalculatorStatus
**   client expects: connect, send "STATUS\n" or "STOP\n", read the
**   response until the connection closes). A STOP command only sets
**   pState->terminateThreads -- BackwardWalkDriver.cpp checks this
**   between levels, never mid-level, matching the project's whole-level-
**   granularity resumability model (there is no finer-grained stopping
**   point to offer).
*/

#pragma once

/* Includes */
#include "CalculatorTypes.h"

/* Structures and Types */

/*
** Type:    CalculatorContext
** @brief   Bundles config/state together for the stats thread job, the
**          same reason LevelSolverThread.h's SolveContext exists --
**          passed by pointer instead of relying on globals from library code.
*/
typedef struct __CalculatorContext
{
    POthelloRingMasterCalculatorConfig  pConfig;
    POthelloRingMasterCalculatorState   pState;
} CalculatorContext, * PCalculatorContext;

/* Functions */

/*
** Function: SubmitCalculatorStatsListenerJob
** @brief    Submits a long-running job to pCtx->pState->pStatsThreadPool
**           that listens for STATUS and STOP commands on
**           pCtx->pConfig->statsPort. Returns immediately.
** @param    pCtx - calculator context
*/
void SubmitCalculatorStatsListenerJob(PCalculatorContext pCtx);
