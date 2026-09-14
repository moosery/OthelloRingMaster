/*
** Filename:  Ring34LookupCheck.cpp
**
** Purpose:
**   DISPOSABLE diagnostic/verification tool -- not part of the permanent
**   solution. Exhaustively verifies OthelloRingMasterLevelIndexer's real
**   output for a whole level -- CellsInUse, Ring_2, AND Ring_3_4, not just
**   Ring_3_4 (its original, narrower scope) -- against each ring's real
**   original (unsegmented) file:
**   walks every record in the original file (sequential, cheap) and looks
**   each one up through the segmented index (binary-search which segment,
**   open+decode it if not already loaded, compare the record byte-for-byte),
**   Fataling immediately on any mismatch -- a broken index is never a soft
**   warning.
**
**   Generic over ring shape (CellsInUse: RSF_SHAPE_PAIR64, pattern+offset;
**   Ring_2: RSF_SHAPE_RING_LEVEL, pattern+offset; Ring_3_4: RSF_SHAPE_LEAF16,
**   pattern only) -- CheckOneRing compares raw bytes rather than any single
**   shape's named fields, so it verifies every field a given ring's records
**   actually carry, not just whichever field Ring_3_4-only code happened to
**   check before.
**
**   Reports two different timing numbers per ring, not one, since they mean
**   very different things: the OVERALL average (dominated by near-free
**   repeat lookups into an already-loaded segment, since this walk is
**   sequential and consecutive ordinals usually share a segment) and the
**   SEGMENT-TRANSITION average/min/max (only the lookups that actually
**   triggered a fresh segment open+decode) -- the second one is the real,
**   honest number that answers "how long does a genuine random lookup
**   take," matching the ~5s lookup budget this whole design was built
**   around.
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
#include <algorithm>

/* Structures and Types */

/*
** Type:    SegmentInfo
** @brief   One discovered segment file: its starting ordinal and full path.
*/
struct SegmentInfo
{
    uint64_t startOrdinal;
    char     path[MAX_FULL_PATH_NAME];
};

/*
** Type:    RingCheckResult
** @brief   What one call to CheckOneRing found, for the final combined
**          summary across all three rings.
*/
struct RingCheckResult
{
    uint64_t totalRecords      = 0;
    size_t   segmentCount      = 0;
    uint64_t transitionCount   = 0;
    double   totalTransitionNs = 0.0, minTransitionNs = -1.0, maxTransitionNs = 0.0;
    double   totalLookupNs     = 0.0;
};

/* Functions */

/*
** Function: RSFShapeSize
** @brief    Returns the on-the-wire record size for a shape this tool
**           actually handles.
*/
static int RSFShapeSize(RSFRecordShape shape)
{
    switch (shape)
    {
        case RSF_SHAPE_PAIR64:     return sizeof(UINT64_PAIR);
        case RSF_SHAPE_RING_LEVEL: return sizeof(RingLevelRec);
        case RSF_SHAPE_LEAF16:     return sizeof(Ring34Rec);
    }
    Fatal(FATAL_MERGE_LOGIC_ERROR, "RSFShapeSize: unsupported shape %d", (int)shape);
    return 0;
}

/*
** Function: FormatHexBytes
** @brief    Renders raw record bytes as a hex string, for mismatch
**           messages -- generic across shapes rather than assuming any one
**           shape's named fields.
*/
static void FormatHexBytes(const uint8_t* data, int len, char* out, size_t outSize)
{
    size_t pos = 0;
    for (int i = 0; i < len && pos + 3 < outSize; i++)
        pos += (size_t)snprintf(out + pos, outSize - pos, "%02x", data[i]);
}

/*
** Function: DiscoverSegments
** @brief    Lists a ring's segment directory and parses each real segment
**           file's own starting ordinal straight from its filename (no
**           separate index needed -- see RSFNameRingSegmentFile). Sorted
**           explicitly rather than trusting directory enumeration order,
**           even though fixed-width hex filenames already sort the same
**           as ordinal order.
** @param    ringSegmentDir - the ring's own segment subdirectory
** @param    pSegments      - out: discovered segments, sorted by startOrdinal
*/
static void DiscoverSegments(const char* ringSegmentDir, std::vector<SegmentInfo>* pSegments)
{
    char pattern[MAX_FULL_PATH_NAME];
    snprintf(pattern, sizeof(pattern), "%s\\seg*.rsfzl", ringSegmentDir);

    WIN32_FIND_DATAA fd = {};
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        Fatal(FATAL_FILE_OPEN, "DiscoverSegments: no segment files found matching '%s' -- has the indexer been run for this level?", pattern);

    do
    {
        unsigned long long ordinal = 0;
        if (sscanf(fd.cFileName, "seg%16llx.rsfzl", &ordinal) != 1)
            Fatal(FATAL_FILE_OPEN, "DiscoverSegments: '%s' doesn't match the expected seg<hex>.rsfzl naming", fd.cFileName);

        SegmentInfo info;
        info.startOrdinal = ordinal;
        snprintf(info.path, sizeof(info.path), "%s\\%s", ringSegmentDir, fd.cFileName);
        pSegments->push_back(info);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    std::sort(pSegments->begin(), pSegments->end(),
              [](const SegmentInfo& a, const SegmentInfo& b) { return a.startOrdinal < b.startOrdinal; });
}

/*
** Function: LoadSegmentRecordsRaw
** @brief    Fully decodes one segment file's records into pOut as raw
**           bytes -- the real cost a genuine random lookup into this
**           segment would have to pay (open + decode start-to-finish),
**           since the target record's exact position within the segment
**           isn't known until decoding reaches it. Shape-generic: works
**           for any of the three ring record shapes via recSize.
** @param    segPath - the segment file to decode
** @param    shape   - the ring's record shape
** @param    recSize - RSFShapeSize(shape), passed in to avoid recomputing
**                     it per segment load
** @param    pOut    - out: every record's raw bytes, in original order
*/
static void LoadSegmentRecordsRaw(const char* segPath, RSFRecordShape shape, int recSize, std::vector<uint8_t>* pOut)
{
    RSFReader* pReader = RSFOpenShaped(segPath, shape);
    if (!pReader)
        Fatal(FATAL_FILE_OPEN, "LoadSegmentRecordsRaw: could not open '%s' (corrupt or truncated)", segPath);

    uint64_t count = RSFReaderTrailer(pReader)->recordCount;
    pOut->clear();
    pOut->resize((size_t)count * (size_t)recSize);

    const int BATCH = 65536;
    std::vector<uint8_t> batch((size_t)BATCH * (size_t)recSize);
    int n;
    uint64_t filled = 0;
    while ((n = RSFReadShaped(pReader, batch.data(), BATCH)) > 0)
    {
        memcpy(pOut->data() + (size_t)filled * (size_t)recSize, batch.data(), (size_t)n * (size_t)recSize);
        filled += (uint64_t)n;
    }
    RSFClose(&pReader);
}

/*
** Function: CheckOneRing
** @brief    Exhaustively verifies one ring's real segmented output against
**           its real original (unsegmented) file -- see this file's own
**           top-of-file Notes for the full method. Fatals immediately on
**           any mismatch or index inconsistency.
** @param    ringName     - "CellsInUse", "Ring2", or "Ring34" (used for the
**                          segment subdirectory name and log lines)
** @param    originalPath - path to the real, unsegmented source file
** @param    shape        - that ring's record shape
** @param    levelDir     - this level/player's own directory (from
**                          RSFNameLevelIndexDir)
** @return   Real counts/timings for the final combined summary.
*/
static RingCheckResult CheckOneRing(const char* ringName, const char* originalPath, RSFRecordShape shape,
                                     const char* levelDir)
{
    RingCheckResult result;
    int recSize = RSFShapeSize(shape);

    char ringSegmentDir[MAX_FULL_PATH_NAME];
    RSFNameRingSegmentDir(ringSegmentDir, sizeof(ringSegmentDir), levelDir, ringName);

    printf("\n=== %s ===\n", ringName);
    printf("Discovering segments in '%s'...\n", ringSegmentDir);
    fflush(stdout);
    std::vector<SegmentInfo> segments;
    DiscoverSegments(ringSegmentDir, &segments);
    printf("Found %zu segments.\n\n", segments.size());
    result.segmentCount = segments.size();

    RSFReader* pReader = RSFOpenShaped(originalPath, shape);
    if (!pReader)
        Fatal(FATAL_FILE_OPEN, "Could not open '%s' (corrupt or truncated)", originalPath);

    uint64_t totalRecords = RSFReaderTrailer(pReader)->recordCount;
    if (totalRecords == 0)
        Fatal(FATAL_FILE_OPEN, "Source '%s' has zero records, nothing to verify", originalPath);

    printf("Source: '%s' -- %llu real records\n\n", originalPath, (unsigned long long)totalRecords);
    fflush(stdout);

    /* Just the start-ordinal list, for binary search. */
    std::vector<uint64_t> segmentStarts(segments.size());
    for (size_t i = 0; i < segments.size(); i++) segmentStarts[i] = segments[i].startOrdinal;

    int      currentSegmentIdx   = -1;
    uint64_t currentSegmentStart = 0, currentSegmentEnd = 0;
    std::vector<uint8_t> currentSegmentRecords;

    uint64_t totalLookups   = 0;
    double   totalLookupNs  = 0.0;
    uint64_t transitionCount   = 0;
    double   totalTransitionNs = 0.0, minTransitionNs = -1.0, maxTransitionNs = 0.0;

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    const int BATCH = 65536;
    std::vector<uint8_t> batch((size_t)BATCH * (size_t)recSize);
    int n;
    uint64_t processed = 0;
    int lastPercentBucket = -1;
    uint64_t startTickMs = GetTickCount64();

    printf("Verifying every %s record...\n", ringName);
    fflush(stdout);

    while ((n = RSFReadShaped(pReader, batch.data(), BATCH)) > 0)
    {
        for (int i = 0; i < n; i++)
        {
            uint64_t ordinal = processed + (uint64_t)i;
            const uint8_t* origRec = &batch[(size_t)i * (size_t)recSize];

            LARGE_INTEGER t0;
            QueryPerformanceCounter(&t0);

            if (currentSegmentIdx < 0 || ordinal >= currentSegmentEnd)
            {
                /* Real binary search -- last segment whose startOrdinal <= ordinal.
                ** Sequential access always moves forward, but this is a genuine
                ** search (not just "advance to next"), so this tool rehearses the
                ** same mechanism a real random-access lookup would use.
                */
                size_t idx = (size_t)(std::upper_bound(segmentStarts.begin(), segmentStarts.end(), ordinal) - segmentStarts.begin());
                if (idx == 0)
                    Fatal(FATAL_MERGE_LOGIC_ERROR, "%s: ordinal %llu falls before the first segment's start -- index is broken",
                          ringName, (unsigned long long)ordinal);
                idx--;

                currentSegmentIdx   = (int)idx;
                currentSegmentStart = segmentStarts[idx];
                currentSegmentEnd   = (idx + 1 < segmentStarts.size()) ? segmentStarts[idx + 1] : totalRecords;

                LoadSegmentRecordsRaw(segments[idx].path, shape, recSize, &currentSegmentRecords);

                LARGE_INTEGER t1;
                QueryPerformanceCounter(&t1);
                double ns = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)freq.QuadPart;
                transitionCount++;
                totalTransitionNs += ns;
                if (minTransitionNs < 0 || ns < minTransitionNs) minTransitionNs = ns;
                if (ns > maxTransitionNs) maxTransitionNs = ns;
            }

            uint64_t localIdx = ordinal - currentSegmentStart;
            if ((localIdx + 1) * (uint64_t)recSize > currentSegmentRecords.size())
                Fatal(FATAL_MERGE_LOGIC_ERROR,
                      "%s: ordinal %llu resolves to local index %llu, but segment '%s' only has %zu records -- index is broken",
                      ringName, (unsigned long long)ordinal, (unsigned long long)localIdx,
                      segments[currentSegmentIdx].path, currentSegmentRecords.size() / (size_t)recSize);

            const uint8_t* segRec = &currentSegmentRecords[(size_t)localIdx * (size_t)recSize];
            if (memcmp(segRec, origRec, (size_t)recSize) != 0)
            {
                char origHex[64], segHex[64];
                FormatHexBytes(origRec, recSize, origHex, sizeof(origHex));
                FormatHexBytes(segRec, recSize, segHex, sizeof(segHex));
                Fatal(FATAL_MERGE_LOGIC_ERROR,
                      "%s: MISMATCH at ordinal %llu -- original bytes %s, segment '%s' local index %llu has bytes %s",
                      ringName, (unsigned long long)ordinal, origHex, segments[currentSegmentIdx].path,
                      (unsigned long long)localIdx, segHex);
            }

            LARGE_INTEGER t2;
            QueryPerformanceCounter(&t2);
            double lookupNs = (double)(t2.QuadPart - t0.QuadPart) * 1e9 / (double)freq.QuadPart;
            totalLookups++;
            totalLookupNs += lookupNs;
        }

        processed += (uint64_t)n;
        if (totalRecords > 0)
        {
            int bucket = (int)(processed * 100 / totalRecords);
            /* Skip bucket 0 -- same real, observed bug fixed in the earlier
            ** tools: the first tiny batch is an almost-zero fraction of a
            ** billion-record file, so an ETA from it is meaningless noise.
            */
            if (bucket > lastPercentBucket && bucket >= 1)
            {
                lastPercentBucket = bucket;
                double pctDone  = (double)processed / (double)totalRecords * 100.0;
                double elapsedS = (double)(GetTickCount64() - startTickMs) / 1000.0;
                double etaS     = (pctDone > 0.0) ? elapsedS * (100.0 - pctDone) / pctDone : 0.0;
                printf("  [%s] %d%% (%llu / %llu records verified, %llu segment loads so far)  elapsed=%.0fs  eta=%.0fs\n",
                       ringName, bucket, (unsigned long long)processed, (unsigned long long)totalRecords,
                       (unsigned long long)transitionCount, elapsedS, etaS);
                fflush(stdout);
            }
        }
    }
    RSFClose(&pReader);

    if (totalLookups != totalRecords)
        Fatal(FATAL_MERGE_LOGIC_ERROR, "%s: verified %llu records but source has %llu -- mismatch",
              ringName, (unsigned long long)totalLookups, (unsigned long long)totalRecords);

    printf("%s: all %llu records verified correctly.\n", ringName, (unsigned long long)totalLookups);
    printf("  Overall avg lookup: %.3f microseconds/lookup (mostly free -- sequential reuse)\n",
           (totalLookups > 0 ? totalLookupNs / (double)totalLookups : 0.0) / 1000.0);
    printf("  Segment transitions (the REAL cost): %llu across %zu segments  avg=%.3fs  min=%.3fs  max=%.3fs\n",
           (unsigned long long)transitionCount, segments.size(),
           (transitionCount > 0 ? totalTransitionNs / (double)transitionCount : 0.0) / 1e9,
           (minTransitionNs < 0 ? 0.0 : minTransitionNs) / 1e9,
           maxTransitionNs / 1e9);

    result.totalRecords      = totalLookups;
    result.transitionCount   = transitionCount;
    result.totalTransitionNs = totalTransitionNs;
    result.minTransitionNs   = minTransitionNs;
    result.maxTransitionNs   = maxTransitionNs;
    result.totalLookupNs     = totalLookupNs;
    return result;
}

/*
** Function: PrintUsage
** @brief    Prints command-line usage help.
*/
static void PrintUsage(const char* prog)
{
    printf("Usage: %s --level N [options]\n\n", prog);
    printf("  --level N             Level to verify (must already be indexed)\n");
    printf("  --color C             black or white                                          [default: black]\n");
    printf("  --board-size N        Board size: 4, 6, or 8                                   [default: 6]\n");
    printf("  --store-drive L       Drive letter the original store lives on                 [default: Y]\n");
    printf("  --store-dir P         Sub-path on store drive (no drive letter)                  [default: \\OthelloRingMaster\\Store]\n");
    printf("  --levelindex-drive L  Drive letter the segmented index lives on                [default: Y]\n");
    printf("  --levelindex-dir P    Sub-path on that drive (no drive letter)                   [default: \\OthelloRingMaster\\Store\\levelIndexDir]\n");
    printf("  --help                Show this help\n\n");
    printf("Walks every real record in each of the level's three original (unsegmented)\n");
    printf("files -- CellsInUse, Ring_2, Ring_3_4 -- and looks each one up through its own\n");
    printf("segmented index, Fataling immediately on any mismatch (compared byte-for-byte,\n");
    printf("not just one named field). Reports both the overall average lookup time (mostly\n");
    printf("free -- sequential access reuses an already-loaded segment for almost every\n");
    printf("record) and the real, meaningful number per ring: timing for only the lookups\n");
    printf("that triggered a fresh segment open+decode, which is what actually answers\n");
    printf("'how long does a real lookup take.'\n\n");
}

int main(int argc, char* argv[])
{
    int  level      = -1;
    char color[16]  = "black";
    int  boardSize  = 6;
    char storeDrive = 'Y';
    char storeDirNoDrive[MAX_FULL_PATH_NAME]      = "\\OthelloRingMaster\\Store";
    char levelIndexDrive = 'Y';
    char levelIndexDirNoDrive[MAX_FULL_PATH_NAME] = "\\OthelloRingMaster\\Store\\levelIndexDir";

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { PrintUsage(argv[0]); return 0; }
#define REQUIRE_NEXT(flag) if (++i >= argc) { printf("ERROR: %s requires a value\n", flag); return 1; }
        if (strcmp(argv[i], "--level") == 0)                 { REQUIRE_NEXT("--level")             level = atoi(argv[i]); }
        else if (strcmp(argv[i], "--color") == 0)            { REQUIRE_NEXT("--color")             strncpy(color, argv[i], sizeof(color) - 1); }
        else if (strcmp(argv[i], "--board-size") == 0)       { REQUIRE_NEXT("--board-size")        boardSize = atoi(argv[i]); }
        else if (strcmp(argv[i], "--store-drive") == 0)      { REQUIRE_NEXT("--store-drive")       storeDrive = (char)toupper((unsigned char)argv[i][0]); }
        else if (strcmp(argv[i], "--store-dir") == 0)        { REQUIRE_NEXT("--store-dir")         strncpy(storeDirNoDrive, argv[i], sizeof(storeDirNoDrive) - 1); }
        else if (strcmp(argv[i], "--levelindex-drive") == 0) { REQUIRE_NEXT("--levelindex-drive")  levelIndexDrive = (char)toupper((unsigned char)argv[i][0]); }
        else if (strcmp(argv[i], "--levelindex-dir") == 0)   { REQUIRE_NEXT("--levelindex-dir")    strncpy(levelIndexDirNoDrive, argv[i], sizeof(levelIndexDirNoDrive) - 1); }
        else { printf("ERROR: unknown argument '%s'\n\n", argv[i]); PrintUsage(argv[0]); return 1; }
#undef REQUIRE_NEXT
    }

    if (level < 0)
    {
        printf("ERROR: --level is required\n\n");
        PrintUsage(argv[0]);
        return 1;
    }

    int player = (strcmp(color, "black") == 0) ? RSF_PLAYER_BLACK : RSF_PLAYER_WHITE;

    char storeDir[MAX_FULL_PATH_NAME];
    snprintf(storeDir, sizeof(storeDir), "%c:%s\\storeDir", storeDrive, storeDirNoDrive);
    char levelIndexDir[MAX_FULL_PATH_NAME];
    snprintf(levelIndexDir, sizeof(levelIndexDir), "%c:%s", levelIndexDrive, levelIndexDirNoDrive);

    char cellsInUsePath[MAX_FULL_PATH_NAME];
    RSFNameCellsInUseFile(cellsInUsePath, sizeof(cellsInUsePath), storeDir, boardSize, level, player, 0);
    char ring2Path[MAX_FULL_PATH_NAME];
    RSFNameRing2File(ring2Path, sizeof(ring2Path), storeDir, boardSize, level, player, 0);
    char ring34Path[MAX_FULL_PATH_NAME];
    RSFNameRing34File(ring34Path, sizeof(ring34Path), storeDir, boardSize, level, player, 0);

    if (GetFileAttributesA(cellsInUsePath) == INVALID_FILE_ATTRIBUTES)
    {
        printf("ERROR: source CellsInUse file not found: '%s'\n", cellsInUsePath);
        return 1;
    }
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

    char levelDir[MAX_FULL_PATH_NAME];
    RSFNameLevelIndexDir(levelDir, sizeof(levelDir), levelIndexDir, boardSize, level, player);

    printf("Level %d, %dx%d, %s -- level index directory: '%s'\n", level, boardSize, boardSize, color, levelDir);

    RingCheckResult cellsInUseResult = CheckOneRing("CellsInUse", cellsInUsePath, RSF_SHAPE_PAIR64,     levelDir);
    RingCheckResult ring2Result      = CheckOneRing("Ring2",      ring2Path,      RSF_SHAPE_RING_LEVEL, levelDir);
    RingCheckResult ring34Result     = CheckOneRing("Ring34",     ring34Path,     RSF_SHAPE_LEAF16,      levelDir);

    uint64_t totalRecordsAll = cellsInUseResult.totalRecords + ring2Result.totalRecords + ring34Result.totalRecords;

    printf("\n=== Summary: level %d, %dx%d, %s -- ALL %llu records across all three rings verified correctly ===\n",
           level, boardSize, boardSize, color, (unsigned long long)totalRecordsAll);
    printf("CellsInUse: %llu records, %zu segment(s)\n",
           (unsigned long long)cellsInUseResult.totalRecords, cellsInUseResult.segmentCount);
    printf("Ring_2:     %llu records, %zu segment(s)\n",
           (unsigned long long)ring2Result.totalRecords, ring2Result.segmentCount);
    printf("Ring_3_4:   %llu records, %zu segment(s)\n",
           (unsigned long long)ring34Result.totalRecords, ring34Result.segmentCount);

    return 0;
}
