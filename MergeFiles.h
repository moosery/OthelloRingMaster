/*
** Filename:  MergeFiles.h
**
** Purpose:
**   Declares the k-way merge / cross-drive consolidation API used by the
**   merge-writer job and the end-of-level loop: FlushMergeWriterBuffer
**   (in-memory merge of one thread's accumulated GPU flush segments, streamed
**   to an RSF file on that thread's NVMe directory) and DoEndOfLevelMerge
**   (consolidates every remaining writer/intermediate-merge file into a
**   single sorted, deduped store file per player).
**
** Notes:
**   Promoted from OthelloLevelBlaster's MergeFiles.h. Declarations only here
**   -- the implementation (MergeFiles.cpp) lands in a later Phase 4 step; see
**   project_ring_layout_implementation_plan memory.
*/

#pragma once

/* Includes */
#include "LevelSolverThread.h"

/* Functions */

/*
** Function: FlushMergeWriterBuffer
** @brief    In-memory k-way merge of accumulated GPU flush segments for
**           merge-writer thread thdIdx. Streams the sorted+deduped result
**           directly to an RSF file on that thread's NVMe directory, then
**           resets the segment tracking.
** @details  Called by the merge-writer job when the buffer is full, and by
**           FlushAllMergeWriterBuffers at end of level.
** @param    thdIdx - the merge-writer thread whose buffer to flush
** @param    pCtx   - solve context
*/
void FlushMergeWriterBuffer(int thdIdx, PSolveContext pCtx);

/*
** Function: DoEndOfLevelMerge
** @brief    Consolidates every remaining writer file (NVMe) and intermediate
**           merge file (medium drives) into a single sorted, deduped store
**           file per player on the store drive.
** @details  Called from the main level loop after all merge-writer buffers
**           have been flushed.
** @param    pCtx - solve context
*/
void DoEndOfLevelMerge(PSolveContext pCtx);
