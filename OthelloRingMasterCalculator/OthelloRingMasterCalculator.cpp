/*
** Filename:  OthelloRingMasterCalculator.cpp
**
** Purpose:
**   Entry point for OthelloRingMasterCalculator: parses CLI args, opens
**   the logger, finds the deepest completed level in RingMaster's
**   finished store, starts the status listener, and runs the full
**   backward walk (deepest level down to 0, sentinel-based resumability)
**   against it -- see project_retrograde_calculator_implementation_plan
**   memory for the phase breakdown this completes through Phase 5.
**   Phase 6+ (4x4 validation, real 6x6 run) are not implemented yet.
**
** Notes:
**   Mirrors OthelloRingMaster.cpp's CLI-parsing shape (same option style,
**   same defaults-then-override pattern), scoped down to exactly what
**   OthelloRingMasterCalculatorConfig actually holds today.
*/

/* Includes */
#include "CalculatorTypes.h"
#include "CalculatorInitLogger.h"
#include "StoreLevelScan.h"
#include "BackwardWalkDriver.h"
#include "CalculatorStatsListener.h"
#include "CalcDriveLedger.h"
#include "CounterWidthConfig.h"
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <thread>

/* Globals */
OthelloRingMasterCalculatorConfig g_config = {};
OthelloRingMasterCalculatorState  g_state  = {};

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
    printf("  --store-drive L   Drive letter holding RingMaster's finished store [default: Y]\n");
    printf("  --store-dir PATH  Sub-path on store drive (no drive letter)  [default: \\OthelloRingMaster\\Store]\n");
    printf("  --counts-drive L  Drive letter this calculator writes its own counts files to [default: Y]\n");
    printf("  --counts-dir PATH Sub-path on counts drive (no drive letter) [default: \\OthelloRingMasterCalculator\\Counts]\n");
    printf("  --cache-dir PATH  Full path for logs and the width-config file [default: C:\\OthelloRingMasterCalculator\\Cache]\n");
    printf("  --use-drives STR  Drive letters available for segmented scratch (e.g. DEFG) [default: auto-enumerate all fixed local drives]\n");
    printf("  --scratch-dir PATH Sub-path (on whichever scratch drive) segments are written under [default: \\OthelloRingMasterCalculator\\Scratch]\n");
    printf("  --port N          Stats listener TCP port                    [default: 17632]\n");
    printf("  --help            Show this help\n\n");
}

/*
** Function: ParseArgs
** @brief    Parses command-line arguments into g_config, applying defaults first.
** @param    argc - argument count
** @param    argv - argument values
*/
static void ParseArgs(int argc, char* argv[])
{
    g_config.boardSize = 6;
    g_config.storeDrive = 'Y';
    strncpy(g_config.storeDirNameNoDrive, "\\OthelloRingMaster\\Store", sizeof(g_config.storeDirNameNoDrive) - 1);
    g_config.countsDrive = 'Y';
    strncpy(g_config.countsDirNameNoDrive, "\\OthelloRingMasterCalculator\\Counts", sizeof(g_config.countsDirNameNoDrive) - 1);
    strncpy(g_config.cacheDirName, "C:\\OthelloRingMasterCalculator\\Cache", sizeof(g_config.cacheDirName) - 1);
    g_config.useDrives[0] = '\0';
    strncpy(g_config.scratchDirNameNoDrive, "\\OthelloRingMasterCalculator\\Scratch", sizeof(g_config.scratchDirNameNoDrive) - 1);
    g_config.statsPort = 17632;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0)
        {
            PrintUsage(argv[0]);
            exit(0);
        }
        else if (strcmp(argv[i], "--board-size") == 0 && i + 1 < argc)
        {
            int n = atoi(argv[++i]);
            if (n != 4 && n != 6 && n != 8) { printf("ERROR: --board-size must be 4, 6, or 8\n"); exit(1); }
            g_config.boardSize = (uint8_t)n;
        }
        else if (strcmp(argv[i], "--store-drive") == 0 && i + 1 < argc)
            g_config.storeDrive = (char)toupper((unsigned char)argv[++i][0]);
        else if (strcmp(argv[i], "--store-dir") == 0 && i + 1 < argc)
            strncpy(g_config.storeDirNameNoDrive, argv[++i], sizeof(g_config.storeDirNameNoDrive) - 1);
        else if (strcmp(argv[i], "--counts-drive") == 0 && i + 1 < argc)
            g_config.countsDrive = (char)toupper((unsigned char)argv[++i][0]);
        else if (strcmp(argv[i], "--counts-dir") == 0 && i + 1 < argc)
            strncpy(g_config.countsDirNameNoDrive, argv[++i], sizeof(g_config.countsDirNameNoDrive) - 1);
        else if (strcmp(argv[i], "--cache-dir") == 0 && i + 1 < argc)
            strncpy(g_config.cacheDirName, argv[++i], sizeof(g_config.cacheDirName) - 1);
        else if (strcmp(argv[i], "--use-drives") == 0 && i + 1 < argc)
            strncpy(g_config.useDrives, argv[++i], sizeof(g_config.useDrives) - 1);
        else if (strcmp(argv[i], "--scratch-dir") == 0 && i + 1 < argc)
            strncpy(g_config.scratchDirNameNoDrive, argv[++i], sizeof(g_config.scratchDirNameNoDrive) - 1);
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            g_config.statsPort = (uint16_t)atoi(argv[++i]);
        else
        {
            fprintf(stderr, "ERROR: unknown argument '%s'\n\n", argv[i]);
            PrintUsage(argv[0]);
            exit(1);
        }
    }
}

/*
** Function: main
** @brief    Parses CLI args, opens the logger, finds the deepest completed
**           level in RingMaster's store, starts the status listener, and
**           runs the full backward walk (deepest level down to 0) against it.
** @param    argc - argument count
** @param    argv - argument values
** @return   0 on success, 1 on argument or precondition error.
*/
int main(int argc, char* argv[])
{
    ParseArgs(argc, argv);

    snprintf(g_state.storeDirectory, sizeof(g_state.storeDirectory), "%c:%s",
             g_config.storeDrive, g_config.storeDirNameNoDrive);
    snprintf(g_state.countsDirectory, sizeof(g_state.countsDirectory), "%c:%s",
             g_config.countsDrive, g_config.countsDirNameNoDrive);
    strncpy(g_state.cacheDirectory, g_config.cacheDirName, sizeof(g_state.cacheDirectory) - 1);

    CalculatorInitLogger(&g_config, &g_state);

    LoggerLog("  Board size    : %dx%d\n", g_config.boardSize, g_config.boardSize);
    LoggerLog("  Store dir     : %s\n", g_state.storeDirectory);
    LoggerLog("  Counts dir    : %s\n", g_state.countsDirectory);
    LoggerLog("  Cache dir     : %s\n", g_state.cacheDirectory);
    LoggerLog("  Stats port    : %d\n", g_config.statsPort);

    int deepestLevel = FindDeepestCompleteLevel(g_state.storeDirectory, g_config.boardSize);
    if (deepestLevel < 0)
    {
        fprintf(stderr, "ERROR: no completed levels found under '%s' -- run OthelloRingMaster first\n", g_state.storeDirectory);
        return 1;
    }

    LoggerLog("Deepest completed level: %d\n", deepestLevel);

    CounterWidthConfig widthConfig;
    CounterWidthConfigLoad(&widthConfig, g_state.cacheDirectory, g_config.boardSize);

    /* Probe/benchmark drives once at startup (cached in cacheDirectory,
    ** same as RingMaster's own driveinfo.json) and seed the scratch
    ** ledger for every drive it found -- both level+1's lookup-source
    ** segments and this level's own output segments draw reservations
    ** from this same ledger for the rest of the run.
    */
    GetDriveInformation(&g_state.driveInfo, g_state.cacheDirectory,
                        g_config.useDrives[0] ? g_config.useDrives : nullptr);
    for (int i = 0; i < g_state.driveInfo.numDrives; i++)
    {
        const DriveInformation* d = &g_state.driveInfo.drives[i];
        if (d->available)
            CalcDriveInitLedger(&g_state, d->driveLetter);
    }

    g_state.pStatsThreadPool = new ThreadPool(1, "CalculatorStatsThreadPool");
    if (!g_state.pStatsThreadPool)
        Fatal(FATAL_ALLOCATION_FAILED, "main: cannot create stats thread pool");
    g_state.pStatsThreadPool->Start();
    g_state.pStatsThreadPool->WaitUntilReady();

    /* Parallelizes per-parent child lookups (real disk seeks against
    ** segmented scratch now, not just vector indexing) -- see
    ** NonTerminalLevelStep.cpp's flushBatch.
    */
    uint32_t lookupThreads = std::thread::hardware_concurrency();
    if (lookupThreads < 4) lookupThreads = 4;
    g_state.pLookupThreadPool = new ThreadPool(lookupThreads, "CalculatorLookupThreadPool");
    if (!g_state.pLookupThreadPool)
        Fatal(FATAL_ALLOCATION_FAILED, "main: cannot create lookup thread pool");
    g_state.pLookupThreadPool->Start();
    g_state.pLookupThreadPool->WaitUntilReady();

    CalculatorContext ctx = { &g_config, &g_state };
    SubmitCalculatorStatsListenerJob(&ctx);

    RunBackwardWalk(&g_config, &g_state, &widthConfig, deepestLevel);

    g_state.terminateStatsListener = true;
    g_state.pStatsThreadPool->Stop();
    delete g_state.pStatsThreadPool;

    g_state.pLookupThreadPool->Stop();
    delete g_state.pLookupThreadPool;

    return 0;
}
