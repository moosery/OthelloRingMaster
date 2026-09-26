/*
** Filename:  OthelloRingMasterCalculatorCountsStats.cpp
**
** Purpose:
**   Entry point for OthelloRingMasterCalculatorCountsStats: a small,
**   read-only command-line tool that scans a finished (or in-progress)
**   OthelloRingMasterCalculator counts directory and prints one CSV row
**   per completed level -- board counts (split by color, plus their sum),
**   the level's confirmed counter-tier width, real compressed bytes, real
**   decompressed bytes (from actually decompressing each color's LZ4
**   stream, never derived/estimated), compression ratio, percent
**   reduction, and bits/board. Board counts and tier width come straight
**   from each level's own calc_complete sentinel -- ground truth the
**   calculator already recorded, not re-derived here.
**
** Notes:
**   CLI mirrors OthelloRingMasterCalculator.exe's own --counts-drive/
**   --counts-dir/--board-size flags and defaults (Y /
**   \OthelloRingMasterCalculator\Counts / 6), so running this tool with no
**   arguments points at the same counts directory a default-configured
**   OthelloRingMasterCalculator.exe run would be writing to. Deliberately
**   does not link against the OthelloRingMasterCalculator project itself
**   (which pulls in a CUDA dependency for its own retrograde kernels) --
**   only the small header-only pieces it actually needs (CalculatorFileName.h,
**   CalculatorTypes.h, CounterWidthConfig.h) plus Utility's generic
**   Lz4Stream.h, same "reimplement/reuse the small piece locally" precedent
**   OthelloRingMasterStoreStats already set for its own board-store scan.
*/

/* Includes */
#include "CountsStatsScan.h"
#include "CountsStatsCsv.h"
#include "FileAndDirUtils.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

/* Structures and Types */

/*
** Type:    CountsStatsConfig
** @brief   Parsed CLI arguments for this run.
*/
struct CountsStatsConfig
{
    int      boardSize                           = 6;
    char     countsDrive                         = 'Y';
    char     countsDirNameNoDrive[MAX_FULL_PATH_NAME] = "\\OthelloRingMasterCalculator\\Counts";
    char     outputPath[MAX_FULL_PATH_NAME]       = "";   /* empty = stdout */
};

/* Functions */

/*
** Function: PrintUsage
** @brief    Prints command-line usage help.
** @param    prog - argv[0], the program's own invocation name
*/
static void PrintUsage(const char* prog)
{
    printf("Usage: %s [options]\n\n", prog);
    printf("  --board-size N     Board size: 4, 6, or 8                      [default: 6]\n");
    printf("  --counts-drive L   Drive letter the counts live on             [default: Y]\n");
    printf("  --counts-dir PATH  Sub-path on counts drive (no drive letter)  [default: \\OthelloRingMasterCalculator\\Counts]\n");
    printf("  --output PATH      Write CSV to PATH instead of stdout\n");
    printf("  --help             Show this help\n\n");
    printf("Scans every completed level's calc_complete sentinel and .counts files\n");
    printf("and prints one CSV row per level: board counts, counter tier width, real\n");
    printf("compressed/decompressed bytes (the latter from actually decompressing each\n");
    printf("color's LZ4 stream), compression ratio, percent reduction, and bits/board.\n\n");
}

/*
** Function: ParseArgs
** @brief    Parses command-line arguments into pConfig, applying defaults first.
** @param    argc    - argument count
** @param    argv    - argument values
** @param    pConfig - out: parsed configuration
*/
static void ParseArgs(int argc, char* argv[], CountsStatsConfig* pConfig)
{
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            PrintUsage(argv[0]);
            exit(0);
        }

#define REQUIRE_NEXT(flag) \
        if (++i >= argc) { printf("ERROR: %s requires a value\n", flag); exit(1); }

        if (strcmp(argv[i], "--board-size") == 0)
        {
            REQUIRE_NEXT("--board-size")
            int n = atoi(argv[i]);
            if (n != 4 && n != 6 && n != 8) { printf("ERROR: --board-size must be 4, 6, or 8\n"); exit(1); }
            pConfig->boardSize = n;
        }
        else if (strcmp(argv[i], "--counts-drive") == 0)
        {
            REQUIRE_NEXT("--counts-drive")
            pConfig->countsDrive = (char)toupper((unsigned char)argv[i][0]);
        }
        else if (strcmp(argv[i], "--counts-dir") == 0)
        {
            REQUIRE_NEXT("--counts-dir")
            strncpy(pConfig->countsDirNameNoDrive, argv[i], sizeof(pConfig->countsDirNameNoDrive) - 1);
        }
        else if (strcmp(argv[i], "--output") == 0)
        {
            REQUIRE_NEXT("--output")
            strncpy(pConfig->outputPath, argv[i], sizeof(pConfig->outputPath) - 1);
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
** Function: main
** @brief    Resolves the counts directory, finds the deepest completed
**           level, and writes one CSV row per level to stdout or --output.
*/
int main(int argc, char* argv[])
{
    CountsStatsConfig config;
    ParseArgs(argc, argv, &config);

    char countsDir[MAX_FULL_PATH_NAME];
    snprintf(countsDir, sizeof(countsDir), "%c:%s", config.countsDrive, config.countsDirNameNoDrive);

    FILE* fpOut = stdout;
    if (config.outputPath[0])
    {
        fpOut = fopen(config.outputPath, "w");
        if (!fpOut)
        {
            fprintf(stderr, "ERROR: could not open '%s' for writing\n", config.outputPath);
            return 1;
        }
    }

    int deepest = CountsStatsFindDeepestCompleteLevel(countsDir, config.boardSize);
    if (deepest < 0)
    {
        fprintf(stderr, "No completed levels found under '%s' for board size %dx%d.\n",
                countsDir, config.boardSize, config.boardSize);
        if (fpOut != stdout)
            FileCloseOrFatal(fpOut, config.outputPath);
        return 1;
    }

    fprintf(stderr, "Scanning '%s' (board size %dx%d), levels 0..%d...\n",
            countsDir, config.boardSize, config.boardSize, deepest);

    CountsStatsWriteCsvHeader(fpOut);
    for (int level = 0; level <= deepest; level++)
    {
        LevelCountsStats stats;

        /* A level whose counts cannot be read still gets a row (its fields
        ** come out blank), so say so -- otherwise a blank row looks like
        ** "no data" rather than "could not read".
        */
        if (!CountsStatsScanLevel(countsDir, config.boardSize, level, &stats))
            fprintf(stderr, "WARNING: level %d: could not read the counts stats -- its row is blank\n", level);
        CountsStatsWriteCsvRow(fpOut, &stats);
    }

    if (fpOut != stdout)
        FileCloseOrFatal(fpOut, config.outputPath);

    return 0;
}
