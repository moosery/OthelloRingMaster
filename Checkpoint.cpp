/*
** Filename:  Checkpoint.cpp
**
** Purpose:
**   Implements the mid-level checkpoint API declared in Checkpoint.h --
**   deciding when a checkpoint is due, and running the pause-time NVMe-side
**   sequence that makes one durable.
**
** Notes:
**   2026-08: reworked to the durability/trailer/disk-scan design (replaces the
**   earlier file-manifest scheme -- see OthelloTypes.h's CheckpointStats and
**   Checkpoint.h for the full rationale). The checkpoint payload is now tiny
**   (position + last-record cross-check + cumulative counters, no file list).
**   Restart trusts the disk: ValidateCheckpointFilesOnDisk confirms every data
**   file has an intact trailer (crash-partials are removed via --forcerestart
**   or Fatal-listed), and CheckpointSeedCountersFromDisk seeds every work dir's
**   next-file index from a scan (max-on-disk + 1) so resumed writes can never
**   overwrite a pre-checkpoint file. See Checkpoint.h for the split of
**   responsibility with LevelSolverThread.cpp (GPU accumulator draining and
**   stream resumption stay there; this file owns everything from "the
**   accumulator is drained" onward).
*/

/* Includes */
#include "Checkpoint.h"
#include "Registry.h"
#include "FlusherPool.h"
#include "IMergePool.h"
#include "ConsolidationMaster.h"
#include "RSFFileName.h"
#include "RingStoreFile.h"
#include "Logger.h"
#include "Error.h"
#include <windows.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
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
void PerformMidLevelCheckpoint(PSolveContext pCtx, int activeSubPass, uint64_t recordsConsumedInSubPass,
                               uint64_t lastRecordHi, uint64_t lastRecordLo, bool haveLastRecord)
{
    POthelloRingMasterState  pSt   = pCtx->pState;
    POthelloRingMasterConfig pCfg  = pCtx->pConfig;
    int                      level = (int)pSt->playLevel;

    LoggerLog("Checkpoint: pausing level %d (%s sub-pass, %llu records consumed) to checkpoint...\n",
              level, RSFPlayerStr(activeSubPass), (unsigned long long)recordsConsumedInSubPass);

    /* Drain, honoring the hard rule (v1.0.11): a flush or an iMerge has
    ** PRIORITY and must FINISH -- it is never interrupted by a checkpoint.
    ** Consolidation is the ONLY thing that may be stopped. This makes the
    ** checkpoint's core guarantee true: all output for input[0..P] is in
    ** COMPLETE (trailer'd) files on disk before we record position P. Barging
    ** through an in-flight flush/iMerge would leave half-written output that
    ** the resume can't account for. So the order is: let everything
    ** flush/iMerge-related go fully quiescent first, then snapshot.
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

    /* Every flush/iMerge is done and consolidation is stopped: all output for
    ** input[0..P] is now in complete, trailer'd files on disk. Capture the tiny
    ** checkpoint payload -- position + last-record cross-check + cumulative
    ** counters. No file names/sizes/indices are recorded: restart trusts the
    ** disk (trailer completeness + disk-scan index seeding, see Checkpoint.h).
    ** MemMalloc (not stack) is retained out of caution though CheckpointStats
    ** is small now; it zero-fills and is freed after the file is written below.
    */
    CheckpointStats* cpPtr = (CheckpointStats*)MemMalloc("checkpointStats", sizeof(CheckpointStats));
    CheckpointStats& cp    = *cpPtr;
    cp.boardSize                = pCfg->boardSize;
    cp.level                    = (uint8_t)level;
    cp.activeSubPass             = (uint8_t)activeSubPass;
    cp.numMergeWriters           = pSt->numMergeWriters;
    cp.haveLastRecord           = haveLastRecord;
    cp.recordsConsumedInSubPass = recordsConsumedInSubPass;
    cp.lastRecordHi             = lastRecordHi;
    cp.lastRecordLo             = lastRecordLo;

    /*
    ** Snapshot this level's live cumulative counters so a cross-process resume
    ** can report the FULL level's work, not just the post-resume slice. The
    ** per-level reset (OthelloRingMaster.cpp) zeroes levelStats[level] fresh on
    ** every start; RunGpuFeederJob restores the counter fields from this on
    ** resume. Captured after the full drain, so every counter reflects work
    ** already durable on disk. See CheckpointStats.levelStatsSnapshot.
    */
    cp.levelStatsSnapshot = pSt->levelStats[level];

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

    /* On-disk file integrity is NOT checked here anymore -- there is no file
    ** manifest to cross-check against. It's handled separately by
    ** ValidateCheckpointFilesOnDisk (trailer completeness) and index seeding by
    ** CheckpointSeedCountersFromDisk, both driven from InitSolver.cpp. This
    ** function only validates the checkpoint PAYLOAD (magic, config match,
    ** staleness/input sentinels above).
    */
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

/*
** Function: cpTrailerIsValid  (file-local helper)
** @brief    True if the file is at least one trailer long and its final 64
**           bytes carry a recognized RSF trailer magic (RSF/RSFZ/RSFZL). The
**           magic is written last on a clean close, so its absence reliably
**           means a crash-partial (incomplete) file.
*/
static bool cpTrailerIsValid(const char* fullPath)
{
    HANDLE h = CreateFileA(fullPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart < (LONGLONG)sizeof(RSFTrailer))
    {
        CloseHandle(h);
        return false;
    }

    LARGE_INTEGER off;
    off.QuadPart = size.QuadPart - (LONGLONG)sizeof(RSFTrailer);
    if (!SetFilePointerEx(h, off, NULL, FILE_BEGIN))
    {
        CloseHandle(h);
        return false;
    }

    RSFTrailer tr;
    DWORD nr = 0;
    bool okRead = ReadFile(h, &tr, (DWORD)sizeof(tr), &nr, NULL) && nr == sizeof(tr);
    CloseHandle(h);
    if (!okRead)
        return false;

    return (tr.magic == RSF_MAGIC || tr.magic == RSFZ_MAGIC || tr.magic == RSFZL_MAGIC);
}

/*
** Function: cpParseTrailingIndex  (file-local helper)
** @brief    Parses the %04d file-index that every writer/imerge file name ends
**           with, right after its last '_' (e.g. writer_black_0014.rsf -> 14,
**           imerge_L016_white_0003.rsfzl -> 3). atoi stops at the '.', so the
**           extension is ignored. Returns -1 if there is no '_'.
*/
static int cpParseTrailingIndex(const char* fname)
{
    const char* us = strrchr(fname, '_');
    if (!us)
        return -1;
    return atoi(us + 1);
}

/*
** Function: cpMaxIndexForPatterns  (file-local helper)
** @brief    Highest file index found across a set of wildcard patterns, or -1
**           if none match. Callers add 1 to get the next index to hand out.
*/
static int cpMaxIndexForPatterns(char patterns[][MAX_FULL_PATH_NAME], int nPat)
{
    int maxIdx = -1;
    for (int p = 0; p < nPat; p++)
    {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(patterns[p], &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        do
        {
            int idx = cpParseTrailingIndex(fd.cFileName);
            if (idx > maxIdx)
                maxIdx = idx;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return maxIdx;
}

/*
** Function: cpValidateDir  (file-local helper)
** @brief    Trailer-checks every file matching the given patterns in one dir.
**           Each file with no valid trailer is counted and either deleted
**           (forceRestart) or logged for the pre-Fatal list (else).
*/
static void cpValidateDir(const char* dir, char patterns[][MAX_FULL_PATH_NAME], int nPat,
                          bool forceRestart, int* pInvalidCount)
{
    for (int p = 0; p < nPat; p++)
    {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(patterns[p], &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        do
        {
            char full[MAX_FULL_PATH_NAME];
            snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
            if (!cpTrailerIsValid(full))
            {
                (*pInvalidCount)++;
                if (forceRestart)
                {
                    LoggerLog("Checkpoint restart: removing crash-partial file (no valid trailer): %s\n", full);
                    DeleteFileA(full);
                }
                else
                {
                    LoggerLog("Checkpoint restart: INVALID file (no valid trailer): %s\n", full);
                }
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
}

/*
** Function: ValidateCheckpointFilesOnDisk
** @brief    See Checkpoint.h.
*/
void ValidateCheckpointFilesOnDisk(PSolveContext pCtx, int level, bool forceRestart)
{
    POthelloRingMasterState pSt = pCtx->pState;
    int invalidCount = 0;

    /* Writer files on every fast drive (both colors, all three formats). */
    for (int i = 0; i < pSt->numMergeWriters; i++)
    {
        for (int player = RSF_PLAYER_WHITE; player <= RSF_PLAYER_BLACK; player++)
        {
            char pats[3][MAX_FULL_PATH_NAME];
            RSFPatternWriterFiles(pats[0],   sizeof(pats[0]), pSt->mwDirectory[i], player);
            RSFZPatternWriterFiles(pats[1],  sizeof(pats[1]), pSt->mwDirectory[i], player);
            RSFZLPatternWriterFiles(pats[2], sizeof(pats[2]), pSt->mwDirectory[i], player);
            cpValidateDir(pSt->mwDirectory[i], pats, 3, forceRestart, &invalidCount);
        }
    }

    /* iMerge files on every medium drive for this level (both colors, all formats). */
    for (int d = 0; d < pSt->numMergeDirs; d++)
    {
        for (int player = RSF_PLAYER_WHITE; player <= RSF_PLAYER_BLACK; player++)
        {
            char pats[3][MAX_FULL_PATH_NAME];
            RSFPatternImergeFiles(pats[0],   sizeof(pats[0]), pSt->mergeDirectory[d], level, player);
            RSFZPatternImergeFiles(pats[1],  sizeof(pats[1]), pSt->mergeDirectory[d], level, player);
            RSFZLPatternImergeFiles(pats[2], sizeof(pats[2]), pSt->mergeDirectory[d], level, player);
            cpValidateDir(pSt->mergeDirectory[d], pats, 3, forceRestart, &invalidCount);
        }
    }

    /* iMerge files on the store-drive fallback dir for this level. */
    for (int player = RSF_PLAYER_WHITE; player <= RSF_PLAYER_BLACK; player++)
    {
        char pats[3][MAX_FULL_PATH_NAME];
        RSFPatternImergeFiles(pats[0],   sizeof(pats[0]), pSt->storeMergeDirectory, level, player);
        RSFZPatternImergeFiles(pats[1],  sizeof(pats[1]), pSt->storeMergeDirectory, level, player);
        RSFZLPatternImergeFiles(pats[2], sizeof(pats[2]), pSt->storeMergeDirectory, level, player);
        cpValidateDir(pSt->storeMergeDirectory, pats, 3, forceRestart, &invalidCount);
    }

    if (invalidCount > 0)
    {
        if (forceRestart)
            LoggerLog("Checkpoint restart: removed %d crash-partial file(s) (--forcerestart).\n", invalidCount);
        else
            Fatal(FATAL_MERGE_LOGIC_ERROR,
                  "Checkpoint restart: found %d data file(s) with no valid trailer (listed above) -- "
                  "crash-partial writes from a run that continued past the checkpoint. Confirm the "
                  "list, then re-run with --forcerestart to remove them and resume.",
                  invalidCount);
    }
    else
    {
        LoggerLog("Checkpoint restart: all data files have intact trailers.\n");
    }
}

/*
** Function: CheckpointSeedCountersFromDisk
** @brief    See Checkpoint.h.
*/
void CheckpointSeedCountersFromDisk(PSolveContext pCtx, int level)
{
    POthelloRingMasterState pSt = pCtx->pState;

    /* Writer next-file index per fast drive: max over BOTH colors + all three
    ** formats, since the writer index is drive-scoped (shared across colors,
    ** the color lives in the filename). max-on-disk + 1 guarantees resumed
    ** writes land above every existing file and never reuse a live index --
    ** even one that post-checkpoint consolidation moved to a higher index.
    */
    for (int i = 0; i < pSt->numMergeWriters; i++)
    {
        char pats[6][MAX_FULL_PATH_NAME];
        RSFPatternWriterFiles(pats[0],   sizeof(pats[0]), pSt->mwDirectory[i], RSF_PLAYER_BLACK);
        RSFZPatternWriterFiles(pats[1],  sizeof(pats[1]), pSt->mwDirectory[i], RSF_PLAYER_BLACK);
        RSFZLPatternWriterFiles(pats[2], sizeof(pats[2]), pSt->mwDirectory[i], RSF_PLAYER_BLACK);
        RSFPatternWriterFiles(pats[3],   sizeof(pats[3]), pSt->mwDirectory[i], RSF_PLAYER_WHITE);
        RSFZPatternWriterFiles(pats[4],  sizeof(pats[4]), pSt->mwDirectory[i], RSF_PLAYER_WHITE);
        RSFZLPatternWriterFiles(pats[5], sizeof(pats[5]), pSt->mwDirectory[i], RSF_PLAYER_WHITE);
        pSt->nextFileIdx[i] = cpMaxIndexForPatterns(pats, 6) + 1;
    }

    /* iMerge counters per medium drive (per color) for this level. */
    for (int d = 0; d < pSt->numMergeDirs; d++)
    {
        char pb[3][MAX_FULL_PATH_NAME], pw[3][MAX_FULL_PATH_NAME];
        RSFPatternImergeFiles(pb[0],   sizeof(pb[0]), pSt->mergeDirectory[d], level, RSF_PLAYER_BLACK);
        RSFZPatternImergeFiles(pb[1],  sizeof(pb[1]), pSt->mergeDirectory[d], level, RSF_PLAYER_BLACK);
        RSFZLPatternImergeFiles(pb[2], sizeof(pb[2]), pSt->mergeDirectory[d], level, RSF_PLAYER_BLACK);
        RSFPatternImergeFiles(pw[0],   sizeof(pw[0]), pSt->mergeDirectory[d], level, RSF_PLAYER_WHITE);
        RSFZPatternImergeFiles(pw[1],  sizeof(pw[1]), pSt->mergeDirectory[d], level, RSF_PLAYER_WHITE);
        RSFZLPatternImergeFiles(pw[2], sizeof(pw[2]), pSt->mergeDirectory[d], level, RSF_PLAYER_WHITE);
        pSt->mergeFileBlackCount[d] = cpMaxIndexForPatterns(pb, 3) + 1;
        pSt->mergeFileWhiteCount[d] = cpMaxIndexForPatterns(pw, 3) + 1;
    }

    /* iMerge counters for the store-drive fallback dir. */
    {
        char pb[3][MAX_FULL_PATH_NAME], pw[3][MAX_FULL_PATH_NAME];
        RSFPatternImergeFiles(pb[0],   sizeof(pb[0]), pSt->storeMergeDirectory, level, RSF_PLAYER_BLACK);
        RSFZPatternImergeFiles(pb[1],  sizeof(pb[1]), pSt->storeMergeDirectory, level, RSF_PLAYER_BLACK);
        RSFZLPatternImergeFiles(pb[2], sizeof(pb[2]), pSt->storeMergeDirectory, level, RSF_PLAYER_BLACK);
        RSFPatternImergeFiles(pw[0],   sizeof(pw[0]), pSt->storeMergeDirectory, level, RSF_PLAYER_WHITE);
        RSFZPatternImergeFiles(pw[1],  sizeof(pw[1]), pSt->storeMergeDirectory, level, RSF_PLAYER_WHITE);
        RSFZLPatternImergeFiles(pw[2], sizeof(pw[2]), pSt->storeMergeDirectory, level, RSF_PLAYER_WHITE);
        pSt->storeMergeBlackFileCount = cpMaxIndexForPatterns(pb, 3) + 1;
        pSt->storeMergeWhiteFileCount = cpMaxIndexForPatterns(pw, 3) + 1;
    }

    LoggerLog("Checkpoint restart: seeded all work-dir file indices from disk scan (max-on-disk + 1).\n");
}
