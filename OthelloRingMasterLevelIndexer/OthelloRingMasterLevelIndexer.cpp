/*
** Filename:  OthelloRingMasterLevelIndexer.cpp
**
** Purpose:
**   Real, permanent tool (not a disposable diagnostic): splits an already-
**   completed level's ring files (CellsInUse, Ring_2, Ring_3_4) into
**   independently-decodable segments, each named by its own starting
**   global record ordinal in hex, so a plain directory listing already
**   sorts the same as ordinal order -- no separate index file needed. See
**   project_othello_web_ui_design memory (2026-09-13/14 sections) for the
**   full design and the real, validated numbers this is built on:
**   ~500MB segments land at ~3.8-3.9s decode time (comfortably under the
**   ~5s lookup budget) with ~0.00% compression-ratio cost, confirmed on
**   real Ring_3_4 data at levels 16 and 20.
**
**   Started as a Ring_3_4-only tool (originally
**   OthelloRingMasterRing34Indexer), then generalized once real Ring_2
**   file sizes were measured across every completed level: Ring_2 itself
**   hits multi-gigabyte sizes just as Ring_3_4 does (real numbers: level
**   21's Ring_2 file alone is 6.09GB compressed, and a wholesale
**   decompress-to-vector of its ~2.2 BILLION group boundaries is exactly
**   what made an earlier version of this tool go dangerously memory-heavy
**   -- see GroupBoundaryStream's own history).
**
**   EVERY ring is now ALWAYS segmented -- even CellsInUse, which real
**   numbers show never gets anywhere close to a real size trigger (tops
**   out under 20MB even at its own real peak, level 19), producing
**   exactly one segment for it at every 6x6 level today. This is
**   deliberate, not wasted work: the goal is one single, consistent
**   on-disk interface every reader goes through for every ring at every
**   level, with no "is this one flat or segmented" branch anywhere. Two
**   real reasons this matters more than it might look like it should:
**   (1) a future 8x8 exploration run would very plausibly blow CellsInUse
**   itself past any reasonable trigger too (8x8's legal-position count is
**   many orders of magnitude past 6x6's, and CellsInUse already grows
**   ~230,000x from level 0 to its 6x6 peak) -- better the read interface
**   is already uniform before that's ever tested than special-cased later;
**   (2) the long-term plan is to eventually fold this segmenting directly
**   into the live solver's own ring-file writers and retire this
**   standalone tool entirely -- keeping the read side consistent now means
**   that switch is a pure backend swap for consumers, not a rewrite.
**
**   Segment boundaries are aligned to the *next ring up*'s own group
**   boundaries where one exists -- never split one parent group's children
**   across two segments (this is what lets a later lookup resolve to
**   either one whole segment or several complete ones, never a partial
**   straddle). Ring_3_4 aligns to Ring_2's own `.offset` field; Ring_2
**   aligns to CellsInUse's own `.offset` field. Both are already real,
**   existing fields (confirmed via RingNestedIndexReader::FindBoardPosition's
**   own usage) -- finding boundaries is just one cheap sequential pass over
**   the parent ring (always much smaller than the ring being segmented),
**   never a guess. CellsInUse itself sits at the ROOT of the hierarchy --
**   nothing points into it by ordinal from above, so it has no group-
**   boundary constraint at all and can be cut at any record position once
**   the size trigger fires (see SegmentOneRing's boundaryParentPath ==
**   nullptr case). The --target-size trigger is a trigger, not a hard cut
**   point where a boundary parent exists: once crossed, the cut waits for
**   the *next* real group boundary rather than firing immediately.
**
**   The size trigger itself checks each segment's REAL compressed bytes
**   written so far (RSFWriterBytesWrittenSoFar, Utility/RingStoreFile.h),
**   not an estimate. An earlier version computed one average bytes/record
**   from the whole source file's own real totals and used records-written
**   as a proxy -- workable, but only possible because this runs as a
**   post-pass over an already-finished file with a known total. A live
**   writer (the eventual solver-native version this is meant to prepare
**   for) would never have that average available in advance, so it would
**   inevitably compute a different one and land on different segment
**   boundaries than this tool did, even against identical data. Checking
**   real bytes instead makes segmenting a pure function of the record
**   stream itself -- same data in, same segment boundaries out, whether
**   this tool built them as a post-pass or a future solver builds them
**   natively while writing. That reproducibility, not just simplicity, is
**   why this was worth changing.
**
**   A ring's segment directory only gets a manifest.txt once segmenting
**   completes AND self-verifies (segmented record count matches the
**   source's real record count exactly) -- this is the one thing a lookup
**   consumer should trust to know a ring is really, safely segmented.
**   Every run purges any stale manifest and any stale segment files for a
**   ring BEFORE writing a single new one, so an interrupted run (killed,
**   crashed, or just an earlier run with a different --target-size) can
**   never leave a manifest that still looks valid over the wrong data.
**
**   The manifest also carries each segment's own [minPattern, maxPattern]
**   (that segment's first and last record's `.pattern` value, since the
**   whole ring is one globally sorted stream just physically chunked) --
**   this is what lets a real value-based lookup (see BoardLookup/
**   BoardLookupSearch.h) binary-search which ONE segment to decode instead
**   of scanning every segment in ordinal order, which would take minutes
**   at levels with thousands of segments.
**
**   Reads from the live store's storeDir (read-only) and writes segments
**   to a dedicated --levelindex-dir, kept entirely separate from
**   storeDir/storeMergeDir/writerDir so this never collides with a live
**   solver's own active I/O. Each level/player gets its own directory
**   under there, and each ring gets its own subdirectory within that --
**   a single level can produce thousands of segments per ring, so this
**   keeps any one directory listing small.
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

/* Records read from a boundary parent, or from the ring being segmented,
** are pulled in batches this large before being re-checked against the
** progress/boundary logic -- keeps memory flat regardless of how many
** total records a real level has.
*/
static constexpr int STREAM_BATCH = 65536;

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
** Function: RSFShapeSize
** @brief    Returns the on-the-wire record size for a shape this tool
**           actually handles (source rings and boundary parents alike).
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
** Function: ExtractPattern
** @brief    Pulls a record's own `.pattern` field out as a uniform uint64_t
**           regardless of shape -- CellsInUse/Ring_2 carry it in the first
**           8/4 bytes respectively, Ring_3_4 in its only 2 bytes. Used to
**           track each segment's own min/max pattern for the manifest, so
**           a future value-based lookup can binary-search which segment to
**           open without decoding every one of them first.
*/
static uint64_t ExtractPattern(RSFRecordShape shape, const uint8_t* rec)
{
    switch (shape)
    {
        case RSF_SHAPE_PAIR64:     return reinterpret_cast<const UINT64_PAIR*>(rec)->hi;
        case RSF_SHAPE_RING_LEVEL: return (uint64_t)reinterpret_cast<const RingLevelRec*>(rec)->pattern;
        case RSF_SHAPE_LEAF16:     return (uint64_t)reinterpret_cast<const Ring34Rec*>(rec)->pattern;
    }
    Fatal(FATAL_MERGE_LOGIC_ERROR, "ExtractPattern: unsupported shape %d", (int)shape);
    return 0;
}

/*
** Type:    SegmentPatternRange
** @brief   One finished segment's own [minPattern, maxPattern] -- since
**          the whole ring is one globally sorted stream just physically
**          chunked, a segment's first record written is always its min
**          and its last is always its max. Written into the manifest so a
**          value-based lookup can pick the one right segment directly.
*/
struct SegmentPatternRange
{
    uint64_t startOrdinal;
    uint64_t minPattern;
    uint64_t maxPattern;
};

/*
** Function: PurgeExistingSegments
** @brief    Deletes every existing seg*.rsfzl file in a ring's own segment
**           directory before a fresh run writes new ones. Without this, a
**           rerun (e.g. after an interrupted prior attempt, or with a
**           different --target-size) could leave stale segments mixed in
**           with the new ones -- since segment boundaries depend on the
**           trigger size and real per-level compression, a differently-
**           configured old run's leftovers wouldn't necessarily get
**           overwritten by a new one, silently corrupting the index.
** @param    ringSegmentDir - the ring's own segment subdirectory (already
**                            created by the caller, or may not exist yet)
*/
static void PurgeExistingSegments(const char* ringSegmentDir)
{
    char pattern[MAX_FULL_PATH_NAME];
    snprintf(pattern, sizeof(pattern), "%s\\seg*.rsfzl", ringSegmentDir);

    WIN32_FIND_DATAA fd = {};
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;   /* nothing to purge -- a fresh directory, or first run for this ring */

    int purged = 0;
    do
    {
        char fullPath[MAX_FULL_PATH_NAME];
        snprintf(fullPath, sizeof(fullPath), "%s\\%s", ringSegmentDir, fd.cFileName);
        if (!DeleteFileA(fullPath))
            Fatal(FATAL_FILE_OPEN, "PurgeExistingSegments: could not delete stale segment '%s'", fullPath);
        purged++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (purged > 0)
        printf("Purged %d stale segment file(s) from a previous run.\n", purged);
}

/*
** Type:    GroupBoundaryStream
** @brief   Streams a parent ring's group-start ordinals (its own `.offset`
**          field) one at a time via a small, fixed-size rolling buffer --
**          never holds a whole parent ring's boundary list in memory.
**          Handles either boundary-parent shape this tool needs:
**          RSF_SHAPE_RING_LEVEL (Ring_2, when segmenting Ring_3_4) or
**          RSF_SHAPE_PAIR64 (CellsInUse, when segmenting Ring_2 --
**          CellsInUseRec is binary-compatible with UINT64_PAIR, pattern
**          in `hi`, offset in `lo`). A real level's boundary count can run
**          into the billions at deep levels (real numbers: level 21's
**          Ring_2 file alone produces ~2.2 billion CellsInUse-independent
**          boundaries), so an earlier, Ring_3_4-only version of this
**          struct that pre-loaded every boundary into one std::vector was
**          a genuine, real wholesale-load bug -- caught, and the user's
**          own machine independently confirmed it (heavy paging) before
**          this streaming version replaced it.
*/
struct GroupBoundaryStream
{
    RSFReader*            pReader     = nullptr;
    RSFRecordShape        shape       = RSF_SHAPE_RING_LEVEL;
    int                   recSize     = 0;
    std::vector<uint8_t>  batch;      /* heap-allocated, not a stack-embedded fixed array --
                                       ** this struct is declared as a plain local in the
                                       ** segmenting function, and a raw fixed-size array
                                       ** member would live on the stack there, real overflow
                                       ** risk against a typical 1MB default thread stack.
                                       ** Same lesson already learned once in this codebase. */
    int                   batchPos        = 0;
    int                   batchFilledRecs = 0;
    bool                  exhausted       = false;
};

/*
** Function: GroupBoundaryExtractOffset
** @brief    Pulls the `.offset` (or `.lo`, for a PAIR64 boundary parent
**           like CellsInUse) field out of one raw record, without needing
**           the caller to know which of the two boundary shapes it is.
** @param    shape - RSF_SHAPE_RING_LEVEL or RSF_SHAPE_PAIR64
** @param    rec   - pointer to one raw record of that shape's size
** @return   The boundary ordinal.
*/
static uint64_t GroupBoundaryExtractOffset(RSFRecordShape shape, const uint8_t* rec)
{
    if (shape == RSF_SHAPE_PAIR64)
        return reinterpret_cast<const UINT64_PAIR*>(rec)->lo;
    if (shape == RSF_SHAPE_RING_LEVEL)
        return reinterpret_cast<const RingLevelRec*>(rec)->offset;
    Fatal(FATAL_MERGE_LOGIC_ERROR, "GroupBoundaryExtractOffset: unsupported boundary shape %d", (int)shape);
    return 0;
}

/*
** Function: GroupBoundaryStreamOpen
** @brief    Opens a parent ring file for streaming boundary access.
** @param    path  - path to the source boundary-parent file
** @param    shape - RSF_SHAPE_RING_LEVEL or RSF_SHAPE_PAIR64
** @param    s     - out: stream state to initialize
*/
static void GroupBoundaryStreamOpen(const char* path, RSFRecordShape shape, GroupBoundaryStream* s)
{
    s->pReader = RSFOpenShaped(path, shape);
    if (!s->pReader)
        Fatal(FATAL_FILE_OPEN, "GroupBoundaryStreamOpen: could not open '%s' (corrupt or truncated)", path);
    s->shape   = shape;
    s->recSize = RSFShapeSize(shape);
    s->batch.resize((size_t)STREAM_BATCH * s->recSize);
}

/*
** Function: GroupBoundaryStreamNext
** @brief    Advances to and returns the next parent group's own starting
**           child ordinal. Refills its small internal batch from disk only
**           when exhausted, so memory stays flat regardless of how many
**           groups the parent ring actually has.
** @param    s          - the stream to advance
** @param    pOutOffset - out: the next boundary ordinal, if returned true
** @return   true if a boundary was returned; false at end of stream.
*/
static bool GroupBoundaryStreamNext(GroupBoundaryStream* s, uint64_t* pOutOffset)
{
    if (s->exhausted) return false;
    if (s->batchPos >= s->batchFilledRecs)
    {
        s->batchFilledRecs = RSFReadShaped(s->pReader, s->batch.data(), STREAM_BATCH);
        s->batchPos = 0;
        if (s->batchFilledRecs == 0) { s->exhausted = true; return false; }
    }
    *pOutOffset = GroupBoundaryExtractOffset(s->shape, &s->batch[(size_t)s->batchPos * s->recSize]);
    s->batchPos++;
    return true;
}

/*
** Function: GroupBoundaryStreamClose
** @brief    Closes a boundary stream's underlying reader.
*/
static void GroupBoundaryStreamClose(GroupBoundaryStream* s)
{
    RSFClose(&s->pReader);
}

/*
** Type:    SegmentRingResult
** @brief   What one call to SegmentOneRing actually did, for the final
**          per-level summary. Segmenting always happens now (even a ring
**          well under --target-size still gets exactly one segment), so
**          this only carries real counts, not a "did it even segment" flag.
*/
struct SegmentRingResult
{
    uint64_t segmentCount = 0;
    uint64_t totalRecords = 0;
};

/*
** Function: SegmentOneRing
** @brief    Segments one ring file (CellsInUse, Ring_2, or Ring_3_4) into
**           independently-decodable segments -- ALWAYS, even when the
**           source is well under targetBytes (that case just naturally
**           produces exactly one segment, since the cut trigger never
**           fires -- see the main loop below). Consistency, not a size
**           optimization, is the point: see this file's own top-of-file
**           Notes.
**
**           When boundaryParentPath is non-null, cuts are aligned to that
**           parent ring's own `.offset` field so a cut never splits one
**           parent group's children across two segments. When it's
**           nullptr (CellsInUse's case -- nothing sits above it in the
**           hierarchy, so there is no group to protect), every record
**           position is a valid cut point and a cut fires as soon as the
**           size trigger crosses, with no boundary wait.
** @param    ringName             - "CellsInUse", "Ring2", or "Ring34"
**                                  (used for the segment subdirectory name
**                                  and log lines)
** @param    sourcePath           - path to the ring file being segmented
** @param    sourceShape          - that ring file's record shape
** @param    boundaryParentPath   - path to the parent ring providing real
**                                  group boundaries (Ring_2 for Ring_3_4;
**                                  CellsInUse for Ring_2), or nullptr if
**                                  this ring has no parent (CellsInUse)
** @param    boundaryParentShape  - the parent ring's record shape (ignored
**                                  when boundaryParentPath is nullptr)
** @param    levelDir             - this level/player's own directory
**                                  (from RSFNameLevelIndexDir)
** @param    targetBytes          - nominal segment size trigger
** @return   Real segment/record counts actually written.
*/
static SegmentRingResult SegmentOneRing(const char* ringName, const char* sourcePath, RSFRecordShape sourceShape,
                                         const char* boundaryParentPath, RSFRecordShape boundaryParentShape,
                                         const char* levelDir, uint64_t targetBytes)
{
    SegmentRingResult result;
    bool alignToBoundary = (boundaryParentPath != nullptr);

    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExA(sourcePath, GetFileExInfoStandard, &fad))
        Fatal(FATAL_FILE_OPEN, "SegmentOneRing(%s): source file not found: '%s'", ringName, sourcePath);
    uint64_t origOnDiskBytes = ((uint64_t)fad.nFileSizeHigh << 32) | (uint64_t)fad.nFileSizeLow;

    char ringSegmentDir[MAX_FULL_PATH_NAME];
    RSFNameRingSegmentDir(ringSegmentDir, sizeof(ringSegmentDir), levelDir, ringName);
    char manifestPath[MAX_FULL_PATH_NAME];
    RSFNameRingManifestFile(manifestPath, sizeof(manifestPath), ringSegmentDir);

    if (!CreateFullPath(ringSegmentDir))
        Fatal(FATAL_CREATE_DIR_FAILED, "Cannot create ring segment directory '%s'", ringSegmentDir);
    /* Delete any stale manifest FIRST, before writing a single new segment
    ** -- a run that gets killed partway through must never leave a
    ** manifest that still looks valid over an incomplete segment set.
    */
    DeleteFileA(manifestPath);
    PurgeExistingSegments(ringSegmentDir);

    printf("\n=== %s ===\n", ringName);

    GroupBoundaryStream boundaryStream;
    uint64_t nextBoundary     = 0;
    bool     haveNextBoundary = false;
    if (alignToBoundary)
    {
        printf("Opening boundary source for streaming: '%s'\n", boundaryParentPath);
        fflush(stdout);
        GroupBoundaryStreamOpen(boundaryParentPath, boundaryParentShape, &boundaryStream);
        haveNextBoundary = GroupBoundaryStreamNext(&boundaryStream, &nextBoundary);
        if (!haveNextBoundary)
            Fatal(FATAL_FILE_OPEN, "%s: boundary source '%s' has zero records -- cannot determine group boundaries",
                  ringName, boundaryParentPath);
    }
    else
    {
        printf("Root of the hierarchy -- no parent group to align to, cutting purely by size trigger.\n");
    }

    RSFReader* pReader = RSFOpenShaped(sourcePath, sourceShape);
    if (!pReader)
        Fatal(FATAL_FILE_OPEN, "Could not open '%s' (corrupt or truncated)", sourcePath);

    uint64_t totalRecords = RSFReaderTrailer(pReader)->recordCount;
    if (totalRecords == 0)
        Fatal(FATAL_FILE_OPEN, "Source '%s' has zero records, nothing to index", sourcePath);

    printf("Source: '%s'\n", sourcePath);
    printf("Real records: %llu, real on-disk compressed bytes: %llu\n",
           (unsigned long long)totalRecords, (unsigned long long)origOnDiskBytes);
    printf("Target segment size: %llu bytes -- checked against each segment's own real\n"
           "compressed bytes as it's written (RSFWriterBytesWrittenSoFar), not estimated\n"
           "from this file's average bytes/record.\n\n",
           (unsigned long long)targetBytes);

    int recSize = RSFShapeSize(sourceShape);

    char firstSegPath[MAX_FULL_PATH_NAME];
    RSFNameRingSegmentFile(firstSegPath, sizeof(firstSegPath), ringSegmentDir, 0);
    RSFWriter* pw = RSFWriterOpenZLShaped(firstSegPath, sourceShape);

    uint64_t segmentStartOrdinal = 0;
    uint64_t currentOrdinal      = 0;
    bool     cutRequested        = false;
    uint64_t segmentCount        = 0;
    uint64_t totalSegmentBytes   = 0;
    uint64_t minSegBytes = UINT64_MAX, maxSegBytes = 0;

    std::vector<SegmentPatternRange> segRanges;
    uint64_t segMinPattern = UINT64_MAX, segMaxPattern = 0;

    std::vector<uint8_t> batch((size_t)STREAM_BATCH * recSize);
    int n;
    uint64_t processed = 0;
    int lastPercentBucket = -1;
    uint64_t startTickMs = GetTickCount64();

    printf("Building %s segments in '%s'...\n", ringName, ringSegmentDir);
    fflush(stdout);

    while ((n = RSFReadShaped(pReader, batch.data(), STREAM_BATCH)) > 0)
    {
        for (int i = 0; i < n; i++)
        {
            /* No parent to align to (CellsInUse) -- every position is a
            ** valid cut point. Otherwise, only a real parent group
            ** boundary counts.
            */
            bool atGroupBoundary = !alignToBoundary || (haveNextBoundary && currentOrdinal == nextBoundary);
            if (alignToBoundary && atGroupBoundary)
                haveNextBoundary = GroupBoundaryStreamNext(&boundaryStream, &nextBoundary);

            /* Only cut once BOTH the size trigger has fired AND we're at a
            ** valid cut point -- never mid-group when one exists, never
            ** mid-record.
            */
            if (cutRequested && atGroupBoundary && currentOrdinal > segmentStartOrdinal)
            {
                uint64_t segBytes = 0;
                RSFWriterClose(pw, &segBytes);
                segmentCount++;
                totalSegmentBytes += segBytes;
                if (segBytes < minSegBytes) minSegBytes = segBytes;
                if (segBytes > maxSegBytes) maxSegBytes = segBytes;
                segRanges.push_back({ segmentStartOrdinal, segMinPattern, segMaxPattern });
                segMinPattern = UINT64_MAX;
                segMaxPattern = 0;

                segmentStartOrdinal = currentOrdinal;
                cutRequested        = false;

                char segPath[MAX_FULL_PATH_NAME];
                RSFNameRingSegmentFile(segPath, sizeof(segPath), ringSegmentDir, segmentStartOrdinal);
                pw = RSFWriterOpenZLShaped(segPath, sourceShape);
            }

            RSFWriterRecordShaped(pw, &batch[(size_t)i * recSize]);
            uint64_t recPattern = ExtractPattern(sourceShape, &batch[(size_t)i * recSize]);
            if (recPattern < segMinPattern) segMinPattern = recPattern;
            if (recPattern > segMaxPattern) segMaxPattern = recPattern;
            currentOrdinal++;

            /* Real bytes actually written to THIS segment so far, not an
            ** estimate derived from the source file's average bytes/record
            ** -- see RSFWriterBytesWrittenSoFar's own Notes.
            */
            if (!cutRequested && RSFWriterBytesWrittenSoFar(pw) >= targetBytes)
                cutRequested = true;
        }

        processed += (uint64_t)n;
        if (totalRecords > 0)
        {
            int bucket = (int)(processed * 100 / totalRecords);   /* 1% granularity */
            /* Skip bucket 0 -- a real bug found live in this tool's
            ** earlier history: the first read batch is an almost-zero
            ** fraction of a billion-record file, so an ETA extrapolated
            ** from it is meaningless noise, not a real estimate.
            */
            if (bucket > lastPercentBucket && bucket >= 1)
            {
                lastPercentBucket = bucket;
                double pctDone  = (double)processed / (double)totalRecords * 100.0;
                double elapsedS = (double)(GetTickCount64() - startTickMs) / 1000.0;
                double etaS     = (pctDone > 0.0) ? elapsedS * (100.0 - pctDone) / pctDone : 0.0;
                printf("  [%s] %d%% (%llu / %llu records, %llu segments so far)  elapsed=%.0fs  eta=%.0fs\n",
                       ringName, bucket, (unsigned long long)processed, (unsigned long long)totalRecords,
                       (unsigned long long)segmentCount, elapsedS, etaS);
                fflush(stdout);
            }
        }
    }
    RSFClose(&pReader);
    if (alignToBoundary)
        GroupBoundaryStreamClose(&boundaryStream);

    /* Close the final segment. */
    {
        uint64_t segBytes = 0;
        RSFWriterClose(pw, &segBytes);
        segmentCount++;
        totalSegmentBytes += segBytes;
        if (segBytes < minSegBytes) minSegBytes = segBytes;
        if (segBytes > maxSegBytes) maxSegBytes = segBytes;
        segRanges.push_back({ segmentStartOrdinal, segMinPattern, segMaxPattern });
    }

    /* Never silently report success on a mismatch -- if the segmented
    ** output doesn't account for exactly the source's real record count,
    ** something is genuinely wrong and must not be trusted.
    */
    if (currentOrdinal != totalRecords)
        Fatal(FATAL_MERGE_LOGIC_ERROR,
              "%s: wrote %llu records across %llu segments but source '%s' has %llu -- "
              "mismatch, refusing to report success",
              ringName, (unsigned long long)currentOrdinal, (unsigned long long)segmentCount, sourcePath,
              (unsigned long long)totalRecords);

    /* Manifest written ONLY now, after self-verification passes -- this is
    ** the one signal a lookup consumer should trust to know this ring is
    ** really, safely segmented (see this file's own Notes).
    */
    FILE* mf = fopen(manifestPath, "w");
    if (!mf)
        Fatal(FATAL_FILE_OPEN, "Could not write manifest '%s'", manifestPath);
    fprintf(mf, "totalRecords=%llu\n", (unsigned long long)totalRecords);
    fprintf(mf, "segmentCount=%llu\n", (unsigned long long)segmentCount);
    fprintf(mf, "targetBytes=%llu\n", (unsigned long long)targetBytes);
    fprintf(mf, "sourceOnDiskBytes=%llu\n", (unsigned long long)origOnDiskBytes);
    /* Per-segment [min,max] pattern, keyed by that segment's own starting
    ** ordinal (same hex form as its filename) -- lets a value-based lookup
    ** binary-search which ONE segment to decode instead of scanning every
    ** segment in order, which would be minutes at levels with thousands
    ** of segments.
    */
    for (const auto& r : segRanges)
    {
        fprintf(mf, "seg%016llx.minPattern=%016llx\n", (unsigned long long)r.startOrdinal, (unsigned long long)r.minPattern);
        fprintf(mf, "seg%016llx.maxPattern=%016llx\n", (unsigned long long)r.startOrdinal, (unsigned long long)r.maxPattern);
    }
    fclose(mf);

    char totalStr[32], origStr[32], avgStr[32], minStr[32], maxStr[32];
    FormatBytes(totalSegmentBytes, totalStr, sizeof(totalStr));
    FormatBytes(origOnDiskBytes, origStr, sizeof(origStr));
    FormatBytes(segmentCount > 0 ? totalSegmentBytes / segmentCount : 0, avgStr, sizeof(avgStr));
    FormatBytes(minSegBytes == UINT64_MAX ? 0 : minSegBytes, minStr, sizeof(minStr));
    FormatBytes(maxSegBytes, maxStr, sizeof(maxStr));

    printf("\n%s done. %llu segments, %llu records (matches source exactly).\n",
           ringName, (unsigned long long)segmentCount, (unsigned long long)currentOrdinal);
    printf("Total: %s (original single-stream: %s)   Avg/segment: %s   Min: %s   Max: %s\n",
           totalStr, origStr, avgStr, minStr, maxStr);
    printf("Segments written to: %s\n", ringSegmentDir);

    result.segmentCount = segmentCount;
    result.totalRecords = currentOrdinal;
    return result;
}

/*
** Function: PrintUsage
** @brief    Prints command-line usage help.
*/
static void PrintUsage(const char* prog)
{
    printf("Usage: %s --level N [options]\n\n", prog);
    printf("  --level N           Level to index (real, already-complete level)\n");
    printf("  --color C           black or white                                          [default: black]\n");
    printf("  --board-size N      Board size: 4, 6, or 8                                   [default: 6]\n");
    printf("  --store-drive L     Drive letter the source store lives on                   [default: Y]\n");
    printf("  --store-dir P       Sub-path on store drive (no drive letter)                  [default: \\OthelloRingMaster\\Store]\n");
    printf("  --levelindex-drive L  Drive letter for the level-index output               [default: Y]\n");
    printf("  --levelindex-dir P  Sub-path on that drive (no drive letter)                  [default: \\OthelloRingMaster\\Store\\levelIndexDir]\n");
    printf("  --target-size SIZE  Nominal segment size trigger (e.g. 500MB)                 [default: 500MB]\n");
    printf("  --help              Show this help\n\n");
    printf("Always segments all three rings (CellsInUse, Ring_2, Ring_3_4) -- even one well\n");
    printf("under --target-size still gets exactly one segment, for a single consistent\n");
    printf("on-disk interface every reader goes through. Ring_2 and Ring_3_4 cuts align to\n");
    printf("the next ring up's own boundary field (CellsInUse for Ring_2, Ring_2 for\n");
    printf("Ring_3_4) so no group is ever split across two segments; CellsInUse has no\n");
    printf("parent to align to, so it cuts purely by size. Writes to --levelindex-dir (one\n");
    printf("directory per level/player, one subdirectory per ring), entirely separate from\n");
    printf("the live store's own working directories, so this is safe to run alongside a\n");
    printf("live solve.\n\n");
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
        else if (strcmp(argv[i], "--levelindex-drive") == 0) { REQUIRE_NEXT("--levelindex-drive") levelIndexDrive = (char)toupper((unsigned char)argv[i][0]); }
        else if (strcmp(argv[i], "--levelindex-dir") == 0)   { REQUIRE_NEXT("--levelindex-dir")   strncpy(levelIndexDirNoDrive, argv[i], sizeof(levelIndexDirNoDrive) - 1); }
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

    uint64_t targetBytes = ParseByteSize(targetSizeArg);
    if (targetBytes == 0)
    {
        printf("ERROR: could not parse --target-size '%s'\n", targetSizeArg);
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
    if (!CreateFullPath(levelDir))
        Fatal(FATAL_CREATE_DIR_FAILED, "Cannot create level index directory '%s'", levelDir);

    printf("Level %d, %dx%d, %s -- level index directory: '%s'\n", level, boardSize, boardSize, color, levelDir);

    /* CellsInUse is the root -- no parent, so no boundary source (nullptr).
    ** Ring_2's own boundaries come from CellsInUse; Ring_3_4's come from
    ** Ring_2 -- always the ORIGINAL flat file in storeDir either way, since
    ** segmenting only ever writes to levelIndexDir and never touches
    ** storeDir itself. Processed top-down to mirror the hierarchy's own
    ** shape, though order doesn't matter for correctness.
    */
    SegmentRingResult cellsInUseResult = SegmentOneRing(
        "CellsInUse", cellsInUsePath, RSF_SHAPE_PAIR64,
        nullptr, RSF_SHAPE_PAIR64,
        levelDir, targetBytes);

    SegmentRingResult ring2Result = SegmentOneRing(
        "Ring2", ring2Path, RSF_SHAPE_RING_LEVEL,
        cellsInUsePath, RSF_SHAPE_PAIR64,
        levelDir, targetBytes);

    SegmentRingResult ring34Result = SegmentOneRing(
        "Ring34", ring34Path, RSF_SHAPE_LEAF16,
        ring2Path, RSF_SHAPE_RING_LEVEL,
        levelDir, targetBytes);

    printf("\n=== Summary: level %d, %dx%d, %s ===\n", level, boardSize, boardSize, color);
    printf("CellsInUse: %llu segment(s), %llu records\n",
           (unsigned long long)cellsInUseResult.segmentCount, (unsigned long long)cellsInUseResult.totalRecords);
    printf("Ring_2:     %llu segment(s), %llu records\n",
           (unsigned long long)ring2Result.segmentCount, (unsigned long long)ring2Result.totalRecords);
    printf("Ring_3_4:   %llu segment(s), %llu records\n",
           (unsigned long long)ring34Result.segmentCount, (unsigned long long)ring34Result.totalRecords);

    return 0;
}
