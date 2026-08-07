/*
** Filename:  IMergePool.h
**
** Purpose:
**   Declares the cross-drive intermediate merge pool -- replaces
**   DoCrossDriveIntermediateMerge (MergeFiles.cpp, retired). Two dedicated
**   threads per trigger (one per color), so black and white sessions run
**   genuinely concurrently (no imergeCS-style single lock spanning both
**   colors). Triggered ONLY by real space pressure (DriveReserve/
**   DriveAvailable failure) -- no fanin/ticket-based trigger. See
**   project_writer_drive_registry_redesign memory for the full design.
**
** Notes:
**   A flush that can't get space for its new output calls
**   IMergeTriggerAndWait, which either starts a fresh session for that
**   color or, if one is already running, just waits on it
**   (OthelloRingMasterState.imergeSession[player]) -- multiple simultaneous
**   waiters is a real case (see that struct's own comment), not a rare
**   corner, so this is a true broadcast wait, not built assuming one caller.
*/

#pragma once

/* Includes */
#include "LevelSolverThread.h"

/* Functions */

/*
** Function: IMergeTriggerAndWait
** @brief    Ensures a cross-drive iMerge session is running (or already
**           running) for the given color, and blocks the caller until that
**           session completes. If a session for this color is already in
**           flight, does not start a redundant one -- just waits on the
**           existing session's completion (ImergeColorSession, OthelloTypes.h).
**           Otherwise dispatches a new session onto pIMergePool and waits
**           for it. Does NOT itself re-check whether space is now
**           sufficient after returning -- the caller (flush) is responsible
**           for re-checking and calling this again if still insufficient
**           (a simple retry loop, not a special case -- see design memory).
** @param    pCtx   - solve context
** @param    player - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE -- which color needs relief
*/
void IMergeTriggerAndWait(PSolveContext pCtx, int player);

/*
** Function: IMergeRunSession
** @brief    The actual cross-drive merge work for one color: reserve every
**           currently-unreserved file for that color across all NVMe
**           drives, move (single file) or merge (multiple) to the medium
**           drive (fallback: store drive if medium is full), delete
**           sources, unreserve, trigger the consolidation master. Runs on
**           a pIMergePool worker thread; not for direct external use
**           outside IMergePool.cpp/IMergeTriggerAndWait -- exposed here
**           only so IMergePool.cpp's dispatch site and this declaration
**           stay in the same header pair as everything else in this file's
**           public surface.
** @param    pCtx   - solve context
** @param    player - which color this session handles
*/
void IMergeRunSession(PSolveContext pCtx, int player);
