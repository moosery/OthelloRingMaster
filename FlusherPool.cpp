/*
** Filename:  FlusherPool.cpp
**
** Purpose:
**   Implements FlushMergeWriterBuffer (FlusherPool.h) -- the real disk-write
**   half of what used to be one combined function in MergeFiles.cpp. See
**   that header and project_writer_drive_registry_redesign memory for the
**   full design.
**
** Notes:
**   Adapted from the retired FlushMergeWriterBuffer -- the actual merge
**   mechanics (MergePoolToWriter) and naming conventions (RSFFileName.h)
**   are unchanged; what's new is registry-based reservation (instead of a
**   ticket) and dispatch onto a dedicated pool (instead of ad-hoc
**   std::thread spawn/join, and instead of running inline on the D2H
**   thread).
*/

/* Includes */
#include "FlusherPool.h"
#include "Registry.h"
#include "IMergePool.h"
#include "ConsolidationMaster.h"
#include "MergeFiles.h"
#include "DriveLedger.h"
#include "RSFFileName.h"
#include "Logger.h"
#include <windows.h>

/* Functions */

/*
** Function: buildWriterFilePath
** @brief    Builds a new writer file's path using the naming convention
**           matching the run's compression mode, mirroring what the retired
**           FlushMergeWriterBuffer did inline.
** @param    out       - out: built path
** @param    outSize   - capacity of out
** @param    pCtx      - solve context
** @param    ti        - writer drive index
** @param    player    - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @param    fileIdx   - naming index (RegistryNextFileIdx)
*/
static void buildWriterFilePath(char* out, size_t outSize, PSolveContext pCtx,
                                 int ti, int player, int fileIdx)
{
    POthelloRingMasterState pSt       = pCtx->pState;
    bool                    compressMW = (pCtx->pConfig->compressMode == COMPRESS_ALL);
    char                    mwDL       = pSt->mwDirectory[ti][0];
    bool                    lz4MW      = compressMW && pCtx->pConfig->lz4Drives[0]
                                       && (strchr(pCtx->pConfig->lz4Drives, mwDL) != nullptr);

    if (lz4MW)
        RSFZLNameWriterFile(out, outSize, pSt->mwDirectory[ti], player, fileIdx);
    else if (compressMW)
        RSFZNameWriterFile(out, outSize, pSt->mwDirectory[ti], player, fileIdx);
    else
        RSFNameWriterFile(out, outSize, pSt->mwDirectory[ti], player, fileIdx);
}

/*
** Function: FlushOneColor
** @brief    Writes one color's currently-accumulated pool for writer ti to
**           a real, registry-tracked file. If space is short it drives a
**           coordinated space-relief event (RelieveSpacePressure) and retries,
**           and it gates on any relief already in progress before starting --
**           see IMergePool.h.
** @param    pCtx   - solve context
** @param    ti     - writer drive index
** @param    player - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
*/
static void FlushOneColor(PSolveContext pCtx, int ti, int player)
{
    POthelloRingMasterState pSt   = pCtx->pState;
    int                     level = (int)pSt->playLevel;
    char                    driveLetter = pSt->mwDirectory[ti][0];
    bool                    compressMW  = (pCtx->pConfig->compressMode == COMPRESS_ALL);

    int      segCount    = (player == RSF_PLAYER_BLACK) ? pSt->mwBlackSegCount[ti]      : pSt->mwWhiteSegCount[ti];
    size_t*  segOffsets  = (player == RSF_PLAYER_BLACK) ? pSt->mwBlackSegOffset[ti]      : pSt->mwWhiteSegOffset[ti];
    size_t*  segSizes    = (player == RSF_PLAYER_BLACK) ? pSt->mwBlackSegSize[ti]        : pSt->mwWhiteSegSize[ti];
    int*     segBoards   = (player == RSF_PLAYER_BLACK) ? pSt->mwBlackSegBoardCount[ti]  : pSt->mwWhiteSegBoardCount[ti];
    size_t   compBytesUsed = (player == RSF_PLAYER_BLACK) ? pSt->mwBlackCompBytesUsed[ti] : pSt->mwWhiteCompBytesUsed[ti];
    int      stagingCount  = (player == RSF_PLAYER_BLACK) ? pSt->mwBlackStagingCount[ti]  : pSt->mwWhiteStagingCount[ti];

    uint8_t*     mwBuf   = (uint8_t*)pSt->pMWBuffer[ti];
    const UINT64_PAIR* stagingBegin = (player == RSF_PLAYER_BLACK)
        ? (const UINT64_PAIR*)mwBuf
        : (const UINT64_PAIR*)(mwBuf + pSt->mwBufferSize - pSt->mwStagingSize);

    /* Worst-case output size: already-compressed segment bytes (real, known)
    ** plus staging boards at their raw uncompressed size (compression can
    ** only shrink this further, never required to be reserved beyond it) --
    ** same "reserve pessimistic, reclaim the gap after" pattern already
    ** established for consolidation/iMerge new-output reservations.
    */
    int64_t reserveBytes = (int64_t)compBytesUsed
                          + (int64_t)stagingCount * (int64_t)sizeof(UINT64_PAIR)
                          + (int64_t)sizeof(RSFTrailer);

    int      fileIdx = RegistryNextFileIdx(pSt, ti);
    char     path[MAX_FULL_PATH_NAME];
    buildWriterFilePath(path, sizeof(path), pCtx, ti, player, fileIdx);

    /* Gate: if a space-relief event is in progress, wait for it before even
    ** attempting to reserve -- otherwise this write could start mid-relief and
    ** keep the coordinator's drain from ever reaching zero (see
    ** SpaceReliefGateWait / the design memory's Finding 2). While gated, this
    ** flusher thread (and, upstream, its merge-writer thread and the GPU
    ** feeder) stall -- the intended, acceptable pause for a relief's duration.
    */
    SpaceReliefGateWait(pCtx);

    PRegistryFileNode pNode = nullptr;
    while (!pNode)
    {
        pNode = RegistryReserveNew(pSt, ti, player, REGISTRY_RESERVED_FLUSH,
                                    path, driveLetter, reserveBytes);
        if (!pNode)
            pNode = RelieveSpacePressure(pCtx, ti, player, path, driveLetter, reserveBytes);
    }

    /* Count this as an active WRITE for its whole disk-holding duration. The
    ** relief coordinator drains activeFlushWriters to zero before a sweep
    ** (Finding 1) -- bracketed around the real write/finish only, never the
    ** reserve/relief wait above, so a flush blocked in relief can't wedge the
    ** drain by waiting on itself. Decremented on every exit path below.
    */
    InterlockedIncrement((volatile LONG*)&pSt->activeFlushWriters);

    RSFWriter* pw = compressMW ? RSFWriterOpenZ(path) : RSFWriterOpen(path);
    MergePoolToWriter(pw, mwBuf, segCount, segOffsets, segSizes, segBoards,
                       stagingBegin, stagingCount,
                       &pSt->terminateThreads, &pSt->mwFlushDoneBytes[ti][player]);

    uint64_t fileBytes = 0;
    uint64_t unique     = RSFWriterClose(pw, &fileBytes);

    if (unique == 0)
    {
        /* Genuinely empty result -- no real file to keep. */
        DeleteFileA(path);
        RegistryAbandonNew(pSt, ti, pNode);
        DriveReclaim(pSt, driveLetter, reserveBytes);
        InterlockedDecrement((volatile LONG*)&pSt->activeFlushWriters);
        return;
    }

    RegistryFinishNew(pSt, ti, pNode, (int64_t)fileBytes);
    DriveReclaim(pSt, driveLetter, reserveBytes - (int64_t)fileBytes);

    uint64_t uncompressedBytes = unique * sizeof(UINT64_PAIR) + sizeof(RSFTrailer);
    InterlockedAdd64((volatile LONG64*)&pSt->levelStats[level].boardsWrittenToDisk, (LONG64)unique);
    InterlockedAdd64((volatile LONG64*)&pSt->levelStats[level].mwFilesCreated,      1);
    InterlockedAdd64((volatile LONG64*)&pSt->levelStats[level].mwBytes,             (LONG64)fileBytes);

    pSt->writerDriveStats[ti].levelFilesWritten      += 1;
    pSt->writerDriveStats[ti].levelBytesWritten      += fileBytes;
    pSt->writerDriveStats[ti].levelBytesUncompressed += uncompressedBytes;

    ConsolidationMasterWake(pCtx);
    InterlockedDecrement((volatile LONG*)&pSt->activeFlushWriters);
}

/*
** Function: FlushMergeWriterBuffer
** @brief    See FlusherPool.h.
*/
void FlushMergeWriterBuffer(int ti, PSolveContext pCtx)
{
    POthelloRingMasterState pSt = pCtx->pState;

    bool hasBlack = pSt->mwBlackSegCount[ti] > 0 || pSt->mwBlackStagingCount[ti] > 0;
    bool hasWhite = pSt->mwWhiteSegCount[ti] > 0 || pSt->mwWhiteStagingCount[ti] > 0;
    if (!hasBlack && !hasWhite) return;

    /* Publish total input boards (per color) before starting so the stats
    ** thread can show live progress for this writer's buffer-full NVMe spill.
    */
    {
        uint64_t blackBoards = (uint64_t)pSt->mwBlackStagingCount[ti];
        for (int s = 0; s < pSt->mwBlackSegCount[ti]; s++)
            blackBoards += (uint64_t)pSt->mwBlackSegBoardCount[ti][s];
        uint64_t whiteBoards = (uint64_t)pSt->mwWhiteStagingCount[ti];
        for (int s = 0; s < pSt->mwWhiteSegCount[ti]; s++)
            whiteBoards += (uint64_t)pSt->mwWhiteSegBoardCount[ti][s];

        pSt->mwFlushTotalBytes[ti][RSF_PLAYER_BLACK] = (int64_t)(blackBoards * sizeof(UINT64_PAIR));
        pSt->mwFlushTotalBytes[ti][RSF_PLAYER_WHITE] = (int64_t)(whiteBoards * sizeof(UINT64_PAIR));
        pSt->mwFlushDoneBytes[ti][RSF_PLAYER_BLACK]  = 0;
        pSt->mwFlushDoneBytes[ti][RSF_PLAYER_WHITE]  = 0;
        pSt->mwFlushStartTickMs[ti][RSF_PLAYER_BLACK] = GetTickCount64();
        pSt->mwFlushStartTickMs[ti][RSF_PLAYER_WHITE] = GetTickCount64();
        pSt->mwFlushActive[ti][RSF_PLAYER_BLACK] = hasBlack ? 1 : 0;
        pSt->mwFlushActive[ti][RSF_PLAYER_WHITE] = hasWhite ? 1 : 0;
    }

    /* Dispatch both colors to the flusher pool concurrently, wait for both.
    ** Manual-reset events, pre-signaled for a color with nothing to do, so
    ** WaitForMultipleObjects(..., TRUE, ...) always waits on exactly two
    ** real handles regardless of which color(s) are actually present.
    */
    HANDLE events[2];
    events[RSF_PLAYER_BLACK] = CreateEventA(nullptr, TRUE, hasBlack ? FALSE : TRUE, nullptr);
    events[RSF_PLAYER_WHITE] = CreateEventA(nullptr, TRUE, hasWhite ? FALSE : TRUE, nullptr);

    if (hasBlack)
        pSt->pFlusherPool->QueueJob([pCtx, ti, &events](uint32_t)
            { FlushOneColor(pCtx, ti, RSF_PLAYER_BLACK); SetEvent(events[RSF_PLAYER_BLACK]); });
    if (hasWhite)
        pSt->pFlusherPool->QueueJob([pCtx, ti, &events](uint32_t)
            { FlushOneColor(pCtx, ti, RSF_PLAYER_WHITE); SetEvent(events[RSF_PLAYER_WHITE]); });

    WaitForMultipleObjects(2, events, TRUE, INFINITE);
    CloseHandle(events[RSF_PLAYER_BLACK]);
    CloseHandle(events[RSF_PLAYER_WHITE]);

    pSt->mwFlushActive[ti][RSF_PLAYER_BLACK] = 0;
    pSt->mwFlushActive[ti][RSF_PLAYER_WHITE] = 0;

    /* Reset all pool and staging state for this thread -- safe now, both
    ** dispatched jobs have finished reading out of mwBuf by the time
    ** WaitForMultipleObjects returns.
    */
    pSt->mwBlackSegCount[ti]      = 0;
    pSt->mwBlackCompBytesUsed[ti] = 0;
    pSt->mwBlackStagingCount[ti]  = 0;
    pSt->mwWhiteSegCount[ti]      = 0;
    pSt->mwWhiteCompBytesUsed[ti] = 0;
    pSt->mwWhiteStagingCount[ti]  = 0;
}
