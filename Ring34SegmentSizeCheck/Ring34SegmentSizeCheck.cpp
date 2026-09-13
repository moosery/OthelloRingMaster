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
**   Reads a real, already-complete Ring_3_4 file ONCE (read-only, never
**   touches the live store or its working drives) and, for each candidate
**   segment size given, buffers records up to an estimated record-count
**   target and independently re-compresses each buffered chunk via a real
**   RSFWriterOpenZMemShaped writer -- the exact same delta+varint+LZ4
**   encoding a real segment file would use -- entirely in memory. No temp
**   files, so this never competes with a live solve's own drive I/O.
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

/* Hard safety cap on any one candidate's in-memory record buffer, regardless
** of what the byte-target math suggests -- real Ring_3_4 compression ratios
** vary enough (real per-level numbers swing 50%-78% reduction just across
** levels 14-24) that trusting the estimate unconditionally could balloon
** memory for a highly-compressible file. Records are 2 bytes each, so this
** caps any one candidate's buffer at 1GB; several candidates run
** concurrently in one pass, so total peak stays a low single-digit number
** of GB -- trivial against this machine's real RAM.
*/
static constexpr uint64_t MAX_RECORDS_PER_CHUNK = 500000000ULL;

/* Structures and Types */

/*
** Type:    SegmentSizeCandidate
** @brief   One candidate segment size being tested, plus its running results.
*/
struct SegmentSizeCandidate
{
    char     label[16] = {};        /* as given on the command line, e.g. "2GB" */
    uint64_t targetBytes = 0;       /* parsed byte target                       */
    uint64_t recordsPerChunk = 0;   /* estimated records to hit targetBytes, capped for safety */

    std::vector<Ring34Rec> buffer;
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
** Function: CompressChunk
** @brief    Compresses candidate's buffered records as one independent
**           segment (the exact real encoding a real segment file would
**           use), entirely in memory, and folds the result into
**           candidate's running totals. Clears the buffer afterward
**           (capacity is retained by std::vector::clear, so the next
**           chunk's fill reuses the same already-reserved memory).
** @param    candidate - the candidate whose buffer to compress and clear
*/
static void CompressChunk(SegmentSizeCandidate* candidate)
{
    if (candidate->buffer.empty()) return;

    /* Worst case ~3 bytes/record post-varint (a 16-bit zigzag delta never
    ** needs more than that), generously padded for LZ4's own small bounded
    ** worst-case expansion -- comfortably oversized, never tight; if this
    ** estimate is ever wrong, RSFWriterRecordShaped/Close Fatal cleanly
    ** rather than overrun anything.
    */
    size_t capacity = candidate->buffer.size() * 5 + 65536;
    std::vector<uint8_t> outBuf(capacity);

    RSFWriter* pw = RSFWriterOpenZMemShaped(outBuf.data(), capacity, RSF_SHAPE_LEAF16);
    for (const Ring34Rec& rec : candidate->buffer)
        RSFWriterRecordShaped(pw, &rec);

    uint64_t compressedBytes = 0;
    RSFWriterClose(pw, &compressedBytes);

    candidate->segmentCount++;
    candidate->totalCompressedBytes += compressedBytes;
    if (compressedBytes < candidate->minCompressedBytes) candidate->minCompressedBytes = compressedBytes;
    if (compressedBytes > candidate->maxCompressedBytes) candidate->maxCompressedBytes = compressedBytes;

    candidate->buffer.clear();
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
    printf("Read-only against the source store. Every candidate chunk is compressed\n");
    printf("entirely in memory -- never writes anywhere near the live store or its\n");
    printf("working drives, so this is safe to run alongside a live solve.\n\n");
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

    RSFReader* pReader = RSFOpenShaped(srcPath, RSF_SHAPE_LEAF16);
    if (!pReader)
    {
        printf("ERROR: could not open '%s' (corrupt or truncated)\n", srcPath);
        return 1;
    }

    uint64_t totalRecords = RSFReaderTrailer(pReader)->recordCount;
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    GetFileAttributesExA(srcPath, GetFileExInfoStandard, &fad);
    uint64_t origOnDiskBytes = ((uint64_t)fad.nFileSizeHigh << 32) | (uint64_t)fad.nFileSizeLow;

    if (totalRecords == 0)
    {
        printf("ERROR: source file has zero records, nothing to measure\n");
        RSFClose(&pReader);
        return 1;
    }

    double bytesPerRecord = (double)origOnDiskBytes / (double)totalRecords;

    printf("Source: '%s'\n", srcPath);
    printf("Real records: %llu, real on-disk compressed bytes: %llu (%.6f bytes/record average)\n\n",
           (unsigned long long)totalRecords, (unsigned long long)origOnDiskBytes, bytesPerRecord);

    for (SegmentSizeCandidate& c : candidates)
    {
        uint64_t estimate = (uint64_t)((double)c.targetBytes / bytesPerRecord);
        if (estimate < 1) estimate = 1;
        bool capped = estimate > MAX_RECORDS_PER_CHUNK;
        c.recordsPerChunk = capped ? MAX_RECORDS_PER_CHUNK : estimate;
        c.buffer.reserve((size_t)c.recordsPerChunk);

        char sizeStr[32];
        FormatBytes(c.targetBytes, sizeStr, sizeof(sizeStr));
        printf("Candidate %-8s target=%-10s -> estimated %llu records/segment%s\n",
               c.label, sizeStr, (unsigned long long)c.recordsPerChunk,
               capped ? "  (capped for memory safety)" : "");
    }
    printf("\n");
    fflush(stdout);   /* force the banner out now -- stdout is fully buffered, not line-buffered,
                       ** when redirected to a file, so without this nothing appears until either
                       ** the buffer fills or the process exits, even though real work is happening */

    const int BATCH = 65536;
    std::vector<Ring34Rec> batch(BATCH);
    int n;
    uint64_t processed = 0;
    int lastPercentBucket = -1;
    uint64_t startTickMs = GetTickCount64();

    while ((n = RSFReadShaped(pReader, batch.data(), BATCH)) > 0)
    {
        for (int i = 0; i < n; i++)
        {
            for (SegmentSizeCandidate& c : candidates)
            {
                c.buffer.push_back(batch[i]);
                if (c.buffer.size() >= c.recordsPerChunk)
                    CompressChunk(&c);
            }
        }

        processed += (uint64_t)n;
        if (totalRecords > 0)
        {
            int bucket = (int)(processed * 100 / totalRecords / 5);
            if (bucket > lastPercentBucket)
            {
                lastPercentBucket = bucket;
                double pctDone   = (double)processed / (double)totalRecords * 100.0;
                double elapsedS  = (double)(GetTickCount64() - startTickMs) / 1000.0;
                double etaS      = (pctDone > 0.0) ? elapsedS * (100.0 - pctDone) / pctDone : 0.0;
                printf("  %d%% (%llu / %llu records)  elapsed=%.0fs  eta=%.0fs\n", bucket * 5,
                       (unsigned long long)processed, (unsigned long long)totalRecords, elapsedS, etaS);
                fflush(stdout);   /* see the banner's own fflush comment above -- same reason */
            }
        }
    }
    RSFClose(&pReader);

    /* Flush each candidate's final, possibly-partial chunk. */
    for (SegmentSizeCandidate& c : candidates)
        CompressChunk(&c);

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
