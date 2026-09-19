/*
** Filename:  BoardLookupSearch.cpp
**
** Purpose:
**   Implements BoardLookupSearch.h -- see that file's own Purpose for the
**   real design (why multi-segment-spanning search is only ever needed
**   for CellsInUse's own top-level search, never for Ring_2/Ring_3_4's
**   group-scoped descent).
**
**   Board size 4 (no Ring_2 at all) and board size 8 (needs Ring_1 too)
**   are deliberately NOT implemented here yet -- OthelloRingMasterLevelIndexer
**   itself only ever segments CellsInUse/Ring_2/Ring_3_4 today, so there is
**   real segmented data for neither case to search against. A board whose
**   size hits either path gets a clear notFoundReason, not a silent wrong
**   answer.
**
**   A ring's manifest (RSFFileName.h's RSFNameRingManifestFile) is never
**   loaded wholesale, not even for CellsInUse's own value-based search:
**   its real entry count comes from one file-size check (fixed-width
**   entries + a fixed-width trailer -- see RSFFileName.h's own
**   RSF_MANIFEST_ENTRY_WIDTH/TRAILER_WIDTH Notes), and the search itself
**   is a real binary search directly against the file, one seek+read per
**   comparison (see SearchUnrestricted/ReadManifestEntry).
*/

/* Includes */
#include "BoardLookupSearch.h"
#include "RSFFileName.h"
#include "RingNestedIndex.h"
#include "RingStoreFile.h"
#include "FileAndDirUtils.h"
#include "Error.h"
#include <windows.h>
#include <intrin.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

/* Structures and Types (internal) */

struct SegmentInfo
{
    uint64_t startOrdinal;
    char     path[BOARD_LOOKUP_MAX_PATH];
};

/*
** Type:    ManifestEntry
** @brief   One decoded fixed-width manifest entry -- see RSFFileName.h's
**          own RSF_MANIFEST_ENTRY_WIDTH/TRAILER_WIDTH Notes for the exact
**          on-disk format this mirrors.
*/
struct ManifestEntry
{
    uint64_t startOrdinal;
    uint64_t minPattern;
    uint64_t maxPattern;
};

/*
** Type:    RingIndex
** @brief   Everything needed to search one ring: its discovered segments
**          (from a directory listing, for ordinal-based access) and its
**          manifest's own path + real entry count (for CellsInUse's own
**          unrestricted, value-only top-level search -- read via direct
**          seeks, never loaded wholesale; see SearchUnrestricted).
*/
struct RingIndex
{
    std::vector<SegmentInfo> segments;
    char                     manifestPath[BOARD_LOOKUP_MAX_PATH] = {};
    uint64_t                 numManifestEntries = 0;
    uint64_t                 totalRecords       = 0;
    RSFRecordShape           shape              = RSF_SHAPE_PAIR64;
    int                      recSize            = 0;
};

/*
** Type:    FoundRecord
** @brief   One search hit, bundled with the decoded segment it came from
**          so the caller can immediately look at the NEXT record (to
**          compute a child group's end boundary) without redecoding.
*/
struct FoundRecord
{
    RingLocation          loc;
    std::vector<uint8_t>  decodedSegment;
    size_t                segmentIdx = 0;
};

/* Functions */

static int RSFShapeSize(RSFRecordShape shape)
{
    switch (shape)
    {
        case RSF_SHAPE_PAIR64:     return sizeof(UINT64_PAIR);
        case RSF_SHAPE_RING_LEVEL: return sizeof(RingLevelRec);
        case RSF_SHAPE_LEAF16:     return sizeof(Ring34Rec);
    }
    Fatal(FATAL_MERGE_LOGIC_ERROR, "BoardLookupSearch: RSFShapeSize unsupported shape %d", (int)shape);
    return 0;
}

static uint64_t ExtractPattern(RSFRecordShape shape, const uint8_t* rec)
{
    switch (shape)
    {
        case RSF_SHAPE_PAIR64:     return reinterpret_cast<const UINT64_PAIR*>(rec)->hi;
        case RSF_SHAPE_RING_LEVEL: return (uint64_t)reinterpret_cast<const RingLevelRec*>(rec)->pattern;
        case RSF_SHAPE_LEAF16:     return (uint64_t)reinterpret_cast<const Ring34Rec*>(rec)->pattern;
    }
    return 0;
}

/*
** Function: ExtractOffset
** @brief    Pulls a record's own `.offset`/`.lo` field -- where ITS
**           children start in the next ring down. Ring_3_4 (LEAF16) has
**           no such field (it's the bottom of the hierarchy); callers
**           never use the return value in that case.
*/
static uint64_t ExtractOffset(RSFRecordShape shape, const uint8_t* rec)
{
    switch (shape)
    {
        case RSF_SHAPE_PAIR64:     return reinterpret_cast<const UINT64_PAIR*>(rec)->lo;
        case RSF_SHAPE_RING_LEVEL: return reinterpret_cast<const RingLevelRec*>(rec)->offset;
        case RSF_SHAPE_LEAF16:     return 0;
    }
    return 0;
}

/*
** Function: DiscoverSegments
** @brief    Lists a ring's segment directory and parses each real segment
**           file's own starting ordinal from its filename (see
**           RSFNameRingSegmentFile), sorted by startOrdinal.
*/
static bool DiscoverSegments(const char* ringSegmentDir, std::vector<SegmentInfo>* pSegments)
{
    char pattern[BOARD_LOOKUP_MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\seg*.rsfzl", ringSegmentDir);

    WIN32_FIND_DATAA fd = {};
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    do
    {
        unsigned long long ordinal = 0;
        if (sscanf(fd.cFileName, "seg%16llx.rsfzl", &ordinal) != 1)
            continue;

        SegmentInfo info;
        info.startOrdinal = ordinal;
        snprintf(info.path, sizeof(info.path), "%s\\%s", ringSegmentDir, fd.cFileName);
        pSegments->push_back(info);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    std::sort(pSegments->begin(), pSegments->end(),
              [](const SegmentInfo& a, const SegmentInfo& b) { return a.startOrdinal < b.startOrdinal; });
    return !pSegments->empty();
}

/*
** Function: LoadSegmentRecordsRaw
** @brief    Fully decodes one segment file's records into pOut as raw
**           bytes -- the real cost a genuine random lookup into a segment
**           has to pay (open + decode start-to-finish).
*/
static void LoadSegmentRecordsRaw(const char* segPath, RSFRecordShape shape, int recSize, std::vector<uint8_t>* pOut)
{
    RSFReader* pReader = RSFOpenShaped(segPath, shape);
    if (!pReader)
        Fatal(FATAL_FILE_OPEN, "BoardLookupSearch: could not open '%s' (corrupt or truncated)", segPath);

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
** Function: LoadRingIndex
** @brief    Loads everything needed to search one ring: the manifest's
**           real entry count and totalRecords (both from one file-size
**           check plus a single fixed-size trailer read -- see
**           RSFFileName.h's own RSF_MANIFEST_ENTRY_WIDTH/TRAILER_WIDTH
**           Notes; no sequential parse of the whole manifest, ever) and
**           its real segment file listing.
*/
static bool LoadRingIndex(const char* levelDir, const char* ringName, RSFRecordShape shape,
                           RingIndex* idx, char* errBuf, size_t errBufSize)
{
    char ringSegmentDir[BOARD_LOOKUP_MAX_PATH];
    RSFNameRingSegmentDir(ringSegmentDir, sizeof(ringSegmentDir), levelDir, ringName);
    RSFNameRingManifestFile(idx->manifestPath, sizeof(idx->manifestPath), ringSegmentDir);

    idx->shape   = shape;
    idx->recSize = RSFShapeSize(shape);

    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExA(idx->manifestPath, GetFileExInfoStandard, &fad))
    {
        snprintf(errBuf, errBufSize,
                 "%s: not indexed yet (no manifest at '%s') -- run OthelloRingMasterLevelIndexer for this level first",
                 ringName, idx->manifestPath);
        return false;
    }
    uint64_t fileSize = ((uint64_t)fad.nFileSizeHigh << 32) | (uint64_t)fad.nFileSizeLow;
    if (fileSize < (uint64_t)RSF_MANIFEST_TRAILER_WIDTH ||
        (fileSize - (uint64_t)RSF_MANIFEST_TRAILER_WIDTH) % (uint64_t)RSF_MANIFEST_ENTRY_WIDTH != 0)
        Fatal(FATAL_FILE_OPEN,
              "%s: manifest '%s' is %llu bytes -- doesn't fit the fixed-width entry+trailer format, index is corrupt",
              ringName, idx->manifestPath, (unsigned long long)fileSize);
    idx->numManifestEntries = (fileSize - (uint64_t)RSF_MANIFEST_TRAILER_WIDTH) / (uint64_t)RSF_MANIFEST_ENTRY_WIDTH;

    /* One seek straight to the trailer -- no sequential read of anything
    ** before it, regardless of how many entries precede it.
    */
    FILE* mf = fopen(idx->manifestPath, "rb");
    if (!mf)
        Fatal(FATAL_FILE_OPEN, "%s: could not open manifest '%s'", ringName, idx->manifestPath);
    if (_fseeki64(mf, (long long)(fileSize - (uint64_t)RSF_MANIFEST_TRAILER_WIDTH), SEEK_SET) != 0)
        Fatal(FATAL_FILE_OPEN, "%s: could not seek to trailer in '%s'", ringName, idx->manifestPath);
    char trailer[RSF_MANIFEST_TRAILER_WIDTH + 1] = {};
    if (fread(trailer, 1, (size_t)RSF_MANIFEST_TRAILER_WIDTH, mf) != (size_t)RSF_MANIFEST_TRAILER_WIDTH)
        Fatal(FATAL_FILE_OPEN, "%s: could not read trailer from '%s'", ringName, idx->manifestPath);
    fclose(mf);

    unsigned long long totalRecordsHex = 0;
    if (sscanf(trailer, "totalRecords=%llx", &totalRecordsHex) != 1)
        Fatal(FATAL_FILE_OPEN, "%s: manifest trailer in '%s' doesn't match the expected fixed format",
              ringName, idx->manifestPath);
    idx->totalRecords = totalRecordsHex;

    if (!DiscoverSegments(ringSegmentDir, &idx->segments))
    {
        snprintf(errBuf, errBufSize, "%s: manifest exists but no segment files found in '%s' -- index looks corrupt",
                 ringName, ringSegmentDir);
        return false;
    }
    return true;
}

/*
** Function: BinarySearchPatternInRange
** @brief    Binary-searches a decoded segment's records, restricted to
**           [loLocal, hiLocalExclusive), for an exact pattern match --
**           records within one segment are always sorted (the whole ring
**           is one globally sorted stream, just physically chunked).
*/
static bool BinarySearchPatternInRange(const std::vector<uint8_t>& decoded, RSFRecordShape shape, int recSize,
                                        size_t loLocal, size_t hiLocalExclusive, uint64_t targetPattern,
                                        size_t* pOutLocalIdx)
{
    while (loLocal < hiLocalExclusive)
    {
        size_t mid = loLocal + (hiLocalExclusive - loLocal) / 2;
        uint64_t midPattern = ExtractPattern(shape, &decoded[mid * (size_t)recSize]);
        if (midPattern == targetPattern) { *pOutLocalIdx = mid; return true; }
        if (midPattern < targetPattern)  loLocal = mid + 1;
        else                             hiLocalExclusive = mid;
    }
    return false;
}

/*
** Function: ReadManifestEntry
** @brief    Reads exactly one fixed-width manifest entry via a direct
**           seek -- never reads any entry other than the one asked for.
** @param    mf         - the manifest file, already open in binary mode
** @param    entryIndex - which entry (0-based)
** @param    manifestPath - for error messages only
** @param    pOut       - out: the decoded entry
*/
static void ReadManifestEntry(FILE* mf, uint64_t entryIndex, const char* manifestPath, ManifestEntry* pOut)
{
    if (_fseeki64(mf, (long long)(entryIndex * (uint64_t)RSF_MANIFEST_ENTRY_WIDTH), SEEK_SET) != 0)
        Fatal(FATAL_FILE_OPEN, "ReadManifestEntry: could not seek to entry %llu in '%s'",
              (unsigned long long)entryIndex, manifestPath);
    char buf[RSF_MANIFEST_ENTRY_WIDTH + 1] = {};
    if (fread(buf, 1, (size_t)RSF_MANIFEST_ENTRY_WIDTH, mf) != (size_t)RSF_MANIFEST_ENTRY_WIDTH)
        Fatal(FATAL_FILE_OPEN, "ReadManifestEntry: could not read entry %llu from '%s'",
              (unsigned long long)entryIndex, manifestPath);
    unsigned long long a = 0, b = 0, c = 0;
    if (sscanf(buf, "%llx %llx %llx", &a, &b, &c) != 3)
        Fatal(FATAL_FILE_OPEN, "ReadManifestEntry: entry %llu in '%s' doesn't match the expected fixed format",
              (unsigned long long)entryIndex, manifestPath);
    pOut->startOrdinal = a;
    pOut->minPattern    = b;
    pOut->maxPattern    = c;
}

/*
** Function: SearchUnrestricted
** @brief    CellsInUse's own top-level search: no ordinal bound is known
**           in advance, so this binary-searches the manifest's fixed-
**           width entries directly on disk by VALUE (each comparison is
**           one seek + one fixed-size read -- the manifest is never
**           loaded wholesale, not even into a std::vector, regardless of
**           how many segments a ring has).
*/
static bool SearchUnrestricted(RingIndex& idx, uint64_t targetPattern, FoundRecord* pOut)
{
    if (idx.numManifestEntries == 0)
        return false;

    FILE* mf = fopen(idx.manifestPath, "rb");
    if (!mf)
        Fatal(FATAL_FILE_OPEN, "SearchUnrestricted: could not open manifest '%s'", idx.manifestPath);

    uint64_t lo = 0, hi = idx.numManifestEntries;
    bool found = false;
    ManifestEntry match{};
    while (lo < hi)
    {
        uint64_t mid = lo + (hi - lo) / 2;
        ManifestEntry e;
        ReadManifestEntry(mf, mid, idx.manifestPath, &e);
        if (targetPattern < e.minPattern)      hi = mid;
        else if (targetPattern > e.maxPattern) lo = mid + 1;
        else { found = true; match = e; break; }
    }
    fclose(mf);

    if (!found)
        return false;   /* falls in a real gap between segments' actual ranges -- genuinely absent */

    auto segIt = std::find_if(idx.segments.begin(), idx.segments.end(),
                               [&](const SegmentInfo& s) { return s.startOrdinal == match.startOrdinal; });
    if (segIt == idx.segments.end())
        Fatal(FATAL_MERGE_LOGIC_ERROR, "BoardLookupSearch: manifest references segment start %llu with no matching segment file",
              (unsigned long long)match.startOrdinal);

    size_t segIdx = (size_t)(segIt - idx.segments.begin());
    LoadSegmentRecordsRaw(segIt->path, idx.shape, idx.recSize, &pOut->decodedSegment);

    size_t recordCount = pOut->decodedSegment.size() / (size_t)idx.recSize;
    size_t localIdx = 0;
    if (!BinarySearchPatternInRange(pOut->decodedSegment, idx.shape, idx.recSize, 0, recordCount, targetPattern, &localIdx))
        return false;   /* within the segment's own range but genuinely absent -- a real, expected "board never reached" outcome */

    pOut->segmentIdx              = segIdx;
    pOut->loc.found               = true;
    pOut->loc.globalOrdinal       = match.startOrdinal + localIdx;
    strncpy(pOut->loc.segmentPath, segIt->path, sizeof(pOut->loc.segmentPath) - 1);
    pOut->loc.segmentStartOrdinal = match.startOrdinal;
    pOut->loc.localIndexInSegment = localIdx;
    pOut->loc.childOffset         = ExtractOffset(idx.shape, &pOut->decodedSegment[localIdx * (size_t)idx.recSize]);
    return true;
}

/*
** Function: SearchRestricted
** @brief    Ring_2/Ring_3_4's group-scoped descent: [rangeStart,rangeEnd)
**           is a real parent group's span, guaranteed by the indexer's own
**           group-boundary alignment to sit entirely within exactly ONE
**           segment (see this file's own top-of-file Notes) -- so this
**           only ever decodes one segment, found by ordinal, then binary-
**           searches the clipped local range by value.
*/
static bool SearchRestricted(RingIndex& idx, uint64_t rangeStart, uint64_t rangeEnd, uint64_t targetPattern, FoundRecord* pOut)
{
    /* Which segment holds rangeStart -- last segment whose startOrdinal <= rangeStart. */
    std::vector<uint64_t> starts(idx.segments.size());
    for (size_t i = 0; i < idx.segments.size(); i++) starts[i] = idx.segments[i].startOrdinal;
    size_t idxPos = (size_t)(std::upper_bound(starts.begin(), starts.end(), rangeStart) - starts.begin());
    if (idxPos == 0)
        Fatal(FATAL_MERGE_LOGIC_ERROR, "BoardLookupSearch: ordinal %llu falls before this ring's first segment -- index is broken",
              (unsigned long long)rangeStart);
    idxPos--;

    LoadSegmentRecordsRaw(idx.segments[idxPos].path, idx.shape, idx.recSize, &pOut->decodedSegment);
    uint64_t segStart = idx.segments[idxPos].startOrdinal;
    size_t   recordCount = pOut->decodedSegment.size() / (size_t)idx.recSize;
    uint64_t segEnd = segStart + (uint64_t)recordCount;

    if (rangeEnd > segEnd)
        Fatal(FATAL_MERGE_LOGIC_ERROR,
              "BoardLookupSearch: group span [%llu,%llu) crosses segment '%s' own end (%llu) -- the indexer's "
              "group-boundary alignment guarantee is violated, index is not trustworthy",
              (unsigned long long)rangeStart, (unsigned long long)rangeEnd, idx.segments[idxPos].path, (unsigned long long)segEnd);

    size_t loLocal = (size_t)(rangeStart - segStart);
    size_t hiLocal = (size_t)(rangeEnd   - segStart);
    size_t localIdx = 0;
    if (!BinarySearchPatternInRange(pOut->decodedSegment, idx.shape, idx.recSize, loLocal, hiLocal, targetPattern, &localIdx))
        return false;

    pOut->segmentIdx              = idxPos;
    pOut->loc.found               = true;
    pOut->loc.globalOrdinal       = segStart + localIdx;
    strncpy(pOut->loc.segmentPath, idx.segments[idxPos].path, sizeof(pOut->loc.segmentPath) - 1);
    pOut->loc.segmentStartOrdinal = segStart;
    pOut->loc.localIndexInSegment = localIdx;
    pOut->loc.childOffset         = ExtractOffset(idx.shape, &pOut->decodedSegment[localIdx * (size_t)idx.recSize]);
    return true;
}

/*
** Function: ComputeGroupEnd
** @brief    The end of a matched record's own children span -- the NEXT
**           record's offset, in this same ring's global ordinal space (or
**           childRingTotalRecords if the match was this ring's very last
**           record). Usually just a peek at the next record already sitting
**           in the same decoded segment; only decodes a second segment in
**           the rare case the match was the last record of its own.
*/
static uint64_t ComputeGroupEnd(RingIndex& idx, const FoundRecord& found, uint64_t childRingTotalRecords)
{
    size_t recordCount = found.decodedSegment.size() / (size_t)idx.recSize;
    if (found.loc.localIndexInSegment + 1 < recordCount)
        return ExtractOffset(idx.shape, &found.decodedSegment[(found.loc.localIndexInSegment + 1) * (size_t)idx.recSize]);

    if (found.segmentIdx + 1 < idx.segments.size())
    {
        std::vector<uint8_t> nextSeg;
        LoadSegmentRecordsRaw(idx.segments[found.segmentIdx + 1].path, idx.shape, idx.recSize, &nextSeg);
        if (nextSeg.empty())
            Fatal(FATAL_MERGE_LOGIC_ERROR, "BoardLookupSearch: segment '%s' is empty -- index is corrupt",
                  idx.segments[found.segmentIdx + 1].path);
        return ExtractOffset(idx.shape, &nextSeg[0]);
    }

    return childRingTotalRecords;
}

BoardLookupResult FindBoardInStore(uint64_t ringCellsInUse, uint64_t ringCellColors, int player,
                                    int boardSize, const char* levelIndexDir)
{
    BoardLookupResult result;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    int popcount = (int)__popcnt64(ringCellsInUse);
    int level = popcount - 4;
    result.level  = level;
    result.player = player;

    if (level < 0)
    {
        snprintf(result.notFoundReason, sizeof(result.notFoundReason),
                 "board has %d occupied cell(s) -- fewer than the 4-cell starting position, not a valid board", popcount);
        return result;
    }

    char levelDir[BOARD_LOOKUP_MAX_PATH];
    RSFNameLevelIndexDir(levelDir, sizeof(levelDir), levelIndexDir, boardSize, level, player);

    char errBuf[256];
    LARGE_INTEGER ringT0, ringT1;

    /* --- CellsInUse: unrestricted top-level search --- */
    QueryPerformanceCounter(&ringT0);
    RingIndex cellsInUseIdx;
    if (!LoadRingIndex(levelDir, "CellsInUse", RSF_SHAPE_PAIR64, &cellsInUseIdx, errBuf, sizeof(errBuf)))
    {
        strncpy(result.notFoundReason, errBuf, sizeof(result.notFoundReason) - 1);
        goto done;
    }

    {
        FoundRecord cellsInUseFound;
        if (!SearchUnrestricted(cellsInUseIdx, ringCellsInUse, &cellsInUseFound))
        {
            QueryPerformanceCounter(&ringT1);
            result.cellsInUseLoc.elapsedSeconds = (double)(ringT1.QuadPart - ringT0.QuadPart) / (double)freq.QuadPart;
            snprintf(result.notFoundReason, sizeof(result.notFoundReason),
                      "level %d: this board's CellsInUse pattern was never reached during the solve", level);
            goto done;
        }
        result.cellsInUseLoc = cellsInUseFound.loc;
        QueryPerformanceCounter(&ringT1);
        result.cellsInUseLoc.elapsedSeconds = (double)(ringT1.QuadPart - ringT0.QuadPart) / (double)freq.QuadPart;

        if (!RingNestedIndexHasRing2(boardSize))
        {
            snprintf(result.notFoundReason, sizeof(result.notFoundReason),
                      "board size %d has no Ring_2 -- direct CellsInUse->Ring_3_4 descent isn't implemented "
                      "(the indexer doesn't segment this board size yet)", boardSize);
            goto done;
        }

        /* --- Ring_2: descent restricted to this CellsInUse group's span --- */
        QueryPerformanceCounter(&ringT0);
        RingIndex ring2Idx;
        if (!LoadRingIndex(levelDir, "Ring2", RSF_SHAPE_RING_LEVEL, &ring2Idx, errBuf, sizeof(errBuf)))
        {
            strncpy(result.notFoundReason, errBuf, sizeof(result.notFoundReason) - 1);
            goto done;
        }

        uint64_t ring2RangeStart = cellsInUseFound.loc.childOffset;
        uint64_t ring2RangeEnd   = ComputeGroupEnd(cellsInUseIdx, cellsInUseFound, ring2Idx.totalRecords);

        /* Ring_2's own subpattern is the middle RING2_BITS of the full
        ** ring-ordered color pattern (RingNestedIndex.h's fixed 28/20/16
        ** bit partition -- Ring_1's slice is unused/zero for a board size
        ** with no Ring_1, per RingNestedIndexHasRing1).
        */
        uint32_t ring2Pattern = (uint32_t)((ringCellColors >> RING2_SHIFT) & ((1ULL << RING2_BITS) - 1));

        FoundRecord ring2Found;
        if (!SearchRestricted(ring2Idx, ring2RangeStart, ring2RangeEnd, (uint64_t)ring2Pattern, &ring2Found))
        {
            QueryPerformanceCounter(&ringT1);
            result.ring2Loc.elapsedSeconds = (double)(ringT1.QuadPart - ringT0.QuadPart) / (double)freq.QuadPart;
            snprintf(result.notFoundReason, sizeof(result.notFoundReason),
                      "level %d: CellsInUse pattern found, but no matching Ring_2 child -- store is inconsistent, or this exact color arrangement was never reached", level);
            goto done;
        }
        result.ring2Loc = ring2Found.loc;
        QueryPerformanceCounter(&ringT1);
        result.ring2Loc.elapsedSeconds = (double)(ringT1.QuadPart - ringT0.QuadPart) / (double)freq.QuadPart;

        /* --- Ring_3_4: descent restricted to this Ring_2 group's span --- */
        QueryPerformanceCounter(&ringT0);
        RingIndex ring34Idx;
        if (!LoadRingIndex(levelDir, "Ring34", RSF_SHAPE_LEAF16, &ring34Idx, errBuf, sizeof(errBuf)))
        {
            strncpy(result.notFoundReason, errBuf, sizeof(result.notFoundReason) - 1);
            goto done;
        }

        uint64_t ring34RangeStart = ring2Found.loc.childOffset;
        uint64_t ring34RangeEnd   = ComputeGroupEnd(ring2Idx, ring2Found, ring34Idx.totalRecords);
        uint16_t ring34Pattern    = (uint16_t)(ringCellColors & ((1ULL << RING34_BITS) - 1));

        FoundRecord ring34Found;
        if (!SearchRestricted(ring34Idx, ring34RangeStart, ring34RangeEnd, (uint64_t)ring34Pattern, &ring34Found))
        {
            QueryPerformanceCounter(&ringT1);
            result.ring34Loc.elapsedSeconds = (double)(ringT1.QuadPart - ringT0.QuadPart) / (double)freq.QuadPart;
            snprintf(result.notFoundReason, sizeof(result.notFoundReason),
                      "level %d: Ring_2 pattern found, but no matching Ring_3_4 child -- store is inconsistent, or this exact board was never reached", level);
            goto done;
        }
        result.ring34Loc = ring34Found.loc;
        QueryPerformanceCounter(&ringT1);
        result.ring34Loc.elapsedSeconds = (double)(ringT1.QuadPart - ringT0.QuadPart) / (double)freq.QuadPart;
        result.found = true;
    }

done:
    QueryPerformanceCounter(&t1);
    result.elapsedSeconds = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    return result;
}
