/*
** Filename:  FlusherPool.h
**
** Purpose:
**   Declares the flusher pool entry point that replaces the old
**   FlushMergeWriterBuffer (MergeFiles.cpp, retired) -- writes one merge-
**   writer thread's accumulated in-memory pool (both colors, concurrently)
**   to real registry-tracked files on NVMe. Same name/signature as the
**   function it replaces, so existing call sites (RunMergeWriterJob's
**   buffer-full path, DoEndOfLevelMerge's leftover-pool force-flush) need no
**   changes beyond the include.
**
** Notes:
**   Real disk writes happen on dedicated pFlusherPool worker threads, not
**   the calling (D2H/compress) thread -- but the caller still blocks until
**   both colors finish, same as before, since the per-writer in-memory pool
**   buffers are shared memory that must not be reused while a dispatched
**   flush job is still reading out of them. See
**   project_writer_drive_registry_redesign memory for the full rationale.
*/

#pragma once

/* Includes */
#include "LevelSolverThread.h"

/* Functions */

/*
** Function: FlushMergeWriterBuffer
** @brief    Writes merge-writer thread ti's currently-accumulated pool data
**           (both colors, whatever is present -- not gated on "full"; the
**           caller decides when to call this) to real files. For each
**           color present: reserves a new registry file (driving a
**           coordinated space-relief event via RelieveSpacePressure if space
**           is short), merges the pool into
**           it (MergePoolToWriter/RSFWriter, unchanged mechanics), finishes
**           the registry node, updates stats, and wakes the consolidation
**           master. Blocks until both colors are done, then resets this
**           thread's pool/staging tracking.
** @param    ti   - the merge-writer thread whose pool to flush
** @param    pCtx - solve context
*/
void FlushMergeWriterBuffer(int ti, PSolveContext pCtx);
