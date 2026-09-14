/*
** Filename:  OthelloRingMasterRing34Indexer.cpp
**
** Purpose:
**   Real, permanent tool (not a disposable diagnostic): splits an already-
**   completed level's Ring_3_4 file into independently-decodable segments,
**   named by each segment's own starting global record ordinal in hex, so
**   a plain directory listing already sorts the same as ordinal order --
**   no separate index file needed. See project_othello_web_ui_design
**   memory (2026-09-13/14 sections) for the full design and the real,
**   validated numbers this is built on: ~500MB segments land at ~3.8-3.9s
**   decode time (comfortably under the ~5s lookup budget) with ~0.00%
**   compression-ratio cost, confirmed on real levels 16 and 20.
**
**   Only levels 14+ need this: levels 0-13's whole Ring_3_4 file already
**   decodes in under the ~5s target as a single unit (real per-level
**   decode-time numbers), so this refuses to run below that rather than
**   silently doing pointless work.
**
**   Segment boundaries are aligned to Ring_2's own group boundaries --
**   never splits one Ring_2 group's Ring_3_4 children across two segments
**   (this is what lets a later lookup resolve to either one whole segment
**   or several complete ones, never a partial straddle). Ring_2's own
**   `.offset` field already IS each group's Ring_3_4 starting ordinal, so
**   finding boundaries is just one cheap sequential pass over Ring_2 (much
**   smaller than Ring_3_4), not a guess. The ~500MB size trigger is a
**   trigger, not a hard cut point: once crossed, the cut waits for the
**   *next* real group boundary rather than firing immediately.
**
**   Reads from the live store's storeDir (read-only) and writes segments
**   to a dedicated --segment-dir, kept entirely separate from
**   storeDir/storeMergeDir/writerDir so this never collides with a live
**   solver's own active I/O.
*/

/* Includes */
#include "RSFFileName.h"
#include "RingNestedIndex.h"
#include "RingStoreFile.h"
#include "FileAndDirUtils.h"
#include "Error.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <vector>

/* Constants */

/* Levels below this already decode as a single whole file in under the
** ~5s lookup budget (real per-level numbers: level 13 is 374MB / 2.9s,
** level 14 is 1.53GB / 12.4s at Y:'s real ~127MB/s) -- segmenting them
** would be pointless extra work and extra files for no benefit.
*/
static constexpr int MIN_INDEXABLE_LEVEL = 14;

/* Functions */

/*
** Function: ParseByteSize
** @brief    Parses a size string like "500MB"/"2GB"/"1048576" into bytes.
**           Case-insensitive suffix; a bare number is treated as bytes.
** @param    s - the string to parse
** @return   The parsed byte count, or 0 if unparseable.
*/
static uint64_t ParseByteSize(const char* s)
{
    char* end = nullptr;
    double val = strtod(s, &end);
    if (end == s) return 0;
    while (*end == ' ') end++;
    if (_strnicmp(end, "GB", 2) == 0) return (uint64_t)(val * 1024.0 * 1024.0 * 1024.0);
    if (_strnicmp(end, "MB", 2) == 0) return (uint64_t)(val * 1024.0 * 1024.0);
    if (_strnicmp(end, "KB", 2) == 0) return (uint64_t)(val * 1024.0);
    return (uint64_t)val;
}

/*
** Function: FormatBytes
** @brief    Formats a byte count as a short human-readable string (B/KB/MB/GB/TB).
** @param    bytes   - value to format
** @param    out     - destination buffer
** @param    outSize - size of out
*/
static void FormatBytes(uint64_t bytes, char* out, size_t outSize)
{
    static const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    snprintf(out, outSize, "%.2f%s", v, units[u]);
}

/*
** Function: LoadRing2GroupBoundaries
** @brief    Reads a level's Ring_2 file sequentially and collects the
**           sequence of Ring_3_4 group-start ordinals -- each RingLevelRec's
**           own .offset field already IS the starting ordinal of that
**           group's Ring_3_4 children (confirmed via RingNestedIndexReader::
**           FindBoardPosition's own real usage of this field). Ring_2 is
**           much smaller than Ring_3_4, so this is a fast, real pass, not
**           an estimate.
** @param    ring2Path   - path to the source Ring_2 file
** @param    pBoundaries - out: group-start ordinals, in the same order
**                         Ring_2 itself is stored in (already sorted)
*/
static void LoadRing2GroupBoundaries(const char* ring2Path, std::vector<uint64_t>* pBoundaries)
{
    RSFReader* pReader = RSFOpenShaped(ring2Path, RSF_SHAPE_RING_LEVEL);
    if (!pReader)
        Fatal(FATAL_FILE_OPEN, "LoadRing2GroupBoundaries: could not open '%s' (corrupt or truncated)", ring2Path);

    uint64_t ring2Count = RSFReaderTrailer(pReader)->recordCount;
    pBoundaries->reserve((size_t)ring2Count);

    const int BATCH = 65536;
    std::vector<RingLevelRec> batch(BATCH);
    int n;
    while ((n = RSFReadShaped(pReader, batch.data(), BATCH)) > 0)
        for (int i = 0; i < n; i++)
            pBoundaries->push_back(batch[i].offset);

    RSFClose(&pReader);
}

/*
** Function: PrintUsage
** @brief    Prints command-line usage help.
*/
static void PrintUsage(const char* prog)
{
    printf("Usage: %s --level N [options]\n\n", prog);
    printf("  --level N           Level to index (real, already-complete Ring_3_4 file); must be >= %d\n", MIN_INDEXABLE_LEVEL);
    printf("  --color C           black or white                                          [default: black]\n");
    printf("  --board-size N      Board size: 4, 6, or 8                                   [default: 6]\n");
    printf("  --store-drive L     Drive letter the source store lives on                   [default: Y]\n");
    printf("  --store-dir P       Sub-path on store drive (no drive letter)                  [default: \\OthelloRingMaster\\Store]\n");
    printf("  --segment-drive L   Drive letter for segment output                          [default: Y]\n");
    printf("  --segment-dir P     Sub-path on segment drive (no drive letter)                [default: \\OthelloRingMaster\\Store\\segmentDir]\n");
    printf("  --target-size SIZE  Nominal segment size trigger (e.g. 500MB)                 [default: 500MB]\n");
    printf("  --help              Show this help\n\n");
    printf("Reads Ring_2 once to find real group boundaries, then splits Ring_3_4 into\n");
    printf("independent segments, only cutting at a group boundary once the target size\n");
    printf("has been reached -- never splits one Ring_2 group's children across two\n");
    printf("segments. Writes to --segment-dir, entirely separate from the live store's\n");
    printf("own working directories, so this is safe to run alongside a live solve.\n\n");
}

int main(int argc, char* argv[])
{
    int  level      = -1;
    char color[16]  = "black";
    int  boardSize  = 6;
    char storeDrive = 'Y';
    char storeDirNoDrive[MAX_FULL_PATH_NAME]   = "\\OthelloRingMaster\\Store";
    char segmentDrive = 'Y';
    char segmentDirNoDrive[MAX_FULL_PATH_NAME] = "\\OthelloRingMaster\\Store\\segmentDir";
    char targetSizeArg[32] = "500MB";

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { PrintUsage(argv[0]); return 0; }
#define REQUIRE_NEXT(flag) if (++i >= argc) { printf("ERROR: %s requires a value\n", flag); return 1; }
        if (strcmp(argv[i], "--level") == 0)              { REQUIRE_NEXT("--level")          level = atoi(argv[i]); }
        else if (strcmp(argv[i], "--color") == 0)         { REQUIRE_NEXT("--color")          strncpy(color, argv[i], sizeof(color) - 1); }
        else if (strcmp(argv[i], "--board-size") == 0)    { REQUIRE_NEXT("--board-size")     boardSize = atoi(argv[i]); }
        else if (strcmp(argv[i], "--store-drive") == 0)   { REQUIRE_NEXT("--store-drive")    storeDrive = (char)toupper((unsigned char)argv[i][0]); }
        else if (strcmp(argv[i], "--store-dir") == 0)     { REQUIRE_NEXT("--store-dir")      strncpy(storeDirNoDrive, argv[i], sizeof(storeDirNoDrive) - 1); }
        else if (strcmp(argv[i], "--segment-drive") == 0) { REQUIRE_NEXT("--segment-drive")  segmentDrive = (char)toupper((unsigned char)argv[i][0]); }
        else if (strcmp(argv[i], "--segment-dir") == 0)   { REQUIRE_NEXT("--segment-dir")    strncpy(segmentDirNoDrive, argv[i], sizeof(segmentDirNoDrive) - 1); }
        else if (strcmp(argv[i], "--target-size") == 0)   { REQUIRE_NEXT("--target-size")    strncpy(targetSizeArg, argv[i], sizeof(targetSizeArg) - 1); }
        else { printf("ERROR: unknown argument '%s'\n\n", argv[i]); PrintUsage(argv[0]); return 1; }
#undef REQUIRE_NEXT
    }

    if (level < 0)
    {
        printf("ERROR: --level is required\n\n");
        PrintUsage(argv[0]);
        return 1;
    }
    if (level < MIN_INDEXABLE_LEVEL)
    {
        printf("ERROR: level %d doesn't need segmenting -- its whole Ring_3_4 file already\n"
               "decodes under the ~5s lookup target as a single unit (real numbers hold this\n"
               "through level %d). Only levels >= %d benefit from segmenting.\n",
               level, MIN_INDEXABLE_LEVEL - 1, MIN_INDEXABLE_LEVEL);
        return 1;
    }

    uint64_t targetBytes = ParseByteSize(targetSizeArg);
    if (targetBytes == 0)
    {
        printf("ERROR: could not parse --target-size '%s'\n", targetSizeArg);
        return 1;
    }

    int player = (strcmp(color, "black") == 0) ? RSF_PLAYER_BLACK : RSF_PLAYER_WHITE;

    char storeDir[MAX_FULL_PATH_NAME];
    snprintf(storeDir, sizeof(storeDir), "%c:%s\\storeDir", storeDrive, storeDirNoDrive);

    char segmentDir[MAX_FULL_PATH_NAME];
    snprintf(segmentDir, sizeof(segmentDir), "%c:%s", segmentDrive, segmentDirNoDrive);

    char ring2Path[MAX_FULL_PATH_NAME];
    RSFNameRing2File(ring2Path, sizeof(ring2Path), storeDir, boardSize, level, player, 0);
    char ring34Path[MAX_FULL_PATH_NAME];
    RSFNameRing34File(ring34Path, sizeof(ring34Path), storeDir, boardSize, level, player, 0);

    if (GetFileAttributesA(ring2Path) == INVALID_FILE_ATTRIBUTES)
    {
        printf("ERROR: source Ring_2 file not found: '%s'\n", ring2Path);
        return 1;
    }
    if (GetFileAttributesA(ring34Path) == INVALID_FILE_ATTRIBUTES)
    {
        printf("ERROR: source Ring_3_4 file not found: '%s'\n", ring34Path);
        return 1;
    }

    if (!CreateFullPath(segmentDir))
        Fatal(FATAL_CREATE_DIR_FAILED, "Cannot create segment directory '%s'", segmentDir);

    printf("Loading Ring_2 group boundaries from '%s'...\n", ring2Path);
    fflush(stdout);
    std::vector<uint64_t> boundaries;
    LoadRing2GroupBoundaries(ring2Path, &boundaries);
    printf("Loaded %llu group boundaries.\n\n", (unsigned long long)boundaries.size());
    fflush(stdout);

    RSFReader* pReader = RSFOpenShaped(ring34Path, RSF_SHAPE_LEAF16);
    if (!pReader)
        Fatal(FATAL_FILE_OPEN, "Could not open '%s' (corrupt or truncated)", ring34Path);

    uint64_t totalRecords = RSFReaderTrailer(pReader)->recordCount;
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    GetFileAttributesExA(ring34Path, GetFileExInfoStandard, &fad);
    uint64_t origOnDiskBytes = ((uint64_t)fad.nFileSizeHigh << 32) | (uint64_t)fad.nFileSizeLow;

    if (totalRecords == 0)
        Fatal(FATAL_FILE_OPEN, "Source Ring_3_4 file '%s' has zero records, nothing to index", ring34Path);

    double   bytesPerRecord         = (double)origOnDiskBytes / (double)totalRecords;
    uint64_t recordsPerChunkEstimate = (uint64_t)((double)targetBytes / bytesPerRecord);
    if (recordsPerChunkEstimate < 1) recordsPerChunkEstimate = 1;

    printf("Source: '%s'\n", ring34Path);
    printf("Real records: %llu, real on-disk compressed bytes: %llu (%.6f bytes/record average)\n",
           (unsigned long long)totalRecords, (unsigned long long)origOnDiskBytes, bytesPerRecord);
    printf("Target segment size: %llu bytes (~%llu records/segment estimate)\n\n",
           (unsigned long long)targetBytes, (unsigned long long)recordsPerChunkEstimate);

    char firstSegPath[MAX_FULL_PATH_NAME];
    RSFNameRing34SegmentFile(firstSegPath, sizeof(firstSegPath), segmentDir, boardSize, level, player, 0);
    RSFWriter* pw = RSFWriterOpenZLShaped(firstSegPath, RSF_SHAPE_LEAF16);

    uint64_t segmentStartOrdinal = 0;
    uint64_t recordsInSegment    = 0;
    uint64_t currentOrdinal      = 0;
    size_t   boundaryIdx         = 0;
    bool     cutRequested        = false;
    uint64_t segmentCount        = 0;
    uint64_t totalSegmentBytes   = 0;
    uint64_t minSegBytes = UINT64_MAX, maxSegBytes = 0;

    const int BATCH = 65536;
    std::vector<Ring34Rec> batch(BATCH);
    int n;
    uint64_t processed = 0;
    int lastPercentBucket = -1;
    uint64_t startTickMs = GetTickCount64();

    printf("Building segments in '%s'...\n", segmentDir);
    fflush(stdout);

    while ((n = RSFReadShaped(pReader, batch.data(), BATCH)) > 0)
    {
        for (int i = 0; i < n; i++)
        {
            bool atGroupBoundary = (boundaryIdx < boundaries.size() && currentOrdinal == boundaries[boundaryIdx]);
            if (atGroupBoundary) boundaryIdx++;

            /* Only cut once BOTH the size trigger has fired AND we're
            ** exactly at a real Ring_2 group boundary -- never mid-group,
            ** never mid-record.
            */
            if (cutRequested && atGroupBoundary && currentOrdinal > segmentStartOrdinal)
            {
                uint64_t segBytes = 0;
                RSFWriterClose(pw, &segBytes);
                segmentCount++;
                totalSegmentBytes += segBytes;
                if (segBytes < minSegBytes) minSegBytes = segBytes;
                if (segBytes > maxSegBytes) maxSegBytes = segBytes;

                segmentStartOrdinal = currentOrdinal;
                recordsInSegment    = 0;
                cutRequested        = false;

                char segPath[MAX_FULL_PATH_NAME];
                RSFNameRing34SegmentFile(segPath, sizeof(segPath), segmentDir, boardSize, level, player, segmentStartOrdinal);
                pw = RSFWriterOpenZLShaped(segPath, RSF_SHAPE_LEAF16);
            }

            RSFWriterRecordShaped(pw, &batch[i]);
            recordsInSegment++;
            currentOrdinal++;

            if (!cutRequested && recordsInSegment >= recordsPerChunkEstimate)
                cutRequested = true;
        }

        processed += (uint64_t)n;
        if (totalRecords > 0)
        {
            int bucket = (int)(processed * 100 / totalRecords);   /* 1% granularity */
            /* Skip bucket 0 -- see Ring34SegmentSizeCheck's own note on this
            ** exact bug (a real one, found live): the first read batch is
            ** an almost-zero fraction of a billion-record file, so an ETA
            ** extrapolated from it is meaningless noise, not a real estimate.
            */
            if (bucket > lastPercentBucket && bucket >= 1)
            {
                lastPercentBucket = bucket;
                double pctDone  = (double)processed / (double)totalRecords * 100.0;
                double elapsedS = (double)(GetTickCount64() - startTickMs) / 1000.0;
                double etaS     = (pctDone > 0.0) ? elapsedS * (100.0 - pctDone) / pctDone : 0.0;
                printf("  %d%% (%llu / %llu records, %llu segments so far)  elapsed=%.0fs  eta=%.0fs\n",
                       bucket, (unsigned long long)processed, (unsigned long long)totalRecords,
                       (unsigned long long)segmentCount, elapsedS, etaS);
                fflush(stdout);
            }
        }
    }
    RSFClose(&pReader);

    /* Close the final segment. */
    {
        uint64_t segBytes = 0;
        RSFWriterClose(pw, &segBytes);
        segmentCount++;
        totalSegmentBytes += segBytes;
        if (segBytes < minSegBytes) minSegBytes = segBytes;
        if (segBytes > maxSegBytes) maxSegBytes = segBytes;
    }

    /* Never silently report success on a mismatch -- if the segmented
    ** output doesn't account for exactly the source's real record count,
    ** something is genuinely wrong and must not be trusted.
    */
    if (currentOrdinal != totalRecords)
        Fatal(FATAL_MERGE_LOGIC_ERROR,
              "Ring34Indexer: wrote %llu records across %llu segments but source '%s' has %llu -- "
              "mismatch, refusing to report success",
              (unsigned long long)currentOrdinal, (unsigned long long)segmentCount, ring34Path,
              (unsigned long long)totalRecords);

    char totalStr[32], origStr[32], avgStr[32], minStr[32], maxStr[32];
    FormatBytes(totalSegmentBytes, totalStr, sizeof(totalStr));
    FormatBytes(origOnDiskBytes, origStr, sizeof(origStr));
    FormatBytes(segmentCount > 0 ? totalSegmentBytes / segmentCount : 0, avgStr, sizeof(avgStr));
    FormatBytes(minSegBytes == UINT64_MAX ? 0 : minSegBytes, minStr, sizeof(minStr));
    FormatBytes(maxSegBytes, maxStr, sizeof(maxStr));

    printf("\nDone. %llu segments, %llu records (matches source exactly).\n",
           (unsigned long long)segmentCount, (unsigned long long)currentOrdinal);
    printf("Total: %s (original single-stream: %s)   Avg/segment: %s   Min: %s   Max: %s\n",
           totalStr, origStr, avgStr, minStr, maxStr);
    printf("Segments written to: %s\n", segmentDir);

    return 0;
}
