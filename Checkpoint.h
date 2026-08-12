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
**
**   Checkpoint design (2026-08, replaces the earlier file-manifest scheme --
**   see OthelloTypes.h's CheckpointStats for the full rationale): the
**   checkpoint records only the feeder position + a last-record cross-check
**   value + the level's cumulative counters. It records NO file names, sizes,
**   or index counters. Restart trusts the disk: it validates every data file
**   has an intact trailer (crash-partial files handled by
**   ValidateCheckpointFilesOnDisk), and seeds every work dir's next-file index
**   from a disk scan (CheckpointSeedCountersFromDisk) so resumed writes land
**   above everything already on disk and can never overwrite a pre-checkpoint
**   file. Cheap to take (just a flush/drain, no data movement) and robust to
**   the fast drives being a live work area that consolidation keeps churning.
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
** @param    lastRecordHi            - BOARD_KEY.ullCellsInUse of the last record consumed (cross-check on restart)
** @param    lastRecordLo            - BOARD_KEY.ullCellColors of the last record consumed
** @param    haveLastRecord         - false if no record was read this sub-pass yet (lastRecord* ignored)
*/
void PerformMidLevelCheckpoint(PSolveContext pCtx, int activeSubPass, uint64_t recordsConsumedInSubPass,
                               uint64_t lastRecordHi, uint64_t lastRecordLo, bool haveLastRecord);

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
**           A false return means "treat this exactly like no checkpoint exists" --
**           the caller falls back to today's behavior (full writer-dir wipe,
**           fresh level start) with no partial/best-effort recovery attempted.
**           NOTE: this only reads/validates the checkpoint PAYLOAD. The on-disk
**           data files are validated separately by ValidateCheckpointFilesOnDisk
**           (trailer completeness), and index counters are seeded by
**           CheckpointSeedCountersFromDisk -- both called from InitSolver.cpp.
** @param    pCtx  - solve context (pConfig/pState only need to be populated so far;
**                   pMachineInfo is unused by this check)
** @param    level - the level to check (the resume level)
** @param    out   - out: filled with the checkpoint's contents, valid only if this returns true
** @return   true if a valid, trustworthy checkpoint exists for this level.
*/
bool ReadValidCheckpoint(PSolveContext pCtx, int level, CheckpointStats* out);

/*
** Function: ValidateCheckpointFilesOnDisk
** @brief    Restart-time integrity pass: scans every real data file across all
**           work dirs (writer dirs, medium/merge dirs, and the store-merge
**           fallback dir) and confirms each has an intact trailer. A file with
**           no valid trailer is a crash-partial write (the trailer magic is
**           written last) -- provably post-checkpoint debris, since the
**           checkpoint was only taken after a full flush/drain, so nothing was
**           mid-write at the checkpoint position. Handling of such files:
**             - forceRestart == true  -> delete them (logged individually), then continue
**             - forceRestart == false -> log the full list and Fatal, so the user
**               can inspect before anything is removed
**           Complete (trailer-valid) files are always kept -- there is no file
**           manifest to compare against; the design trusts trailer completeness
**           plus disk-scan index seeding (see CheckpointSeedCountersFromDisk).
** @param    pCtx        - solve context (pState dirs must be populated)
** @param    level       - the resume level (selects which imerge_L{level}_* files to check)
** @param    forceRestart - pConfig->forceRestart; delete-vs-Fatal on partial files
*/
void ValidateCheckpointFilesOnDisk(PSolveContext pCtx, int level, bool forceRestart);

/*
** Function: CheckpointSeedCountersFromDisk
** @brief    Restart-time index seeding: for every work dir, sets the next
**           file-index counter to (highest index actually on disk + 1), so
**           resumed writes always land ABOVE every existing file and can never
**           overwrite a pre-checkpoint file -- even one that post-checkpoint
**           consolidation moved to a higher index before the crash. Seeds the
**           per-writer-drive nextFileIdx (shared across colors), the per-merge-
**           dir mergeFile{Black,White}Count, and the store-merge fallback
**           storeMerge{Black,White}FileCount. This is the crux of the design:
**           trust what's on disk, never a recorded index.
** @param    pCtx  - solve context (pState dirs/counters populated)
** @param    level - the resume level (imerge files are named imerge_L{level}_*)
*/
void CheckpointSeedCountersFromDisk(PSolveContext pCtx, int level);

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
