/*
** Filename:  LevelSolverThread.h
**
** Purpose:
**   Declares the per-level solve pipeline's two thread-pool jobs: the GPU
**   feeder (reads a level's store files, drives GpuKernels expansion) and
**   the merge-writer (D2H-copies a completed GPU flush, compresses it into
**   the per-thread pool buffer). Also declares FlushDescriptor (handoff
**   between the two) and SolveContext (the config/state/machine-info bundle
**   passed everywhere instead of touching globals directly).
**
** Notes:
**   Adapted from an earlier solver implementation, renamed onto this
**   solution's own types: BOARD_KEY_DISK -> UINT64_PAIR, the old
**   record-file prefix -> RSF*, the old config/state names ->
**   OthelloRingMasterConfig/State. FlushDescriptor/SolveContext/
**   PSolveContext keep their original names -- nothing about them was
**   solution-specific to begin with.
*/

#pragma once

/* Includes */
#include "OthelloTypes.h"
#include "OthelloBasics.h"
#include "GpuKernels.h"
#include "GetMachineInfo.h"
#include <windows.h>

/* Macros and Defines */
#define PING_PONG_SLOTS 4

/* Structures and Types */

/*
** Type:    FlushDescriptor
** @brief   Passed from the GPU feeder to a merge-writer pool job. hDoneEvent
**          is signaled by the merge-writer after both D2H ops complete so the
**          GPU feeder can reset the accumulator and start the next batch
**          immediately.
*/
typedef struct __FlushDescriptor
{
    GpuAccumulator*  pAccum;
    int              blackCount;   /* unique black boards in this flush         */
    int              whiteCount;   /* unique white boards in this flush         */
    HANDLE           hDoneEvent;   /* auto-reset; signaled after D2H is done    */
} FlushDescriptor, * PFlushDescriptor;

/*
** Type:    SolveContext
** @brief   Bundles config/state/machine-info together, passed by pointer
**          everywhere instead of accessing globals directly from library code.
*/
typedef struct __SolveContext
{
    POthelloRingMasterConfig  pConfig;
    POthelloRingMasterState   pState;
    PMachineInfo              pMachineInfo;
} SolveContext, * PSolveContext;

/* Functions */

/*
** Function: SubmitGpuFeederJob
** @brief    Queues the GPU feeder job for one level onto the GPU feeder thread pool.
** @param    pCtx  - solve context
** @param    level - level to solve
*/
void SubmitGpuFeederJob(PSolveContext pCtx, uint8_t level);

/*
** Function: SubmitMergeWriterJob
** @brief    Queues a completed GPU flush onto the merge-writer thread pool for D2H+compression.
** @param    pCtx  - solve context
** @param    pDesc - the flush to process; freed by the queued job after it runs
*/
void SubmitMergeWriterJob(PSolveContext pCtx, PFlushDescriptor pDesc);

/*
** Function: FlushAllMergeWriterBuffers
** @brief    Safety-net flush for any uncompressed staging data still live on
**           any merge-writer thread. Normally a no-op -- every job either
**           compresses staging into the pool or flushes immediately.
** @param    pCtx - solve context
*/
void FlushAllMergeWriterBuffers(PSolveContext pCtx);
