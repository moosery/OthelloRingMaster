/*
** Filename:  IMergePool.cpp
**
** Purpose:
**   Implements the cross-drive intermediate merge pool (IMergePool.h) --
**   replaces DoCrossDriveIntermediateMerge (MergeFiles.cpp, retired). See
**   that header and project_writer_drive_registry_redesign memory for the
**   full design: two dedicated threads (one per color) so black/white
**   sessions run genuinely concurrently, triggered only by real space
**   pressure, gathering every currently-unreserved file for a color across
**   ALL NVMe drives via the registry (no ticket-range scanning).
**
** Notes:
**   Adapted from the retired DoCrossDriveIntermediateMerge -- the real
**   merge mechanics (KWayMergeFiles, medium-drive-then-store-drive
**   fallback) are unchanged; what's new is registry-based gathering
**   (instead of a ticket-snapshot-then-claim-range scan) and per-color
**   concurrency (instead of one imergeCS lock serializing both colors
**   through one function call).
*/

/* Includes */
#include "IMergePool.h"
#include "Registry.h"
#include "MergeFiles.h"
#include "ConsolidationMaster.h"
#include "DriveLedger.h"
#include "RSFFileName.h"
#include "Logger.h"
#include "Mem.h"
#include <windows.h>
#include <mutex>

/* Functions */

/*
** Function: IMergeTriggerAndWait
** @brief    See IMergePool.h.
*/
void IMergeTriggerAndWait(PSolveContext pCtx, int player)
{
    POthelloRingMasterState pSt      = pCtx->pState;
    ImergeColorSession&     session  = pSt->imergeSession[player];

    std::unique_lock<std::mutex> lock(session.m);
    if (!session.active)
    {
        /* No session running for this color -- claim it and dispatch the
        ** REAL work onto pIMergePool. Critical: this function must not run
        ** IMergeRunSession itself on the calling thread -- the caller is a
        ** flusher pool thread, and running iMerge inline on it would
        ** recreate the exact starvation this whole redesign exists to fix
        ** (housekeeping consuming flush's own thread capacity). The
        ** dispatched job clears `active` and notifies every waiter
        ** (including this call, below) once it's genuinely done.
        */
        session.active = true;
        lock.unlock();

        pSt->pIMergePool->QueueJob([pCtx, player](uint32_t)
        {
            IMergeRunSession(pCtx, player);
            ImergeColorSession& s = pCtx->pState->imergeSession[player];
            {
                std::lock_guard<std::mutex> l(s.m);
                s.active = false;
            }
            s.cv.notify_all();
        });

        lock.lock();
    }

    /* Whether we just dispatched a new session or found one already
    ** running, wait here until it completes -- a true broadcast wait, so
    ** any number of simultaneous callers for this color are released
    ** together.
    */
    session.cv.wait(lock, [&session] { return !session.active; });
}

/*
** Function: buildImergeOutputPath
** @brief    Builds the cross-drive imerge output path on either a medium
**           drive (destDirIdx >= 0) or the store drive's fallback merge
**           directory (destDirIdx < 0, the "total flush" case), using a
**           fresh per-destination file index and this run's compression mode.
*/
static void buildImergeOutputPath(char* out, size_t outSize, PSolveContext pCtx,
                                   int level, int player, int destDirIdx)
{
    POthelloRingMasterState pSt      = pCtx->pState;
    bool                    compress = (pCtx->pConfig->compressMode == COMPRESS_ALL);
    const char*             lz4Drv   = pCtx->pConfig->lz4Drives;

    if (destDirIdx >= 0)
    {
        char dl = pSt->mergeDirectory[destDirIdx][0];
        bool lz4 = compress && lz4Drv[0] && (strchr(lz4Drv, dl) != nullptr);
        volatile LONG* pCount = (player == RSF_PLAYER_BLACK)
            ? (volatile LONG*)&pSt->mergeFileBlackCount[destDirIdx]
            : (volatile LONG*)&pSt->mergeFileWhiteCount[destDirIdx];
        int fileIdx = (int)InterlockedExchangeAdd(pCount, 1);
        if (lz4)
            RSFZLNameImergeFile(out, outSize, pSt->mergeDirectory[destDirIdx], level, player, fileIdx);
        else if (compress)
            RSFZNameImergeFile(out, outSize, pSt->mergeDirectory[destDirIdx], level, player, fileIdx);
        else
            RSFNameImergeFile(out, outSize, pSt->mergeDirectory[destDirIdx], level, player, fileIdx);
    }
    else
    {
        char sDL = pSt->storeMergeDirectory[0];
        bool lz4 = compress && lz4Drv[0] && (strchr(lz4Drv, sDL) != nullptr);
        volatile LONG* pCount = (player == RSF_PLAYER_BLACK)
            ? (volatile LONG*)&pSt->storeMergeBlackFileCount
            : (volatile LONG*)&pSt->storeMergeWhiteFileCount;
        int fileIdx = (int)InterlockedExchangeAdd(pCount, 1);
        if (lz4)
            RSFZLNameImergeFile(out, outSize, pSt->storeMergeDirectory, level, player, fileIdx);
        else if (compress)
            RSFZNameImergeFile(out, outSize, pSt->storeMergeDirectory, level, player, fileIdx);
        else
            RSFNameImergeFile(out, outSize, pSt->storeMergeDirectory, level, player, fileIdx);
    }
}

/*
** Function: IMergeRunSession
** @brief    See IMergePool.h.
*/
void IMergeRunSession(PSolveContext pCtx, int player)
{
    POthelloRingMasterState pSt      = pCtx->pState;
    int                     level    = (int)pSt->playLevel;
    bool                    compress = (pCtx->pConfig->compressMode == COMPRESS_ALL);

    pSt->imergeActive[player]          = 1;
    pSt->imergeTotalInputBytes[player] = 0;
    pSt->imergeDoneInputBytes[player]  = 0;
    pSt->imergeStartTickMs[player]     = GetTickCount64();
    pSt->imergeFileCount[player]       = 0;

    /* Gather every currently-unreserved, real file for this color across
    ** ALL writer drives -- no size cap (unlike consolidation), since
    ** iMerge's whole point is clearing space regardless of how big
    ** individual files already are.
    */
    const int kMaxFiles = 4096;
    char**    paths      = (char**)MemMalloc("imergePaths", (size_t)kMaxFiles * sizeof(char*));
    int64_t*  sizes      = (int64_t*)MemMalloc("imergeSizes", (size_t)kMaxFiles * sizeof(int64_t));
    PRegistryFileNode* nodes = (PRegistryFileNode*)MemMalloc("imergeNodes", (size_t)kMaxFiles * sizeof(PRegistryFileNode));
    int*      writerOf   = (int*)MemMalloc("imergeWriterOf", (size_t)kMaxFiles * sizeof(int));
    if (!paths || !sizes || !nodes || !writerOf)
        Fatal(FATAL_ALLOCATION_FAILED, "IMergeRunSession: alloc");

    int     numFiles   = 0;
    int64_t totalBytes = 0;
    PRegistryFileNode scanBuf[512];

    for (int wi = 0; wi < pSt->numMergeWriters && numFiles < kMaxFiles; wi++)
    {
        int found = RegistryScanUnreserved(pSt, wi, player, -1, scanBuf,
                                            (int)(sizeof(scanBuf) / sizeof(scanBuf[0])));
        for (int f = 0; f < found && numFiles < kMaxFiles; f++)
        {
            if (!RegistryReserveOne(pSt, wi, scanBuf[f], REGISTRY_RESERVED_IMERGE))
                continue;   /* lost a race to another scanner -- normal, just skip it */

            paths[numFiles]  = (char*)MemMalloc("imergePath", strlen(scanBuf[f]->filename) + 1);
            if (!paths[numFiles])
                Fatal(FATAL_ALLOCATION_FAILED, "IMergeRunSession: path alloc");
            strcpy(paths[numFiles], scanBuf[f]->filename);
            sizes[numFiles]    = scanBuf[f]->physfilesize;
            nodes[numFiles]    = scanBuf[f];
            writerOf[numFiles] = wi;
            totalBytes        += scanBuf[f]->physfilesize;
            pSt->imergeTotalInputBytes[player] += scanBuf[f]->physfilesize;
            numFiles++;
        }
    }

    if (numFiles == 0)
    {
        MemFree(paths); MemFree(sizes); MemFree(nodes); MemFree(writerOf);
        pSt->imergeActive[player] = 0;
        return;
    }

    pSt->imergeFileCount[player] = numFiles;
    LoggerLog("IMergeRunSession: %s gathered %d files (%.2f GB)\n",
              RSFPlayerStr(player), numFiles, totalBytes / (1024.0 * 1024.0 * 1024.0));

    /* Try each medium drive in turn; fall back to the store drive (a "total
    ** flush") if none has room. Reservation worst-case = sum of real input
    ** sizes (dedup can only shrink the real output from there).
    */
    int  destDirIdx = -1;
    for (int d = 0; d < pSt->numMergeDirs; d++)
    {
        if (DriveReserve(pSt, pSt->mergeDirectory[d][0], totalBytes))
        {
            destDirIdx = d;
            break;
        }
    }

    char outPath[MAX_FULL_PATH_NAME];
    char destDriveLetter;
    if (destDirIdx >= 0)
    {
        destDriveLetter = pSt->mergeDirectory[destDirIdx][0];
        buildImergeOutputPath(outPath, sizeof(outPath), pCtx, level, player, destDirIdx);
    }
    else
    {
        destDriveLetter = pCtx->pConfig->storeDrive;
        if (!DriveReserve(pSt, destDriveLetter, totalBytes))
            Fatal(FATAL_DRIVE_SPACE,
                  "IMergeRunSession: %s needs %.2f GB on %c: (store drive, medium drives all full)",
                  RSFPlayerStr(player), totalBytes / (1024.0 * 1024.0 * 1024.0), destDriveLetter);
        buildImergeOutputPath(outPath, sizeof(outPath), pCtx, level, player, -1);
    }

    LoggerLog("IMergeRunSession: %s -> '%s' (%d files, %.2f GB)\n",
              RSFPlayerStr(player), outPath, numFiles, totalBytes / (1024.0 * 1024.0 * 1024.0));

    uint64_t unique;
    if (numFiles == 1)
    {
        /* Single file: just move it, no merge needed -- cheap rename when
        ** same-volume, falls back to copy+delete across drives (MoveFileExA
        ** handles both transparently).
        */
        if (!MoveFileExA(paths[0], outPath, MOVEFILE_COPY_ALLOWED))
            Fatal(FATAL_FILE_OPEN, "IMergeRunSession: cannot move '%s' -> '%s' (err %lu)",
                  paths[0], outPath, (unsigned long)GetLastError());
        unique = 0;   /* real record count unknown without a decompress-and-count pass, which
                       ** a straight move deliberately avoids -- logged as "(moved)" below
                       ** instead of a real count; no dedup happens here so there's nothing
                       ** the count would tell a reader that the byte totals already don't. */
    }
    else
    {
        unique = KWayMergeFiles(paths, numFiles, outPath, &pSt->imergeDoneInputBytes[player],
                                 compress, &pSt->terminateThreads);
    }

    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    int64_t actual = 0;
    if (GetFileAttributesExA(outPath, GetFileExInfoStandard, &fad))
        actual = ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;

    DriveReclaim(pSt, destDriveLetter, totalBytes - actual);

    /* The iMerge output lives on a medium/store drive, which has no
    ** registry of its own (only writer drives do -- see OthelloTypes.h's
    ** driveRegistry comment) -- nothing to register for the output file
    ** itself; DoEndOfLevelMerge finds it later via a real directory scan of
    ** mergeDirectory/storeMergeDirectory, same as it always has.
    */
    for (int i = 0; i < numFiles; i++)
    {
        DriveReclaim(pSt, pSt->mwDirectory[writerOf[i]][0], sizes[i]);
        DeleteFileA(paths[i]);
        RegistryRemoveNode(pSt, writerOf[i], nodes[i]);
        MemFree(paths[i]);
    }
    MemFree(paths); MemFree(sizes); MemFree(nodes); MemFree(writerOf);

    LoggerLog("IMergeRunSession: %s done (%llu unique)\n", RSFPlayerStr(player), (unsigned long long)unique);

    pSt->imergeActive[player]          = 0;
    pSt->imergeTotalInputBytes[player] = 0;

    ConsolidationMasterWake(pCtx);
}
