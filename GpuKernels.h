/*
** Filename:  GpuKernels.h
**
** Purpose:
**   Declares the GPU accumulator: the per-level expansion/dedup pipeline
**   that takes batches of ring-ordered UINT64_PAIR board records, expands
**   each into its legal children, deduplicates them, and makes the unique
**   results available for the CPU-side merge writer to read back.
**
** Notes:
**   Promoted from OthelloLevelBlaster's GpuKernels.h/.cu. Renamed
**   BOARD_KEY_DISK -> UINT64_PAIR (see Utility/RingStoreFile.h) throughout;
**   playerBit values are 1=black, 0=white (see RSFFileName.h's
**   RSF_PLAYER_BLACK/RSF_PLAYER_WHITE). The one real adaptation beyond
**   renaming is internal to ExpandKernel (GpuKernels.cu) -- see that file's
**   own header comment for the ring<->row-major boundary conversion detail.
**
**   GpuAccumulatorCreate calls GpuKernels_InitRingPermutationTables()
**   internally so callers never have to remember it -- see that function's
**   own comment for why a second, GpuKernels.cu-local init call is needed
**   even though OthelloRingMaster.cpp already calls
**   OBCuda_InitRingPermutationTables() once at startup.
*/

#pragma once

/* Includes */
#include "RingStoreFile.h"   /* UINT64_PAIR */

/* Structures and Types */

/*
** Type:    GpuAccumulator
** @brief   Opaque GPU accumulator handle -- full definition is internal to
**          GpuKernels.cu.
*/
struct __GpuAccumulator;
typedef struct __GpuAccumulator GpuAccumulator;

/* Functions */

/*
** Function: GpuAccumulatorCreate
** @brief    Allocates device memory (80% of totalGpuBytes) and creates a new accumulator.
** @param    batchSize        - boards per H2D batch
** @param    maxMovesPerBoard - worst-case children per board, for capacity checks
** @param    totalGpuBytes    - total GPU memory budget to size the accumulator from
** @return   A new GpuAccumulator.
*/
GpuAccumulator* GpuAccumulatorCreate(int batchSize, int maxMovesPerBoard, size_t totalGpuBytes);

/*
** Function: GpuAccumulatorDestroy
** @brief    Frees all device/pinned-host memory owned by pAccum.
** @param    pAccum - the accumulator to destroy
*/
void GpuAccumulatorDestroy(GpuAccumulator* pAccum);

/*
** Function: GpuAccumulatorHasRoom
** @brief    Checks (pessimistically -- assumes every board produces
**           maxMovesPerBoard children) whether pAccum has room for another batch.
** @param    pAccum         - the accumulator to check
** @param    nextBatchCount - number of boards in the next batch
** @return   true if the batch is safe to process without overflowing the accumulator.
*/
bool GpuAccumulatorHasRoom(const GpuAccumulator* pAccum, int nextBatchCount);

/*
** Function: GpuProcessBatch
** @brief    H2D-copies count boards, expands each (move generation, flip,
**           canonicalize), and scatters unique-so-far children directly into
**           the accumulator's two-stack layout. Caller must ensure
**           GpuAccumulatorHasRoom() first.
** @param    pAccum     - the accumulator to expand into
** @param    pBoards    - ring-ordered board records to expand
** @param    count      - number of records in pBoards
** @param    playerBit  - 1=black, 0=white -- next player to move for this whole batch
*/
void GpuProcessBatch(GpuAccumulator* pAccum, const UINT64_PAIR* pBoards,
                     int count, uint8_t playerBit);

/*
** Function: GpuFlushPrepare
** @brief    Syncs the stream and sorts+dedups both stack regions on device.
** @details  After this call, GpuFlushBlackCount()/GpuFlushWhiteCount() are valid.
** @param    pAccum - the accumulator to flush
** @return   Total unique board count (black + white); 0 if nothing to flush.
*/
int GpuFlushPrepare(GpuAccumulator* pAccum);

/*
** Function: GpuFlushRead
** @brief    D2H-copies a chunk of the sorted+deduped result for one player.
** @details  GpuFlushPrepare must have been called first.
** @param    pAccum   - the accumulator to read from
** @param    player   - 1=black (reads the first uniqueBlackCount entries), 0=white (reads the rest)
** @param    offset   - starting index within that player's unique results
** @param    pOut     - out: destination buffer, at least maxCount records
** @param    maxCount - maximum number of records to read
** @return   Count actually copied.
*/
int GpuFlushRead(GpuAccumulator* pAccum, int player, size_t offset,
                 UINT64_PAIR* pOut, int maxCount);

/*
** Function: GpuFlushBlackCount
** @brief    Returns the unique black board count from the last GpuFlushPrepare.
** @param    pAccum - the accumulator to query
** @return   Unique black board count.
*/
int GpuFlushBlackCount(const GpuAccumulator* pAccum);

/*
** Function: GpuFlushWhiteCount
** @brief    Returns the unique white board count from the last GpuFlushPrepare.
** @param    pAccum - the accumulator to query
** @return   Unique white board count.
*/
int GpuFlushWhiteCount(const GpuAccumulator* pAccum);

/*
** Function: GpuFlushReset
** @brief    Resets the accumulator for the next accumulation window. Must be
**           called after all GpuFlushRead calls for a flush are done.
** @param    pAccum - the accumulator to reset
*/
void GpuFlushReset(GpuAccumulator* pAccum);

/*
** Function: GpuAccumulatorWriteOffset
** @brief    Returns the total raw boards accumulated since the last GpuFlushReset
**           (pre-dedup; both stacks combined).
** @param    pAccum - the accumulator to query
** @return   Total raw board count.
*/
size_t GpuAccumulatorWriteOffset(const GpuAccumulator* pAccum);

/*
** Function: GpuFlushPassBoards
** @brief    Returns the pass-board count accumulated in the current flush window.
** @param    pAccum - the accumulator to query
** @return   Pass-board count. Valid only after GpuFlushPrepare; zeroed by GpuFlushReset.
*/
uint32_t GpuFlushPassBoards(const GpuAccumulator* pAccum);

/*
** Function: GpuFlushTermBoards
** @brief    Returns the terminal-board count accumulated in the current flush window.
** @param    pAccum - the accumulator to query
** @return   Terminal-board count. Valid only after GpuFlushPrepare; zeroed by GpuFlushReset.
*/
uint32_t GpuFlushTermBoards(const GpuAccumulator* pAccum);

/*
** Function: GpuFlushMaxMoves
** @brief    Returns the maximum child count produced by any single board in
**           the current flush window.
** @param    pAccum - the accumulator to query
** @return   Max moves seen. Valid only after GpuFlushPrepare; zeroed by GpuFlushReset.
*/
uint32_t GpuFlushMaxMoves(const GpuAccumulator* pAccum);
