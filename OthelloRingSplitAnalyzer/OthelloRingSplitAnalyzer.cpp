#include "BlasterFile.h"
#include "FileAndDirUtils.h"
#include "lz4frame.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <queue>
#include <algorithm>
#include <utility>
#include <string>

// ============================================================================
// OthelloRingSplitAnalyzer
//
// Experiment: instead of the current row-major bit layout and the flat
// CellsInUse/ColorInCells split (see OthelloStoreSplitAnalyzer), gather each
// board's 64 bits into concentric "rings" (outermost perimeter first,
// innermost 2x2 last) and build a nested index:
//
//   CellsInUse : unique occupancy pattern (in ring-gathered order) + offset
//                into Ring_1 for the first board with that pattern.
//   Ring_1     : one entry per distinct (pattern, ring1-color) group  -- count
//                of member boards + the 28-bit outermost-ring color subpattern
//                + offset into Ring_2 for that group's boards.
//   Ring_2     : one entry per distinct (pattern, ring1-color, ring2-color)
//                group -- count + the 20-bit second-ring color subpattern +
//                offset into Ring_3_4 for that group's boards.
//   Ring_3_4   : one entry per distinct full (pattern, color) combination --
//                count + the 16-bit combined mid+inner-ring color subpattern.
//                No win/loss/tie stats in this pass (not available per-board
//                in the store today).
//
// Ring geometry is ALWAYS the full 8x8 depth (28/20/12/4 bits), regardless of
// the actual board size being analyzed -- this is deliberate, not an
// oversight. A smaller board's active cells are already centered within the
// 8x8 word by the production encoding (see OthelloTypes.h's
// SetBoardSizeForRun), so gathering the full 64 bits using the 8x8 ring walk
// means Ring_1 (the true 8x8 border) is exactly the cells OUTSIDE a smaller
// board's active area. For a 6x6 board (centered at offset 1), that makes
// Ring_1 always the never-occupied absolute border, and Ring_2/Ring_3/Ring_4
// exactly the board's real outer/mid/inner rings. This lets every board size
// share the same fixed 4-file layout (CellsInUse, Ring_1, Ring_2, Ring_3_4)
// with no board-size-dependent branching -- a smaller board just produces a
// trivial/degenerate Ring_1 (and, for 4x4, a trivial Ring_2 too) instead of
// the file being omitted. That degeneracy is itself checked and reported
// below rather than assumed.
//
// This is a pure offline analysis tool: it reads an already-built, already
// row-major-encoded production store (via the existing BLFOpen/BLFRead), and
// does the ring remap/resort/split entirely within this process. It does not
// touch OthelloBasics.h's GETINDEX/FIRSTBIT convention, move generation, flip
// logic, or the byte-per-row bit-tricks the rotate/mirror code relies on --
// none of that is affected by this experiment.
// ============================================================================

static const int kBoardSize = 6;   // board actually being solved -- affects filenames/defaults only
static const int kLevel = 15;      // level being analyzed -- affects filenames/defaults only

// Ring geometry -- always the full 8x8 depth, see header comment above.
static const int kRingFrameSize = 8;
static const int kTotalBits = kRingFrameSize * kRingFrameSize;  // 64, always

static const int kRing1Bits = 28, kRing2Bits = 20, kRing34Bits = 16;  // 28+20+16 = 64
static const int kRing1Shift  = kTotalBits - kRing1Bits;                // 36
static const int kRing2Shift  = kTotalBits - kRing1Bits - kRing2Bits;   // 16
static const int kRing34Shift = 0;

// ============================================================================
// Ring order generation (generic for any even N) + self-check
// ============================================================================

// Local (row,col), 0-indexed within the NxN board, in ring order:
// outermost ring first (clockwise from top-left corner), innermost last.
static std::vector<std::pair<int,int>> GenerateRingOrder(int N)
{
    std::vector<std::pair<int,int>> order;
    order.reserve((size_t)N * N);
    for (int d = 0; d < N / 2; d++)
    {
        int lo = d, hi = N - 1 - d;
        for (int c = lo; c <= hi; c++)              order.push_back({ lo, c });   // top row, L->R
        for (int r = lo + 1; r <= hi; r++)           order.push_back({ r, hi });   // right col, top->bottom
        if (hi > lo)
            for (int c = hi - 1; c >= lo; c--)       order.push_back({ hi, c });   // bottom row, R->L
        for (int r = hi - 1; r >= lo + 1; r--)       order.push_back({ r, lo });   // left col, bottom->top
    }
    return order;
}

// Literal cell order given by hand for N=8 (outer ring first, inner last),
// used only to self-check GenerateRingOrder(8) at startup.
static const char* kLiteralN8Order[64] = {
    "A1","A2","A3","A4","A5","A6","A7","A8","B8","C8","D8","E8","F8","G8","H8","H7",
    "H6","H5","H4","H3","H2","H1","G1","F1","E1","D1","C1","B1","B2","B3","B4","B5",
    "B6","B7","C7","D7","E7","F7","G7","G6","G5","G4","G3","G2","F2","E2","D2","C2",
    "C3","C4","C5","C6","D6","E6","F6","F5","F4","F3","E3","D3","D4","D5","E5","E4"
};

// Runs GenerateRingOrder(8) against kLiteralN8Order and reports mismatches.
// (One known discrepancy: the hand-typed list this was transcribed from had
// "D3" at position 47 where the ring-walk geometry -- confirmed by hand for
// every other position -- requires "D2"; kLiteralN8Order above already has
// that corrected. This function exists so any *other* mismatch fails loudly
// instead of being trusted silently.)
static bool SelfCheckRingOrder()
{
    auto order = GenerateRingOrder(8);
    bool ok = true;
    for (int i = 0; i < 64; i++)
    {
        int row = order[i].first, col = order[i].second;
        char buf[4];
        snprintf(buf, sizeof(buf), "%c%d", 'A' + row, col + 1);
        if (strcmp(buf, kLiteralN8Order[i]) != 0)
        {
            fprintf(stderr, "SelfCheckRingOrder MISMATCH at index %d: generated '%s', expected '%s'\n",
                    i, buf, kLiteralN8Order[i]);
            ok = false;
        }
    }
    printf(ok ? "SelfCheckRingOrder: PASS (N=8 ring walk matches the hand-derived cell order)\n"
              : "SelfCheckRingOrder: FAIL -- see mismatches above\n");
    return ok;
}

// Maps ring-order position k (0 = MSB of the gathered value) to the
// *original* row-major bit index-from-MSB (row*8+col). boardOffset centers a
// ring frame smaller than 8 within the 8x8 word; always 0 here since the
// ring frame is always the full 8x8 (see header comment).
static std::vector<int> BuildRingPermutation(int N, int boardOffset)
{
    auto order = GenerateRingOrder(N);
    std::vector<int> perm(order.size());
    for (size_t k = 0; k < order.size(); k++)
    {
        int absRow = order[k].first + boardOffset;
        int absCol = order[k].second + boardOffset;
        perm[k] = absRow * 8 + absCol;   // index-from-MSB, matches OthelloBasics.h's GETINDEX
    }
    return perm;
}

// Gathers the kTotalBits meaningful bits of `original` (row-major, index-from-MSB
// convention, FIRSTBIT = MSB) into ring order, placing them at the top
// (MSB) of the returned 64-bit value.
static inline uint64_t GatherRingBits(uint64_t original, const int* perm, int numBits)
{
    uint64_t result = 0;
    for (int k = 0; k < numBits; k++)
    {
        uint64_t bit = (original >> (63 - perm[k])) & 1ULL;
        result |= (bit << (63 - k));
    }
    return result;
}

// ============================================================================
// Packed raw record layouts (tight, honest widths -- see BOARD_KEY_DISK's
// own static_assert precedent in BlasterFile.h; these use the same
// pack(push,1) discipline so no hidden alignment padding sneaks into the
// "raw" size measurements). RingLevelRawRec is shared by Ring_1 and Ring_2 --
// both have the identical (count, pattern, offset) shape.
// ============================================================================

#pragma pack(push, 1)
struct RingLevelRawRec
{
    uint64_t count;
    uint32_t pattern;   // low kRing1Bits or kRing2Bits meaningful, rest always 0
    uint64_t offset;    // record index into the next level down
};
// No count field here: pattern + Ring_1 + Ring_2 together already reconstruct
// the full (pattern, color) pair exactly, and the source store has no
// duplicate boards -- so every Ring_3_4 group has exactly 1 member (verified
// at runtime, not just assumed -- see Aggregator::CloseRing34Group and the
// report's "groups with count != 1" line). A count of 1 for every single
// record would be pure dead weight, so it's omitted rather than stored.
struct Ring34RawRec
{
    uint16_t pattern;   // low kRing34Bits meaningful (mid+inner ring combined)
};
#pragma pack(pop)
static_assert(sizeof(RingLevelRawRec) == 20, "RingLevelRawRec must be tight (20 bytes)");
static_assert(sizeof(Ring34RawRec) == 2, "Ring34RawRec must be tight (2 bytes)");

// ============================================================================
// Small IO helpers (same pattern as OthelloStoreSplitAnalyzer.cpp)
// ============================================================================

static uint64_t FileSizeOnDisk(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    _fseeki64(f, 0, SEEK_END);
    uint64_t sz = (uint64_t)_ftelli64(f);
    fclose(f);
    return sz;
}

static FILE* OpenOrDie(const char* path, const char* mode)
{
    FILE* f = fopen(path, mode);
    if (!f)
    {
        fprintf(stderr, "ERROR: cannot open '%s' (mode '%s')\n", path, mode);
        exit(2);
    }
    return f;
}

// Prints "<label>: NN%" the first time `done/total` crosses each 5% boundary.
// *lastBucket must start at -1 (one per progress instance -- each phase/file
// gets its own so they don't interfere with each other).
static void ReportProgress(const char* label, uint64_t done, uint64_t total, int* lastBucket)
{
    int bucket = total ? (int)((done * 20ULL) / total) : 20;  // 20 buckets = 5% each
    if (bucket > *lastBucket)
    {
        *lastBucket = bucket;
        int pct = bucket * 5; if (pct > 100) pct = 100;
        printf("  %s: %d%%\n", label, pct);
    }
}

// ============================================================================
// LZ4 frame streaming compressor for the Ring_1/Ring_2/Ring_3_4 raw files.
// These records don't fit BOARD_KEY_DISK's two-64-bit-field shape, so they
// can't reuse BLFWriterOpenZ's delta+varint pipeline -- compress the raw
// bytes directly instead, using the exact same LZ4F settings BlasterFile.cpp
// uses for .blfzl (default LZ4F_preferences_t, content checksum enabled, no
// HC), so the comparison against the production store stays apples-to-apples.
// ============================================================================

static uint64_t CompressRawFileToLZ4Frame(const char* rawPath, const char* outPath, const char* progressLabel)
{
    FILE* in = OpenOrDie(rawPath, "rb");
    FILE* out = OpenOrDie(outPath, "wb");
    uint64_t totalInBytes = FileSizeOnDisk(rawPath);

    LZ4F_preferences_t prefs = {};
    prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;

    const size_t inChunkSize = 1 << 20; // 1 MB
    size_t outBufSize = LZ4F_compressBound(inChunkSize, &prefs) + LZ4F_HEADER_SIZE_MAX + 32;
    std::vector<uint8_t> inBuf(inChunkSize);
    std::vector<uint8_t> outBuf(outBufSize);

    LZ4F_cctx* cctx = nullptr;
    LZ4F_errorCode_t err = LZ4F_createCompressionContext(&cctx, LZ4F_VERSION);
    if (LZ4F_isError(err))
    {
        fprintf(stderr, "ERROR: LZ4F_createCompressionContext failed: %s\n", LZ4F_getErrorName(err));
        exit(2);
    }

    uint64_t totalOut = 0;
    size_t headerSize = LZ4F_compressBegin(cctx, outBuf.data(), outBuf.size(), &prefs);
    fwrite(outBuf.data(), 1, headerSize, out);
    totalOut += headerSize;

    uint64_t bytesIn = 0;
    int lastBucket = -1;
    size_t n;
    while ((n = fread(inBuf.data(), 1, inChunkSize, in)) > 0)
    {
        size_t compSize = LZ4F_compressUpdate(cctx, outBuf.data(), outBuf.size(), inBuf.data(), n, nullptr);
        if (LZ4F_isError(compSize))
        {
            fprintf(stderr, "ERROR: LZ4F_compressUpdate failed: %s\n", LZ4F_getErrorName(compSize));
            exit(2);
        }
        fwrite(outBuf.data(), 1, compSize, out);
        totalOut += compSize;

        bytesIn += n;
        ReportProgress(progressLabel, bytesIn, totalInBytes, &lastBucket);
    }

    size_t endSize = LZ4F_compressEnd(cctx, outBuf.data(), outBuf.size(), nullptr);
    fwrite(outBuf.data(), 1, endSize, out);
    totalOut += endSize;

    LZ4F_freeCompressionContext(cctx);
    fclose(in);
    fclose(out);
    return totalOut;
}

// ============================================================================
// Phase 1: stream the original store, remap bits, sort in memory-sized
// chunks, spill each sorted chunk to a run file on scratch-fast storage.
// ============================================================================

struct BoardKeyLess
{
    bool operator()(const BOARD_KEY_DISK& a, const BOARD_KEY_DISK& b) const
    {
        if (a.ullCellsInUse != b.ullCellsInUse) return a.ullCellsInUse < b.ullCellsInUse;
        return a.ullCellColors < b.ullCellColors;
    }
};

// Reads + remaps the whole input store, writing sorted run files
// "<scratchDir>\\run_<color>_<NNNN>.blf". Returns the run file paths.
static std::vector<std::string> RemapAndSpillRuns(const char* inputPath, const char* scratchDir,
                                                     const char* colorName, const int* perm,
                                                     uint64_t chunkCapacity, uint64_t totalRecords)
{
    BLFReader* r = BLFOpen(inputPath);
    if (!r)
    {
        fprintf(stderr, "ERROR: cannot open store file '%s'\n", inputPath);
        exit(2);
    }

    std::vector<std::string> runPaths;
    std::vector<BOARD_KEY_DISK> chunk;
    chunk.reserve((size_t)chunkCapacity);

    char progressLabel[64];
    snprintf(progressLabel, sizeof(progressLabel), "%s remap+sort", colorName);
    uint64_t recordsProcessed = 0;
    int lastBucket = -1;

    static BOARD_KEY_DISK buf[8192];
    int n;
    int runIndex = 0;
    while ((n = BLFRead(r, buf, 8192)) > 0)
    {
        for (int i = 0; i < n; i++)
        {
            BOARD_KEY_DISK rec;
            rec.ullCellsInUse  = GatherRingBits(buf[i].ullCellsInUse,  perm, kTotalBits);
            rec.ullCellColors  = GatherRingBits(buf[i].ullCellColors,  perm, kTotalBits);
            chunk.push_back(rec);
            recordsProcessed++;
            ReportProgress(progressLabel, recordsProcessed, totalRecords, &lastBucket);

            if (chunk.size() >= chunkCapacity)
            {
                std::sort(chunk.begin(), chunk.end(), BoardKeyLess());
                char runPath[700];
                snprintf(runPath, sizeof(runPath), "%s\\run_%s_%04d.blf", scratchDir, colorName, runIndex++);
                BLFWrite(runPath, chunk.data(), chunk.size());
                runPaths.push_back(runPath);
                chunk.clear();
            }
        }
    }
    if (!chunk.empty())
    {
        std::sort(chunk.begin(), chunk.end(), BoardKeyLess());
        char runPath[700];
        snprintf(runPath, sizeof(runPath), "%s\\run_%s_%04d.blf", scratchDir, colorName, runIndex++);
        BLFWrite(runPath, chunk.data(), chunk.size());
        runPaths.push_back(runPath);
    }

    BLFClose(&r);
    printf("  %s: remap+sort done, %d run file(s)\n", colorName, (int)runPaths.size());
    return runPaths;
}

// ============================================================================
// Phase 2: k-way merge the sorted runs, streaming the fully-sorted output
// directly into the hierarchical aggregator below (no intermediate merged
// file is ever materialized).
// ============================================================================

struct RunSource
{
    BLFReader* reader = nullptr;
    BOARD_KEY_DISK buf[4096];
    int count = 0;
    int pos = 0;

    bool Refill()
    {
        if (pos < count) return true;
        count = BLFRead(reader, buf, 4096);
        pos = 0;
        return count > 0;
    }
};

struct HeapEntry
{
    BOARD_KEY_DISK key;
    int srcIdx;
};

// Min-heap ordering -- same (ullCellsInUse, ullCellColors) priority as
// MergeFiles.cpp's MergeHeadGreater.
struct HeapEntryGreater
{
    bool operator()(const HeapEntry& a, const HeapEntry& b) const
    {
        if (a.key.ullCellsInUse != b.key.ullCellsInUse) return a.key.ullCellsInUse > b.key.ullCellsInUse;
        return a.key.ullCellColors > b.key.ullCellColors;
    }
};

// ============================================================================
// Aggregator: consumes the fully sorted (pattern, color) stream and builds
// all four output files in one pass, tracking nested group boundaries
// (pattern -> ring1-color -> ring2-color -> combined ring3+4-color).
// ============================================================================

struct AggregatorStats
{
    uint64_t cellsInUseRecords = 0;
    uint64_t ring1Records = 0;
    uint64_t ring2Records = 0;
    uint64_t ring34Records = 0;
    uint64_t ring34GroupsWithCountNot1 = 0;  // sanity check -- see report output
    uint64_t totalBoards = 0;
};

struct Aggregator
{
    BLFWriter* cellsInUseCompWriter = nullptr;  // BLFWriterOpenZ, reuses BOARD_KEY_DISK shape
    FILE* cellsInUseRawFile = nullptr;
    FILE* ring1RawFile = nullptr;
    FILE* ring2RawFile = nullptr;
    FILE* ring34RawFile = nullptr;

    bool havePattern = false;
    uint64_t curPattern = 0;

    bool haveRing1Group = false;
    uint32_t curRing1Pattern = 0;
    uint64_t ring1GroupRing2Start = 0;

    bool haveRing2Group = false;
    uint32_t curRing2Pattern = 0;
    uint64_t ring2GroupRing34Start = 0;

    bool haveRing34Group = false;
    uint16_t curRing34Pattern = 0;
    uint64_t ring34GroupCount = 0;

    AggregatorStats stats;

    void CloseRing34Group()
    {
        if (!haveRing34Group) return;
        // Not storing ring34GroupCount on disk (see Ring34RawRec) -- but it's
        // still checked here every time, so a violation of the "always 1"
        // assumption would show up in the report instead of silently losing data.
        Ring34RawRec rec{ curRing34Pattern };
        fwrite(&rec, sizeof(rec), 1, ring34RawFile);
        if (ring34GroupCount != 1) stats.ring34GroupsWithCountNot1++;
        stats.ring34Records++;
        haveRing34Group = false;
    }

    void CloseRing2Group()
    {
        CloseRing34Group();
        if (!haveRing2Group) return;
        uint64_t count = stats.ring34Records - ring2GroupRing34Start;
        RingLevelRawRec rec{ count, curRing2Pattern, ring2GroupRing34Start };
        fwrite(&rec, sizeof(rec), 1, ring2RawFile);
        stats.ring2Records++;
        haveRing2Group = false;
    }

    void CloseRing1Group()
    {
        CloseRing2Group();
        if (!haveRing1Group) return;
        uint64_t count = stats.ring2Records - ring1GroupRing2Start;
        RingLevelRawRec rec{ count, curRing1Pattern, ring1GroupRing2Start };
        fwrite(&rec, sizeof(rec), 1, ring1RawFile);
        stats.ring1Records++;
        haveRing1Group = false;
    }

    void Process(uint64_t pattern, uint64_t color)
    {
        uint32_t ring1Pattern  = (uint32_t)((color >> kRing1Shift)  & ((1u << kRing1Bits) - 1));
        uint32_t ring2Pattern  = (uint32_t)((color >> kRing2Shift)  & ((1u << kRing2Bits) - 1));
        uint16_t ring34Pattern = (uint16_t)((color >> kRing34Shift) & ((1u << kRing34Bits) - 1));

        if (!havePattern || pattern != curPattern)
        {
            CloseRing1Group();

            BOARD_KEY_DISK rec{ pattern, stats.ring1Records };
            BLFWriterRecord(cellsInUseCompWriter, &rec);
            fwrite(&rec, sizeof(rec), 1, cellsInUseRawFile);
            stats.cellsInUseRecords++;

            curPattern = pattern;
            havePattern = true;
        }

        if (!haveRing1Group || ring1Pattern != curRing1Pattern)
        {
            CloseRing1Group();
            curRing1Pattern = ring1Pattern;
            ring1GroupRing2Start = stats.ring2Records;
            haveRing1Group = true;
        }

        if (!haveRing2Group || ring2Pattern != curRing2Pattern)
        {
            CloseRing2Group();
            curRing2Pattern = ring2Pattern;
            ring2GroupRing34Start = stats.ring34Records;
            haveRing2Group = true;
        }

        if (!haveRing34Group || ring34Pattern != curRing34Pattern)
        {
            CloseRing34Group();
            curRing34Pattern = ring34Pattern;
            ring34GroupCount = 0;
            haveRing34Group = true;
        }
        ring34GroupCount++;
        stats.totalBoards++;
    }

    void Finish()
    {
        CloseRing1Group();
    }
};

// ============================================================================
// Per-color pipeline + reporting
// ============================================================================

struct ColorResult
{
    const char* colorName = "";
    uint64_t originalCompBytes = 0;
    uint64_t originalRecordCount = 0;

    AggregatorStats agg;

    uint64_t cellsInUseRawBytes = 0, cellsInUseCompBytes = 0;
    uint64_t ring1RawBytes = 0, ring1CompBytes = 0;
    uint64_t ring2RawBytes = 0, ring2CompBytes = 0;
    uint64_t ring34RawBytes = 0, ring34CompBytes = 0;
};

static ColorResult ProcessOneColor(const char* inputPath, const char* outDir, const char* scratchDir,
                                     const char* colorName, const int* perm, uint64_t chunkCapacity)
{
    printf("Processing %s: %s\n", colorName, inputPath);
    ColorResult result;
    result.colorName = colorName;
    result.originalCompBytes = FileSizeOnDisk(inputPath);

    {
        BLFReader* peek = BLFOpen(inputPath);
        if (!peek) { fprintf(stderr, "ERROR: cannot open store file '%s'\n", inputPath); exit(2); }
        result.originalRecordCount = BLFTrailer(peek)->recordCount;
        BLFClose(&peek);
    }

    // Phase 1: remap + chunked sort + spill sorted runs.
    std::vector<std::string> runPaths = RemapAndSpillRuns(inputPath, scratchDir, colorName, perm, chunkCapacity,
                                                            result.originalRecordCount);

    // Output file paths -- board size + level embedded in the name.
    char cellsInUseRaw[700], cellsInUseComp[700];
    char ring1Raw[700], ring1Comp[700];
    char ring2Raw[700], ring2Comp[700];
    char ring34Raw[700], ring34Comp[700];
    snprintf(cellsInUseRaw,  sizeof(cellsInUseRaw),  "%s\\CellsInUse_%s_%dx%d_lvl%d.raw",   outDir, colorName, kBoardSize, kBoardSize, kLevel);
    snprintf(cellsInUseComp, sizeof(cellsInUseComp), "%s\\CellsInUse_%s_%dx%d_lvl%d.blfzl", outDir, colorName, kBoardSize, kBoardSize, kLevel);
    snprintf(ring1Raw,  sizeof(ring1Raw),  "%s\\Ring_1_%s_%dx%d_lvl%d.raw",   outDir, colorName, kBoardSize, kBoardSize, kLevel);
    snprintf(ring1Comp, sizeof(ring1Comp), "%s\\Ring_1_%s_%dx%d_lvl%d.lz4",   outDir, colorName, kBoardSize, kBoardSize, kLevel);
    snprintf(ring2Raw,  sizeof(ring2Raw),  "%s\\Ring_2_%s_%dx%d_lvl%d.raw",   outDir, colorName, kBoardSize, kBoardSize, kLevel);
    snprintf(ring2Comp, sizeof(ring2Comp), "%s\\Ring_2_%s_%dx%d_lvl%d.lz4",   outDir, colorName, kBoardSize, kBoardSize, kLevel);
    snprintf(ring34Raw, sizeof(ring34Raw), "%s\\Ring_3_4_%s_%dx%d_lvl%d.raw", outDir, colorName, kBoardSize, kBoardSize, kLevel);
    snprintf(ring34Comp,sizeof(ring34Comp),"%s\\Ring_3_4_%s_%dx%d_lvl%d.lz4", outDir, colorName, kBoardSize, kBoardSize, kLevel);

    // Phase 2+3: k-way merge the runs, streaming straight into the aggregator.
    Aggregator agg;
    agg.cellsInUseCompWriter = BLFWriterOpenZ(cellsInUseComp);
    agg.cellsInUseRawFile = OpenOrDie(cellsInUseRaw, "wb");
    agg.ring1RawFile  = OpenOrDie(ring1Raw, "wb");
    agg.ring2RawFile  = OpenOrDie(ring2Raw, "wb");
    agg.ring34RawFile = OpenOrDie(ring34Raw, "wb");

    std::vector<RunSource> sources(runPaths.size());
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, HeapEntryGreater> heap;
    for (size_t i = 0; i < runPaths.size(); i++)
    {
        sources[i].reader = BLFOpen(runPaths[i].c_str());
        if (!sources[i].reader) { fprintf(stderr, "ERROR: cannot reopen run file '%s'\n", runPaths[i].c_str()); exit(2); }
        if (sources[i].Refill())
            heap.push({ sources[i].buf[sources[i].pos], (int)i });
    }

    char mergeProgressLabel[64];
    snprintf(mergeProgressLabel, sizeof(mergeProgressLabel), "%s merge+aggregate", colorName);
    int mergeLastBucket = -1;

    while (!heap.empty())
    {
        HeapEntry top = heap.top();
        heap.pop();
        agg.Process(top.key.ullCellsInUse, top.key.ullCellColors);
        ReportProgress(mergeProgressLabel, agg.stats.totalBoards, result.originalRecordCount, &mergeLastBucket);

        RunSource& src = sources[top.srcIdx];
        src.pos++;
        if (src.Refill())
            heap.push({ src.buf[src.pos], top.srcIdx });
    }
    agg.Finish();

    result.agg = agg.stats;

    BLFWriterClose(agg.cellsInUseCompWriter);
    fclose(agg.cellsInUseRawFile);
    fclose(agg.ring1RawFile);
    fclose(agg.ring2RawFile);
    fclose(agg.ring34RawFile);
    for (auto& s : sources) BLFClose(&s.reader);

    // Clean up transient run files.
    for (auto& p : runPaths) remove(p.c_str());

    printf("  %s: merge+aggregate done -- %llu CellsInUse, %llu Ring_1, %llu Ring_2, %llu Ring_3_4 records (%llu boards)\n",
           colorName,
           (unsigned long long)result.agg.cellsInUseRecords,
           (unsigned long long)result.agg.ring1Records,
           (unsigned long long)result.agg.ring2Records,
           (unsigned long long)result.agg.ring34Records,
           (unsigned long long)result.agg.totalBoards);

    // Phase 4: compress Ring_1/Ring_2/Ring_3_4 (CellsInUse was already compressed inline above).
    char ring1CompLabel[64], ring2CompLabel[64], ring34CompLabel[64];
    snprintf(ring1CompLabel,  sizeof(ring1CompLabel),  "%s Ring_1 compress",   colorName);
    snprintf(ring2CompLabel,  sizeof(ring2CompLabel),  "%s Ring_2 compress",   colorName);
    snprintf(ring34CompLabel, sizeof(ring34CompLabel), "%s Ring_3_4 compress", colorName);

    result.cellsInUseRawBytes  = FileSizeOnDisk(cellsInUseRaw);
    result.cellsInUseCompBytes = FileSizeOnDisk(cellsInUseComp);
    result.ring1RawBytes  = FileSizeOnDisk(ring1Raw);
    result.ring1CompBytes = CompressRawFileToLZ4Frame(ring1Raw, ring1Comp, ring1CompLabel);
    result.ring2RawBytes  = FileSizeOnDisk(ring2Raw);
    result.ring2CompBytes = CompressRawFileToLZ4Frame(ring2Raw, ring2Comp, ring2CompLabel);
    result.ring34RawBytes  = FileSizeOnDisk(ring34Raw);
    result.ring34CompBytes = CompressRawFileToLZ4Frame(ring34Raw, ring34Comp, ring34CompLabel);
    printf("  %s: compression done\n\n", colorName);

    return result;
}

static void PrintRow(FILE* out, const char* label, uint64_t bytes, uint64_t baselineBytes)
{
    double gb = (double)bytes / (1024.0 * 1024 * 1024);
    double ratio = baselineBytes ? (double)bytes / (double)baselineBytes : 0.0;
    fprintf(out, "  %-28s %18llu  %10.3f GB  %8.4f\n",
            label, (unsigned long long)bytes, gb, ratio);
}

int main(int argc, char* argv[])
{
    if (!SelfCheckRingOrder())
    {
        fprintf(stderr, "Aborting: ring order generator failed self-check.\n");
        return 2;
    }

    char defaultBlack[512], defaultWhite[512], defaultOutDir[512], defaultScratchDir[512];
    snprintf(defaultBlack, sizeof(defaultBlack),
             "Y:\\OthelloLevelBlaster\\Store\\storeDir\\Level_%04d_%dx%d_black_0000.blfzl", kLevel, kBoardSize, kBoardSize);
    snprintf(defaultWhite, sizeof(defaultWhite),
             "Y:\\OthelloLevelBlaster\\Store\\storeDir\\Level_%04d_%dx%d_white_0000.blfzl", kLevel, kBoardSize, kBoardSize);
    snprintf(defaultOutDir, sizeof(defaultOutDir),
             "E:\\OthelloRingSplitAnalysis\\Level_%04d", kLevel);
    snprintf(defaultScratchDir, sizeof(defaultScratchDir),
             "D:\\OthelloRingSplitScratch\\Level_%04d", kLevel);

    const char* blackPath; const char* whitePath; const char* outDir; const char* scratchDir;
    uint64_t chunkCapacity = 200000000ULL; // 200M records/chunk (~3.2 GB) -- tune via 6th CLI arg if needed

    if (argc == 1)
    {
        blackPath = defaultBlack; whitePath = defaultWhite;
        outDir = defaultOutDir; scratchDir = defaultScratchDir;
        printf("No arguments given -- defaulting to level %d:\n  %s\n  %s\n  out: %s\n  scratch: %s\n\n",
               kLevel, blackPath, whitePath, outDir, scratchDir);
    }
    else if (argc == 5 || argc == 6)
    {
        blackPath = argv[1]; whitePath = argv[2]; outDir = argv[3]; scratchDir = argv[4];
        if (argc == 6) chunkCapacity = _strtoui64(argv[5], nullptr, 10);
    }
    else
    {
        fprintf(stderr, "Usage: %s [<black-store-file> <white-store-file> <output-dir> <scratch-dir> [chunk-record-count]]\n", argv[0]);
        return 2;
    }

    if (!CreateFullPath(outDir))     { fprintf(stderr, "ERROR: cannot create output dir '%s'\n", outDir); return 2; }
    if (!CreateFullPath(scratchDir)) { fprintf(stderr, "ERROR: cannot create scratch dir '%s'\n", scratchDir); return 2; }

    std::vector<int> perm = BuildRingPermutation(kRingFrameSize, 0);

    const char* colorNames[2] = { "black", "white" };
    const char* inputPaths[2] = { blackPath, whitePath };
    ColorResult results[2];
    for (int c = 0; c < 2; c++)
        results[c] = ProcessOneColor(inputPaths[c], outDir, scratchDir, colorNames[c], perm.data(), chunkCapacity);

    // --------------------------------------------------------------------
    // Report
    // --------------------------------------------------------------------
    char reportPath[700];
    snprintf(reportPath, sizeof(reportPath), "%s\\report.txt", outDir);
    FILE* rf = OpenOrDie(reportPath, "w");

    FILE* outs[2] = { stdout, rf };
    for (FILE* out : outs)
    {
        fprintf(out, "OthelloRingSplitAnalyzer -- Level %d (ring-gathered, 4-file nested split)\n", kLevel);
        fprintf(out, "================================================================\n");

        uint64_t totalOrigComp = 0, totalOrigRawEquiv = 0;
        uint64_t totalNestedRaw = 0, totalNestedComp = 0;
        uint64_t totalRing34CountNot1 = 0;

        for (int c = 0; c < 2; c++)
        {
            const ColorResult& r = results[c];
            uint64_t origRawEquiv = r.originalRecordCount * sizeof(BOARD_KEY_DISK);
            uint64_t nestedRaw  = r.cellsInUseRawBytes + r.ring1RawBytes + r.ring2RawBytes + r.ring34RawBytes;
            uint64_t nestedComp = r.cellsInUseCompBytes + r.ring1CompBytes + r.ring2CompBytes + r.ring34CompBytes;

            fprintf(out, "\n--- %s (%llu boards, %llu unique patterns, %llu Ring_1 groups, %llu Ring_2 groups) ---\n",
                    r.colorName, (unsigned long long)r.originalRecordCount,
                    (unsigned long long)r.agg.cellsInUseRecords,
                    (unsigned long long)r.agg.ring1Records,
                    (unsigned long long)r.agg.ring2Records);
            fprintf(out, "  %-28s %18s  %14s  %8s\n", "", "bytes", "size", "ratio*");
            PrintRow(out, "Original store (.blfzl)", r.originalCompBytes, r.originalCompBytes);
            PrintRow(out, "Original raw-equivalent", origRawEquiv, r.originalCompBytes);
            PrintRow(out, "CellsInUse.raw", r.cellsInUseRawBytes, origRawEquiv);
            PrintRow(out, "CellsInUse.blfzl", r.cellsInUseCompBytes, r.originalCompBytes);
            PrintRow(out, "Ring_1.raw", r.ring1RawBytes, origRawEquiv);
            PrintRow(out, "Ring_1.lz4", r.ring1CompBytes, r.originalCompBytes);
            PrintRow(out, "Ring_2.raw", r.ring2RawBytes, origRawEquiv);
            PrintRow(out, "Ring_2.lz4", r.ring2CompBytes, r.originalCompBytes);
            PrintRow(out, "Ring_3_4.raw", r.ring34RawBytes, origRawEquiv);
            PrintRow(out, "Ring_3_4.lz4", r.ring34CompBytes, r.originalCompBytes);
            PrintRow(out, "Nested raw total", nestedRaw, origRawEquiv);
            PrintRow(out, "Nested compressed total", nestedComp, r.originalCompBytes);
            fprintf(out, "  Ring_1 records == CellsInUse records: %s (%llu vs %llu) -- expected for a %dx%d\n"
                         "    board, since the true 8x8 border is never occupied and Ring_1's subpattern\n"
                         "    is always 0 (one trivial Ring_1 group per top-level pattern, no real split)\n",
                    (r.agg.ring1Records == r.agg.cellsInUseRecords) ? "YES" : "NO -- unexpected",
                    (unsigned long long)r.agg.ring1Records, (unsigned long long)r.agg.cellsInUseRecords,
                    kBoardSize, kBoardSize);
            fprintf(out, "  Ring_3_4 groups with count != 1: %llu (expected 0 -- pattern+Ring_1+Ring_2+Ring_3_4\n"
                         "    together reconstruct the full board exactly, and the source store has no\n"
                         "    duplicate boards, so every Ring_3_4 group should have exactly 1 member)\n",
                    (unsigned long long)r.agg.ring34GroupsWithCountNot1);
            fprintf(out, "  (* ratio is row/new-item vs. original-raw-equivalent for raw rows, "
                         "vs. original .blfzl for compressed rows)\n");

            totalOrigComp += r.originalCompBytes;
            totalOrigRawEquiv += origRawEquiv;
            totalNestedRaw += nestedRaw;
            totalNestedComp += nestedComp;
            totalRing34CountNot1 += r.agg.ring34GroupsWithCountNot1;
        }

        fprintf(out, "\n--- Combined (black + white) ---\n");
        PrintRow(out, "Original store (.blfzl)", totalOrigComp, totalOrigComp);
        PrintRow(out, "Original raw-equivalent", totalOrigRawEquiv, totalOrigComp);
        PrintRow(out, "Nested raw total", totalNestedRaw, totalOrigRawEquiv);
        PrintRow(out, "Nested compressed total", totalNestedComp, totalOrigComp);
        fprintf(out, "Ring_3_4 groups with count != 1 (both colors): %llu\n", (unsigned long long)totalRing34CountNot1);
        fprintf(out, "\n");
    }

    fclose(rf);
    printf("Report also written to: %s\n", reportPath);
    return 0;
}
