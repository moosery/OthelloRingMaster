/*
** Filename:  OthelloRingMasterStoreStats.cpp
**
** Purpose:
**   Entry point for OthelloRingMasterStoreStats: a small, read-only
**   command-line tool that scans a finished (or in-progress) OthelloRingMaster
**   store directory's ring-store file trailers and prints one CSV row per
**   completed level -- board counts (split by color, plus their sum),
**   compressed bytes, uncompressed-equivalent bytes, compression ratio,
**   percent reduction, and bits/board. Never decompresses a file's body --
**   every figure comes from each file's 64-byte trailer (recordCount) and
**   its real on-disk size.
**
** Notes:
**   CLI mirrors OthelloRingMaster.exe's own --store-drive/--store-dir/
**   --board-size flags and defaults (Y / \OthelloRingMaster\Store / 6), so
**   running this tool with no arguments points at the same store a
**   default-configured OthelloRingMaster.exe run would be writing to.
*/

/* Includes */
#include "StoreStatsScan.h"
#include "StoreStatsCsv.h"
#include "StoreStatsRing34BitStats.h"
#include "FileAndDirUtils.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

/* Constants */
#define RING34_BITSTATS_DEFAULT_LIMIT 50000000ULL   /* records; --limit 0 means unlimited (whole file) */

/* Structures and Types */

/*
** Type:    StoreStatsConfig
** @brief   Parsed CLI arguments for this run.
*/
struct StoreStatsConfig
{
    int      boardSize                          = 6;
    char     storeDrive                         = 'Y';
    char     storeDirNameNoDrive[MAX_FULL_PATH_NAME] = "\\OthelloRingMaster\\Store";
    char     outputPath[MAX_FULL_PATH_NAME]      = "";   /* empty = stdout */

    /* --ring34-bitstats mode: a single-level diagnostic report instead of
    ** the normal per-level CSV table -- see StoreStatsRing34BitStats.h.
    */
    bool     ring34BitStatsMode                 = false;
    int      ring34BitStatsLevel                = -1;     /* required when ring34BitStatsMode is set */
    uint64_t ring34BitStatsLimit                = RING34_BITSTATS_DEFAULT_LIMIT;
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
    printf("  --board-size N    Board size: 4, 6, or 8                     [default: 6]\n");
    printf("  --store-drive L   Drive letter the store lives on            [default: Y]\n");
    printf("  --store-dir PATH  Sub-path on store drive (no drive letter)  [default: \\OthelloRingMaster\\Store]\n");
    printf("  --output PATH     Write CSV to PATH instead of stdout\n");
    printf("  --help            Show this help\n\n");
    printf("Scans every completed level's ring-store file trailers (no decompression)\n");
    printf("and prints one CSV row per level: board counts, compressed/uncompressed\n");
    printf("bytes, compression ratio, percent reduction, and bits/board.\n\n");
    printf("Diagnostic mode -- Ring_3_4 bit-occupancy report for ONE level:\n");
    printf("  --ring34-bitstats Switch to this mode instead of the normal per-level table\n");
    printf("  --level N         Level to report on (required in this mode)\n");
    printf("  --limit N         Stop after N total records, both colors combined\n");
    printf("                    [default: %llu; 0 = unlimited, read every record]\n\n", (unsigned long long)RING34_BITSTATS_DEFAULT_LIMIT);
    printf("This mode DOES decompress record bodies (delta+varint+LZ4) -- unlike the\n");
    printf("normal CSV table, which only ever reads trailers. Records are read from the\n");
    printf("start of each color's file, so a bounded --limit is a leading sample, not a\n");
    printf("uniformly-random one (the compressed stream can't be seeked into at an\n");
    printf("arbitrary offset -- only sequential decode from the start is possible).\n\n");
}

/*
** Function: ParseArgs
** @brief    Parses command-line arguments into pConfig, applying defaults first.
** @param    argc    - argument count
** @param    argv    - argument values
** @param    pConfig - out: parsed configuration
*/
static void ParseArgs(int argc, char* argv[], StoreStatsConfig* pConfig)
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
        else if (strcmp(argv[i], "--store-drive") == 0)
        {
            REQUIRE_NEXT("--store-drive")
            pConfig->storeDrive = (char)toupper((unsigned char)argv[i][0]);
        }
        else if (strcmp(argv[i], "--store-dir") == 0)
        {
            REQUIRE_NEXT("--store-dir")
            strncpy(pConfig->storeDirNameNoDrive, argv[i], sizeof(pConfig->storeDirNameNoDrive) - 1);
        }
        else if (strcmp(argv[i], "--output") == 0)
        {
            REQUIRE_NEXT("--output")
            strncpy(pConfig->outputPath, argv[i], sizeof(pConfig->outputPath) - 1);
        }
        else if (strcmp(argv[i], "--ring34-bitstats") == 0)
        {
            pConfig->ring34BitStatsMode = true;
        }
        else if (strcmp(argv[i], "--level") == 0)
        {
            REQUIRE_NEXT("--level")
            pConfig->ring34BitStatsLevel = atoi(argv[i]);
        }
        else if (strcmp(argv[i], "--limit") == 0)
        {
            REQUIRE_NEXT("--limit")
            pConfig->ring34BitStatsLimit = _strtoui64(argv[i], nullptr, 10);
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
** @brief    Resolves the store directory, finds the deepest completed
**           level, and writes one CSV row per level to stdout or --output.
*/
int main(int argc, char* argv[])
{
    StoreStatsConfig config;
    ParseArgs(argc, argv, &config);

    if (config.ring34BitStatsMode && config.ring34BitStatsLevel < 0)
    {
        printf("ERROR: --ring34-bitstats requires --level N\n");
        return 1;
    }

    char storeDir[MAX_FULL_PATH_NAME];
    snprintf(storeDir, sizeof(storeDir), "%c:%s\\storeDir", config.storeDrive, config.storeDirNameNoDrive);

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

    if (config.ring34BitStatsMode)
    {
        if (config.ring34BitStatsLimit == 0)
            fprintf(stderr, "Reading ALL Ring_3_4 records for level %d under '%s' -- this decompresses the whole level, no limit set...\n",
                    config.ring34BitStatsLevel, storeDir);
        else
            fprintf(stderr, "Reading up to %llu Ring_3_4 records for level %d under '%s'...\n",
                    (unsigned long long)config.ring34BitStatsLimit, config.ring34BitStatsLevel, storeDir);

        Ring34BitStats stats;
        StoreStatsCollectRing34BitStats(storeDir, config.boardSize, config.ring34BitStatsLevel,
                                         config.ring34BitStatsLimit, &stats);
        StoreStatsPrintRing34BitStats(fpOut, config.ring34BitStatsLevel, &stats);

        if (fpOut != stdout)
            fclose(fpOut);
        return 0;
    }

    int deepest = StoreStatsFindDeepestCompleteLevel(storeDir, config.boardSize);
    if (deepest < 0)
    {
        fprintf(stderr, "No completed levels found under '%s' for board size %dx%d.\n",
                storeDir, config.boardSize, config.boardSize);
        if (fpOut != stdout)
            fclose(fpOut);
        return 1;
    }

    fprintf(stderr, "Scanning '%s' (board size %dx%d), levels 0..%d...\n",
            storeDir, config.boardSize, config.boardSize, deepest);

    StoreStatsWriteCsvHeader(fpOut);
    uint64_t cumulativeBoardsGenerated = 0;
    for (int level = 0; level <= deepest; level++)
    {
        LevelStoreStats stats;
        StoreStatsScanLevel(storeDir, config.boardSize, level, &stats);
        cumulativeBoardsGenerated += stats.boardsGenerated;
        StoreStatsWriteCsvRow(fpOut, &stats, cumulativeBoardsGenerated);
    }

    if (fpOut != stdout)
        fclose(fpOut);

    return 0;
}
