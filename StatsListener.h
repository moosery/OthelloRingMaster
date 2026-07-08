/*
** Filename:  StatsListener.h
**
** Purpose:
**   Declares SubmitStatsListenerJob, which submits a long-running job that
**   listens for STATUS and STOP commands on pConfig->statsPort and returns immediately.
**
** Notes:
**   Adapted from an earlier solver implementation; only the config/state
**   type it operates through changed (via LevelSolverThread.h's
**   PSolveContext, already renamed).
*/

#pragma once

/* Includes */
#include "LevelSolverThread.h"

/* Functions */

/*
** Function: SubmitStatsListenerJob
** @brief    Submits a long-running job to pCtx->pState->pStatsThreadPool
**           that listens for STATUS and STOP commands on
**           pCtx->pConfig->statsPort. Returns immediately.
** @param    pCtx - solve context
*/
void SubmitStatsListenerJob(PSolveContext pCtx);
