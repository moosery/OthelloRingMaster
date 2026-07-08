/*
** Filename:  StatsListener.cpp
**
** Purpose:
**   Implements the status thread declared in StatsListener.h: a tiny TCP
**   server on pConfig->statsPort that responds to STATUS (a human-readable
**   progress report) and STOP (graceful shutdown request) commands from the
**   standalone OthelloRingMasterStatus client.
**
** Notes:
**   Adapted from an earlier solver implementation. Only the config/state
**   type names changed (-> OthelloRingMasterConfig/State) and the version
**   banner text (-> "OthelloRingMaster v%s") -- everything else here
**   formats already-generically-named LevelStats/WriterDriveStats fields,
**   so nothing else needed to change.
*/

/* Includes */
/* Include winsock2 before any project headers to prevent winsock1 conflicts */
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#include <windows.h>

#include "StatsListener.h"
#include "DriveLedger.h"
#include "OthelloTypes.h"
#include "Logger.h"
#include <stdio.h>
#include <string.h>

/* Functions */

/*
** Function: FormatDuration
** @brief    Formats a nanosecond duration as "HH:MM:SS".
** @param    nanos   - duration in nanoseconds
** @param    out     - out: formatted string
** @param    outSize - capacity of out
*/
static void FormatDuration(int64_t nanos, char* out, int outSize)
{
    int64_t s = nanos / 1000000000LL;
    snprintf(out, outSize, "%02lld:%02lld:%02lld",
             s / 3600, (s % 3600) / 60, s % 60);
}

/*
** Function: FormatEta
** @brief    ETA for a progress line with known done/total GB and a current MB/s rate.
** @details  "--:--:--" when the rate or remaining amount doesn't support an
**           estimate yet (e.g. right after the operation starts, or it's
**           already effectively done).
** @param    doneGB  - GB completed so far
** @param    totalGB - total GB expected
** @param    mbps    - current throughput in MB/s
** @param    out     - out: formatted ETA string
** @param    outSize - capacity of out
*/
static void FormatEta(double doneGB, double totalGB, double mbps, char* out, int outSize)
{
    if (mbps <= 0.0 || totalGB <= doneGB)
    {
        snprintf(out, outSize, "--:--:--");
        return;
    }
    double remainingGB = totalGB - doneGB;
    double etaSec       = remainingGB * 1024.0 / mbps;
    FormatDuration((int64_t)(etaSec * 1e9), out, outSize);
}

/*
** Function: MbpsToBoardsPerSec
** @brief    Converts a progress line's "MB/s" figure to boards/sec.
** @details  These progress lines' "MB/s" is really (records processed x 16
**           bytes) per second (see KWayMergeFiles/MergePoolToWriter) -- a
**           duplicate record costs far less CPU than a unique one, so it
**           isn't literal network/disk throughput. Showing boards/sec
**           alongside makes that honest instead of implying a real transfer rate.
** @param    mbps - MB/s figure to convert
** @return   Equivalent boards/sec.
*/
static double MbpsToBoardsPerSec(double mbps)
{
    return mbps * (1024.0 * 1024.0 / 16.0);
}

/*
** Function: BuildStatusResponse
** @brief    Builds the full human-readable STATUS response: current-level
**           live stats, per-drive breakdown, active merge/flush/cascade
**           progress, and the level history table.
** @param    pCtx    - solve context
** @param    buf     - out: destination buffer
** @param    bufSize - capacity of buf
*/
static void BuildStatusResponse(PSolveContext pCtx, char* buf, int bufSize)
{
    POthelloRingMasterConfig pCfg = pCtx->pConfig;
    POthelloRingMasterState  pSt  = pCtx->pState;
    int      curLevel  = (int)pSt->playLevel;
    int      maxLevel  = (int)pCfg->boardSize * (int)pCfg->boardSize - 4;
    char     dur[16];

    int n = 0;
    n += snprintf(buf + n, bufSize - n,
                  "OthelloRingMaster v%s  |  Board: %dx%d  |  Levels: 0..%d\n",
                  VERSION, pCfg->boardSize, pCfg->boardSize, maxLevel - 1);
    n += snprintf(buf + n, bufSize - n, "\n");

    /* --- Current level (live stats) --- */
    const LevelStats* cur = &pSt->levelStats[curLevel];
    bool    curDone      = (cur->totalNanos > 0);
    int64_t elapsedNanos = curDone ? cur->totalNanos
                                   : ClockNanosSinceStart((PClockTick)&cur->startTick);
    uint64_t brdPerSec   = (elapsedNanos > 0)
                           ? (uint64_t)((double)cur->boardsReadFromStore * 1e9 / (double)elapsedNanos) : 0;
    uint64_t nsBrd       = (cur->boardsReadFromStore > 0)
                           ? (uint64_t)(elapsedNanos / (int64_t)cur->boardsReadFromStore) : 0;
    FormatDuration(elapsedNanos, dur, sizeof(dur));

    const char* phase = curDone ? "done"
                               : (pSt->currentPhase ? pSt->currentPhase : "RUNNING");
    n += snprintf(buf + n, bufSize - n,
                  "=== Level %d / %d  [%s]  %s  (%llu brd/s  %llu ns/brd) ===\n",
                  curLevel, maxLevel - 1, phase,
                  dur, (unsigned long long)brdPerSec, (unsigned long long)nsBrd);
    n += snprintf(buf + n, bufSize - n,
                  "  Boards in (store)      : %llu\n",
                  (unsigned long long)cur->boardsReadFromStore);
    n += snprintf(buf + n, bufSize - n,
                  "  Boards generated (GPU) : %llu\n",
                  (unsigned long long)cur->boardsGenerated);
    n += snprintf(buf + n, bufSize - n,
                  "  GPU dups removed       : %llu\n",
                  (unsigned long long)cur->gpuDupsRemoved);
    n += snprintf(buf + n, bufSize - n,
                  "  GPU flushes            : %llu\n",
                  (unsigned long long)cur->gpuFlushes);
    n += snprintf(buf + n, bufSize - n,
                  "  Boards recv'd (GPU)    : %llu\n",
                  (unsigned long long)cur->boardsReceivedFromGpu);
    n += snprintf(buf + n, bufSize - n,
                  "  Merge dups removed     : %llu\n",
                  (unsigned long long)cur->mrgDupsRemoved);
    n += snprintf(buf + n, bufSize - n,
                  "  MW files created       : %llu\n",
                  (unsigned long long)cur->mwFilesCreated);
    n += snprintf(buf + n, bufSize - n,
                  "  Boards written to disk : %llu  (%.2f GB)\n",
                  (unsigned long long)cur->boardsWrittenToDisk,
                  cur->mwBytes / (1024.0 * 1024.0 * 1024.0));
    if (cur->passBoards > 0 || cur->terminalBoards > 0)
    {
        n += snprintf(buf + n, bufSize - n,
                      "  Pass boards            : %llu\n",
                      (unsigned long long)cur->passBoards);
        n += snprintf(buf + n, bufSize - n,
                      "  Terminal boards        : %llu\n",
                      (unsigned long long)cur->terminalBoards);
    }

    /* Estimated time remaining for the WHOLE current level (solve + merge
    ** combined), not any one operation. Blends the two phases using the
    ** solve/total time split from the most recently completed level as a
    ** guide, since that ratio tends to hold fairly steady level-to-level.
    */
    if (!curDone)
    {
        double solveFrac = 0.5;   /* default guess if there's no prior level to learn from */
        if (curLevel > 0)
        {
            const LevelStats* prev = &pSt->levelStats[curLevel - 1];
            if (prev->totalNanos > 0 && prev->solverNanos > 0 && prev->solverNanos < prev->totalNanos)
                solveFrac = (double)prev->solverNanos / (double)prev->totalNanos;
        }

        double overallFrac  = 0.0;
        bool   haveEstimate = true;
        if (pSt->currentPhase && strcmp(pSt->currentPhase, "Merging to store") == 0)
        {
            double wPct = (pSt->mergeTotalInputBytes[0] > 0)
                ? (double)pSt->mergeProgressBytes[0] / (double)pSt->mergeTotalInputBytes[0] : 0.0;
            double bPct = (pSt->mergeTotalInputBytes[1] > 0)
                ? (double)pSt->mergeProgressBytes[1] / (double)pSt->mergeTotalInputBytes[1] : 0.0;
            overallFrac = solveFrac + (1.0 - solveFrac) * ((wPct + bPct) / 2.0);
        }
        else if (pSt->currentPhase && strcmp(pSt->currentPhase, "Flushing buffers") == 0)
        {
            overallFrac = solveFrac;   /* solve just finished, merge hasn't started yet */
        }
        else if (pSt->currentPhase && strcmp(pSt->currentPhase, "GPU solving") == 0
                 && pSt->currentLevelTotalBoards > 0)
        {
            double solvePct = (double)cur->boardsReadFromStore / (double)pSt->currentLevelTotalBoards;
            overallFrac = solvePct * solveFrac;
        }
        else
        {
            haveEstimate = false;
        }

        if (haveEstimate && overallFrac > 0.001 && overallFrac < 1.0)
        {
            int64_t etaNanos = (int64_t)((double)elapsedNanos / overallFrac * (1.0 - overallFrac));
            char    etaDur[16];
            FormatDuration(etaNanos, etaDur, sizeof(etaDur));
            n += snprintf(buf + n, bufSize - n,
                          "  Est. time remaining    : %s  (~%.1f%% of level done)\n",
                          etaDur, overallFrac * 100.0);
        }
    }
    n += snprintf(buf + n, bufSize - n, "\n");

    /* Current-level drive breakdown (cumulative since level start) */
    n += snprintf(buf + n, bufSize - n,
                  "  Drv  Files       Disk GB     Uncomp GB       Free GB   Blk   Wht\n");
    n += snprintf(buf + n, bufSize - n,
                  "  ---  -----  ------------  ------------  ------------  ----  ----\n");
    for (int i = 0; i < pSt->numWriterDrives; i++)
    {
        const WriterDriveStats* d = &pSt->writerDriveStats[i];

        /* Sum live writer file counts across threads on this drive */
        int liveBlack = 0, liveWhite = 0;
        for (int ti = 0; ti < pSt->numMergeWriters; ti++)
        {
            if (pSt->mwDirectory[ti][0] == d->driveLetter)
            {
                liveBlack += pSt->mwBlackFileCount[ti];
                liveWhite += pSt->mwWhiteFileCount[ti];
            }
        }

        bool showUncomp = (d->levelBytesUncompressed > 0
                           && d->levelBytesUncompressed != d->levelBytesWritten);
        if (showUncomp)
            n += snprintf(buf + n, bufSize - n,
                          "    %c  %5llu  %9.2f GB  %9.2f GB  %9.2f GB  %4d  %4d\n",
                          d->driveLetter,
                          (unsigned long long)d->levelFilesWritten,
                          d->levelBytesWritten      / (1024.0 * 1024.0 * 1024.0),
                          d->levelBytesUncompressed / (1024.0 * 1024.0 * 1024.0),
                          DriveAvailable(pSt, d->driveLetter) / (1024.0 * 1024.0 * 1024.0),
                          liveBlack, liveWhite);
        else
            n += snprintf(buf + n, bufSize - n,
                          "    %c  %5llu  %9.2f GB                %9.2f GB  %4d  %4d\n",
                          d->driveLetter,
                          (unsigned long long)d->levelFilesWritten,
                          d->levelBytesWritten / (1024.0 * 1024.0 * 1024.0),
                          DriveAvailable(pSt, d->driveLetter) / (1024.0 * 1024.0 * 1024.0),
                          liveBlack, liveWhite);
    }

    /* MW compressed-pool segment counts: live vs. lifetime high-water (never
    ** reset), so it's visible whether MAX_MW_SEGS has real headroom left.
    */
    for (int ti = 0; ti < pSt->numMergeWriters; ti++)
    {
        n += snprintf(buf + n, bufSize - n,
                      "  Segs %c: black %3d (hi %3d/%d)   white %3d (hi %3d/%d)\n",
                      pSt->mwDirectory[ti][0],
                      pSt->mwBlackSegCount[ti], pSt->mwBlackSegCountHighWater[ti], MAX_MW_SEGS,
                      pSt->mwWhiteSegCount[ti], pSt->mwWhiteSegCountHighWater[ti], MAX_MW_SEGS);
    }

    /* Merge drives (medium speed) -- imerge file counts, bytes written, free space */
    for (int i = 0; i < pSt->numMergeDirs; i++)
    {
        char     dl     = pSt->mergeDirectory[i][0];
        int      blk    = pSt->mergeFileBlackCount[i];
        int      wht    = pSt->mergeFileWhiteCount[i];
        uint64_t disk   = pSt->mergeFileBytesBlack[i] + pSt->mergeFileBytesWhite[i];
        uint64_t uncomp = pSt->mergeFileUncompBlack[i] + pSt->mergeFileUncompWhite[i];
        bool showUncomp = (uncomp > 0 && uncomp != disk);
        double freeGB   = DriveAvailable(pSt, dl) / (1024.0 * 1024.0 * 1024.0);
        if (showUncomp)
            n += snprintf(buf + n, bufSize - n,
                          "    %c  %4d  %5d  %9.2f GB  %9.2f GB  %9.2f GB  %4d  %4d\n",
                          dl, 1, blk + wht,
                          disk   / (1024.0 * 1024.0 * 1024.0),
                          uncomp / (1024.0 * 1024.0 * 1024.0),
                          freeGB, blk, wht);
        else if (disk > 0)
            n += snprintf(buf + n, bufSize - n,
                          "    %c  %4d  %5d  %9.2f GB                %9.2f GB  %4d  %4d\n",
                          dl, 1, blk + wht,
                          disk / (1024.0 * 1024.0 * 1024.0),
                          freeGB, blk, wht);
        else
            n += snprintf(buf + n, bufSize - n,
                          "    %c  %4d  %5d                              %9.2f GB  %4d  %4d\n",
                          dl, 1, blk + wht, freeGB, blk, wht);
    }

    /* Store drive -- storeMerge file counts, bytes written, free space */
    {
        char     dl     = pCfg->storeDrive;
        int      blk    = pSt->storeMergeBlackFileCount;
        int      wht    = pSt->storeMergeWhiteFileCount;
        uint64_t disk   = pSt->storeMergeBytesWritten;
        uint64_t uncomp = pSt->storeMergeBytesUncompressed;
        bool showUncomp = (uncomp > 0 && uncomp != disk);
        double freeGB   = DriveAvailable(pSt, dl) / (1024.0 * 1024.0 * 1024.0);
        if (showUncomp)
            n += snprintf(buf + n, bufSize - n,
                          "    %c  %4d  %5d  %9.2f GB  %9.2f GB  %9.2f GB  %4d  %4d\n",
                          dl, 1, blk + wht,
                          disk   / (1024.0 * 1024.0 * 1024.0),
                          uncomp / (1024.0 * 1024.0 * 1024.0),
                          freeGB, blk, wht);
        else if (disk > 0)
            n += snprintf(buf + n, bufSize - n,
                          "    %c  %4d  %5d  %9.2f GB                %9.2f GB  %4d  %4d\n",
                          dl, 1, blk + wht,
                          disk / (1024.0 * 1024.0 * 1024.0),
                          freeGB, blk, wht);
        else
            n += snprintf(buf + n, bufSize - n,
                          "    %c  %4d  %5d                              %9.2f GB  %4d  %4d\n",
                          dl, 1, blk + wht, freeGB, blk, wht);
    }

    /* Active intermediate merges (indexed by player: 0=white, 1=black) */
    {
        uint64_t nowMs = GetTickCount64();
        static const char* kPlayerNames[2] = { "white", "black" };
        for (int p = 0; p <= 1; p++)
        {
            if (pSt->imergeActive[p])
            {
                double   doneGB  = pSt->imergeDoneInputBytes[p]  / (1024.0 * 1024.0 * 1024.0);
                double   totalGB = pSt->imergeTotalInputBytes[p] / (1024.0 * 1024.0 * 1024.0);
                double   pct     = (pSt->imergeTotalInputBytes[p] > 0)
                                   ? 100.0 * (double)pSt->imergeDoneInputBytes[p]
                                           / (double)pSt->imergeTotalInputBytes[p]
                                   : 0.0;
                uint64_t elapsedMs = nowMs - pSt->imergeStartTickMs[p];
                double   mbps      = (elapsedMs > 200 && pSt->imergeDoneInputBytes[p] > 0)
                                   ? (double)pSt->imergeDoneInputBytes[p] / (1024.0 * 1024.0)
                                     / (elapsedMs / 1000.0)
                                   : 0.0;
                char etaStr[16];
                FormatEta(doneGB, totalGB, mbps, etaStr, sizeof(etaStr));
                n += snprintf(buf + n, bufSize - n,
                              "  %-7s %-14s: %6.2f / %6.2f GB  (%7.3f%%)  @ %5.0f MB/s  %9.0f brd/s   ETA: %s\n",
                              "iMerge", kPlayerNames[p], doneGB, totalGB, pct, mbps,
                              MbpsToBoardsPerSec(mbps), etaStr);
            }
        }
    }

    /* Active writer buffer-full flushes (pool -> NVMe spill, one per
    ** merge-writer thread). Without this, a big flush makes the whole
    ** status display look frozen even though the solve is progressing
    ** normally underneath it.
    */
    {
        uint64_t nowMs = GetTickCount64();
        for (int ti = 0; ti < pSt->numMergeWriters; ti++)
        {
            if (pSt->mwFlushActive[ti])
            {
                double   doneGB  = pSt->mwFlushDoneBytes[ti]  / (1024.0 * 1024.0 * 1024.0);
                double   totalGB = pSt->mwFlushTotalBytes[ti] / (1024.0 * 1024.0 * 1024.0);
                double   pct     = (pSt->mwFlushTotalBytes[ti] > 0)
                                   ? 100.0 * (double)pSt->mwFlushDoneBytes[ti]
                                           / (double)pSt->mwFlushTotalBytes[ti]
                                   : 0.0;
                uint64_t elapsedMs = nowMs - pSt->mwFlushStartTickMs[ti];
                double   mbps      = (elapsedMs > 200 && pSt->mwFlushDoneBytes[ti] > 0)
                                   ? (double)pSt->mwFlushDoneBytes[ti] / (1024.0 * 1024.0)
                                     / (elapsedMs / 1000.0)
                                   : 0.0;
                char detail[16];
                snprintf(detail, sizeof(detail), "%c: pool->disk", pSt->mwDirectory[ti][0]);
                char etaStr[16];
                FormatEta(doneGB, totalGB, mbps, etaStr, sizeof(etaStr));
                n += snprintf(buf + n, bufSize - n,
                              "  %-7s %-14s: %6.2f / %6.2f GB  (%7.3f%%)  @ %5.0f MB/s  %9.0f brd/s   ETA: %s\n",
                              "Flush", detail, doneGB, totalGB, pct, mbps,
                              MbpsToBoardsPerSec(mbps), etaStr);
            }
        }
    }

    /* Active end-of-level merge (per player, runs concurrently for white and black) */
    if (pSt->currentPhase && strcmp(pSt->currentPhase, "Merging to store") == 0)
    {
        uint64_t nowMs = GetTickCount64();
        const char* playerNames[2] = { "white", "black" };
        for (int p = 0; p <= 1; p++)
        {
            if (pSt->mergeTotalInputBytes[p] > 0)
            {
                double   doneGB    = (double)pSt->mergeProgressBytes[p]   / (1024.0 * 1024.0 * 1024.0);
                double   totalGB   = (double)pSt->mergeTotalInputBytes[p] / (1024.0 * 1024.0 * 1024.0);
                double   pct       = 100.0 * (double)pSt->mergeProgressBytes[p]
                                           / (double)pSt->mergeTotalInputBytes[p];
                /* Freeze elapsed time at completion so the rate doesn't keep
                ** decaying toward 0 while the finished player waits for the
                ** other to catch up.
                */
                uint64_t endMs     = pSt->mergeEndTickMs[p] ? pSt->mergeEndTickMs[p] : nowMs;
                uint64_t elapsedMs = endMs - pSt->mergeStartTickMs[p];
                double   mbps      = (elapsedMs > 200 && pSt->mergeProgressBytes[p] > 0)
                                   ? (double)pSt->mergeProgressBytes[p] / (1024.0 * 1024.0)
                                     / (elapsedMs / 1000.0)
                                   : 0.0;
                char detail[16];
                snprintf(detail, sizeof(detail), "%s->%c:", playerNames[p], pCfg->storeDrive);
                char etaStr[16];
                FormatEta(doneGB, totalGB, mbps, etaStr, sizeof(etaStr));
                n += snprintf(buf + n, bufSize - n,
                              "  %-7s %-14s: %6.2f / %6.2f GB  (%7.3f%%)  @ %5.0f MB/s  %9.0f brd/s   ETA: %s"
                              "   src: %df+%dp\n",
                              "Merge", detail, doneGB, totalGB, pct, mbps,
                              MbpsToBoardsPerSec(mbps), etaStr,
                              pSt->mergeInputFileCount[p], pSt->mergeInputPoolReaderCount[p]);
            }
        }
    }

    /* Cascade progress -- shown when CascadingMerge is writing intermediate
    ** temp files. That player's merge % (in the history row) will read 0
    ** until the final pass starts; these lines give visibility into what
    ** would otherwise look like a stalled merge.
    */
    {
        uint64_t nowMs = GetTickCount64();
        for (int p = 0; p <= 1; p++)
        {
            if (pSt->cascadeActive[p])
            {
                const char* playerName = (p == 1) ? "black" : "white";
                double   gbWritten = (double)(int64_t)pSt->cascadeGroupProgressBytes[p]
                                     / (1024.0 * 1024.0 * 1024.0);
                uint64_t elapsedMs = nowMs - pSt->cascadeGroupStartTickMs[p];
                double   mbps      = (elapsedMs > 200 && pSt->cascadeGroupProgressBytes[p] > 0)
                                   ? (double)(int64_t)pSt->cascadeGroupProgressBytes[p] / (1024.0 * 1024.0)
                                     / (elapsedMs / 1000.0)
                                   : 0.0;

                /* No single done/total GB for the whole cascade (each
                ** group's target size isn't known upfront), so ETA here is
                ** a coarser average-per-group estimate instead of
                ** FormatEta's rate-based one: how long the completed groups
                ** took on average, times how many groups remain.
                */
                char etaStr[16];
                int  groupsRemaining = pSt->cascadeNumGroups[p] - pSt->cascadeGroupsDone[p];
                if (pSt->cascadeGroupsDone[p] > 0 && groupsRemaining > 0)
                {
                    uint64_t totalElapsedMs = nowMs - pSt->cascadeStartTickMs[p];
                    double   avgMsPerGroup  = (double)totalElapsedMs / pSt->cascadeGroupsDone[p];
                    FormatDuration((int64_t)(avgMsPerGroup * groupsRemaining * 1e6), etaStr, sizeof(etaStr));
                }
                else
                    snprintf(etaStr, sizeof(etaStr), "--:--:--");

                n += snprintf(buf + n, bufSize - n,
                              "  %-7s %-14s: group %2d / %2d  (%6.2f GB to temp)  @ %5.0f MB/s  %9.0f brd/s   ETA: %s\n",
                              "Cascade", playerName,
                              pSt->cascadeGroupsDone[p] + 1,
                              pSt->cascadeNumGroups[p],
                              gbWritten, mbps, MbpsToBoardsPerSec(mbps), etaStr);
            }
        }
    }

    /* --- Level history table (completed levels + current in-progress row) --- */
    n += snprintf(buf + n, bufSize - n, "\n");
    n += snprintf(buf + n, bufSize - n,
                  "Lvl        BoardsIn        Generated         GpuDups         MrgDups         Written       SlvGB  Duration      ns/brd\n");
    n += snprintf(buf + n, bufSize - n,
                  "---  --------------  ---------------  --------------  --------------  --------------  ----------  --------  ----------\n");
    for (int lvl = 0; lvl < curLevel; lvl++)
    {
        if (n >= (int)bufSize - 512) break;   /* safety guard -- buffer nearly full */
        const LevelStats* ls = &pSt->levelStats[lvl];
        if (ls->totalNanos == 0) continue;   /* no stats (legacy sentinel or not yet run) */
        FormatDuration(ls->totalNanos, dur, sizeof(dur));
        uint64_t ns = (ls->boardsReadFromStore > 0)
                      ? (uint64_t)(ls->totalNanos / (int64_t)ls->boardsReadFromStore) : 0;
        n += snprintf(buf + n, bufSize - n,
                      "%3d  %14llu  %15llu  %14llu  %14llu  %14llu  %10.2f  %8s  %10llu\n",
                      lvl,
                      (unsigned long long)ls->boardsReadFromStore,
                      (unsigned long long)ls->boardsGenerated,
                      (unsigned long long)ls->gpuDupsRemoved,
                      (unsigned long long)ls->mrgDupsRemoved,
                      (unsigned long long)ls->boardsWrittenToDisk,
                      ls->mwBytes / (1024.0 * 1024.0 * 1024.0),
                      dur,
                      (unsigned long long)ns);
    }
    /* Current level row with live partial data and phase tag */
    {
        char curDur[16];
        FormatDuration(elapsedNanos, curDur, sizeof(curDur));
        char phaseStr[24] = "[running]";
        if (curDone)
            snprintf(phaseStr, sizeof(phaseStr), "[done]");
        else if (pSt->currentPhase
                 && strcmp(pSt->currentPhase, "Merging to store") == 0)
        {
            double wPct = (pSt->mergeTotalInputBytes[0] > 0)
                ? 100.0 * (double)pSt->mergeProgressBytes[0] / (double)pSt->mergeTotalInputBytes[0] : 0.0;
            double bPct = (pSt->mergeTotalInputBytes[1] > 0)
                ? 100.0 * (double)pSt->mergeProgressBytes[1] / (double)pSt->mergeTotalInputBytes[1] : 0.0;
            snprintf(phaseStr, sizeof(phaseStr), "[W:%7.3f%%/B:%7.3f%%]", wPct, bPct);
        }
        else if (pSt->currentPhase
                 && strcmp(pSt->currentPhase, "Flushing buffers") == 0)
            snprintf(phaseStr, sizeof(phaseStr), "[flushing]");
        else if (pSt->currentPhase
                 && strcmp(pSt->currentPhase, "GPU solving") == 0
                 && pSt->currentLevelTotalBoards > 0)
            snprintf(phaseStr, sizeof(phaseStr), "[solve%8.3f%%]",
                     100.0 * (double)cur->boardsReadFromStore
                           / (double)pSt->currentLevelTotalBoards);
        n += snprintf(buf + n, bufSize - n,
                      "%3d  %14llu  %15llu  %14llu  %14llu  %14llu  %10.2f  %8s  %s\n",
                      curLevel,
                      (unsigned long long)cur->boardsReadFromStore,
                      (unsigned long long)cur->boardsGenerated,
                      (unsigned long long)cur->gpuDupsRemoved,
                      (unsigned long long)cur->mrgDupsRemoved,
                      (unsigned long long)cur->boardsWrittenToDisk,
                      cur->mwBytes / (1024.0 * 1024.0 * 1024.0),
                      curDur,
                      phaseStr);
    }

    n += snprintf(buf + n, bufSize - n, "END\n");
    (void)n;
}

/*
** Function: HandleClient
** @brief    Reads one command (STATUS or STOP) from an accepted client
**           socket, responds, and closes the connection.
** @param    client - the accepted client socket
** @param    pCtx   - solve context
*/
static void HandleClient(SOCKET client, PSolveContext pCtx)
{
    char cmd[64] = {};
    int got = recv(client, cmd, (int)sizeof(cmd) - 1, 0);
    if (got <= 0) { closesocket(client); return; }

    /* Trim trailing whitespace/newlines */
    for (int i = got - 1; i >= 0 && (cmd[i] == '\r' || cmd[i] == '\n' || cmd[i] == ' '); i--)
        cmd[i] = '\0';

    if (_stricmp(cmd, "STOP") == 0)
    {
        const char* msg = "Stopping...\n";
        send(client, msg, (int)strlen(msg), 0);
        LoggerLog("STOP command received via stats port -- requesting graceful shutdown...\n");
        pCtx->pState->terminateThreads = true;
    }
    else
    {
        char buf[MAX_LEVELS * 160 + 8192];   /* ~49 KB; 160 bytes/row * 256 levels + header */
        BuildStatusResponse(pCtx, buf, sizeof(buf));
        int toSend = (int)strlen(buf);
        int sent   = 0;
        while (sent < toSend)
        {
            int r = send(client, buf + sent, toSend - sent, 0);
            if (r == SOCKET_ERROR) break;
            sent += r;
        }
    }

    closesocket(client);
}

/*
** Function: RunStatsListenerJob
** @brief    The status thread's main loop: binds pCtx->pConfig->statsPort
**           and accepts/handles client connections until told to terminate.
** @param    pCtx - solve context
*/
static void RunStatsListenerJob(uint32_t /*thdIdx*/, PSolveContext pCtx)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) { WSACleanup(); return; }

    BOOL reuse = TRUE;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(pCtx->pConfig->statsPort);

    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(listenSock, 4) != 0)
    {
        LoggerLog("Stats listener: failed to bind/listen on port %d\n",
                  (int)pCtx->pConfig->statsPort);
        closesocket(listenSock);
        WSACleanup();
        return;
    }

    LoggerLog("Stats listener running on port %d\n", (int)pCtx->pConfig->statsPort);

    while (!pCtx->pState->terminateStatsListener)
    {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSock, &readSet);
        timeval tv = { 0, 50000 };   /* 50 ms */

        if (select(0, &readSet, nullptr, nullptr, &tv) <= 0)
            continue;

        SOCKET client = accept(listenSock, nullptr, nullptr);
        if (client != INVALID_SOCKET)
            HandleClient(client, pCtx);
    }

    closesocket(listenSock);
    WSACleanup();
    LoggerLog("Stats listener stopped.\n");
}

/*
** Function: SubmitStatsListenerJob
** @brief    Submits the status thread job to the stats thread pool. Returns immediately.
** @param    pCtx - solve context
*/
void SubmitStatsListenerJob(PSolveContext pCtx)
{
    pCtx->pState->pStatsThreadPool->QueueJob(
        [pCtx](uint32_t thdIdx)
        {
            RunStatsListenerJob(thdIdx, pCtx);
        }
    );
}
