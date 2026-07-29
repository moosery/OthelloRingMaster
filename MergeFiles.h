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
**   Adapted from an earlier solver implementation. Declarations only here
**   -- see MergeFiles.cpp for the implementation.
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

/*
** Function: StartConsolidationWorkers
** @brief    Queues CONSOLIDATION_POOL_THREADS persistent background-
**           consolidation worker threads for the level about to run.
** @details  Call once per level, right after terminateConsolidation is
**           reset to false. Each worker loops (sleep, sweep every writer
**           drive/color pair, sleep again) until terminateConsolidation is
**           set at the solve->merge transition.
** @param    pCtx - solve context
*/
void StartConsolidationWorkers(PSolveContext pCtx);

/*
** Function: FindConsolidationCandidate
** @brief    Locates writer file (writerIdx, player, idx), trying LZ4, then
**           plain-compressed, then uncompressed naming. Exposed (not static)
**           so StatsListener.cpp's --consol diagnostic can look up the exact
**           same real on-disk files/sizes TryConsolidatePair itself reasons
**           about, rather than re-deriving its own directory scan.
** @param    outPath   - out: full path, if found
** @param    outSize   - capacity of outPath
** @param    pCtx      - solve context
** @param    writerIdx - which writer drive/thread
** @param    player    - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @param    idx       - file index to look up
** @param    pFileSize - out: real on-disk byte size, if found
** @return   true if the file exists.
*/
bool FindConsolidationCandidate(char* outPath, size_t outSize, PSolveContext pCtx,
                                 int writerIdx, int player, int idx, uint64_t* pFileSize);
