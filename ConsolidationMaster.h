/*
** Filename:  ConsolidationMaster.h
**
** Purpose:
**   Declares the single, event-driven consolidation master thread that
**   replaces the old 12-thread polling consolidation pool
**   (ConsolidationWorkerLoop/TryConsolidatePair, MergeFiles.cpp, retired).
**   The master sleeps until woken by a real trigger (a flush completing, a
**   consolidator worker finishing, or an iMerge finishing), then dispatches
**   bounded work onto the consolidator worker pool -- see
**   project_writer_drive_registry_redesign memory for the full design
**   rationale.
**
** Notes:
**   Not a ThreadPool job -- this is exactly one thread for the whole run
**   (OthelloRingMasterState.consolidationMasterThread), started once in
**   InitSolver and joined once in CleanupSolver/the final-merge stop
**   sequence, not re-queued per level the way the old pool was.
*/

#pragma once

/* Includes */
#include "LevelSolverThread.h"

/* Functions */

/*
** Function: ConsolidationMasterLoop
** @brief    Thread entry point: waits on consolidationMasterCV until woken,
**           then for each drive/color, if a consolidator worker is free,
**           scans the registry for >2 unreserved files under the
**           consolidation-eligibility cap and dispatches a worker if found.
**           Loops until terminateConsolidation (level's solve->merge
**           transition or shutdown) is set and every dispatched worker has
**           finished, then returns.
** @param    pCtx - solve context
*/
void ConsolidationMasterLoop(PSolveContext pCtx);

/*
** Function: ConsolidationMasterWake
** @brief    Signals the master thread to re-evaluate now, instead of
**           waiting for its next natural wake. Called by: a flush
**           completing (a new file may be consolidation-eligible), a
**           consolidator worker finishing (frees a slot, and its own output
**           may itself be eligible for further consolidation), and an
**           iMerge finishing (files it didn't take may now be worth
**           examining -- note the space-relief coordinator stops and restarts
**           this master around a sweep, see SpaceReliefCoordinator in
**           OthelloTypes.h and RelieveSpacePressure in IMergePool.cpp).
** @param    pCtx - solve context
*/
void ConsolidationMasterWake(PSolveContext pCtx);

/*
** Function: ConsolidationMasterStop
** @brief    Sets terminateConsolidation, wakes the master so it notices
**           promptly, and joins the thread. Call after the consolidator
**           worker pool itself has drained (WaitForPoolIdle-equivalent) --
**           see the final-merge stop sequence in
**           project_writer_drive_registry_redesign memory: master stops
**           first (no new work gets dispatched), then workers are waited
**           for, mirroring this function's own internal ordering.
** @param    pCtx - solve context
*/
void ConsolidationMasterStop(PSolveContext pCtx);
