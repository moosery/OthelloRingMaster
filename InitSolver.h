/*
** Filename:  InitSolver.h
**
** Purpose:
**   Declares InitSolver/CleanupSolver: the one-time startup sequence (memory
**   budget -> buffer sizing -> resume scan -> directory setup -> drive
**   ledgers -> thread pools) and matching teardown for the live solver.
**
** Notes:
**   Promoted from OthelloLevelBlaster's InitSolver.h; only the config/state
**   type names change (OthelloLevelBlasterConfig/State ->
**   OthelloRingMasterConfig/State).
*/

#pragma once

/* Includes */
#include "OthelloTypes.h"
#include "GetMachineInfo.h"

/* Functions */

/*
** Function: InitSolver
** @brief    Runs the full one-time startup sequence: empties recycle bins on
**           local run drives, probes machine capability, sizes every
**           buffer, scans for a resumable level, purges ephemeral working
**           directories, creates fresh ones, seeds drive ledgers, and
**           starts (and waits ready for) all three thread pools.
** @param    pConfig      - run configuration
** @param    pState       - out: fully initialized solver state
** @param    pMachineInfo - out: filled with probed machine information
*/
void InitSolver(POthelloRingMasterConfig pConfig, POthelloRingMasterState pState, PMachineInfo pMachineInfo);

/*
** Function: CleanupSolver
** @brief    Releases the instance lock, stops and frees all three thread
**           pools, frees every large buffer, and destroys the imerge critical section.
** @param    pState - the solver state to tear down
*/
void CleanupSolver(POthelloRingMasterState pState);
