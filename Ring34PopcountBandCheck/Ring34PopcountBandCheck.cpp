/*
** Filename:  Ring34PopcountBandCheck.cpp
**
** Purpose:
**   DISPOSABLE diagnostic tool -- not part of the permanent solution.
**   Converts one real, already-solved level's Ring_3_4 data (read-only,
**   from the live store) into a popcount-band-split layout: every record
**   is routed to one of 17 bands by its own popcount (how many of its 16
**   covered board cells are occupied), and within a band every record is
**   packed at a FIXED width via a combinatorial rank/unrank encoding
**   (ceil(log2(C(16,popcount))) bits -- the exact number needed to
**   uniquely number all C(16,popcount) possible bit-patterns for that
**   popcount, no more). This measures the REAL achievable uncompressed
**   size for the popcount-band splitting idea (see the
**   project_natural_compaction_brainstorm memory file) instead of the
**   histogram-weighted ESTIMATE computed earlier -- same purpose the
**   earlier (now-deleted) Ring34SplitCheck tool served for the rejected
**   Ring_3/Ring_4 split idea.
**
**   Output goes to C: (not Y:/D:/E:/F:) so this never competes with the
**   live solve's own I/O. Read-only against the real store.
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

/* ============================================================
** Combinatorial rank/unrank (the "combinadic" number system)
** ============================================================ */

/* Pascal's triangle, C(n,r) for n,r = 0..16 -- more than enough for a
** 16-bit field (popcount 0..16).
*/
static uint32_t g_binomial[17][17];

/*
** Function: BuildBinomialTable
** @brief    Fills g_binomial via Pascal's triangle. C(n,r)=0 for r>n by convention.
*/
static void BuildBinomialTable()
{
    for (int n = 0; n <= 16; n++)
    {
        for (int r = 0; r <= 16; r++)
        {
            if (r > n)              g_binomial[n][r] = 0;
            else if (r == 0 || r == n) g_binomial[n][r] = 1;
            else                     g_binomial[n][r] = g_binomial[n - 1][r - 1] + g_binomial[n - 1][r];
        }
    }
}

/*
** Function: BitsNeededForPopcount
** @brief    Minimum fixed bit-width to uniquely number all C(16,k) possible
**           16-bit patterns with exactly k bits set (0 for k=0 or k=16,
**           since there's exactly one such pattern -- nothing to store).
*/
static int BitsNeededForPopcount(int k)
{
    uint32_t count = g_binomial[16][k];
    if (count <= 1) return 0;
    int bits = 0;
    while ((1u << bits) < count) bits++;
    return bits;
}

/*
** Function: CombinadicRank
** @brief    Ranks a k-combination (the set-bit positions of pattern, 0..15)
**           into [0, C(16,k)) using the standard colex combinatorial number
**           system: rank = sum of C(position_i, i) for the i-th smallest
**           set bit (1-indexed).
** @param    pattern - 16-bit value; only the low 16 bits are meaningful
** @param    k       - popcount(pattern), passed in since the caller already knows it
** @return   The pattern's rank among all C(16,k) same-popcount patterns.
*/
static uint32_t CombinadicRank(uint16_t pattern, int k)
{
    uint32_t rank = 0;
    int i = 1;
    for (int pos = 0; pos < 16; pos++)
    {
        if (pattern & (1u << pos))
        {
            rank += g_binomial[pos][i];
            i++;
        }
    }
    return rank;
}

/*
** Function: CombinadicUnrank
** @brief    Inverse of CombinadicRank: reconstructs the original 16-bit
**           pattern from its rank and popcount via the standard greedy
**           combinadic decode.
** @param    rank - a value in [0, C(16,k))
** @param    k    - the popcount the pattern must have
** @return   The reconstructed 16-bit pattern.
*/
static uint16_t CombinadicUnrank(uint32_t rank, int k)
{
    uint16_t pattern = 0;
    for (int i = k; i >= 1; i--)
    {
        int pos = i - 1;
        while (g_binomial[pos + 1][i] <= rank) pos++;
        pattern |= (uint16_t)(1u << pos);
        rank -= g_binomial[pos][i];
    }
    return pattern;
}

/*
** Function: SelfTestCombinadic
** @brief    Exhaustively verifies CombinadicUnrank(CombinadicRank(x)) == x
**           for all 65536 possible 16-bit patterns -- cheap enough to do
**           completely rather than sample, so the size measurement below
**           can be trusted.
** @return   true if every one of the 65536 patterns round-trips correctly.
*/
static bool SelfTestCombinadic()
{
    for (uint32_t pattern = 0; pattern <= 0xFFFF; pattern++)
    {
        int k = 0;
        for (int b = 0; b < 16; b++) if (pattern & (1u << b)) k++;

        uint32_t rank = CombinadicRank((uint16_t)pattern, k);
        if (rank >= g_binomial[16][k])
        {
            printf("SELF-TEST FAILED: pattern %04x (k=%d) rank %u >= C(16,%d)=%u\n",
                   pattern, k, rank, k, g_binomial[16][k]);
            return false;
        }
        uint16_t back = CombinadicUnrank(rank, k);
        if (back != (uint16_t)pattern)
        {
            printf("SELF-TEST FAILED: pattern %04x (k=%d) -> rank %u -> %04x (mismatch)\n",
                   pattern, k, rank, back);
            return false;
        }
    }
    return true;
}

/* ============================================================
** Bit-packed per-band writer
** ============================================================ */

/*
** Type:    BitWriter
** @brief   Accumulates fixed-width values and streams completed bytes
**          straight to an already-open output file in small fixed chunks --
**          never holds more than CHUNK_SIZE bytes per band in RAM,
**          regardless of level size. The earlier version buffered an
**          entire band's output in a std::vector for the whole run (up to
**          ~15GB peak on level 16 black alone) before writing anything --
**          a real violation of this project's own "never load a level
**          wholesale" rule, caught when the user watched system memory
**          climb during a real run. This version keeps memory flat.
*/
struct BitWriter
{
    static constexpr size_t CHUNK_SIZE = 1 << 20;   /* 1 MB flush threshold per band */

    FILE*    file          = nullptr;
    uint8_t  chunk[CHUNK_SIZE];
    size_t   chunkUsed      = 0;
    uint32_t accum          = 0;
    int      accumBits      = 0;
    uint64_t recordsWritten = 0;
    uint64_t bytesWritten   = 0;

    void Open(const char* path)
    {
        file = fopen(path, "wb");
    }

    void PushByte(uint8_t b)
    {
        chunk[chunkUsed++] = b;
        if (chunkUsed == CHUNK_SIZE)
            FlushChunk();
    }

    void FlushChunk()
    {
        if (chunkUsed > 0)
        {
            if (file) fwrite(chunk, 1, chunkUsed, file);
            bytesWritten += chunkUsed;
            chunkUsed = 0;
        }
    }

    void WriteBits(uint32_t value, int width)
    {
        if (width == 0) { recordsWritten++; return; }   /* nothing to store (popcount 0 or 16) */
        accum = (accum << width) | (value & ((1u << width) - 1));
        accumBits += width;
        while (accumBits >= 8)
        {
            accumBits -= 8;
            PushByte((uint8_t)((accum >> accumBits) & 0xFF));
        }
        recordsWritten++;
    }

    void Close()
    {
        if (accumBits > 0)
        {
            PushByte((uint8_t)((accum << (8 - accumBits)) & 0xFF));
            accumBits = 0;
        }
        FlushChunk();
        if (file) { fclose(file); file = nullptr; }
    }
};

/* ============================================================
** Main
** ============================================================ */

/*
** Function: PrintUsage
** @brief    Prints command-line usage help.
*/
static void PrintUsage(const char* prog)
{
    printf("Usage: %s [options]\n\n", prog);
    printf("  --level N       Level to convert                              [default: 15]\n");
    printf("  --color C       black or white                                [default: black]\n");
    printf("  --board-size N  Board size: 4, 6, or 8                        [default: 6]\n");
    printf("  --store-drive L Drive letter the source store lives on        [default: Y]\n");
    printf("  --store-dir P   Sub-path on store drive (no drive letter)     [default: \\OthelloRingMaster\\Store]\n");
    printf("  --out-dir P     Output directory for converted files          [default: C:\\Ring34PopcountBandCheck]\n");
    printf("  --help          Show this help\n\n");
    printf("Read-only against the source store. Writes converted output to --out-dir\n");
    printf("(C: by default) so this never competes with a live solve's own I/O.\n\n");
}

int main(int argc, char* argv[])
{
    int  level      = 16;
    char color[16]  = "black";
    int  boardSize  = 6;
    char storeDrive = 'Y';
    char storeDirNoDrive[MAX_FULL_PATH_NAME] = "\\OthelloRingMaster\\Store";
    char outDir[MAX_FULL_PATH_NAME]          = "C:\\Ring34PopcountBandCheck";

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { PrintUsage(argv[0]); return 0; }
#define REQUIRE_NEXT(flag) if (++i >= argc) { printf("ERROR: %s requires a value\n", flag); return 1; }
        if (strcmp(argv[i], "--level") == 0)             { REQUIRE_NEXT("--level")       level = atoi(argv[i]); }
        else if (strcmp(argv[i], "--color") == 0)        { REQUIRE_NEXT("--color")       strncpy(color, argv[i], sizeof(color) - 1); }
        else if (strcmp(argv[i], "--board-size") == 0)   { REQUIRE_NEXT("--board-size")  boardSize = atoi(argv[i]); }
        else if (strcmp(argv[i], "--store-drive") == 0)  { REQUIRE_NEXT("--store-drive") storeDrive = (char)toupper((unsigned char)argv[i][0]); }
        else if (strcmp(argv[i], "--store-dir") == 0)    { REQUIRE_NEXT("--store-dir")   strncpy(storeDirNoDrive, argv[i], sizeof(storeDirNoDrive) - 1); }
        else if (strcmp(argv[i], "--out-dir") == 0)      { REQUIRE_NEXT("--out-dir")     strncpy(outDir, argv[i], sizeof(outDir) - 1); }
        else { printf("ERROR: unknown argument '%s'\n\n", argv[i]); PrintUsage(argv[0]); return 1; }
#undef REQUIRE_NEXT
    }

    int player = (strcmp(color, "black") == 0) ? RSF_PLAYER_BLACK : RSF_PLAYER_WHITE;

    BuildBinomialTable();

    printf("Running exhaustive combinadic self-test (all 65536 patterns)...\n");
    if (!SelfTestCombinadic())
    {
        printf("Aborting -- combinadic implementation is not trustworthy.\n");
        return 1;
    }
    printf("Self-test PASSED -- every 16-bit pattern round-trips correctly.\n\n");

    printf("Bit-width table (popcount -> bits needed):\n");
    for (int k = 0; k <= 16; k++)
        printf("  k=%2d  C(16,%2d)=%6u  bits=%2d\n", k, k, g_binomial[16][k], BitsNeededForPopcount(k));
    printf("\n");

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
    uint64_t origFlatUncompressedBytes = totalRecords * 2;

    printf("Converting level %d, %s: %llu records from '%s'\n", level, color, (unsigned long long)totalRecords, srcPath);
    printf("(original on-disk compressed size: %llu bytes; flat uncompressed baseline: %llu bytes)\n\n",
           (unsigned long long)origOnDiskBytes, (unsigned long long)origFlatUncompressedBytes);

    /* Open the output directory and all 17 band files up front -- each
    ** BitWriter streams its own completed bytes straight to its file in
    ** 1MB chunks as the conversion loop runs below, so peak memory stays
    ** flat (17 x 1MB) regardless of how many records this level has,
    ** instead of the earlier version's whole-band-in-RAM approach.
    */
    CreateDirectoryA(outDir, NULL);
    std::vector<BitWriter> bands(17);
    for (int k = 0; k <= 16; k++)
    {
        char outPath[MAX_FULL_PATH_NAME];
        snprintf(outPath, sizeof(outPath), "%s\\Level_%04d_%s_pop%02d.bin", outDir, level, color, k);
        bands[k].Open(outPath);
        if (!bands[k].file)
        {
            printf("ERROR: could not open output file '%s' for writing\n", outPath);
            return 1;
        }
    }

    const int BATCH = 65536;
    std::vector<Ring34Rec> batch(BATCH);
    int n;
    uint64_t processed = 0;
    int lastPercentBucket = -1;

    while ((n = RSFReadShaped(pReader, batch.data(), BATCH)) > 0)
    {
        for (int i = 0; i < n; i++)
        {
            uint16_t pattern = batch[i].pattern;
            int k = 0;
            for (int b = 0; b < 16; b++) if (pattern & (1u << b)) k++;

            uint32_t rank  = CombinadicRank(pattern, k);
            int      width = BitsNeededForPopcount(k);
            bands[k].WriteBits(rank, width);

            processed++;
            if (totalRecords > 0)
            {
                int bucket = (int)(processed * 100 / totalRecords / 5);
                if (bucket > lastPercentBucket)
                {
                    lastPercentBucket = bucket;
                    printf("  %d%% (%llu / %llu records)\n", bucket * 5, (unsigned long long)processed, (unsigned long long)totalRecords);
                }
            }
        }
    }
    RSFClose(&pReader);

    uint64_t totalPackedBytes = 0;
    uint64_t totalPackedRecords = 0;
    printf("\nPer-band results:\n");
    printf("Popcount,Records,BitWidth,Bytes\n");
    for (int k = 0; k <= 16; k++)
    {
        bands[k].Close();
        if (bands[k].recordsWritten == 0) continue;

        printf("%d,%llu,%d,%llu\n", k, (unsigned long long)bands[k].recordsWritten,
               BitsNeededForPopcount(k), (unsigned long long)bands[k].bytesWritten);
        totalPackedBytes    += bands[k].bytesWritten;
        totalPackedRecords  += bands[k].recordsWritten;
    }

    printf("\nTotals:\n");
    printf("RecordsConverted,%llu\n", (unsigned long long)totalPackedRecords);
    printf("PackedBytes,%llu\n", (unsigned long long)totalPackedBytes);
    printf("FlatUncompressedBaselineBytes,%llu\n", (unsigned long long)origFlatUncompressedBytes);
    if (origFlatUncompressedBytes > 0)
    {
        double pct = (1.0 - (double)totalPackedBytes / (double)origFlatUncompressedBytes) * 100.0;
        printf("ReductionVsFlatUncompressed,%.4f%%\n", pct);
    }
    printf("OriginalOnDiskCompressedBytes,%llu\n", (unsigned long long)origOnDiskBytes);
    printf("PackedBitsPerRecord,%.4f\n", totalPackedRecords > 0 ? (totalPackedBytes * 8.0 / totalPackedRecords) : 0.0);

    printf("\nOutput written to: %s\n", outDir);
    return 0;
}
