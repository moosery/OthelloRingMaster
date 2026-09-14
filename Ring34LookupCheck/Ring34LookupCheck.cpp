/*
** Filename:  Ring34LookupCheck.cpp
**
** Purpose:
**   DISPOSABLE diagnostic/verification tool -- not part of the permanent
**   solution. Exhaustively verifies OthelloRingMasterLevelIndexer's real
**   Ring_3_4 output against the real original Ring_3_4 file it was built
**   from:
**   walks every record in the original file (sequential, cheap) and looks
**   each one up through the segmented index (binary-search which segment,
**   open+decode it if not already loaded, compare the record), Fataling
**   immediately on any mismatch -- a broken index is never a soft warning.
**
**   Reports two different timing numbers, not one, since they mean very
**   different things: the OVERALL average (dominated by near-free repeat
**   lookups into an already-loaded segment, since this walk is sequential
**   and consecutive ordinals usually share a segment) and the SEGMENT-
**   TRANSITION average/min/max (only the lookups that actually triggered a
**   fresh segment open+decode) -- the second one is the real, honest number
**   that answers "how long does a genuine random lookup take," matching
**   the ~5s lookup budget this whole design was built around.
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

/* Functions */

/*
** Function: DiscoverSegments
** @brief    Lists a level/player's segment directory and parses each real
**           segment file's own starting ordinal straight from its filename
**           (no separate index needed -- see RSFNameRingSegmentFile).
**           Sorted explicitly rather than trusting directory enumeration
**           order, even though fixed-width hex filenames already sort the
**           same as ordinal order.
** @param    levelSegmentDir - the level/player's own segment subdirectory
** @param    pSegments       - out: discovered segments, sorted by startOrdinal
*/
static void DiscoverSegments(const char* levelSegmentDir, std::vector<SegmentInfo>* pSegments)
{
    char pattern[MAX_FULL_PATH_NAME];
    snprintf(pattern, sizeof(pattern), "%s\\seg*.rsfzl", levelSegmentDir);

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
        snprintf(info.path, sizeof(info.path), "%s\\%s", levelSegmentDir, fd.cFileName);
        pSegments->push_back(info);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    std::sort(pSegments->begin(), pSegments->end(),
              [](const SegmentInfo& a, const SegmentInfo& b) { return a.startOrdinal < b.startOrdinal; });
}

/*
** Function: LoadSegmentRecords
** @brief    Fully decodes one segment file's records into pOut -- the real
**           cost a genuine random lookup into this segment would have to
**           pay (open + decode start-to-finish), since the target record's
**           exact position within the segment isn't known until decoding
**           reaches it.
** @param    segPath - the segment file to decode
** @param    pOut    - out: every record in the segment, in original order
*/
static void LoadSegmentRecords(const char* segPath, std::vector<Ring34Rec>* pOut)
{
    RSFReader* pReader = RSFOpenShaped(segPath, RSF_SHAPE_LEAF16);
    if (!pReader)
        Fatal(FATAL_FILE_OPEN, "LoadSegmentRecords: could not open '%s' (corrupt or truncated)", segPath);

    uint64_t count = RSFReaderTrailer(pReader)->recordCount;
    pOut->clear();
    pOut->reserve((size_t)count);

    const int BATCH = 65536;
    std::vector<Ring34Rec> batch(BATCH);
    int n;
    while ((n = RSFReadShaped(pReader, batch.data(), BATCH)) > 0)
        pOut->insert(pOut->end(), batch.begin(), batch.begin() + n);

    RSFClose(&pReader);
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
    printf("Walks every real record in the original (unsegmented) Ring_3_4 file and looks\n");
    printf("each one up through the segmented index, Fataling immediately on any mismatch.\n");
    printf("Reports both the overall average lookup time (mostly free -- sequential access\n");
    printf("reuses an already-loaded segment for almost every record) and the real,\n");
    printf("meaningful number: timing for only the lookups that triggered a fresh segment\n");
    printf("open+decode, which is what actually answers 'how long does a real lookup take.'\n\n");
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

    char ring34Path[MAX_FULL_PATH_NAME];
    RSFNameRing34File(ring34Path, sizeof(ring34Path), storeDir, boardSize, level, player, 0);
    if (GetFileAttributesA(ring34Path) == INVALID_FILE_ATTRIBUTES)
    {
        printf("ERROR: source Ring_3_4 file not found: '%s'\n", ring34Path);
        return 1;
    }

    char levelDir[MAX_FULL_PATH_NAME];
    RSFNameLevelIndexDir(levelDir, sizeof(levelDir), levelIndexDir, boardSize, level, player);
    char levelSegmentDir[MAX_FULL_PATH_NAME];
    RSFNameRingSegmentDir(levelSegmentDir, sizeof(levelSegmentDir), levelDir, "Ring34");

    printf("Discovering segments in '%s'...\n", levelSegmentDir);
    fflush(stdout);
    std::vector<SegmentInfo> segments;
    DiscoverSegments(levelSegmentDir, &segments);
    printf("Found %zu segments.\n\n", segments.size());

    RSFReader* pReader = RSFOpenShaped(ring34Path, RSF_SHAPE_LEAF16);
    if (!pReader)
        Fatal(FATAL_FILE_OPEN, "Could not open '%s' (corrupt or truncated)", ring34Path);

    uint64_t totalRecords = RSFReaderTrailer(pReader)->recordCount;
    if (totalRecords == 0)
        Fatal(FATAL_FILE_OPEN, "Source Ring_3_4 file '%s' has zero records, nothing to verify", ring34Path);

    printf("Source: '%s' -- %llu real records\n\n", ring34Path, (unsigned long long)totalRecords);
    fflush(stdout);

    /* Just the start-ordinal list, for binary search. */
    std::vector<uint64_t> segmentStarts(segments.size());
    for (size_t i = 0; i < segments.size(); i++) segmentStarts[i] = segments[i].startOrdinal;

    int      currentSegmentIdx   = -1;
    uint64_t currentSegmentStart = 0, currentSegmentEnd = 0;
    std::vector<Ring34Rec> currentSegmentRecords;

    uint64_t totalLookups   = 0;
    double   totalLookupNs  = 0.0;
    uint64_t transitionCount   = 0;
    double   totalTransitionNs = 0.0, minTransitionNs = -1.0, maxTransitionNs = 0.0;

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    const int BATCH = 65536;
    std::vector<Ring34Rec> batch(BATCH);
    int n;
    uint64_t processed = 0;
    int lastPercentBucket = -1;
    uint64_t startTickMs = GetTickCount64();

    printf("Verifying every record...\n");
    fflush(stdout);

    while ((n = RSFReadShaped(pReader, batch.data(), BATCH)) > 0)
    {
        for (int i = 0; i < n; i++)
        {
            uint64_t ordinal = processed + (uint64_t)i;

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
                    Fatal(FATAL_MERGE_LOGIC_ERROR, "Ring34LookupCheck: ordinal %llu falls before the first segment's start -- index is broken", (unsigned long long)ordinal);
                idx--;

                currentSegmentIdx   = (int)idx;
                currentSegmentStart = segmentStarts[idx];
                currentSegmentEnd   = (idx + 1 < segmentStarts.size()) ? segmentStarts[idx + 1] : totalRecords;

                LoadSegmentRecords(segments[idx].path, &currentSegmentRecords);

                LARGE_INTEGER t1;
                QueryPerformanceCounter(&t1);
                double ns = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)freq.QuadPart;
                transitionCount++;
                totalTransitionNs += ns;
                if (minTransitionNs < 0 || ns < minTransitionNs) minTransitionNs = ns;
                if (ns > maxTransitionNs) maxTransitionNs = ns;
            }

            uint64_t localIdx = ordinal - currentSegmentStart;
            if (localIdx >= currentSegmentRecords.size())
                Fatal(FATAL_MERGE_LOGIC_ERROR,
                      "Ring34LookupCheck: ordinal %llu resolves to local index %llu, but segment '%s' only has %zu records -- index is broken",
                      (unsigned long long)ordinal, (unsigned long long)localIdx,
                      segments[currentSegmentIdx].path, currentSegmentRecords.size());

            if (currentSegmentRecords[(size_t)localIdx].pattern != batch[i].pattern)
                Fatal(FATAL_MERGE_LOGIC_ERROR,
                      "Ring34LookupCheck: MISMATCH at ordinal %llu -- original pattern 0x%04x, segment '%s' local index %llu has pattern 0x%04x",
                      (unsigned long long)ordinal, batch[i].pattern, segments[currentSegmentIdx].path,
                      (unsigned long long)localIdx, currentSegmentRecords[(size_t)localIdx].pattern);

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
                printf("  %d%% (%llu / %llu records verified, %llu segment loads so far)  elapsed=%.0fs  eta=%.0fs\n",
                       bucket, (unsigned long long)processed, (unsigned long long)totalRecords,
                       (unsigned long long)transitionCount, elapsedS, etaS);
                fflush(stdout);
            }
        }
    }
    RSFClose(&pReader);

    if (totalLookups != totalRecords)
        Fatal(FATAL_MERGE_LOGIC_ERROR, "Ring34LookupCheck: verified %llu records but source has %llu -- mismatch",
              (unsigned long long)totalLookups, (unsigned long long)totalRecords);

    printf("\nAll %llu records verified correctly -- every lookup matched the original exactly.\n\n",
           (unsigned long long)totalLookups);
    printf("Overall average lookup time (mostly free -- sequential access reuses an already-\n");
    printf("loaded segment for almost every record): %.3f microseconds/lookup\n\n",
           (totalLookups > 0 ? totalLookupNs / (double)totalLookups : 0.0) / 1000.0);
    printf("Segment transitions (the REAL cost -- a fresh open+decode, what a genuine random\n");
    printf("lookup actually has to pay): %llu transitions across %zu segments\n",
           (unsigned long long)transitionCount, segments.size());
    printf("  avg=%.3f sec   min=%.3f sec   max=%.3f sec\n",
           (transitionCount > 0 ? totalTransitionNs / (double)transitionCount : 0.0) / 1e9,
           (minTransitionNs < 0 ? 0.0 : minTransitionNs) / 1e9,
           maxTransitionNs / 1e9);

    return 0;
}
