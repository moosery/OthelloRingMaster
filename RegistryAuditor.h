/*
** Filename:  RegistryAuditor.h
**
** Purpose:
**   Declares the registry-vs-disk background auditor thread: periodically
**   checks the file registry (Registry.h/OthelloTypes.h) against real
**   directory contents, catching internal drift (a leaked reservation, a
**   missed release path) early rather than only at the next checkpoint's
**   integrity manifest cross-check. See
**   project_writer_drive_registry_redesign memory for the full design
**   (two separate checks -- size-mismatch and stuck-reservation -- and why
**   both are WARNING-severity, not Fatal, mid-run).
**
** Notes:
**   Own dedicated thread (OthelloRingMasterState.registryAuditorThread),
**   read-only with respect to everything else -- holds no resource anyone
**   else needs, so the final-merge stop sequence can just signal it and move
**   on rather than waiting on it the way the pools require.
*/

#pragma once

/* Includes */
#include "LevelSolverThread.h"

/* Functions */

/*
** Function: RegistryAuditorLoop
** @brief    Thread entry point: sleeps for the configured audit interval
**           (AUDIT_INTERVAL_SECONDS_DEFAULT, or
**           pConfig->auditIntervalSecondsOverride if nonzero), then for each
**           drive snapshots the registry briefly under lock, compares
**           against a fresh directory scan taken without holding the lock,
**           and logs a WARNING for any node that fails either check
**           (size-mismatch for non-reserved nodes; stuck-reservation for a
**           reserved node whose linked live progress counter -- see
**           RegistryLinkProgress, Registry.h -- has stopped moving, or, for
**           the rare node with no progress link, one reserved far longer
**           than a no-progress operation should take) on 2 or more
**           consecutive passes -- a single-pass discrepancy is treated as a
**           benign race against real concurrent activity, not reported.
**           Loops until terminateThreads is set.
** @param    pCtx - solve context
*/
void RegistryAuditorLoop(PSolveContext pCtx);
