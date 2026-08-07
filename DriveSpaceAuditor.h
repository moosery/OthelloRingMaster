/*
** Filename:  DriveSpaceAuditor.h
**
** Purpose:
**   Declares the drive-space-reconciliation background auditor thread:
**   periodically recomputes each writer/medium drive's real used/free space
**   from scratch (real on-disk size of finished registry files + reservedBytes
**   of in-progress ones) and corrects DriveLedger.h's running total against
**   that ground truth -- catches both external interference (e.g. a manual
**   file copy onto a writer drive) and any internal DriveReserve/DriveReclaim
**   bookkeeping drift, not just one or the other. See
**   project_writer_drive_registry_redesign memory for the full design,
**   including the asymmetric urgency between the two correction directions.
**
** Notes:
**   Own dedicated thread (OthelloRingMasterState.driveSpaceAuditorThread),
**   read-only with respect to the registry (only DriveLedger's ledger value
**   itself is corrected) -- holds no resource anyone else needs, so the
**   final-merge stop sequence can just signal it and move on.
*/

#pragma once

/* Includes */
#include "LevelSolverThread.h"

/* Functions */

/*
** Function: DriveSpaceAuditorLoop
** @brief    Thread entry point: sleeps for the configured audit interval
**           (AUDIT_INTERVAL_SECONDS_DEFAULT, or
**           pConfig->auditIntervalSecondsOverride if nonzero), then for each
**           writer and medium drive recomputes real used bytes from the
**           registry (sum of finished-file real sizes + in-progress
**           reservedBytes) and reconciles DriveLedger's tracked free value
**           against (driveCapacity - used). Corrects promptly when real
**           usage is higher than expected (unambiguous -- something external
**           consumed space); requires the discrepancy to persist across 2+
**           consecutive passes when real usage is lower than expected
**           (routinely confounded with normal reservation overestimate
**           slack, see design rationale). Loops until terminateThreads is set.
** @param    pCtx - solve context
*/
void DriveSpaceAuditorLoop(PSolveContext pCtx);
