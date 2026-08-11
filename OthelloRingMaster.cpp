/*
** Filename:  OthelloRingMaster.cpp
**
** Purpose:
**   Entry point for OthelloRingMaster: parses CLI args, runs the ring
**   boundary-conversion self-test once at startup (a real correctness
**   check, not the whole program's purpose -- see RingConversion.h), then
**   drives the per-level solve loop (GPU solve -> merge-writer drain ->
**   end-of-level merge -> stats snapshot -> sentinel write) until every
**   level up to the board's max is complete.
**
** Notes:
**   Adapted from an earlier solver implementation's own main entry point
**   file (not the executable name -- this solution's own
**   OthelloRingMaster.exe plays that same role). Renamed onto this
**   solution's own types (-> OthelloRingMasterConfig/State, RSFFileName.h,
**   RSF_SENTINEL_STATS_MAGIC), default statsPort changed to 17532, default
**   cache/store directory names, and file-extension mentions in usage text
**   updated to .rsf/.rsfz/.rsfzl. Loop choreography unchanged.
*/

/* Includes */
#include "InitLogger.h"
#include "InitSolver.h"
#include "CreateSeedFile.h"
#include "DriveLedger.h"
#include "Registry.h"
#include "LevelSolverThread.h"
#include "MergeFiles.h"
#include "FlusherPool.h"
#include "IMergePool.h"
#include "ConsolidationMaster.h"
#include "RegistryAuditor.h"
#include "DriveSpaceAuditor.h"
#include "Checkpoint.h"
#include "StatsListener.h"
#include "RSFFileName.h"
#include "RingConversion.h"
#include <windows.h>
#include <ctype.h>
#include <stdio.h>

/* Globals */
OthelloRingMasterConfig g_config      = {};
OthelloRingMasterState  g_state       = {};
MachineInfo             g_machineInfo = {};

/* Functions */

/*
** Function: PrintUsage
** @brief    Prints command-line usage help.
** @param    prog - argv[0], the program's own invocation name
*/
static void PrintUsage(const char* prog)
{
    printf("Usage: %s [options]\n\n", prog);
    printf("  --board-size N    Board size: 4, 6, or 8                     [default: 6]\n");
    printf("  --drives LETTERS  Drive letters to use, e.g. DEFY           [default: DEFY]\n");
    printf("  --store-drive L   Drive letter for NAS/store output          [default: Y]\n");
    printf("  --store-dir PATH  Sub-path on store drive (no drive letter)  [default: \\OthelloRingMaster\\Store]\n");
    printf("  --cache-dir PATH  Full path for logs and drive-bench cache   [default: C:\\OthelloRingMaster\\Cache]\n");
    printf("  --port N          Stats listener TCP port                    [default: 17532]\n");
    printf("  --compress        Compress writer + intermediate merge files as .rsfz [default]\n");
    printf("  --compress-store-only  Compress intermediate merge files only; writer files stay .rsf\n");
    printf("  --no-compress     Write writer + intermediate merge files as .rsf (uncompressed)\n");
    printf("                    (a level's actual permanent store output is always compressed via\n");
    printf("                    the ring nested-index's own delta+varint+LZ4 tier regardless of this)\n");
    printf("  --lz4-drives DEF  Drive letters that get LZ4 on top of varint (.rsfzl) [default: DEF]\n");
    printf("                    Only applies when --compress is active. Use \"\" to disable.\n");
    printf("  --memory-limit SZ Force the memory budget instead of using real free RAM\n");
    printf("                    (e.g. \"4GB\", \"512MB\") -- testing/validation only, e.g. to force\n");
    printf("                    more frequent merge-writer flushes at small levels. Still capped\n");
    printf("                    by actual free RAM. [default: unset, uses recommended-vs-free-RAM]\n");
    printf("  --checkpoint-interval-hours H\n");
    printf("                    Mid-level checkpoint interval in hours (fractional allowed, e.g.\n");
    printf("                    0.05 for testing). <= 0 disables periodic checkpointing. [default: 5]\n");
    printf("  --drive-space-low-gb N\n");
    printf("                    Override the free-space threshold that triggers a cross-drive\n");
    printf("                    iMerge -- testing/validation only, to force real iMerge activity\n");
    printf("                    on a small real drive/level. [default: 20, DRIVE_SPACE_LOW_GB]\n");
    printf("  --max-file-size-gb N\n");
    printf("                    Override the consolidation-eligibility size cap -- testing/\n");
    printf("                    validation only. Must be a comfortable multiple (5-10x) of what an\n");
    printf("                    individual flushed file actually comes out to under whatever\n");
    printf("                    --memory-limit is set, or nothing will ever be eligible to merge.\n");
    printf("                    [default: 100, CONSOLIDATION_SIZE_CAP_GB]\n");
    printf("  --audit-interval-seconds N\n");
    printf("                    Override the interval for both background auditors (registry-vs-\n");
    printf("                    disk, drive-space reconciliation) -- testing/validation only, to\n");
    printf("                    observe them fire within a short run. [default: 120]\n");
    printf("  --help            Show this help\n\n");
    printf("Auto-resume: if storeDir already contains level files from a previous run,\n");
    printf("  the solver automatically resumes from the first missing level.\n");
    printf("  To start fresh, delete or move the storeDir manually.\n\n");
}

/*
** Function: ParseArgs
** @brief    Parses command-line arguments into g_config, applying defaults first.
** @param    argc - argument count
** @param    argv - argument values
*/
static void ParseArgs(int argc, char* argv[])
{
    /* Defaults */
    g_config.boardSize    = 6;
    g_config.storeDrive   = 'Y';
    g_config.statsPort    = 17532;
    g_config.compressMode = COMPRESS_ALL;
    strncpy(g_config.useDrives,           "DEFY",                          sizeof(g_config.useDrives)           - 1);
    strncpy(g_config.cacheDirName,        "C:\\OthelloRingMaster\\Cache",  sizeof(g_config.cacheDirName)        - 1);
    strncpy(g_config.storeDirNameNoDrive, "\\OthelloRingMaster\\Store",    sizeof(g_config.storeDirNameNoDrive) - 1);
    strncpy(g_config.lz4Drives,           "DEFY",                         sizeof(g_config.lz4Drives)           - 1);
    g_config.memoryLimitBytes = 0;   /* 0 = no override, use MM_RECOMMENDED against real free RAM */
    g_config.checkpointIntervalHours = 5.0;
    g_config.driveSpaceLowGBOverride      = 0;   /* 0 = use DRIVE_SPACE_LOW_GB */
    g_config.maxFileSizeGBOverride        = 0;   /* 0 = use CONSOLIDATION_SIZE_CAP_GB */
    g_config.auditIntervalSecondsOverride = 0;   /* 0 = use AUDIT_INTERVAL_SECONDS_DEFAULT */

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            PrintUsage(argv[0]);
            exit(0);
        }

#define REQUIRE_NEXT(flag) \
        if (++i >= argc) { printf("ERROR: %s requires a value\n", flag); exit(1); }

        if (strcmp(argv[i], "--compress") == 0)
        {
            g_config.compressMode = COMPRESS_ALL;
        }
        else if (strcmp(argv[i], "--compress-store-only") == 0)
        {
            g_config.compressMode = COMPRESS_STORE_ONLY;
        }
        else if (strcmp(argv[i], "--no-compress") == 0)
        {
            g_config.compressMode = COMPRESS_NONE;
        }
        else if (strcmp(argv[i], "--lz4-drives") == 0)
        {
            REQUIRE_NEXT("--lz4-drives")
            memset(g_config.lz4Drives, 0, sizeof(g_config.lz4Drives));
            for (const char* p = argv[i]; *p; p++)
                if (isalpha((unsigned char)*p))
                {
                    char ul = (char)toupper((unsigned char)*p);
                    if (!strchr(g_config.lz4Drives, ul))
                    {
                        size_t len = strlen(g_config.lz4Drives);
                        if (len + 1 < sizeof(g_config.lz4Drives))
                            g_config.lz4Drives[len] = ul;
                    }
                }
        }
        else if (strcmp(argv[i], "--board-size") == 0)
        {
            REQUIRE_NEXT("--board-size")
            int n = atoi(argv[i]);
            if (n != 4 && n != 6 && n != 8) { printf("ERROR: --board-size must be 4, 6, or 8\n"); exit(1); }
            g_config.boardSize = (uint8_t)n;
        }
        else if (strcmp(argv[i], "--drives") == 0)
        {
            REQUIRE_NEXT("--drives")
            strncpy(g_config.useDrives, argv[i], sizeof(g_config.useDrives) - 1);
        }
        else if (strcmp(argv[i], "--store-drive") == 0)
        {
            REQUIRE_NEXT("--store-drive")
            g_config.storeDrive = (char)toupper((unsigned char)argv[i][0]);
        }
        else if (strcmp(argv[i], "--store-dir") == 0)
        {
            REQUIRE_NEXT("--store-dir")
            strncpy(g_config.storeDirNameNoDrive, argv[i], sizeof(g_config.storeDirNameNoDrive) - 1);
        }
        else if (strcmp(argv[i], "--cache-dir") == 0)
        {
            REQUIRE_NEXT("--cache-dir")
            strncpy(g_config.cacheDirName, argv[i], sizeof(g_config.cacheDirName) - 1);
        }
        else if (strcmp(argv[i], "--port") == 0)
        {
            REQUIRE_NEXT("--port")
            g_config.statsPort = (uint16_t)atoi(argv[i]);
        }
        else if (strcmp(argv[i], "--memory-limit") == 0)
        {
            REQUIRE_NEXT("--memory-limit")
            uint64_t n = ParseMemorySize(argv[i]);
            if (n == 0) { printf("ERROR: --memory-limit could not parse '%s'\n", argv[i]); exit(1); }
            g_config.memoryLimitBytes = n;
        }
        else if (strcmp(argv[i], "--checkpoint-interval-hours") == 0)
        {
            REQUIRE_NEXT("--checkpoint-interval-hours")
            g_config.checkpointIntervalHours = atof(argv[i]);
        }
        else if (strcmp(argv[i], "--drive-space-low-gb") == 0)
        {
            REQUIRE_NEXT("--drive-space-low-gb")
            g_config.driveSpaceLowGBOverride = (uint64_t)atoi(argv[i]);
        }
        else if (strcmp(argv[i], "--max-file-size-gb") == 0)
        {
            REQUIRE_NEXT("--max-file-size-gb")
            g_config.maxFileSizeGBOverride = (uint64_t)atoi(argv[i]);
        }
        else if (strcmp(argv[i], "--audit-interval-seconds") == 0)
        {
            REQUIRE_NEXT("--audit-interval-seconds")
            g_config.auditIntervalSecondsOverride = (uint32_t)atoi(argv[i]);
        }
        else
        {
            printf("ERROR: unknown argument '%s'\n\n", argv[i]);
            PrintUsage(argv[0]);
            exit(1);
        }

#undef REQUIRE_NEXT
    }
}

/*
** Function: WriteSentinelStats
** @brief    Writes a _complete sentinel file with a magic header followed by
**           the full LevelStats payload, so a future restart can restore
**           the history table without re-solving completed levels.
** @param    path - sentinel file path
** @param    ls   - the level's stats to persist
*/
static void WriteSentinelStats(const char* path, const LevelStats* ls)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    uint64_t magic = RSF_SENTINEL_STATS_MAGIC;
    DWORD nw;
    WriteFile(h, &magic, (DWORD)sizeof(magic), &nw, NULL);
    WriteFile(h, ls,     (DWORD)sizeof(*ls),   &nw, NULL);
    CloseHandle(h);
}

/*
** Function: CtrlHandler
** @brief    Console Ctrl+C/Ctrl+Break handler: requests graceful shutdown
**           instead of an abrupt process kill.
** @param    dwCtrlType - the console control event type
** @return   TRUE if handled (Ctrl+C/Break), FALSE otherwise (let the default handler run).
*/
static BOOL WINAPI CtrlHandler(DWORD dwCtrlType)
{
    if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT)
    {
        LoggerLog("Ctrl+C received - requesting graceful shutdown...\n");
        g_state.terminateThreads       = true;
        g_state.terminateConsolidation = true;

        /* The consolidation master sleeps on this CV until woken -- without
        ** this it wouldn't notice terminateThreads until its next natural
        ** wake (flush/consolidator/iMerge completion), which may never come
        ** once shutdown is underway.
        */
        {
            std::lock_guard<std::mutex> lock(g_state.consolidationMasterMutex);
            g_state.consolidationMasterWake = true;
        }
        g_state.consolidationMasterCV.notify_all();
        return TRUE;
    }
    return FALSE;
}

/* WaitForPoolIdle lives in Utility/ThreadPool.h -- shared with the
** checkpoint orchestration code (Checkpoint.cpp) and this file's own
** solve->merge transition, both of which need the identical wait behavior
** when draining pMergeWriterPool/pFlusherPool/pIMergePool/pConsolidatorPool.
*/

/*
** ============================================================
** Per-level summary line
**
** Columns:
**   Lv  BoardsIn  NewBoards  Pass  GpuDups  MrgDups  UniqueOut  Ends  Fls
**   SlvGB  MrgGB  SlvTm  MrgTm  TotTm  ns/brd  DateTime
** ============================================================
*/

/*
** Function: PrintLevelStatsHeader
** @brief    Logs the column header row for the per-level summary table.
*/
static void PrintLevelStatsHeader()
{
    LoggerLog(
        "\n  Lv        BoardsIn       NewBoards         Pass         GpuDups"
        "         MrgDups       UniqueOut      Ends  MaxMv    Fls      SlvGB"
        "      MrgGB    SlvTm(s)    MrgTm(s)    TotTm(s)       SlvNs/b"
        "       MrgNs/b       TotNs/b  DateTime\n"
        "  --  --------------  --------------  ----------  --------------"
        "  --------------  --------------  --------  -----  -----  ---------"
        "  ---------  ----------  ----------  ----------  ------------"
        "  ------------  ------------  -------------------\n"
    );
}

/*
** Function: LogLevelSummary
** @brief    Logs one level's completed-stats summary row (plus a per-drive
**           breakdown line for each writer drive and the store drive).
** @param    level - the level to summarize
** @param    pCtx  - solve context
*/
static void LogLevelSummary(int level, PSolveContext pCtx)
{
    POthelloRingMasterState pSt = pCtx->pState;
    const LevelStats*       ls  = &pSt->levelStats[level];

    uint64_t uniqueOut = (ls->boardsWrittenToDisk >= ls->mrgDupsRemoved)
                         ? ls->boardsWrittenToDisk - ls->mrgDupsRemoved : 0;
    double   slvTm     = ls->solverNanos / 1.0e9;
    double   mrgTm     = (ls->totalNanos - ls->solverNanos) / 1.0e9;
    double   totTm     = ls->totalNanos  / 1.0e9;
    uint64_t brdCount  = ls->boardsReadFromStore > 0 ? ls->boardsReadFromStore : 1;
    uint64_t slvNsBrd  = (uint64_t)(ls->solverNanos / (int64_t)brdCount);
    uint64_t mrgNsBrd  = (uint64_t)((ls->totalNanos - ls->solverNanos) / (int64_t)brdCount);
    uint64_t totNsBrd  = (uint64_t)(ls->totalNanos  / (int64_t)brdCount);
    double   slvGB     = ls->mwBytes          / (1024.0 * 1024.0 * 1024.0);
    double   mrgGB     = ls->mergeActualBytes / (1024.0 * 1024.0 * 1024.0);
    double   mrgEquivGB = ls->mergeBytes      / (1024.0 * 1024.0 * 1024.0);

    LoggerLog(
        "  %2d  %14llu  %14llu  %10llu  %14llu  %14llu  %14llu"
        "  %8llu  %5u  %5llu  %9.2f  %9.2f  %10.3f  %10.3f  %10.3f"
        "  %12llu  %12llu  %12llu  %s\n",
        level,
        (unsigned long long)(ls->boardsReadFromStore + ls->passBoards),
        (unsigned long long)ls->boardsGenerated,
        (unsigned long long)ls->passBoards,
        (unsigned long long)ls->gpuDupsRemoved,
        (unsigned long long)ls->mrgDupsRemoved,
        (unsigned long long)uniqueOut,
        (unsigned long long)ls->terminalBoards,
        ls->maxMovesInLevel,
        (unsigned long long)ls->gpuFlushes,
        slvGB, mrgGB,
        slvTm, mrgTm, totTm,
        (unsigned long long)slvNsBrd,
        (unsigned long long)mrgNsBrd,
        (unsigned long long)totNsBrd,
        ls->completedAt
    );

    /* Per-drive breakdown from snapshot captured at level completion */
    for (int i = 0; i < ls->numDriveSnapshot; i++)
    {
        const WriterDriveStats* d = &ls->driveSnapshot[i];
        if (d->levelBytesUncompressed > 0
            && d->levelBytesUncompressed != d->levelBytesWritten)
            LoggerLog("      %c:  files=%4llu  %8.2f GB on disk  (%8.2f GB uncomp)  free=%9.2f GB\n",
                      d->driveLetter,
                      (unsigned long long)d->levelFilesWritten,
                      d->levelBytesWritten      / (1024.0 * 1024.0 * 1024.0),
                      d->levelBytesUncompressed / (1024.0 * 1024.0 * 1024.0),
                      d->lastFreeBytes          / (1024.0 * 1024.0 * 1024.0));
        else
            LoggerLog("      %c:  files=%4llu  %8.2f GB  free=%9.2f GB\n",
                      d->driveLetter,
                      (unsigned long long)d->levelFilesWritten,
                      d->levelBytesWritten / (1024.0 * 1024.0 * 1024.0),
                      d->lastFreeBytes     / (1024.0 * 1024.0 * 1024.0));
    }
    if (ls->mergeActualBytes > 0 && ls->mergeActualBytes != ls->mergeBytes)
        LoggerLog("      %c:  files=%4u  %8.2f GB on disk  (%8.2f GB uncomp)  free=%9.2f GB\n",
                  pCtx->pConfig->storeDrive,
                  ls->mergeFilesWritten,
                  mrgGB, mrgEquivGB,
                  ls->storeFreeBytes / (1024.0 * 1024.0 * 1024.0));
    else
        LoggerLog("      %c:  files=%4u  %8.2f GB  free=%9.2f GB\n",
                  pCtx->pConfig->storeDrive,
                  ls->mergeFilesWritten,
                  mrgGB,
                  ls->storeFreeBytes / (1024.0 * 1024.0 * 1024.0));
    LoggerLog("\n");
}

/*
** Function: main
** @brief    Parses args, runs the ring boundary-conversion self-test, then
**           drives the per-level solve loop until every level up to the
**           board's max is complete.
** @param    argc - argument count
** @param    argv - argument values
** @return   0 on a clean finish; 1 if the ring self-test fails.
*/
int main(int argc, char* argv[])
{
    ParseArgs(argc, argv);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    InitLogger(&g_config, &g_state);

    /* Real correctness check, not the whole program's purpose: prove the
    ** ring<->row-major GPU boundary conversion is still valid on this
    ** machine's GPU before trusting it with a real solve. A silently wrong
    ** permutation table would corrupt every board from here on with no
    ** other symptom, so this fatals rather than continuing.
    */
    OBCuda_InitRingPermutationTables();
    bool ringOk = OBCuda_TestRingRoundTrip();
    LoggerLog("Ring<->row-major GPU boundary conversion: %s\n", ringOk ? "PASS" : "FAIL");
    if (!ringOk)
        return 1;

    InitSolver(&g_config, &g_state, &g_machineInfo);
    CreateSeedFile(&g_config, &g_state);

    SolveContext ctx = { &g_config, &g_state, &g_machineInfo };
    SubmitStatsListenerJob(&ctx);

    /* Own dedicated threads for the whole run (not per-level, unlike the
    ** consolidation master) -- both need a stable PSolveContext, which is
    ** why they start here rather than inside InitSolver (see InitSolver.cpp's
    ** comment right above where these were deliberately NOT started).
    */
    g_state.registryAuditorThread   = std::thread(RegistryAuditorLoop, &ctx);
    g_state.driveSpaceAuditorThread = std::thread(DriveSpaceAuditorLoop, &ctx);

    /* 4 pieces are pre-placed at game start; each level adds one piece.
    ** The last level (all squares filled) generates no children but counts
    ** terminal boards.
    */
    const int maxLevel   = g_config.boardSize * g_config.boardSize - 3;
    const int startLevel = g_state.resumeLevel;

    if (startLevel > 0)
        LoggerLog("Resuming from level %d (levels 0..%d already complete).\n",
                  startLevel, startLevel - 1);

    PrintLevelStatsHeader();

    /* Print restored history for levels already completed before this run.
    ** Stats are loaded from the _complete sentinel files by ScanForResumeLevel.
    ** Levels whose sentinels were zero-byte (legacy / manual) have
    ** totalNanos==0 and are silently skipped.
    */
    for (int lvl = 0; lvl < startLevel; lvl++)
        if (g_state.levelStats[lvl].totalNanos > 0)
            LogLevelSummary(lvl, &ctx);

    for (int level = startLevel; level < maxLevel && !g_state.terminateThreads; level++)
    {
        g_state.playLevel = (uint8_t)level;

        /* Mid-level checkpointing (Checkpoint.h): fresh interval window and
        ** no pending on-demand request at the start of every level.
        */
        g_state.checkpointIntervalStartTickMs = GetTickCount64();
        g_state.checkpointRequestedNow        = false;
        g_state.resumeSkipActive              = false;   /* STATUS resume banner off until a sub-pass sets it */
        g_state.resumeSkipTotal               = 0;
        g_state.resumeSkipDone                = 0;

        /* Reset per-level per-thread state -- EXCEPT the registry/naming
        ** counter when this exact level is resuming from a validated
        ** mid-level checkpoint (Checkpoint.h): InitSolver already rebuilt
        ** the registry from a real disk scan and restored nextFileIdx from
        ** the checkpoint's own snapshot, and clearing either here would
        ** make RegistryNextFileIdx hand out index 0 again, colliding with
        ** (and silently overwriting) the real writer files already sitting
        ** on disk from before the pause. Every other per-level counter here
        ** is purely a display metric, harmless to reset.
        */
        bool resumingThisLevelFromCheckpoint = g_state.resumeFromCheckpoint && level == g_state.resumeLevel;
        g_state.consolidationBytesInput = 0;
        if (!resumingThisLevelFromCheckpoint)
            for (int i = 0; i < g_state.numMergeWriters; i++)
                RegistryResetForLevel(&g_state, i);

        g_state.consolidatorFreeCount = CONSOLIDATOR_POOL_THREADS;
        for (int w = 0; w < CONSOLIDATOR_POOL_THREADS; w++)
            g_state.consolSlot[w] = {};   /* clear live per-worker consolidation progress (STATUS display) */

        /* The consolidation master thread genuinely exits its loop whenever
        ** terminateConsolidation is set (see ConsolidationMasterLoop's doc
        ** comment) -- the previous level's solve->merge transition below
        ** set it and joined the thread, so it must be started fresh here
        ** for every level, including the very first.
        */
        g_state.terminateConsolidation = false;
        g_state.consolidationMasterThread = std::thread(ConsolidationMasterLoop, &ctx);

        for (int p = 0; p < 2; p++)
        {
            g_state.imergeActive[p]          = 0;
            g_state.imergeTotalInputBytes[p] = 0;
            g_state.imergeDoneInputBytes[p]  = 0;
            g_state.imergeFileCount[p]       = 0;
        }
        g_state.spaceRelief.active = false;   /* no relief in progress at a fresh level start */
        g_state.activeFlushWriters = 0;
        for (int i = 0; i < g_state.numMergeWriters; i++)
            for (int p = 0; p < 2; p++)
            {
                g_state.mwFlushActive[i][p]     = 0;
                g_state.mwFlushTotalBytes[i][p] = 0;
                g_state.mwFlushDoneBytes[i][p]  = 0;
            }
        for (int i = 0; i < g_state.numMergeDirs; i++)
        {
            g_state.mergeFileBlackCount[i]  = 0;
            g_state.mergeFileWhiteCount[i]  = 0;
            g_state.mergeFileBytesBlack[i]  = 0;
            g_state.mergeFileBytesWhite[i]  = 0;
            g_state.mergeFileUncompBlack[i] = 0;
            g_state.mergeFileUncompWhite[i] = 0;
        }
        g_state.storeMergeBlackFileCount    = 0;
        g_state.storeMergeWhiteFileCount    = 0;
        g_state.storeMergeBytesWritten      = 0;
        g_state.storeMergeBytesUncompressed = 0;
        g_state.currentLevelTotalBoards     = 0;
        for (int i = 0; i < g_state.numWriterDrives; i++)
        {
            g_state.writerDriveStats[i].levelFilesWritten      = 0;
            g_state.writerDriveStats[i].levelBytesWritten      = 0;
            g_state.writerDriveStats[i].levelBytesUncompressed = 0;
        }
        g_state.levelStats[level] = {};
        ClockStart(&g_state.levelStats[level].startTick);

        /* Re-initialize ledgers from the OS at each level start. NVMe and
        ** merge drives should be empty (prior level cleaned up); the store
        ** drive reflects permanent store accumulation. The safety buffer is
        ** re-applied so space decisions never consume the last bytes.
        */
        for (int i = 0; i < g_state.numMergeWriters; i++)
            DriveInitLedger(&g_state, g_state.mwDirectory[i][0], g_config.driveSpaceLowGBOverride);
        for (int i = 0; i < g_state.numMergeDirs; i++)
            DriveInitLedger(&g_state, g_state.mergeDirectory[i][0]);
        DriveInitLedger(&g_state, g_config.storeDrive);

        g_state.currentPhase = "GPU solving";
        SubmitGpuFeederJob(&ctx, (uint8_t)level);
        WaitForPoolIdle(g_state.pGPUFeederThreadPool);

        /* Drain every pool that could still be touching a writer-drive file,
        ** same order as PerformMidLevelCheckpoint's mid-level drain
        ** (Checkpoint.cpp) -- here it's a full level-end drain rather than a
        ** pause, so the consolidation master is stopped and NOT restarted;
        ** the per-level reset block at the top of the next iteration starts
        ** a fresh one. Must finish before DoEndOfLevelMerge starts
        ** enumerating writer-drive files, so nothing is still mid-write or
        ** mid-consolidate at that moment.
        */
        WaitForPoolIdle(g_state.pMergeWriterPool);
        if (!g_state.terminateThreads)
        {
            g_state.currentPhase = "Flushing buffers";
            FlushAllMergeWriterBuffers(&ctx);
        }
        WaitForPoolIdle(g_state.pFlusherPool);
        WaitForPoolIdle(g_state.pIMergePool);
        ConsolidationMasterStop(&ctx);
        WaitForPoolIdle(g_state.pConsolidatorPool);
        g_state.levelStats[level].solverNanos =
            ClockNanosSinceStart(&g_state.levelStats[level].startTick);

        if (!g_state.terminateThreads)
        {
            /* Consolidate all NVMe writer files + medium-drive merge files -> single store file */
            g_state.currentPhase = "Merging to store";
            DoEndOfLevelMerge(&ctx);
            g_state.levelStats[level].totalNanos =
                ClockNanosSinceStart(&g_state.levelStats[level].startTick);

            g_state.levelStats[level].storeFreeBytes =
                (uint64_t)DriveAvailable(&g_state, g_config.storeDrive);

            /* Populate lastFreeBytes from ledger before snapshotting for the history table */
            for (int i = 0; i < g_state.numWriterDrives; i++)
                g_state.writerDriveStats[i].lastFreeBytes =
                    (uint64_t)DriveAvailable(&g_state, g_state.writerDriveStats[i].driveLetter);

            /* Snapshot per-drive stats before they're reset at the next level's start */
            g_state.levelStats[level].numDriveSnapshot = g_state.numWriterDrives;
            for (int i = 0; i < g_state.numWriterDrives; i++)
                g_state.levelStats[level].driveSnapshot[i] = g_state.writerDriveStats[i];

            SYSTEMTIME _st = {};
            GetLocalTime(&_st);
            snprintf(g_state.levelStats[level].completedAt,
                     sizeof(g_state.levelStats[level].completedAt),
                     "%04d-%02d-%02d %02d:%02d:%02d",
                     _st.wYear, _st.wMonth, _st.wDay,
                     _st.wHour, _st.wMinute, _st.wSecond);

            /* Write _complete sentinel with full stats payload so a future
            ** restart can restore the history table without re-solving
            ** completed levels.
            */
            char sentComplete[MAX_FULL_PATH_NAME];
            SentinelNameComplete(sentComplete, sizeof(sentComplete),
                                 g_state.storeDirectory, (int)g_config.boardSize, level + 1);
            WriteSentinelStats(sentComplete, &g_state.levelStats[level]);

            /* This level's own mid-level checkpoint (if any) is obsolete now
            ** that the whole level -- solve AND merge -- is durably complete.
            */
            DeleteLevelCheckpoint(&ctx, level);
        }
        else
        {
            g_state.levelStats[level].totalNanos =
                ClockNanosSinceStart(&g_state.levelStats[level].startTick);
        }

        g_state.currentPhase = nullptr;

        LogLevelSummary(level, &ctx);
    }

    /* Print completed-level history table */
    if (g_state.playLevel > 0)
    {
        LoggerLog("\n--- Completed level history ---\n");
        PrintLevelStatsHeader();
        for (int lvl = startLevel; lvl <= (int)g_state.playLevel; lvl++)
            LogLevelSummary(lvl, &ctx);
    }

    CleanupSolver(&g_state);
    return 0;
}
