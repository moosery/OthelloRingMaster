/*
** Filename:  IMergePool.h
**
** Purpose:
**   Declares the coordinated cross-drive space-relief path -- replaces
**   DoCrossDriveIntermediateMerge (MergeFiles.cpp, retired) and the earlier
**   per-color IMergeTriggerAndWait design. When a writer-drive flush can't
**   reserve space, exactly ONE flusher thread runs a single global relief
**   event (SpaceReliefCoordinator, OthelloTypes.h): it gates all other
**   flushes, drains in-flight writes, and -- only if the reclaimed slack from
**   that drain isn't enough -- pauses consolidation and sweeps BOTH colors off
**   every NVMe drive to the medium/store drive in parallel. See
**   project_writer_drive_registry_redesign memory for the full design and the
**   detailed deadlock/livelock analysis behind it (the two subtle
**   requirements: a WRITE-only "active writers" signal distinct from
**   mwFlushActive, and a gate every flush checks before reserving so the drain
**   can actually reach zero).
*/

#pragma once

/* Includes */
#include "LevelSolverThread.h"

/* Functions */

/*
** Function: SpaceReliefGateWait
** @brief    Called at the top of a flush, BEFORE it tries to reserve: if a
**           space-relief event is currently in progress, block here until it
**           finishes. This is what keeps new flushes from starting a fresh
**           write mid-relief -- without it the GPU could keep feeding the
**           other drive, new writes would keep starting, and the coordinator's
**           "wait for all writes to finish" drain would never reach zero
**           (livelock). While every flush is gated, the merge-writer threads
**           back up and the GPU feeder stalls -- the intended, acceptable
**           stall for the duration of a relief.
** @param    pCtx - solve context
*/
void SpaceReliefGateWait(PSolveContext pCtx);

/*
** Function: RelieveSpacePressure
** @brief    Called by a flush whose RegistryReserveNew just failed. Either
**           becomes the single relief coordinator or (if one is already
**           running) waits for it to finish. The coordinator: gates new
**           flushes, drains all in-flight writes to zero, re-tries the
**           caller's reservation (the drained writers' worst-case reclaims may
**           already be enough -- no sweep needed), and only if that still
**           fails pauses consolidation, runs a both-color cross-drive sweep,
**           restarts consolidation, and reserves again.
** @param    pCtx         - solve context
** @param    ti           - writer drive index of the caller's flush
** @param    player       - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @param    path         - the caller's already-built output file path
** @param    driveLetter  - the caller's writer-drive letter
** @param    reserveBytes - the caller's worst-case reservation size
** @return   A reserved node for the caller's file when this call did the
**           coordinating (caller writes into it), or nullptr for a coalesced
**           waiter (caller simply retries RegistryReserveNew in its own loop,
**           which now succeeds -- the drive has been swept).
*/
PRegistryFileNode RelieveSpacePressure(PSolveContext pCtx, int ti, int player,
                                        const char* path, char driveLetter, int64_t reserveBytes);

/*
** Function: IMergeRunSession
** @brief    The actual cross-drive merge work for ONE color: gather every
**           currently-unreserved file for that color across all NVMe drives,
**           capped to what a medium drive currently has room for (so one
**           sweep can never balloon into a multi-hour merge that stalls
**           every later flush -- see the cap comment in the .cpp), move
**           (single file) or k-way merge (multiple) to that medium drive
**           (fallback: store drive if no medium drive has room even for the
**           first file), delete + deregister the sources. Anything left
**           ungathered because of the cap stays on disk, unreserved, for a
**           future relief round. Run on a pIMergePool worker thread by the
**           relief coordinator (both colors dispatched together); not a
**           standalone trigger.
** @param    pCtx   - solve context
** @param    player - which color this session handles
*/
void IMergeRunSession(PSolveContext pCtx, int player);
