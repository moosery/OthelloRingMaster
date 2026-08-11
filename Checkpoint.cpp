/*
** Filename:  Checkpoint.cpp
**
** Purpose:
**   Implements the mid-level checkpoint API declared in Checkpoint.h --
**   deciding when a checkpoint is due, and running the pause-time NVMe-side
**   sequence that makes one durable.
**
** Notes:
**   v1.0.0 (2026-08-xx): reworked for the registry redesign. What's
**   persisted is now a per-drive naming counter (nextFileIdx, naming only,
**   never logic) plus a full {filename,color,size} integrity manifest for
**   every real file per drive -- see OthelloTypes.h's CheckpointStats
**   comment. Restart re-scans each writer drive (Registry.h's
**   RegistryRebuildFromDisk, called from InitSolver.cpp) and this file
**   Fatals on any disagreement with the manifest (missing file, untracked
**   file, size mismatch) -- three independent hard-Fatal rules, since by
**   the time a checkpoint is trusted enough to resume from, disagreement
**   means real data loss or tampering, not something to silently paper over.
**   See Checkpoint.h for the split of responsibility with LevelSolverThread.cpp
**   (GPU accumulator draining and stream resumption stay there; this file
**   owns everything from "the accumulator is drained" onward).
*/

/* Includes */
#include "Checkpoint.h"
#include "Registry.h"
#include "FlusherPool.h"
#include "IMergePool.h"
#include "ConsolidationMaster.h"
#include "RSFFileName.h"
#include "Logger.h"
#include "Error.h"
#include <windows.h>
#include <time.h>
#include "Mem.h"

/* Functions */

/*
** Function: CheckpointDueNow
** @brief    See Checkpoint.h.
*/
bool CheckpointDueNow(PSolveContext pCtx)
{
    POthelloRingMasterState  pSt  = pCtx->pState;
    POthelloRingMasterConfig pCfg = pCtx->pConfig;

    if (pSt->checkpointRequestedNow)
        return true;

    if (pCfg->checkpointIntervalHours <= 0.0)
        return false;   /* periodic checkpointing disabled */

    uint64_t nowMs        = GetTickCount64();
    double   elapsedHours = (double)(nowMs - pSt->checkpointIntervalStartTickMs) / 3600000.0;
    return elapsedHours >= pCfg->checkpointIntervalHours;
}

/*
** Function: PerformMidLevelCheckpoint
** @brief    See Checkpoint.h.
*/
void PerformMidLevelCheckpoint(PSolveContext pCtx, int activeSubPass, uint64_t recordsConsumedInSubPass)
{
    POthelloRingMasterState  pSt   = pCtx->pState;
    POthelloRingMasterConfig pCfg  = pCtx->pConfig;
    int                      level = (int)pSt->playLevel;

    LoggerLog("Checkpoint: pausing level %d (%s sub-pass, %llu records consumed) to checkpoint...\n",
              level, RSFPlayerStr(activeSubPass), (unsigned long long)recordsConsumedInSubPass);

    /* Drain, honoring the hard rule (fixed v1.0.11): a flush or an iMerge has
    ** PRIORITY and must FINISH -- it is never interrupted by a checkpoint.
    ** Consolidation is the ONLY thing that may be stopped. Taking a checkpoint
    ** while a space-relief iMerge was mid-flight (plowing through it with a
    ** force-flush + a second ConsolidationMasterStop while the relief was
    ** still coordinating its own) left a half-migrated iMerge output on the
    ** medium drive that the writer-drive-only manifest never covered -- real
    ** data loss (a resumed level came back ~1.7B boards short). So the order
    ** is now: let everything flush/iMerge-related go fully quiescent first,
    ** and only then stop consolidation and snapshot.
    **
    ** (1) Let any in-flight space relief run to completion -- it drives the
    **     iMerges and its own consolidation stop/restart cycle; never barge in.
    ** (2) Drain the GPU->pool (D2H/compress) handoff.
    ** (3) Force-flush leftover in-memory pool data to disk. This is a flush;
    **     it must fully complete, and if it hits space pressure it drives its
    **     own relief (iMerge), which must also complete -- (4) waits for that.
    ** (4) Confirm every flush/iMerge/relief is genuinely idle.
    ** (5) NOW stop consolidation -- the one interruptible thing -- and (6, below)
    **     snapshot a fully quiescent, flush/iMerge-complete state.
    */
    SpaceReliefGateWait(pCtx);                                   /* (1) */
    WaitForPoolIdle(pSt->pMergeWriterPool);                      /* (2) */
    for (int ti = 0; ti < (int)pSt->numMergeWriters; ti++)      /* (3) */
        FlushMergeWriterBuffer(ti, pCtx);
    SpaceReliefGateWait(pCtx);                                   /* (4) any relief (3) kicked off */
    WaitForPoolIdle(pSt->pFlusherPool);
    WaitForPoolIdle(pSt->pIMergePool);
    ConsolidationMasterStop(pCtx);                              /* (5) */
    WaitForPoolIdle(pSt->pConsolidatorPool);

    /* Every writer-drive file is now real, finished, and unreserved --
    ** capture the naming counter and a full integrity manifest straight
    ** from the registry (already reflects exactly this quiescent state,
    ** no need for a separate directory scan here).
    */
    /* Heap, never stack -- CheckpointStats (~30 MB, manifest arrays) would
    ** overflow the 1 MB stack on entry. MemMalloc zero-fills; freed after the
    ** checkpoint file is written (below).
    */
    CheckpointStats* cpPtr = (CheckpointStats*)MemMalloc("checkpointStats", sizeof(CheckpointStats));
    CheckpointStats& cp    = *cpPtr;
    cp.boardSize                = pCfg->boardSize;
    cp.level                    = (uint8_t)level;
    cp.activeSubPass             = (uint8_t)activeSubPass;
    cp.numMergeWriters           = pSt->numMergeWriters;
    cp.recordsConsumedInSubPass = recordsConsumedInSubPass;
    for (int wi = 0; wi < pSt->numMergeWriters; wi++)
    {
        cp.nextFileIdx[wi] = pSt->nextFileIdx[wi];

        int count = 0;
        EnterCriticalSection(&pSt->driveRegistryCS[wi]);
        for (auto& n : pSt->driveRegistry[wi])
        {
            if (count >= CHECKPOINT_MANIFEST_MAX_FILES)
            {
                LeaveCriticalSection(&pSt->driveRegistryCS[wi]);
                Fatal(FATAL_MERGE_LOGIC_ERROR,
                      "PerformMidLevelCheckpoint: writer %d has more than %d real files -- "
                      "checkpoint manifest capacity exceeded, cannot write a trustworthy checkpoint",
                      wi, CHECKPOINT_MANIFEST_MAX_FILES);
            }
            CheckpointManifestEntry& e = cp.manifest[wi][count];
            strncpy_s(e.filename, sizeof(e.filename), n.filename, _TRUNCATE);
            e.color = n.color;
            e.size  = n.physfilesize;
            count++;
        }
        LeaveCriticalSection(&pSt->driveRegistryCS[wi]);
        cp.manifestCount[wi] = count;
    }
    {
        time_t    now = time(NULL);
        struct tm t   = {};
        localtime_s(&t, &now);
        snprintf(cp.timestamp, sizeof(cp.timestamp), "%04d-%02d-%02d %02d:%02d:%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    }

    char path[MAX_FULL_PATH_NAME];
    SentinelNameCheckpoint(path, sizeof(path), pSt->storeDirectory, cp.boardSize, level);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        Fatal(FATAL_FILE_OPEN, "PerformMidLevelCheckpoint: cannot create checkpoint file '%s'", path);
    uint64_t magic = CHECKPOINT_STATS_MAGIC;
    DWORD    nw;
    WriteFile(h, &magic, (DWORD)sizeof(magic), &nw, NULL);
    WriteFile(h, &cp,    (DWORD)sizeof(cp),    &nw, NULL);
    CloseHandle(h);
    MemFree(cpPtr);

    LoggerLog("Checkpoint: wrote '%s'\n", path);

    /* Resume for the rest of the level. Deliberately NOT a full per-level
    ** reset (see OthelloRingMaster.cpp's own per-level loop for comparison)
    ** -- only the consolidation master/workers need respawning here; the
    ** registry and every naming counter are left exactly as they were,
    ** since this is a pause, not a level boundary.
    */
    pSt->terminateConsolidation = false;
    pSt->consolidationMasterThread = std::thread(ConsolidationMasterLoop, pCtx);

    pSt->checkpointRequestedNow        = false;
    pSt->checkpointIntervalStartTickMs = GetTickCount64();
}

/*
** Function: ReadValidCheckpoint
** @brief    See Checkpoint.h.
*/
bool ReadValidCheckpoint(PSolveContext pCtx, int level, CheckpointStats* out)
{
    POthelloRingMasterState  pSt  = pCtx->pState;
    POthelloRingMasterConfig pCfg = pCtx->pConfig;

    char path[MAX_FULL_PATH_NAME];
    SentinelNameCheckpoint(path, sizeof(path), pSt->storeDirectory, pCfg->boardSize, level);

    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;   /* no checkpoint -- normal, not an error */

    uint64_t        magic = 0;
    /* Heap, never stack -- CheckpointStats (~30 MB, manifest arrays) would
    ** overflow the 1 MB stack on entry. MemMalloc zero-fills; every exit path
    ** below frees it (the failure returns and the validated return true; the
    ** Fatal paths intentionally don't, since Fatal ends the process).
    */
    CheckpointStats* cpPtr = (CheckpointStats*)MemMalloc("checkpointStats", sizeof(CheckpointStats));
    CheckpointStats& cp    = *cpPtr;
    DWORD           nr    = 0;
    bool ok = ReadFile(h, &magic, (DWORD)sizeof(magic), &nr, NULL)
              && nr == sizeof(magic)
              && magic == CHECKPOINT_STATS_MAGIC
              && ReadFile(h, &cp, (DWORD)sizeof(cp), &nr, NULL)
              && nr == sizeof(cp);
    CloseHandle(h);

    if (!ok)
    {
        LoggerLog("Checkpoint: '%s' exists but is not a valid checkpoint payload -- ignoring, falling back to a fresh level start.\n", path);
        MemFree(cpPtr);
        return false;
    }

    if (cp.boardSize != pCfg->boardSize || cp.level != (uint8_t)level ||
        cp.numMergeWriters != pSt->numMergeWriters)
    {
        LoggerLog("Checkpoint: '%s' doesn't match the current run (boardSize/level/numMergeWriters) -- ignoring.\n", path);
        MemFree(cpPtr);
        return false;
    }

    if (cp.activeSubPass != RSF_PLAYER_BLACK && cp.activeSubPass != RSF_PLAYER_WHITE)
    {
        LoggerLog("Checkpoint: '%s' has an invalid activeSubPass value -- ignoring.\n", path);
        MemFree(cpPtr);
        return false;
    }

    /* This solve step must genuinely still be in progress. CRITICAL
    ** level-numbering off-by-one (fixed v1.0.9 -- this rejected EVERY
    ** checkpoint on restart before): iteration `level` READS Level_`level`
    ** and WRITES Level_`level+1` (see InitSolver.cpp's resume scan and
    ** project_level_numbering_offset_by_one). So "this step already
    ** finished" is marked by Level_`level+1`'s _complete sentinel, NOT
    ** Level_`level`'s -- Level_`level`_complete is this step's INPUT-ready
    ** marker (written by the PREVIOUS step) and is ALWAYS present while this
    ** step runs. Checking `level` here (the original bug) therefore always
    ** fired and discarded the checkpoint. Same off-by-one for _merging (the
    ** merge that could be mid-flight produces Level_`level+1`).
    */
    char sentPath[MAX_FULL_PATH_NAME];
    SentinelNameComplete(sentPath, sizeof(sentPath), pSt->storeDirectory, pCfg->boardSize, level + 1);
    if (GetFileAttributesA(sentPath) != INVALID_FILE_ATTRIBUTES)
    {
        LoggerLog("Checkpoint: level %d solve step already completed (Level_%04d _complete present) -- ignoring stale checkpoint '%s'.\n", level, level + 1, path);
        MemFree(cpPtr);
        return false;
    }
    SentinelNameMerging(sentPath, sizeof(sentPath), pSt->storeDirectory, pCfg->boardSize, level + 1);
    if (GetFileAttributesA(sentPath) != INVALID_FILE_ATTRIBUTES)
    {
        LoggerLog("Checkpoint: level %d output is mid-merge (Level_%04d _merging present) -- ignoring stale checkpoint '%s'.\n", level, level + 1, path);
        MemFree(cpPtr);
        return false;
    }

    /* This step's INPUT must be genuinely complete: iteration `level` reads
    ** Level_`level`, whose readiness is marked by Level_`level`_complete
    ** (written by the previous step). Re-asserts the precondition
    ** ScanForResumeLevel already established when it chose this resume level.
    ** (Was `level - 1` -- same off-by-one; harmless in practice since every
    ** lower level is complete too, but corrected for consistency.)
    */
    if (level > 0)
    {
        SentinelNameComplete(sentPath, sizeof(sentPath), pSt->storeDirectory, pCfg->boardSize, level);
        if (GetFileAttributesA(sentPath) == INVALID_FILE_ATTRIBUTES)
        {
            LoggerLog("Checkpoint: input level %d has no _complete sentinel -- ignoring checkpoint '%s'.\n", level, path);
            MemFree(cpPtr);
            return false;
        }
    }

    /* Integrity manifest cross-check: three independent hard-Fatal rules,
    ** not a soft "ignore and fall back" like the checks above -- once a
    ** checkpoint has passed every structural check, disagreement between
    ** what it recorded and what's actually on disk means real data loss or
    ** tampering, not something to silently paper over (see
    ** project_writer_drive_registry_redesign memory).
    */
    for (int wi = 0; wi < pSt->numMergeWriters; wi++)
    {
        bool matched[CHECKPOINT_MANIFEST_MAX_FILES] = {};

        /* Rule 2 (untracked file) needs a real directory scan; do it once
        ** per drive and check both directions (manifest vs. disk) against
        ** the same scan results, matched by real file size not name-first,
        ** since the point is confirming reality matches the record, not
        ** just that a same-named file happens to exist.
        */
        for (int player = RSF_PLAYER_WHITE; player <= RSF_PLAYER_BLACK; player++)
        {
            char patterns[3][MAX_FULL_PATH_NAME];
            RSFPatternWriterFiles(patterns[0], sizeof(patterns[0]), pSt->mwDirectory[wi], player);
            RSFZPatternWriterFiles(patterns[1], sizeof(patterns[1]), pSt->mwDirectory[wi], player);
            RSFZLPatternWriterFiles(patterns[2], sizeof(patterns[2]), pSt->mwDirectory[wi], player);

            for (int p = 0; p < 3; p++)
            {
                WIN32_FIND_DATAA fd;
                HANDLE fh = FindFirstFileA(patterns[p], &fd);
                if (fh == INVALID_HANDLE_VALUE) continue;
                do
                {
                    char realPath[MAX_FULL_PATH_NAME];
                    snprintf(realPath, sizeof(realPath), "%s\\%s", pSt->mwDirectory[wi], fd.cFileName);
                    ULARGE_INTEGER sz; sz.LowPart = fd.nFileSizeLow; sz.HighPart = (DWORD)fd.nFileSizeHigh;

                    int foundIdx = -1;
                    for (int m = 0; m < cp.manifestCount[wi]; m++)
                    {
                        if (_stricmp(cp.manifest[wi][m].filename, realPath) == 0)
                        {
                            foundIdx = m;
                            break;
                        }
                    }
                    if (foundIdx < 0)
                    {
                        FindClose(fh);
                        Fatal(FATAL_MERGE_LOGIC_ERROR,
                              "Checkpoint: '%s' exists on disk but is not in checkpoint '%s' -- "
                              "untracked file, refusing to resume without explicit investigation",
                              realPath, path);
                    }
                    if ((int64_t)sz.QuadPart != cp.manifest[wi][foundIdx].size)
                    {
                        FindClose(fh);
                        Fatal(FATAL_MERGE_LOGIC_ERROR,
                              "Checkpoint: '%s' real size %lld != checkpoint '%s' recorded size %lld -- "
                              "possible corruption/tampering, refusing to resume",
                              realPath, path, (long long)sz.QuadPart, (long long)cp.manifest[wi][foundIdx].size);
                    }
                    matched[foundIdx] = true;
                } while (FindNextFileA(fh, &fd));
                FindClose(fh);
            }
        }

        for (int m = 0; m < cp.manifestCount[wi]; m++)
        {
            if (!matched[m])
                Fatal(FATAL_MERGE_LOGIC_ERROR,
                      "Checkpoint: '%s' lists '%s' but it's missing on disk -- "
                      "real potential data loss, refusing to resume",
                      path, cp.manifest[wi][m].filename);
        }
    }

    *out = cp;
    LoggerLog("Checkpoint: '%s' validated OK (%s sub-pass, %llu records consumed, written %s).\n",
              path, RSFPlayerStr(cp.activeSubPass),
              (unsigned long long)cp.recordsConsumedInSubPass, cp.timestamp);
    MemFree(cpPtr);
    return true;
}

/*
** Function: DeleteLevelCheckpoint
** @brief    See Checkpoint.h.
*/
void DeleteLevelCheckpoint(PSolveContext pCtx, int level)
{
    POthelloRingMasterState  pSt  = pCtx->pState;
    POthelloRingMasterConfig pCfg = pCtx->pConfig;

    char path[MAX_FULL_PATH_NAME];
    SentinelNameCheckpoint(path, sizeof(path), pSt->storeDirectory, pCfg->boardSize, level);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
        DeleteFileA(path);
}
