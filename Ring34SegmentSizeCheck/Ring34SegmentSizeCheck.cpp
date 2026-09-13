/*
** Filename:  Ring34SegmentSizeCheck.cpp
**
** Purpose:
**   DISPOSABLE diagnostic tool -- not part of the permanent solution (may be
**   removed once segment sizing is settled, same convention as the earlier
**   Ring34PopcountBandCheck/Ring34SplitCheck tools). Measures the REAL
**   compression-ratio cost of splitting an already-completed level's
**   Ring_3_4 file into independent segments at various candidate sizes --
**   the one thing that can't be predicted analytically, since it depends on
**   how much cross-record compression benefit real board data actually has.
**
**   Reads a real, already-complete Ring_3_4 file, once per DISTINCT candidate
**   size (read-only, never touches the live store or its working drives),
**   and streams records directly into a real RSFWriterOpenZMemShaped writer
**   -- the exact same delta+varint+LZ4 encoding a real segment file would
**   use -- closing and reopening it at each chunk boundary. No temp files,
**   entirely in memory, so this never competes with a live solve's own
**   drive I/O.
**
**   Deliberately one full pass PER distinct candidate size, not all
**   candidates accumulated concurrently in a single pass: this bounds peak
**   memory to roughly ONE candidate's own target size at a time (the output
**   buffer is sized directly from --segment-sizes, not derived from a
**   record-count estimate), instead of needing all candidates' buffers
**   resident simultaneously. Chosen deliberately to stay safe running
**   alongside a live solve that already budgets the bulk of this machine's
**   RAM for itself -- trades some extra wall-clock time (re-reading the
**   source file once per distinct candidate) for a much smaller, bounded
**   memory footprint. Candidates that estimate to the same records/chunk as
**   an earlier one still skip their own pass entirely and copy that
**   candidate's results, since the outcome is guaranteed identical.
**
**   See project_othello_web_ui_design memory (2026-09-13 section) for the
**   design this measures: segments named by starting record ordinal in hex,
**   boundaries aligned to existing nested-index group boundaries (not
**   modeled here -- this tool only measures the size/ratio tradeoff, not
**   the group-alignment mechanics).
*/

/* Includes */
#include "RSFFileName.h"
#include "RingNestedIndex.h"
#include "RingStoreFile.h"
#include "FileAndDirUtils.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <vector>

/* Constants */

/* Defensive sanity cap on the estimated records/chunk, well beyond anything
** a real --segment-sizes target should ever produce -- guards against a
** degenerate bytes/record estimate (e.g. a corrupt/empty source) rather
** than acting as the memory-safety mechanism it used to be. Peak memory is
** now bounded by each candidate's own targetBytes (the output buffer),
** not by record count, so this can afford to be generous.
*/
static constexpr uint64_t MAX_RECORDS_PER_CHUNK = 50000000000ULL;

/* Structures and Types */

/*
** Type:    SegmentSizeCandidate
** @brief   One candidate segment size being tested, plus its running results.
*/
struct SegmentSizeCandidate
{
    char     label[16] = {};        /* as given on the command line, e.g. "2GB" */
    uint64_t targetBytes = 0;       /* parsed byte target                       */
    uint64_t recordsPerChunk = 0;   /* estimated records to hit targetBytes     */

    /* Index into the candidates vector of the earlier candidate that shares
    ** this exact recordsPerChunk value, or -1 if this candidate needs its
    ** own real pass. Two candidates with the same recordsPerChunk would hit
    ** identical chunk boundaries against identical data and produce
    ** byte-for-byte identical results -- real, observed waste on a
    ** billion-record file (three candidates all landing on the same
    ** estimate tripled the CPU-bound recompression cost, and under this
    ** one-pass-per-candidate design would triple a full source re-read too).
    ** Only the representative gets a real pass; the rest just copy its
    ** results once processing is done.
    */
    int representativeIndex = -1;

    uint64_t segmentCount         = 0;
    uint64_t totalCompressedBytes = 0;
    uint64_t minCompressedBytes   = UINT64_MAX;
    uint64_t maxCompressedBytes   = 0;
};

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
** Function: PrintUsage
** @brief    Prints command-line usage help.
*/
static void PrintUsage(const char* prog)
{
    printf("Usage: %s [options]\n\n", prog);
    printf("  --level N            Level to test (real, already-complete Ring_3_4 file)   [default: 23]\n");
    printf("  --color C            black or white                                         [default: black]\n");
    printf("  --board-size N       Board size: 4, 6, or 8                                  [default: 6]\n");
    printf("  --store-drive L      Drive letter the source store lives on                  [default: Y]\n");
    printf("  --store-dir P        Sub-path on store drive (no drive letter)                [default: \\OthelloRingMaster\\Store]\n");
    printf("  --segment-sizes LIST Comma-separated candidate sizes (e.g. 500MB,1GB,2GB,4GB) [default: 500MB,1GB,2GB,4GB]\n");
    printf("  --help               Show this help\n\n");
    printf("Read-only against the source store. One full pass per distinct candidate size,\n");
    printf("each compressed entirely in memory -- never writes anywhere near the live store\n");
    printf("or its working drives, and peak memory stays bounded to roughly one candidate's\n");
    printf("own target size at a time, safe to run alongside a live solve.\n\n");
}

int main(int argc, char* argv[])
{
    int  level      = 23;
    char color[16]  = "black";
    int  boardSize  = 6;
    char storeDrive = 'Y';
    char storeDirNoDrive[MAX_FULL_PATH_NAME] = "\\OthelloRingMaster\\Store";
    char segmentSizesArg[512] = "500MB,1GB,2GB,4GB";

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { PrintUsage(argv[0]); return 0; }
#define REQUIRE_NEXT(flag) if (++i >= argc) { printf("ERROR: %s requires a value\n", flag); return 1; }
        if (strcmp(argv[i], "--level") == 0)              { REQUIRE_NEXT("--level")         level = atoi(argv[i]); }
        else if (strcmp(argv[i], "--color") == 0)         { REQUIRE_NEXT("--color")         strncpy(color, argv[i], sizeof(color) - 1); }
        else if (strcmp(argv[i], "--board-size") == 0)    { REQUIRE_NEXT("--board-size")    boardSize = atoi(argv[i]); }
        else if (strcmp(argv[i], "--store-drive") == 0)   { REQUIRE_NEXT("--store-drive")   storeDrive = (char)toupper((unsigned char)argv[i][0]); }
        else if (strcmp(argv[i], "--store-dir") == 0)     { REQUIRE_NEXT("--store-dir")     strncpy(storeDirNoDrive, argv[i], sizeof(storeDirNoDrive) - 1); }
        else if (strcmp(argv[i], "--segment-sizes") == 0) { REQUIRE_NEXT("--segment-sizes") strncpy(segmentSizesArg, argv[i], sizeof(segmentSizesArg) - 1); }
        else { printf("ERROR: unknown argument '%s'\n\n", argv[i]); PrintUsage(argv[0]); return 1; }
#undef REQUIRE_NEXT
    }

    int player = (strcmp(color, "black") == 0) ? RSF_PLAYER_BLACK : RSF_PLAYER_WHITE;

    /* Parse the comma-separated candidate size list. */
    std::vector<SegmentSizeCandidate> candidates;
    {
        char buf[512];
        strncpy(buf, segmentSizesArg, sizeof(buf) - 1);
        char* ctx = nullptr;
        char* tok = strtok_s(buf, ",", &ctx);
        while (tok)
        {
            SegmentSizeCandidate c;
            strncpy(c.label, tok, sizeof(c.label) - 1);
            c.targetBytes = ParseByteSize(tok);
            if (c.targetBytes == 0)
            {
                printf("ERROR: could not parse segment size '%s'\n", tok);
                return 1;
            }
            candidates.push_back(c);
            tok = strtok_s(nullptr, ",", &ctx);
        }
    }
    if (candidates.empty())
    {
        printf("ERROR: no candidate segment sizes given\n");
        return 1;
    }

    char storeDir[MAX_FULL_PATH_NAME];
    snprintf(storeDir, sizeof(storeDir), "%c:%s\\storeDir", storeDrive, storeDirNoDrive);

    char srcPath[MAX_FULL_PATH_NAME];
    RSFNameRing34File(srcPath, sizeof(srcPath), storeDir, boardSize, level, player, 0);

    if (GetFileAttributesA(srcPath) == INVALID_FILE_ATTRIBUTES)
    {
        printf("ERROR: source file not found: '%s'\n", srcPath);
        return 1;
    }

    /* One quick open just to read the trailer (real record count, real
    ** on-disk compressed size) -- closed immediately; each candidate's own
    ** pass below opens its own fresh reader from the start of the file.
    */
    uint64_t totalRecords;
    uint64_t origOnDiskBytes;
    {
        RSFReader* pProbe = RSFOpenShaped(srcPath, RSF_SHAPE_LEAF16);
        if (!pProbe)
        {
            printf("ERROR: could not open '%s' (corrupt or truncated)\n", srcPath);
            return 1;
        }
        totalRecords = RSFReaderTrailer(pProbe)->recordCount;
        RSFClose(&pProbe);

        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        GetFileAttributesExA(srcPath, GetFileExInfoStandard, &fad);
        origOnDiskBytes = ((uint64_t)fad.nFileSizeHigh << 32) | (uint64_t)fad.nFileSizeLow;
    }

    if (totalRecords == 0)
    {
        printf("ERROR: source file has zero records, nothing to measure\n");
        return 1;
    }

    double bytesPerRecord = (double)origOnDiskBytes / (double)totalRecords;

    printf("Source: '%s'\n", srcPath);
    printf("Real records: %llu, real on-disk compressed bytes: %llu (%.6f bytes/record average)\n\n",
           (unsigned long long)totalRecords, (unsigned long long)origOnDiskBytes, bytesPerRecord);

    for (size_t idx = 0; idx < candidates.size(); idx++)
    {
        SegmentSizeCandidate& c = candidates[idx];
        uint64_t estimate = (uint64_t)((double)c.targetBytes / bytesPerRecord);
        if (estimate < 1) estimate = 1;
        bool capped = estimate > MAX_RECORDS_PER_CHUNK;
        c.recordsPerChunk = capped ? MAX_RECORDS_PER_CHUNK : estimate;

        for (size_t prior = 0; prior < idx; prior++)
        {
            if (candidates[prior].recordsPerChunk == c.recordsPerChunk)
            {
                c.representativeIndex = (int)prior;
                break;
            }
        }

        char sizeStr[32];
        FormatBytes(c.targetBytes, sizeStr, sizeof(sizeStr));
        if (c.representativeIndex >= 0)
            printf("Candidate %-8s target=%-10s -> estimated %llu records/segment  (identical to %s -- no separate pass)\n",
                   c.label, sizeStr, (unsigned long long)c.recordsPerChunk, candidates[c.representativeIndex].label);
        else
            printf("Candidate %-8s target=%-10s -> estimated %llu records/segment%s\n",
                   c.label, sizeStr, (unsigned long long)c.recordsPerChunk,
                   capped ? "  (sanity-capped, unexpectedly high estimate)" : "");
    }
    printf("\n");
    fflush(stdout);   /* force the banner out now -- stdout is fully buffered, not line-buffered,
                       ** when redirected to a file, so without this nothing appears until either
                       ** the buffer fills or the process exits, even though real work is happening */

    int distinctCount = 0;
    for (const SegmentSizeCandidate& c : candidates)
        if (c.representativeIndex < 0) distinctCount++;

    const int BATCH = 65536;
    std::vector<Ring34Rec> batch(BATCH);
    int passNum = 0;

    for (SegmentSizeCandidate& c : candidates)
    {
        if (c.representativeIndex >= 0) continue;   /* shares an earlier candidate's exact chunk size -- copied at the end, no pass needed */
        passNum++;

        /* Output buffer sized directly from this candidate's own target
        ** bytes (a 50% margin plus fixed slop for LZ4/varint overhead),
        ** NOT derived from record count -- this is the whole point of the
        ** redesign: peak memory for this pass is bounded by what the user
        ** actually asked for on the command line, regardless of how many
        ** billions of records happen to fit in it. If real compression
        ** ever does worse than the margin allows, RSFWriterRecordShaped/
        ** Close Fatal cleanly rather than overrun anything.
        */
        size_t outBufCapacity = (size_t)(c.targetBytes * 3 / 2) + (1 << 20);
        std::vector<uint8_t> outBuf(outBufCapacity);

        printf("Pass %d/%d: candidate %s (output buffer %.2fGB)\n", passNum, distinctCount, c.label,
               (double)outBufCapacity / (1024.0 * 1024.0 * 1024.0));
        fflush(stdout);

        RSFReader* pReader = RSFOpenShaped(srcPath, RSF_SHAPE_LEAF16);
        if (!pReader)
        {
            printf("ERROR: could not open '%s' for pass %d (corrupt or truncated)\n", srcPath, passNum);
            return 1;
        }

        RSFWriter* pw = RSFWriterOpenZMemShaped(outBuf.data(), outBufCapacity, RSF_SHAPE_LEAF16);
        uint64_t recordsInChunk = 0;
        uint64_t processed      = 0;
        int      lastPercentBucket = -1;
        uint64_t startTickMs    = GetTickCount64();
        int      n;

        while ((n = RSFReadShaped(pReader, batch.data(), BATCH)) > 0)
        {
            for (int i = 0; i < n; i++)
            {
                RSFWriterRecordShaped(pw, &batch[i]);
                recordsInChunk++;

                if (recordsInChunk >= c.recordsPerChunk)
                {
                    uint64_t compressedBytes = 0;
                    RSFWriterClose(pw, &compressedBytes);

                    c.segmentCount++;
                    c.totalCompressedBytes += compressedBytes;
                    if (compressedBytes < c.minCompressedBytes) c.minCompressedBytes = compressedBytes;
                    if (compressedBytes > c.maxCompressedBytes) c.maxCompressedBytes = compressedBytes;

                    recordsInChunk = 0;
                    pw = RSFWriterOpenZMemShaped(outBuf.data(), outBufCapacity, RSF_SHAPE_LEAF16);
                }
            }

            processed += (uint64_t)n;
            if (totalRecords > 0)
            {
                int bucket = (int)(processed * 100 / totalRecords);   /* 1% granularity */
                /* Skip bucket 0 entirely -- on a file with billions of
                ** records, the very first read batch already rounds down to
                ** "0%" while representing an almost-zero real fraction of
                ** the file. Computing an ETA from that tiny a sample
                ** amplifies any cold-start timing noise (first network
                ** read, file-open latency) by a factor in the millions --
                ** a real, observed bug, not a hypothetical one. Waiting for
                ** the first genuine 1% milestone gives ETA a real amount of
                ** elapsed, representative throughput to extrapolate from.
                */
                if (bucket > lastPercentBucket && bucket >= 1)
                {
                    lastPercentBucket = bucket;
                    double pctDone  = (double)processed / (double)totalRecords * 100.0;
                    double elapsedS = (double)(GetTickCount64() - startTickMs) / 1000.0;
                    double etaS     = (pctDone > 0.0) ? elapsedS * (100.0 - pctDone) / pctDone : 0.0;
                    printf("  [pass %d/%d] %d%% (%llu / %llu records)  elapsed=%.0fs  eta=%.0fs\n",
                           passNum, distinctCount, bucket,
                           (unsigned long long)processed, (unsigned long long)totalRecords, elapsedS, etaS);
                    fflush(stdout);   /* see the banner's own fflush comment above -- same reason */
                }
            }
        }
        RSFClose(&pReader);

        /* Flush this candidate's final, possibly-partial chunk. */
        if (recordsInChunk > 0)
        {
            uint64_t compressedBytes = 0;
            RSFWriterClose(pw, &compressedBytes);
            c.segmentCount++;
            c.totalCompressedBytes += compressedBytes;
            if (compressedBytes < c.minCompressedBytes) c.minCompressedBytes = compressedBytes;
            if (compressedBytes > c.maxCompressedBytes) c.maxCompressedBytes = compressedBytes;
        }
        else
        {
            RSFWriterClose(pw, nullptr);   /* empty trailing chunk -- discard, nothing to tally */
        }
    }

    /* Copy results into every candidate that shared a representative's exact
    ** recordsPerChunk -- same underlying data, same chunk boundaries, so the
    ** results are guaranteed identical without needing a separate pass.
    */
    for (SegmentSizeCandidate& c : candidates)
    {
        if (c.representativeIndex < 0) continue;
        const SegmentSizeCandidate& rep = candidates[c.representativeIndex];
        c.segmentCount         = rep.segmentCount;
        c.totalCompressedBytes = rep.totalCompressedBytes;
        c.minCompressedBytes   = rep.minCompressedBytes;
        c.maxCompressedBytes   = rep.maxCompressedBytes;
    }

    printf("\nResults (source: %llu records, %llu bytes original single-stream compressed):\n\n",
           (unsigned long long)totalRecords, (unsigned long long)origOnDiskBytes);
    printf("%-8s %10s %12s %14s %10s %12s %12s %12s\n",
           "Target", "Segments", "Recs/Seg", "TotalBytes", "vs Orig", "AvgSeg", "MinSeg", "MaxSeg");
    for (const SegmentSizeCandidate& c : candidates)
    {
        double pctDiff = origOnDiskBytes > 0
            ? ((double)c.totalCompressedBytes - (double)origOnDiskBytes) / (double)origOnDiskBytes * 100.0
            : 0.0;
        double avgSeg = c.segmentCount > 0 ? (double)c.totalCompressedBytes / (double)c.segmentCount : 0.0;
        char totalStr[32], avgStr[32], minStr[32], maxStr[32];
        FormatBytes(c.totalCompressedBytes, totalStr, sizeof(totalStr));
        FormatBytes((uint64_t)avgSeg, avgStr, sizeof(avgStr));
        FormatBytes(c.minCompressedBytes == UINT64_MAX ? 0 : c.minCompressedBytes, minStr, sizeof(minStr));
        FormatBytes(c.maxCompressedBytes, maxStr, sizeof(maxStr));
        printf("%-8s %10llu %12llu %14s %+9.2f%% %12s %12s %12s\n",
               c.label, (unsigned long long)c.segmentCount, (unsigned long long)c.recordsPerChunk,
               totalStr, pctDiff, avgStr, minStr, maxStr);
    }

    printf("\nEstimated decode time per average segment at Y:'s real ~127MB/s:\n");
    for (const SegmentSizeCandidate& c : candidates)
    {
        double avgSeg  = c.segmentCount > 0 ? (double)c.totalCompressedBytes / (double)c.segmentCount : 0.0;
        double seconds = avgSeg / (127.0 * 1024.0 * 1024.0);
        printf("  %-8s  %.2f sec\n", c.label, seconds);
    }

    return 0;
}
