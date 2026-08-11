/*
** Filename:  Checkpoint.h
**
** Purpose:
**   Declares the mid-level checkpoint API: deciding when a checkpoint is
**   due, performing the pause-time NVMe-side sequence (drain the merge-
**   writer/flusher/iMerge pools and the consolidation master, capture the
**   registry's naming counter + a full file integrity manifest, write the
**   checkpoint file, restart the consolidation master), and validating/
**   reading a checkpoint back at resume time (v1.0.0: registry-based --
**   see Registry.h).
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
** @brief    Runs the full pause-time checkpoint sequence: drains the merge-
**           writer/D2H pool, force-flushes every writer's leftover pool
**           data, drains the flusher pool and the iMerge pool, stops the
**           consolidation master (waits for its worker pool to idle too) --
**           at that point every real writer-drive file is finished and
**           unreserved -- captures the registry's per-drive naming counter
**           and a full integrity manifest directly from that now-quiescent
**           state, writes the checkpoint file, then restarts the
**           consolidation master for the remainder of the level (the level
**           isn't ending, only pausing). Clears checkpointRequestedNow and
**           resets the interval timer before returning. Called synchronously
**           from inside the GPU feeder's own read callback
**           (FeedBoardIntoBatch, LevelSolverThread.cpp) -- the read stream
**           is never unwound for this, so there is nothing to reseek or
**           re-decode on return; only a real terminateThreads shutdown ever
**           interrupts the stream.
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
** Function: LogDiskBoardCensus
** @brief    DIAGNOSTIC (v1.0.13). Logs the real board count durably on disk
**           right now (all writer/merge/store-merge dirs, all compression
**           tiers), per player + grand total, plus per-drive nextFileIdx and
**           the checkpoint record position. Call at CHECKPOINT-DONE and again
**           at RESUME-AT-POSITION (after skip-decode, before new GPU work) to
**           localize any lost boards: a drop between the two points implicates
**           the checkpoint/resume path; intact-then-short-at-level-end
**           implicates the resumed-solve consolidation / end-of-level merge.
** @param    pCtx  - solve context
** @param    label - short tag printed in the census header
*/
void LogDiskBoardCensus(PSolveContext pCtx, const char* label);

/*
** Function: ReadValidCheckpoint
** @brief    Reads and validates level's checkpoint file, if one exists.
** @details  Validation (all must pass, or this returns false):
**             - file exists, has the right magic, and is exactly sizeof(CheckpointStats)
**             - boardSize/level/numMergeWriters match the current run's config/resume level
**             - this step's OUTPUT sentinels are ABSENT: Level_(level+1)'s _complete and
**               _merging (iteration `level` writes Level_(level+1), so those -- not
**               Level_(level)'s -- mark this step done/mid-merge; the step must genuinely
**               still be in progress). Checking Level_(level)'s here was a real off-by-one
**               that rejected every checkpoint on restart, fixed v1.0.9.
**             - this step's INPUT is complete: Level_(level)'s _complete sentinel exists
**               (the data iteration `level` reads, written by the previous step)
**             - a real directory scan of every writer drive, cross-checked against the
**               checkpoint's own integrity manifest, agrees on all three counts: no
**               manifest file missing on disk, no real file on disk the manifest never
**               mentioned, and no size mismatch between the two -- any disagreement here
**               is a hard Fatal (not a false return), since it means the on-disk state no
**               longer matches what the checkpoint claims closely enough to trust it
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
