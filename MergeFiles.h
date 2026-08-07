/*
** Filename:  MergeFiles.h
**
** Purpose:
**   Declares the merge MECHANICS shared across this project's writer-drive
**   pipeline: KWayMergeFiles (generic sorted-dedup k-way merge of N input
**   files to one output), MergePoolToWriter (merges an in-memory MW pool's
**   segments+staging into an open RSFWriter), and DoEndOfLevelMerge
**   (consolidates every remaining writer/intermediate-merge file into a
**   single sorted, deduped store file per player).
**
** Notes:
**   v1.0.0 (2026-08-06): the old FlushMergeWriterBuffer/StartConsolidationWorkers/
**   FindConsolidationCandidate entry points (ticket-based flush/consolidation
**   coordination) are retired -- see FlusherPool.h, ConsolidationMaster.h,
**   IMergePool.h for their replacements. KWayMergeFiles/MergePoolToWriter
**   were file-private (static) before this redesign; exported here
**   specifically so the new FlusherPool.cpp/IMergePool.cpp/
**   ConsolidationMaster.cpp can call them without duplicating merge logic --
**   see project_writer_drive_registry_redesign memory. Their own
**   implementation is untouched by this redesign, only their visibility
**   changed.
*/

#pragma once

/* Includes */
#include "LevelSolverThread.h"
#include "RSFFileName.h"
#include <vector>

/* Functions */

/*
** Function: KWayMergeFiles
** @brief    Generic sorted-dedup k-way merge: reads numInputs already-sorted
**           files, dedupes on the board key, writes the merged result to
**           outputPath. Used by the flusher pool (single-drive segment
**           flush... actually via MergePoolToWriter, not directly),
**           consolidation workers, and the iMerge pool for their real
**           merge I/O.
** @param    inputPaths     - files to merge
** @param    numInputs      - number of files in inputPaths
** @param    outputPath     - merged output path
** @param    pProgressBytes - out (optional): incremented by sizeof(UINT64_PAIR) per record popped
** @param    compressed     - true to open outputPath via RSFWriterOpenZ
** @param    pTerminate     - out-of-band cancellation flag, checked between pops
** @param    extraReaders   - already-open readers to merge in alongside inputPaths
** @return   Unique record count written to outputPath.
*/
uint64_t KWayMergeFiles(char** inputPaths, int numInputs, const char* outputPath,
                         volatile int64_t* pProgressBytes, bool compressed = false,
                         const volatile bool* pTerminate = nullptr,
                         const std::vector<RSFReader*>& extraReaders = {});

/*
** Function: MergePoolToWriter
** @brief    Merges all pool segments + optional uncompressed staging into
**           an open RSFWriter. Does NOT close pw; caller is responsible for
**           RSFWriterClose.
** @param    pw             - the open writer to merge into
** @param    mwBuf          - this thread's MW buffer (base for segOffsets)
** @param    segCount       - number of compressed pool segments
** @param    segOffsets     - byte offset of each segment within mwBuf
** @param    segSizes       - compressed byte size of each segment
** @param    segBoardCounts - record count of each segment
** @param    stagingBegin   - start of any live uncompressed staging (may be unused if stagingCount is 0)
** @param    stagingCount   - record count of live staging (0 = none)
** @param    pTerminate     - out-of-band cancellation flag, checked between pops
** @param    pProgressBytes - out (optional): atomically incremented (in 16 MB batches) as
**                            records are popped, so the stats thread can show live flush progress
*/
void MergePoolToWriter(
    RSFWriter* pw,
    uint8_t* mwBuf,
    int segCount, const size_t* segOffsets, const size_t* segSizes, const int* segBoardCounts,
    const UINT64_PAIR* stagingBegin, int stagingCount,
    const volatile bool* pTerminate,
    volatile int64_t* pProgressBytes = nullptr);

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
