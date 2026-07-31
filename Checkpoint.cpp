/*
** Filename:  Checkpoint.cpp
**
** Purpose:
**   Implements the mid-level checkpoint API declared in Checkpoint.h --
**   deciding when a checkpoint is due, and running the pause-time NVMe-side
**   sequence that makes one durable.
**
** Notes:
**   See Checkpoint.h for the split of responsibility with LevelSolverThread.cpp
**   (GPU accumulator draining and stream resumption stay there; this file
**   owns everything from "the accumulator is drained" onward).
*/

/* Includes */
#include "Checkpoint.h"
#include "MergeFiles.h"
#include "RSFFileName.h"
#include "Logger.h"
#include "Error.h"
#include <windows.h>
#include <time.h>

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

    /* Drain the CPU-side merge-writer pool buffer to real NVMe files --
    ** the same function used at the real solve->merge transition -- except
    ** NOT FlushAllMergeWriterBuffers: that's a narrow safety net for
    ** uncompressed staging data only, and deliberately leaves real pool-
    ** buffer segments resident in memory for DoEndOfLevelMerge to consume
    ** directly at the real end of the level (see its own doc comment). A
    ** checkpoint has no such luxury -- everything must be durably on disk
    ** before the writer-dir scan below can be trusted, so every writer
    ** thread's buffer is force-flushed here unconditionally (FlushMergeWriterBuffer
    ** already no-ops safely if a given thread has nothing accumulated).
    **
    ** WaitForPoolIdle first: FlushMergeWriterBuffer isn't safe to call
    ** directly while a pool job could be concurrently flushing the same
    ** thread's buffer, and this same pool is what DoCrossDriveIntermediateMerge
    ** runs on too -- waiting for it covers both cases, exactly like the
    ** real solve->merge transition's own first wait.
    */
    WaitForPoolIdle(pSt->pMergeWriterPool);
    for (int ti = 0; ti < (int)pSt->numMergeWriters; ti++)
        FlushMergeWriterBuffer(ti, pCtx);

    /* Abort in-flight consolidation cleanly (identical mechanism to the
    ** real solve->merge transition: partial merge output deleted, originals
    ** untouched, every claim released). Workers get restarted below since
    ** the level itself isn't ending -- only mwNextFileIdx/claimRegistry/etc
    ** are left completely untouched by this, since those represent real,
    ** ongoing progress within the level that a checkpoint pause must never
    ** reset.
    */
    pSt->terminateConsolidation = true;
    WaitForPoolIdle(pSt->pConsolidationPool);

    /* Snapshot every (writer, color) ticket high-water mark now -- this is
    ** the one point in the whole sequence guaranteed quiescent (nothing
    ** claimed, nothing mid-merge, every real file already flushed to disk).
    */
    CheckpointStats cp = {};
    cp.boardSize                = pCfg->boardSize;
    cp.level                    = (uint8_t)level;
    cp.activeSubPass             = (uint8_t)activeSubPass;
    cp.numMergeWriters           = pSt->numMergeWriters;
    cp.recordsConsumedInSubPass = recordsConsumedInSubPass;
    for (int wi = 0; wi < pSt->numMergeWriters; wi++)
    {
        cp.mwNextFileIdx[wi][RSF_PLAYER_BLACK] = pSt->mwNextFileIdx[wi][RSF_PLAYER_BLACK];
        cp.mwNextFileIdx[wi][RSF_PLAYER_WHITE] = pSt->mwNextFileIdx[wi][RSF_PLAYER_WHITE];
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

    LoggerLog("Checkpoint: wrote '%s'\n", path);

    /* Resume consolidation workers for the rest of the level. Deliberately
    ** NOT a full per-level reset (see OthelloRingMaster.cpp's own per-level
    ** loop for comparison) -- only the worker threads themselves need
    ** respawning here; every ticket/claim/consolidated-up-to counter is
    ** left exactly as it was, since this is a pause, not a level boundary.
    */
    pSt->terminateConsolidation = false;
    StartConsolidationWorkers(pCtx);

    /* Ready for the caller to resume streaming. */
    pSt->checkpointRequestedNow        = false;
    pSt->checkpointIntervalStartTickMs = GetTickCount64();
    pSt->checkpointPauseFlag           = false;
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
    CheckpointStats  cp    = {};
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
        return false;
    }

    if (cp.boardSize != pCfg->boardSize || cp.level != (uint8_t)level ||
        cp.numMergeWriters != pSt->numMergeWriters)
    {
        LoggerLog("Checkpoint: '%s' doesn't match the current run (boardSize/level/numMergeWriters) -- ignoring.\n", path);
        return false;
    }

    if (cp.activeSubPass != RSF_PLAYER_BLACK && cp.activeSubPass != RSF_PLAYER_WHITE)
    {
        LoggerLog("Checkpoint: '%s' has an invalid activeSubPass value -- ignoring.\n", path);
        return false;
    }

    /* This level must genuinely still be in progress -- a checkpoint
    ** coexisting with a _complete or _merging sentinel for the SAME level
    ** means something is inconsistent (stale checkpoint left behind by an
    ** interrupted attempt, most likely); never trust it in that case.
    */
    char sentPath[MAX_FULL_PATH_NAME];
    SentinelNameComplete(sentPath, sizeof(sentPath), pSt->storeDirectory, pCfg->boardSize, level);
    if (GetFileAttributesA(sentPath) != INVALID_FILE_ATTRIBUTES)
    {
        LoggerLog("Checkpoint: level %d already has a _complete sentinel -- ignoring stale checkpoint '%s'.\n", level, path);
        return false;
    }
    SentinelNameMerging(sentPath, sizeof(sentPath), pSt->storeDirectory, pCfg->boardSize, level);
    if (GetFileAttributesA(sentPath) != INVALID_FILE_ATTRIBUTES)
    {
        LoggerLog("Checkpoint: level %d has a _merging sentinel -- ignoring stale checkpoint '%s'.\n", level, path);
        return false;
    }

    /* Previous level must be genuinely complete (matches the same
    ** precondition ScanForResumeLevel already establishes to have picked
    ** this level as the resume point in the first place -- re-asserted
    ** explicitly here rather than assumed).
    */
    if (level > 0)
    {
        SentinelNameComplete(sentPath, sizeof(sentPath), pSt->storeDirectory, pCfg->boardSize, level - 1);
        if (GetFileAttributesA(sentPath) == INVALID_FILE_ATTRIBUTES)
        {
            LoggerLog("Checkpoint: previous level %d has no _complete sentinel -- ignoring checkpoint '%s'.\n", level - 1, path);
            return false;
        }
    }

    /* Cross-check the writer-dir's actual on-disk state against what the
    ** checkpoint claims: for every (writer, color) with a nonzero recorded
    ** ticket high-water mark, the file at (mark - 1) must genuinely exist.
    ** Not a full reconstruction -- just confirms this checkpoint is
    ** describing the writer-dir that's actually sitting there right now.
    */
    for (int wi = 0; wi < pSt->numMergeWriters; wi++)
    {
        for (int player = 0; player <= 1; player++)
        {
            int highTicket = cp.mwNextFileIdx[wi][player];
            if (highTicket <= 0)
                continue;

            char     candPath[MAX_FULL_PATH_NAME];
            uint64_t candSize;
            if (!FindConsolidationCandidate(candPath, sizeof(candPath), pCtx, wi, player, highTicket - 1, &candSize))
            {
                LoggerLog("Checkpoint: writer %d %s ticket %d (checkpoint's own recorded high-water mark) "
                          "not found on disk -- ignoring checkpoint '%s'.\n",
                          wi, RSFPlayerStr(player), highTicket - 1, path);
                return false;
            }
        }
    }

    *out = cp;
    LoggerLog("Checkpoint: '%s' validated OK (%s sub-pass, %llu records consumed, written %s).\n",
              path, RSFPlayerStr(cp.activeSubPass),
              (unsigned long long)cp.recordsConsumedInSubPass, cp.timestamp);
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
