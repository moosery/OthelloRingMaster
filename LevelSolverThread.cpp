/*
** Filename:  LevelSolverThread.cpp
**
** Purpose:
**   Implements the two thread-pool jobs declared in LevelSolverThread.h:
**   the merge-writer job (RunMergeWriterJob, D2H + compress a completed GPU
**   flush into the per-thread pool buffer) and the GPU feeder job
**   (RunGpuFeederJob, the real per-level driver loop: reads a level's store
**   files in two sub-passes, black then white, and feeds batches to
**   GpuKernels). EnumerateStoreFilesForLevel and FlushAccumulator are private
**   helpers used only by these two jobs.
**
** Notes:
**   Promoted from OthelloLevelBlaster's LevelSolverThread.cpp. Renamed
**   BOARD_KEY_DISK -> UINT64_PAIR, BLF* -> RSF*, BlasterFileTrailer ->
**   RSFTrailer, BlasterFileName.h -> RSFFileName.h. Logic and choreography
**   unchanged -- this file never interprets board bits, it only moves opaque
**   UINT64_PAIR records between file/GPU/pool buffer.
*/

/* Includes */
#include "LevelSolverThread.h"
#include "DriveLedger.h"
#include "MergeFiles.h"
#include "RSFFileName.h"
#include "OthelloBasics.h"
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
** Function: EnumerateStoreFilesForLevel
** @brief    Enumerates RSF store files for a given level and player,
**           trying plain/.rsfz/.rsfzl in turn (whichever tier this run used).
** @param    storeDir     - store directory to search
** @param    boardSize    - board size
** @param    level        - level to enumerate
** @param    player       - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @param    pOutPaths    - out: array of newly-allocated path strings (caller frees each)
** @param    maxFiles     - capacity of pOutPaths
** @return   Number of files found.
*/
static int EnumerateStoreFilesForLevel(const char* storeDir, int boardSize,
                                        int level, int player,
                                        char** pOutPaths, int maxFiles)
{
    char pattern[MAX_FULL_PATH_NAME];
    RSFPatternStoreFiles(pattern, sizeof(pattern), storeDir, boardSize, level, player);

    /* Extract directory component from the pattern */
    char dir[MAX_FULL_PATH_NAME];
    strncpy_s(dir, sizeof(dir), pattern, _TRUNCATE);
    char* lastSlash = strrchr(dir, '\\');
    if (!lastSlash) return 0;
    *lastSlash = '\0';

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        RSFZPatternStoreFiles(pattern, sizeof(pattern), storeDir, boardSize, level, player);
        h = FindFirstFileA(pattern, &fd);
    }
    if (h == INVALID_HANDLE_VALUE)
    {
        RSFZLPatternStoreFiles(pattern, sizeof(pattern), storeDir, boardSize, level, player);
        h = FindFirstFileA(pattern, &fd);
    }
    if (h == INVALID_HANDLE_VALUE) return 0;

    int count = 0;
    do
    {
        if (count >= maxFiles) break;
        char full[MAX_FULL_PATH_NAME];
        snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
        pOutPaths[count] = (char*)MemMalloc("levelFilePath", strlen(full) + 1);
        if (!pOutPaths[count])
            Fatal(FATAL_ALLOCATION_FAILED, "EnumerateStoreFilesForLevel: cannot allocate path");
        strcpy(pOutPaths[count], full);
        count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return count;
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
** @brief    The per-level driver loop: enumerates a level's store files,
**           reads them in black-then-white sub-passes through a ping-pong
**           buffer, and feeds each batch to the GPU accumulator, flushing
**           whenever there isn't room for the next batch.
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

    static const int kMaxFiles = 65536;
    char** blackFiles = (char**)MemMalloc("blackFiles", (size_t)kMaxFiles * sizeof(char*));
    char** whiteFiles = (char**)MemMalloc("whiteFiles", (size_t)kMaxFiles * sizeof(char*));
    if (!blackFiles || !whiteFiles)
        Fatal(FATAL_ALLOCATION_FAILED, "GpuFeederJob: cannot allocate file lists");

    int numBlack = EnumerateStoreFilesForLevel(pSt->storeDirectory, boardSize, level,
                                               RSF_PLAYER_BLACK, blackFiles, kMaxFiles);
    int numWhite = EnumerateStoreFilesForLevel(pSt->storeDirectory, boardSize, level,
                                               RSF_PLAYER_WHITE, whiteFiles, kMaxFiles);

    /* Pre-scan for StatsListener solve-phase % progress */
    {
        uint64_t total = 0;
        for (int fi = 0; fi < numBlack; fi++)
        {
            RSFReader* r = RSFOpen(blackFiles[fi]);
            if (r) { total += RSFReaderTrailer(r)->recordCount; RSFClose(&r); }
        }
        for (int fi = 0; fi < numWhite; fi++)
        {
            RSFReader* r = RSFOpen(whiteFiles[fi]);
            if (r) { total += RSFReaderTrailer(r)->recordCount; RSFClose(&r); }
        }
        pSt->currentLevelTotalBoards = total;
    }

    int slotIdx = 0;

    /* Sub-pass 1: black-turn input boards -> playerBit = RSF_PLAYER_BLACK */
    for (int fi = 0; fi < numBlack && !pSt->terminateThreads; fi++)
    {
        RSFReader* r = RSFOpen(blackFiles[fi]);
        if (!r)
        {
            LoggerLog("GpuFeederJob: WARNING cannot open '%s', skipping\n", blackFiles[fi]);
            MemFree(blackFiles[fi]);
            continue;
        }

        while (!pSt->terminateThreads)
        {
            int got = RSFRead(r, slots[slotIdx], optBatch);
            if (got == 0) break;

            pSt->levelStats[pSt->playLevel].boardsReadFromStore += (uint64_t)got;

            if (!GpuAccumulatorHasRoom(pAccum, got))
                FlushAccumulator(pAccum, pCtx);

            GpuProcessBatch(pAccum, slots[slotIdx], got, RSF_PLAYER_BLACK);
            slotIdx = (slotIdx + 1) % PING_PONG_SLOTS;
        }

        RSFClose(&r);
        MemFree(blackFiles[fi]);
    }

    /* Sub-pass 2: white-turn input boards -> playerBit = RSF_PLAYER_WHITE */
    for (int fi = 0; fi < numWhite && !pSt->terminateThreads; fi++)
    {
        RSFReader* r = RSFOpen(whiteFiles[fi]);
        if (!r)
        {
            LoggerLog("GpuFeederJob: WARNING cannot open '%s', skipping\n", whiteFiles[fi]);
            MemFree(whiteFiles[fi]);
            continue;
        }

        while (!pSt->terminateThreads)
        {
            int got = RSFRead(r, slots[slotIdx], optBatch);
            if (got == 0) break;

            pSt->levelStats[pSt->playLevel].boardsReadFromStore += (uint64_t)got;

            if (!GpuAccumulatorHasRoom(pAccum, got))
                FlushAccumulator(pAccum, pCtx);

            GpuProcessBatch(pAccum, slots[slotIdx], got, RSF_PLAYER_WHITE);
            slotIdx = (slotIdx + 1) % PING_PONG_SLOTS;
        }

        RSFClose(&r);
        MemFree(whiteFiles[fi]);
    }

    if (!pSt->terminateThreads)
        FlushAccumulator(pAccum, pCtx);

    MemFree(blackFiles);
    MemFree(whiteFiles);
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
