/*
** Filename:  ConsolidationMaster.cpp
**
** Purpose:
**   Implements the single, event-driven consolidation master thread
**   (ConsolidationMaster.h) -- replaces the old 12-thread polling
**   consolidation pool (ConsolidationWorkerLoop/TryConsolidatePair,
**   MergeFiles.cpp, retired). See that header and
**   project_writer_drive_registry_redesign memory for the full design.
**
** Notes:
**   Adapted from the retired TryConsolidatePair -- the real merge mechanics
**   (KWayMergeFiles) are unchanged; what's new is registry-based candidate
**   selection (instead of a ticket-range scan with a low-water-mark hint)
**   and event-driven dispatch from one master thread (instead of 12 threads
**   independently polling every pair on a timer).
*/

/* Includes */
#include "ConsolidationMaster.h"
#include "Registry.h"
#include "MergeFiles.h"
#include "DriveLedger.h"
#include "RSFFileName.h"
#include "RingStoreFile.h"
#include "Logger.h"
#include "Mem.h"
#include <windows.h>
#include <mutex>

/* Functions */

/*
** Function: maxFileSizeBytes
** @brief    The real consolidation-eligibility cap for this run --
**           pConfig->maxFileSizeGBOverride if set (test runs), else the
**           production default CONSOLIDATION_SIZE_CAP_GB.
*/
static int64_t maxFileSizeBytes(PSolveContext pCtx)
{
    uint64_t gb = pCtx->pConfig->maxFileSizeGBOverride
                ? pCtx->pConfig->maxFileSizeGBOverride
                : CONSOLIDATION_SIZE_CAP_GB;
    return (int64_t)(gb * 1024ULL * 1024ULL * 1024ULL);
}

/*
** Function: buildConsolOutputPath
** @brief    Builds a new consolidation-merge output file's path on writer
**           drive wi, using a fresh registry naming index.
*/
static void buildConsolOutputPath(char* out, size_t outSize, PSolveContext pCtx,
                                   int wi, int player, int fileIdx)
{
    POthelloRingMasterState pSt        = pCtx->pState;
    bool                    compressMW = (pCtx->pConfig->compressMode == COMPRESS_ALL);
    char                    mwDL       = pSt->mwDirectory[wi][0];
    bool                    lz4MW      = compressMW && pCtx->pConfig->lz4Drives[0]
                                       && (strchr(pCtx->pConfig->lz4Drives, mwDL) != nullptr);

    if (lz4MW)
        RSFZLNameWriterFile(out, outSize, pSt->mwDirectory[wi], player, fileIdx);
    else if (compressMW)
        RSFZNameWriterFile(out, outSize, pSt->mwDirectory[wi], player, fileIdx);
    else
        RSFNameWriterFile(out, outSize, pSt->mwDirectory[wi], player, fileIdx);
}

/*
** Function: ConsolidatorWorkerBody
** @brief    Runs one consolidation merge: the files in nodes[0..count) (all
**           already reserved by the master) merge into outPath (already
**           reserved via the registry+DriveReserve by the master too).
**           Deletes the real inputs and updates stats on success. On
**           termination mid-merge, abandons the partial output and releases
**           everything back to the registry, unmerged -- no self-chain
**           needed, the level is ending. Always ends by incrementing
**           consolidatorFreeCount and waking the master, whether it
**           succeeded, was terminated, or (in principle) failed some other
**           way -- a slot freeing up is always real news the master should
**           re-evaluate against.
** @param    pCtx     - solve context
** @param    wi       - writer drive index
** @param    player   - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @param    nodes    - the reserved input nodes (ownership: this function deletes/removes them)
** @param    paths    - parallel array of each node's real path (MemMalloc'd; this function frees)
** @param    count    - number of input files
** @param    outNode  - the reserved new-output node (RegistryReserveNew)
** @param    outPath  - the new output file's path
** @param    reserveBytes - worst-case bytes reserved for outPath
** @param    slot     - this worker's pConsolidatorPool thread index, used to
**                      index consolSlot[] for the live STATUS progress line
*/
static void ConsolidatorWorkerBody(PSolveContext pCtx, int slot, int wi, int player,
                                    PRegistryFileNode* nodes, char** paths, int count,
                                    PRegistryFileNode outNode, char outPath[MAX_FULL_PATH_NAME],
                                    int64_t reserveBytes)
{
    POthelloRingMasterState pSt      = pCtx->pState;
    int                     level    = (int)pSt->playLevel;
    bool                    compress = (pCtx->pConfig->compressMode == COMPRESS_ALL);
    char                    driveLetter = pSt->mwDirectory[wi][0];

    /* Publish live progress for the STATUS display. totalBytes is the
    ** uncompressed-equivalent (recordCount*16) of every input, matching the
    ** unit KWayMergeFiles increments doneBytes in, so the percentage/rate read
    ** the same as every other progress line. Read each input's trailer for its
    ** record count (cheap -- trailer only, small file counts); fields are set
    ** before active=1 so the stats reader never sees a half-populated slot.
    */
    ConsolidatorSlotStats* ps = &pSt->consolSlot[slot];
    int64_t totalRecs = 0;
    for (int i = 0; i < count; i++)
    {
        RSFReader* r = RSFOpen(paths[i]);
        if (r) { totalRecs += (int64_t)RSFReaderTrailer(r)->recordCount; RSFClose(&r); }
    }
    ps->writerIdx   = wi;
    ps->player      = player;
    ps->fileCount   = count;
    ps->totalBytes  = totalRecs * (int64_t)sizeof(UINT64_PAIR);
    ps->doneBytes   = 0;
    ps->startTickMs = GetTickCount64();
    ps->active      = 1;

    uint64_t unique = KWayMergeFiles(paths, count, outPath, &ps->doneBytes,
                                      compress, &pSt->terminateConsolidation);

    if (pSt->terminateConsolidation)
    {
        ps->active = 0;
        /* Abandon cleanly: delete the partial output, release the new-file
        ** reservation and node, release every input back to available
        ** (unreserve, not remove -- the real files are untouched), no
        ** self-chain (the level is ending).
        */
        DeleteFileA(outPath);
        RegistryAbandonNew(pSt, wi, outNode);
        DriveReclaim(pSt, driveLetter, reserveBytes);
        for (int i = 0; i < count; i++)
        {
            RegistryUnreserveOne(pSt, wi, nodes[i]);
            MemFree(paths[i]);
        }
        MemFree(paths); MemFree(nodes);
        InterlockedIncrement((volatile LONG*)&pSt->consolidatorFreeCount);
        ConsolidationMasterWake(pCtx);
        return;
    }

    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    int64_t actual = 0;
    if (GetFileAttributesExA(outPath, GetFileExInfoStandard, &fad))
        actual = ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;

    RegistryFinishNew(pSt, wi, outNode, actual);
    DriveReclaim(pSt, driveLetter, reserveBytes - actual);

    int64_t inputTotal = 0;
    for (int i = 0; i < count; i++)
    {
        inputTotal += nodes[i]->physfilesize;
        DriveReclaim(pSt, driveLetter, nodes[i]->physfilesize);
        DeleteFileA(paths[i]);
        RegistryRemoveNode(pSt, wi, nodes[i]);
        MemFree(paths[i]);
    }
    MemFree(paths); MemFree(nodes);

    InterlockedAdd64((volatile LONG64*)&pSt->levelStats[level].consolidationFilesCreated, 1);
    InterlockedAdd64((volatile LONG64*)&pSt->levelStats[level].consolidationFilesRemoved, (LONG64)count);
    InterlockedAdd64((volatile LONG64*)&pSt->levelStats[level].consolidationBytesWritten, (LONG64)actual);
    InterlockedAdd64((volatile LONG64*)&pSt->consolidationBytesInput, (LONG64)inputTotal);

    LoggerLog("ConsolidatorWorkerBody: %s wi=%d merged %d files -> '%s' (%llu unique, %lld -> %lld bytes)\n",
              RSFPlayerStr(player), wi, count, outPath, (unsigned long long)unique,
              (long long)inputTotal, (long long)actual);

    ps->active = 0;
    InterlockedIncrement((volatile LONG*)&pSt->consolidatorFreeCount);
    ConsolidationMasterWake(pCtx);
}

/*
** Function: ConsolidationMasterWake
** @brief    See ConsolidationMaster.h.
*/
void ConsolidationMasterWake(PSolveContext pCtx)
{
    POthelloRingMasterState pSt = pCtx->pState;
    {
        std::lock_guard<std::mutex> lock(pSt->consolidationMasterMutex);
        pSt->consolidationMasterWake = true;
    }
    pSt->consolidationMasterCV.notify_all();
}

/*
** Function: ConsolidationMasterStop
** @brief    See ConsolidationMaster.h.
*/
void ConsolidationMasterStop(PSolveContext pCtx)
{
    POthelloRingMasterState pSt = pCtx->pState;
    pSt->terminateConsolidation = true;
    ConsolidationMasterWake(pCtx);
    if (pSt->consolidationMasterThread.joinable())
        pSt->consolidationMasterThread.join();
}

/*
** Function: ConsolidationMasterLoop
** @brief    See ConsolidationMaster.h.
*/
void ConsolidationMasterLoop(PSolveContext pCtx)
{
    POthelloRingMasterState pSt = pCtx->pState;

    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(pSt->consolidationMasterMutex);
            pSt->consolidationMasterCV.wait(lock, [pSt]
                { return pSt->consolidationMasterWake || pSt->terminateThreads; });
            pSt->consolidationMasterWake = false;
        }

        /* Both flags mean "this thread function should exit now" -- the two
        ** cases (real shutdown vs. a level ending/a checkpoint pause) are
        ** distinguished entirely by what the CALLER does next:
        ** CleanupSolver never restarts it; PerformMidLevelCheckpoint starts
        ** a brand-new std::thread right after ConsolidationMasterStop's
        ** join returns. The loop itself doesn't need to know which case
        ** it's in -- it just needs to actually return so join() doesn't
        ** hang forever (a real bug caught here: terminateConsolidation
        ** alone used to just `continue` back to waiting, which meant
        ** ConsolidationMasterStop's join() would never complete).
        */
        if (pSt->terminateThreads || pSt->terminateConsolidation)
            return;

        int64_t maxSize = maxFileSizeBytes(pCtx);

        for (int wi = 0; wi < pSt->numMergeWriters; wi++)
        {
            for (int player = RSF_PLAYER_WHITE; player <= RSF_PLAYER_BLACK; player++)
            {
                if (InterlockedCompareExchange((volatile LONG*)&pSt->consolidatorFreeCount, 0, 0) <= 0)
                    continue;   /* no free worker -- leave these files unreserved, re-evaluate next wake */

                PRegistryFileNode candidates[MAX_CONSOLIDATION_BATCH];
                int found = RegistryScanUnreserved(pSt, wi, player, maxSize,
                                                    candidates, MAX_CONSOLIDATION_BATCH);
                if (found <= 2)
                    continue;

                /* Reserve every candidate found -- master and workers are
                ** the only reservers of already-unreserved files here (iMerge
                ** takes from the same pool but under its own registry lock,
                ** so a lost race on any one candidate is normal, not a bug).
                */
                PRegistryFileNode* nodes = (PRegistryFileNode*)MemMalloc("consolNodes",
                                               (size_t)found * sizeof(PRegistryFileNode));
                char**             paths = (char**)MemMalloc("consolPaths", (size_t)found * sizeof(char*));
                if (!nodes || !paths)
                    Fatal(FATAL_ALLOCATION_FAILED, "ConsolidationMasterLoop: alloc");

                int     count      = 0;
                int64_t runningSize = 0;
                for (int i = 0; i < found; i++)
                {
                    if (!RegistryReserveOne(pSt, wi, candidates[i], REGISTRY_RESERVED_CONSOL))
                        continue;   /* lost a race -- normal */
                    nodes[count] = candidates[i];
                    paths[count] = (char*)MemMalloc("consolPath", strlen(candidates[i]->filename) + 1);
                    if (!paths[count])
                        Fatal(FATAL_ALLOCATION_FAILED, "ConsolidationMasterLoop: path alloc");
                    strcpy(paths[count], candidates[i]->filename);
                    runningSize += candidates[i]->physfilesize;
                    count++;
                }

                if (count <= 2)
                {
                    /* Lost enough races that this batch isn't worth it anymore. */
                    for (int i = 0; i < count; i++) { RegistryUnreserveOne(pSt, wi, nodes[i]); MemFree(paths[i]); }
                    MemFree(nodes); MemFree(paths);
                    continue;
                }

                int  fileIdx = RegistryNextFileIdx(pSt, wi);
                char outPath[MAX_FULL_PATH_NAME];
                buildConsolOutputPath(outPath, sizeof(outPath), pCtx, wi, player, fileIdx);
                char driveLetter = pSt->mwDirectory[wi][0];

                PRegistryFileNode outNode = RegistryReserveNew(pSt, wi, player, REGISTRY_RESERVED_CONSOL,
                                                                 outPath, driveLetter, runningSize);
                if (!outNode)
                {
                    /* No space for the new output -- release the inputs,
                    ** leave it to the space-pressure trigger (iMerge) to
                    ** relieve; no retry loop here, this is opportunistic
                    ** housekeeping, not a blocking dependency like flush.
                    */
                    for (int i = 0; i < count; i++) { RegistryUnreserveOne(pSt, wi, nodes[i]); MemFree(paths[i]); }
                    MemFree(nodes); MemFree(paths);
                    continue;
                }

                InterlockedDecrement((volatile LONG*)&pSt->consolidatorFreeCount);
                pSt->pConsolidatorPool->QueueJob(
                    /* outPath (a char[MAX_FULL_PATH_NAME]) is captured by value --
                    ** the closure owns its own copy of the buffer, safe to use
                    ** after this scope returns.
                    */
                    [pCtx, wi, player, nodes, paths, count, outNode, outPath, runningSize](uint32_t thdIdx) mutable
                    {
                        ConsolidatorWorkerBody(pCtx, (int)thdIdx, wi, player, nodes, paths, count,
                                                outNode, outPath, runningSize);
                    });
            }
        }
    }
}
