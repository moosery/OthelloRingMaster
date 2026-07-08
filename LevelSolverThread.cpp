/*
** Filename:  LevelSolverThread.cpp
**
** Purpose:
**   Implements the two thread-pool jobs declared in LevelSolverThread.h:
**   the merge-writer job (RunMergeWriterJob, D2H + compress a completed GPU
**   flush into the per-thread pool buffer) and the GPU feeder job
**   (RunGpuFeederJob, the real per-level driver loop: expands a level's
**   ring nested-index store in two sub-passes, black then white, and feeds
**   batches to GpuKernels). FeedNestedIndexLevel/FeedBoardIntoBatch and
**   FlushAccumulator are private helpers used only by these two jobs.
**
** Notes:
**   Promoted from OthelloLevelBlaster's LevelSolverThread.cpp. Renamed
**   BOARD_KEY_DISK -> UINT64_PAIR, BLF* -> RSF*, BlasterFileTrailer ->
**   RSFTrailer, BlasterFileName.h -> RSFFileName.h. Merge-writer job logic
**   and choreography unchanged -- it never interprets board bits, it only
**   moves opaque UINT64_PAIR records between GPU/pool buffer.
**
**   RunGpuFeederJob's read side is NOT a straight port: Blaster (and this
**   project until now) read a level's input as flat RSF store files.
**   FeedNestedIndexLevel/FeedBoardIntoBatch are new, replacing the old
**   EnumerateStoreFilesForLevel + RSFOpen/RSFRead loop with
**   RingNestedIndexReader::Load/ExpandAll, since the store format is now
**   the 4-file ring nested index (see MergeFiles.cpp's
**   ConvertLevelOutputToNestedIndex, which is what produces it).
*/

/* Includes */
#include "LevelSolverThread.h"
#include "DriveLedger.h"
#include "MergeFiles.h"
#include "RSFFileName.h"
#include "OthelloBasics.h"
#include "RingNestedIndex.h"
#include "Logger.h"
#include "Mem.h"
#include <string.h>
#include <algorithm>
#include <windows.h>

/* Functions */

/*
** ============================================================
** Merge-writer pool job
**
** Receives a completed GPU flush (sorted+deduped black and white boards in
** d_gather). D2H copies each player region into the matching end of the
** thread's two-stack MW buffer, signals the GPU feeder so it can reset the
** accumulator, then checks whether the buffer needs to be flushed to disk.
** ============================================================
*/

/*
** Function: RunMergeWriterJob
** @brief    D2H-copies a completed GPU flush into this thread's MW buffer,
**           signals the feeder, then compresses each player's staging area
**           into the shared pool (or flushes to disk if there's no room).
** @param    thdIdx - this merge-writer thread's stable index (maps to buffer/dir)
** @param    pCtx   - solve context
** @param    pDesc  - the completed flush to process
*/
static void RunMergeWriterJob(uint32_t thdIdx, PSolveContext pCtx, PFlushDescriptor pDesc)
{
    POthelloRingMasterState pSt = pCtx->pState;
    const int  ti         = (int)thdIdx;
    const int  blackCount = pDesc->blackCount;
    const int  whiteCount = pDesc->whiteCount;

    uint8_t* mwBuf = (uint8_t*)pSt->pMWBuffer[ti];

    /* Fixed staging destinations: black at start of buffer, white at end.
    ** These addresses never change so the GPU always DMAs to the same place.
    */
    UINT64_PAIR* blackDest = (UINT64_PAIR*)mwBuf;
    UINT64_PAIR* whiteDest = (UINT64_PAIR*)(mwBuf + pSt->mwBufferSize - pSt->mwStagingSize);

    /* D2H both player regions into the fixed staging areas */
    if (blackCount > 0)
        GpuFlushRead(pDesc->pAccum, RSF_PLAYER_BLACK, 0, blackDest, blackCount);
    if (whiteCount > 0)
        GpuFlushRead(pDesc->pAccum, RSF_PLAYER_WHITE, 0, whiteDest, whiteCount);

    /* Signal the feeder as soon as D2H is done, not after compression. There
    ** is exactly one GPU feeder thread and it blocks on this event
    ** (WaitForSingleObject in FlushAccumulator) before generating more work,
    ** so only one flush is ever in flight system-wide -- no other job can
    ** touch this worker's staging buffer until this function returns. Gating
    ** the event on compression instead just serializes GPU compute behind
    ** single-threaded LZ4 work for no safety benefit.
    */
    SetEvent(pDesc->hDoneEvent);

    /* Mark staging areas as live (before compression so flush path can see them) */
    pSt->mwBlackStagingCount[ti] = blackCount;
    pSt->mwWhiteStagingCount[ti] = whiteCount;

    /* Compressed pool occupies the middle of mwBuf between the two staging
    ** areas. Black and white compressed segments both grow left-to-right
    ** into this pool.
    */
    const size_t poolBase = pSt->mwStagingSize;
    const size_t poolSize = pSt->mwBufferSize - 2 * pSt->mwStagingSize;

    bool bufferFull = false;

    /* Try to compress black staging into the pool. Worst-case: no LZ4
    ** compression gain + varint overhead + frame overhead + trailer.
    */
    if (blackCount > 0)
    {
        const size_t poolUsed = pSt->mwBlackCompBytesUsed[ti] + pSt->mwWhiteCompBytesUsed[ti];
        const size_t avail    = (poolUsed < poolSize) ? (poolSize - poolUsed) : 0;
        const size_t worst    = (size_t)blackCount * 20 + sizeof(RSFTrailer) + 100;
        if (avail >= worst && pSt->mwBlackSegCount[ti] < MAX_MW_SEGS)
        {
            uint64_t    compBytes = 0;
            RSFWriter*  pw = RSFWriterOpenZMem(mwBuf + poolBase + poolUsed, avail);
            for (int r = 0; r < blackCount; r++)
                RSFWriterRecord(pw, &blackDest[r]);
            RSFWriterClose(pw, &compBytes);

            int bs = pSt->mwBlackSegCount[ti]++;
            pSt->mwBlackSegOffset[ti][bs]     = poolBase + poolUsed;
            pSt->mwBlackSegSize[ti][bs]       = (size_t)compBytes;
            pSt->mwBlackSegBoardCount[ti][bs] = blackCount;
            pSt->mwBlackCompBytesUsed[ti]    += (size_t)compBytes;
            pSt->mwBlackStagingCount[ti]      = 0;   /* staging consumed into pool */
            if (pSt->mwBlackSegCount[ti] > pSt->mwBlackSegCountHighWater[ti])
                pSt->mwBlackSegCountHighWater[ti] = pSt->mwBlackSegCount[ti];
        }
        else
        {
            bufferFull = true;   /* staging stays live; flush will merge it uncompressed */
        }
    }

    /* Try to compress white staging (pool usage may have grown from black compression above) */
    if (whiteCount > 0)
    {
        const size_t poolUsed = pSt->mwBlackCompBytesUsed[ti] + pSt->mwWhiteCompBytesUsed[ti];
        const size_t avail    = (poolUsed < poolSize) ? (poolSize - poolUsed) : 0;
        const size_t worst    = (size_t)whiteCount * 20 + sizeof(RSFTrailer) + 100;
        if (avail >= worst && pSt->mwWhiteSegCount[ti] < MAX_MW_SEGS)
        {
            uint64_t    compBytes = 0;
            RSFWriter*  pw = RSFWriterOpenZMem(mwBuf + poolBase + poolUsed, avail);
            for (int r = 0; r < whiteCount; r++)
                RSFWriterRecord(pw, &whiteDest[r]);
            RSFWriterClose(pw, &compBytes);

            int ws = pSt->mwWhiteSegCount[ti]++;
            pSt->mwWhiteSegOffset[ti][ws]     = poolBase + poolUsed;
            pSt->mwWhiteSegSize[ti][ws]       = (size_t)compBytes;
            pSt->mwWhiteSegBoardCount[ti][ws] = whiteCount;
            pSt->mwWhiteCompBytesUsed[ti]    += (size_t)compBytes;
            pSt->mwWhiteStagingCount[ti]      = 0;   /* staging consumed into pool */
            if (pSt->mwWhiteSegCount[ti] > pSt->mwWhiteSegCountHighWater[ti])
                pSt->mwWhiteSegCountHighWater[ti] = pSt->mwWhiteSegCount[ti];
        }
        else
        {
            bufferFull = true;
        }
    }

    InterlockedAdd64((volatile LONG64*)&pSt->levelStats[pSt->playLevel].boardsReceivedFromGpu,
                     (LONG64)(blackCount + whiteCount));

    if (bufferFull)
        FlushMergeWriterBuffer(ti, pCtx);
}

/*
** Function: SubmitMergeWriterJob
** @brief    Queues a completed GPU flush onto the merge-writer thread pool for D2H+compression.
** @param    pCtx  - solve context
** @param    pDesc - the flush to process; freed after the job runs
*/
void SubmitMergeWriterJob(PSolveContext pCtx, PFlushDescriptor pDesc)
{
    pCtx->pState->pMergeWriterPool->QueueJob(
        [pCtx, pDesc](uint32_t thdIdx)
        {
            RunMergeWriterJob(thdIdx, pCtx, pDesc);
            MemFree(pDesc);
        }
    );
}

/*
** Function: FlushAllMergeWriterBuffers
** @brief    Safety-net flush for any truly uncompressed staging data that
**           survived past the pool going idle. Normally a no-op -- every job
**           either compresses staging into the pool (StagingCount -> 0) or
**           calls FlushMergeWriterBuffer immediately (also resets
**           StagingCount to 0). Pool segments are intentionally left in
**           memory for DoEndOfLevelMerge.
** @param    pCtx - solve context
*/
void FlushAllMergeWriterBuffers(PSolveContext pCtx)
{
    POthelloRingMasterState pSt = pCtx->pState;

    for (int ti = 0; ti < (int)pSt->numMergeWriters; ti++)
    {
        if (pSt->mwBlackStagingCount[ti] > 0 || pSt->mwWhiteStagingCount[ti] > 0)
            FlushMergeWriterBuffer(ti, pCtx);
    }
}

/*
** ============================================================
** Helpers used by the GPU feeder job
** ============================================================
*/

/*
** Type:    FeedBatchState
** @brief   Ping-pong batching state threaded through FeedBoardIntoBatch as
**          RingNestedIndexReader::ExpandAll walks one player's boards.
*/
struct FeedBatchState
{
    UINT64_PAIR*     slots[PING_PONG_SLOTS];
    int              slotIdx;
    int              count;      /* records currently buffered in slots[slotIdx] */
    int              optBatch;
    uint8_t          playerBit;
    GpuAccumulator*  pAccum;
    PSolveContext    pCtx;
};

/*
** Function: FeedBoardIntoBatch
** @brief    Appends one expanded board into the current ping-pong slot,
**           flushing a full batch to the GPU accumulator once optBatch is reached.
** @param    st  - the batching state to append into
** @param    key - the ring-ordered board to feed
*/
static void FeedBoardIntoBatch(FeedBatchState* st, const BOARD_KEY& key)
{
    POthelloRingMasterState pSt = st->pCtx->pState;
    if (pSt->terminateThreads) return;

    st->slots[st->slotIdx][st->count].hi = key.ullCellsInUse;
    st->slots[st->slotIdx][st->count].lo = key.ullCellColors;
    st->count++;
    pSt->levelStats[pSt->playLevel].boardsReadFromStore++;

    if (st->count < st->optBatch) return;

    if (!GpuAccumulatorHasRoom(st->pAccum, st->count))
        FlushAccumulator(st->pAccum, st->pCtx);

    GpuProcessBatch(st->pAccum, st->slots[st->slotIdx], st->count, st->playerBit);
    st->slotIdx = (st->slotIdx + 1) % PING_PONG_SLOTS;
    st->count   = 0;
}

/*
** Function: FeedNestedIndexLevel
** @brief    Loads one player's nested-index store for a level and feeds
**           every board through FeedBoardIntoBatch, flushing any leftover
**           partial batch at the end.
** @param    pCtx      - solve context
** @param    pAccum    - the GPU accumulator to feed
** @param    slots     - the ping-pong slot buffers (shared across both players)
** @param    pSlotIdx  - in/out: current slot index, carried across calls
** @param    optBatch  - records per full batch
** @param    boardSize - board size
** @param    level     - level to read
** @param    player    - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @param    playerBit - the same value as player, passed through to GpuProcessBatch
*/
static void FeedNestedIndexLevel(PSolveContext pCtx, GpuAccumulator* pAccum,
                                  UINT64_PAIR* const slots[PING_PONG_SLOTS], int* pSlotIdx,
                                  int optBatch, int boardSize, int level, int player, uint8_t playerBit)
{
    POthelloRingMasterState pSt = pCtx->pState;

    char cellsInUsePath[MAX_FULL_PATH_NAME];
    char ring1Path[MAX_FULL_PATH_NAME];
    char ring2Path[MAX_FULL_PATH_NAME];
    char ring34Path[MAX_FULL_PATH_NAME];
    RSFNameCellsInUseFile(cellsInUsePath, sizeof(cellsInUsePath), pSt->storeDirectory, boardSize, level, player, 0);
    RSFNameRing1File(ring1Path,           sizeof(ring1Path),      pSt->storeDirectory, boardSize, level, player, 0);
    RSFNameRing2File(ring2Path,           sizeof(ring2Path),      pSt->storeDirectory, boardSize, level, player, 0);
    RSFNameRing34File(ring34Path,         sizeof(ring34Path),     pSt->storeDirectory, boardSize, level, player, 0);

    RingNestedIndexReader reader;
    if (!reader.Load(cellsInUsePath, ring1Path, ring2Path, ring34Path))
    {
        LoggerLog("GpuFeederJob: no nested-index data for level %d %s, skipping\n",
                  level, RSFPlayerStr(player));
        return;
    }

    FeedBatchState st;
    for (int i = 0; i < PING_PONG_SLOTS; i++) st.slots[i] = slots[i];
    st.slotIdx   = *pSlotIdx;
    st.count     = 0;
    st.optBatch  = optBatch;
    st.playerBit = playerBit;
    st.pAccum    = pAccum;
    st.pCtx      = pCtx;

    reader.ExpandAll([&st](const BOARD_KEY& key) { FeedBoardIntoBatch(&st, key); });

    /* Flush whatever's left in the current partially-filled slot. */
    if (st.count > 0 && !pSt->terminateThreads)
    {
        if (!GpuAccumulatorHasRoom(pAccum, st.count))
            FlushAccumulator(pAccum, pCtx);
        GpuProcessBatch(pAccum, st.slots[st.slotIdx], st.count, playerBit);
        st.slotIdx = (st.slotIdx + 1) % PING_PONG_SLOTS;
    }

    *pSlotIdx = st.slotIdx;
}

/*
** Function: FlushAccumulator
** @brief    Prepares (sorts+dedups) the accumulator, updates level stats,
**           and -- if there's anything unique to write -- submits a
**           merge-writer job and blocks until its D2H copy is done before
**           resetting the accumulator for the next window.
** @param    pAccum - the accumulator to flush
** @param    pCtx   - solve context
*/
static void FlushAccumulator(GpuAccumulator* pAccum, PSolveContext pCtx)
{
    uint64_t generated = GpuAccumulatorWriteOffset(pAccum);
    pCtx->pState->levelStats[pCtx->pState->playLevel].boardsGenerated += generated;

    int uniqueCount = GpuFlushPrepare(pAccum);

    pCtx->pState->levelStats[pCtx->pState->playLevel].passBoards     += GpuFlushPassBoards(pAccum);
    pCtx->pState->levelStats[pCtx->pState->playLevel].terminalBoards += GpuFlushTermBoards(pAccum);
    uint32_t flushMax = GpuFlushMaxMoves(pAccum);
    if (flushMax > pCtx->pState->levelStats[pCtx->pState->playLevel].maxMovesInLevel)
        pCtx->pState->levelStats[pCtx->pState->playLevel].maxMovesInLevel = flushMax;

    if (uniqueCount == 0)
    {
        GpuFlushReset(pAccum);
        return;
    }
    pCtx->pState->levelStats[pCtx->pState->playLevel].gpuDupsRemoved +=
        generated - (uint64_t)uniqueCount;
    pCtx->pState->levelStats[pCtx->pState->playLevel].gpuFlushes++;

    PFlushDescriptor pDesc = (PFlushDescriptor)MemMalloc("FlushDescriptor",
                                                          sizeof(FlushDescriptor));
    if (!pDesc)
        Fatal(FATAL_ALLOCATION_FAILED, "FlushAccumulator: cannot allocate FlushDescriptor");

    pDesc->pAccum     = pAccum;
    pDesc->blackCount = GpuFlushBlackCount(pAccum);
    pDesc->whiteCount = GpuFlushWhiteCount(pAccum);
    pDesc->hDoneEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!pDesc->hDoneEvent)
        Fatal(FATAL_ALLOCATION_FAILED, "FlushAccumulator: cannot create done event");

    SubmitMergeWriterJob(pCtx, pDesc);

    WaitForSingleObject(pDesc->hDoneEvent, INFINITE);
    CloseHandle(pDesc->hDoneEvent);

    GpuFlushReset(pAccum);
}

/*
** ============================================================
** GPU feeder job
**
** Two sub-passes per level: first processes all black-turn input files
** (playerBit = RSF_PLAYER_BLACK), then all white-turn files
** (playerBit = RSF_PLAYER_WHITE). The MW buffer accumulates boards from
** both players concurrently via its two-stack layout, so no mid-level flush
** between sub-passes is required.
** ============================================================
*/

/*
** Function: RunGpuFeederJob
** @brief    The per-level driver loop: loads a level's nested-index store
**           for each player, expands it back into a flat board stream in
**           black-then-white sub-passes through a ping-pong buffer, and
**           feeds each batch to the GPU accumulator, flushing whenever
**           there isn't room for the next batch.
** @param    pCtx  - solve context
** @param    level - level to solve
*/
static void RunGpuFeederJob(uint32_t /*thdIdx*/, PSolveContext pCtx, uint8_t level)
{
    POthelloRingMasterConfig pCfg = pCtx->pConfig;
    POthelloRingMasterState  pSt  = pCtx->pState;
    PMachineInfo             pMI  = pCtx->pMachineInfo;

    const int    optBatch    = pMI->g_gpuInfo.optimalBatchSize;
    const int    maxMoves    = GetMaxMovesForBoardSize(pCfg->boardSize);
    const size_t totalGpuMem = pMI->g_gpuInfo.totalGlobalMemBytes;
    const int    boardSize   = (int)pCfg->boardSize;

    UINT64_PAIR* pingPong = (UINT64_PAIR*)pSt->pPingPongBuffer;
    UINT64_PAIR* slots[PING_PONG_SLOTS];
    for (int i = 0; i < PING_PONG_SLOTS; i++)
        slots[i] = pingPong + (size_t)i * (size_t)optBatch;

    GpuAccumulator* pAccum = GpuAccumulatorCreate(optBatch, maxMoves, totalGpuMem);

    /* Pre-scan for StatsListener solve-phase % progress -- GetBoardCount()
    ** reads straight off the already-loaded index, no extra I/O beyond the
    ** Load() FeedNestedIndexLevel does anyway below.
    */
    {
        uint64_t total = 0;
        for (int player = RSF_PLAYER_WHITE; player <= RSF_PLAYER_BLACK; player++)
        {
            char cellsInUsePath[MAX_FULL_PATH_NAME];
            char ring1Path[MAX_FULL_PATH_NAME];
            char ring2Path[MAX_FULL_PATH_NAME];
            char ring34Path[MAX_FULL_PATH_NAME];
            RSFNameCellsInUseFile(cellsInUsePath, sizeof(cellsInUsePath), pSt->storeDirectory, boardSize, level, player, 0);
            RSFNameRing1File(ring1Path,           sizeof(ring1Path),      pSt->storeDirectory, boardSize, level, player, 0);
            RSFNameRing2File(ring2Path,           sizeof(ring2Path),      pSt->storeDirectory, boardSize, level, player, 0);
            RSFNameRing34File(ring34Path,         sizeof(ring34Path),     pSt->storeDirectory, boardSize, level, player, 0);

            RingNestedIndexReader reader;
            if (reader.Load(cellsInUsePath, ring1Path, ring2Path, ring34Path))
                total += reader.GetBoardCount();
        }
        pSt->currentLevelTotalBoards = total;
    }

    int slotIdx = 0;

    /* Sub-pass 1: black-turn input boards -> playerBit = RSF_PLAYER_BLACK */
    if (!pSt->terminateThreads)
        FeedNestedIndexLevel(pCtx, pAccum, slots, &slotIdx, optBatch, boardSize,
                              level, RSF_PLAYER_BLACK, RSF_PLAYER_BLACK);

    /* Sub-pass 2: white-turn input boards -> playerBit = RSF_PLAYER_WHITE */
    if (!pSt->terminateThreads)
        FeedNestedIndexLevel(pCtx, pAccum, slots, &slotIdx, optBatch, boardSize,
                              level, RSF_PLAYER_WHITE, RSF_PLAYER_WHITE);

    if (!pSt->terminateThreads)
        FlushAccumulator(pAccum, pCtx);

    GpuAccumulatorDestroy(pAccum);
}

/*
** Function: SubmitGpuFeederJob
** @brief    Queues the GPU feeder job for one level onto the GPU feeder thread pool.
** @param    pCtx  - solve context
** @param    level - level to solve
*/
void SubmitGpuFeederJob(PSolveContext pCtx, uint8_t level)
{
    pCtx->pState->pGPUFeederThreadPool->QueueJob(
        [pCtx, level](uint32_t thdIdx)
        {
            RunGpuFeederJob(thdIdx, pCtx, level);
        }
    );
}
