/*
** Filename:  Checkpoint.h
**
** Purpose:
**   Declares the mid-level checkpoint API: deciding when a checkpoint is
**   due, performing the pause-time NVMe-side sequence (force-flush merge-
**   writer buffers, abort in-flight consolidation, snapshot writer-dir
**   ticket state, write the checkpoint file, restart consolidation
**   workers), and validating/reading a checkpoint back at resume time.
**
** Notes:
**   Deliberately does not know about the GPU accumulator or ping-pong
**   batching -- that's LevelSolverThread.cpp's domain. This file only
**   handles what happens once the feeder has already stopped pulling new
**   records and flushed everything through to the merge-writer pool
**   buffer; the caller is responsible for flushing the GPU accumulator
**   (FlushAccumulator) before calling PerformMidLevelCheckpoint, and for
**   resuming the input stream (with skip-N-records) afterward.
*/

#pragma once

/* Includes */
#include "LevelSolverThread.h"

/* Functions */

/*
** Function: CheckpointDueNow
** @brief    True if a mid-level checkpoint should be taken right now --
**           either the configured interval has elapsed since the last one,
**           or an on-demand CHECKPT request is pending. Cheap; safe to call
**           on every board fed to the GPU accumulator.
** @param    pCtx - solve context
** @return   true if PerformMidLevelCheckpoint should be invoked now.
*/
bool CheckpointDueNow(PSolveContext pCtx);

/*
** Function: PerformMidLevelCheckpoint
** @brief    Runs the full pause-time checkpoint sequence: force-flushes
**           every merge-writer buffer to real NVMe files, aborts any
**           in-flight background consolidation (same terminateConsolidation
**           + WaitForPoolIdle mechanism used at the real solve->merge
**           transition), snapshots every (writer, color) ticket high-water
**           mark, writes the checkpoint file, then restarts consolidation
**           workers for the remainder of the level. Clears
**           checkpointPauseFlag/checkpointRequestedNow and resets the
**           interval timer before returning.
** @details Caller must have already flushed the GPU accumulator
**          (FlushAccumulator) before calling this -- by the time this
**          function starts, boardsReadFromStore must already equal exactly
**          what's durably reflected in the merge-writer pool buffer.
** @param    pCtx                    - solve context
** @param    activeSubPass           - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE, the sub-pass paused in
** @param    recordsConsumedInSubPass - position within activeSubPass's own input stream
*/
void PerformMidLevelCheckpoint(PSolveContext pCtx, int activeSubPass, uint64_t recordsConsumedInSubPass);

/*
** Function: ReadValidCheckpoint
** @brief    Reads and validates level's checkpoint file, if one exists.
** @details  Validation (all must pass, or this returns false):
**             - file exists, has the right magic, and is exactly sizeof(CheckpointStats)
**             - boardSize/level/numMergeWriters match the current run's config/resume level
**             - level's own _complete/_merging sentinels are ABSENT (this level must
**               genuinely still be in progress, never both a checkpoint and a
**               finished/mid-merge level at once)
**             - for level > 0, the PREVIOUS level's _complete sentinel exists
**             - for every (writerIdx, color) with a nonzero recorded mwNextFileIdx,
**               a real file actually exists at ticket (mwNextFileIdx - 1) --
**               confirms the writer-dir's on-disk state still matches what the
**               checkpoint claims, rather than trusting a possibly-stale file
**           A false return means "treat this exactly like no checkpoint exists" --
**           the caller falls back to today's behavior (full writer-dir wipe,
**           fresh level start) with no partial/best-effort recovery attempted.
** @param    pCtx  - solve context (pConfig/pState only need to be populated so far;
**                   pMachineInfo is unused by this check)
** @param    level - the level to check (the resume level)
** @param    out   - out: filled with the checkpoint's contents, valid only if this returns true
** @return   true if a valid, trustworthy checkpoint exists for this level.
*/
bool ReadValidCheckpoint(PSolveContext pCtx, int level, CheckpointStats* out);

/*
** Function: DeleteLevelCheckpoint
** @brief    Deletes a level's checkpoint file, if any -- called once that
**           level's _complete sentinel is written, since the checkpoint is
**           obsolete the moment the whole level finishes. Also called
**           defensively right before a fresh (non-resuming) level start, so
**           a stale checkpoint from an abandoned/corrupt prior attempt can
**           never be picked up by a later run of the same level.
** @param    pCtx  - solve context
** @param    level - the level whose checkpoint file to delete
*/
void DeleteLevelCheckpoint(PSolveContext pCtx, int level);
