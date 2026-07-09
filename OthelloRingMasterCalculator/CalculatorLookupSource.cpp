/*
** Filename:  CalculatorLookupSource.cpp
**
** Purpose:
**   Implements LoadLookupSource/ReleaseLookupSource/LookupChildTriple,
**   declared in CalculatorLookupSource.h.
*/

/* Includes */
#include "CalculatorLookupSource.h"
#include "CalculatorFileName.h"
#include "CalculatorCountsFile.h"
#include "RSFFileName.h"
#include "RingNestedIndex.h"
#include <string.h>

/* Internal Helpers */

/*
** Function: StreamCellsInUseToSegments
** @brief    Streams a compressed CellsInUse file straight into a segmented
**           store, one record at a time -- no rehydration, just decompress
**           and rewrite. CellsInUseRec's (pattern, offset) shape is
**           bit-identical to UINT64_PAIR, so this reads via the plain
**           RSFOpen/RSFRead path (same as the flat store format).
** @param    path    - path to the CellsInUse file
** @param    pWriter - already-Init()'d segmented store writer (recordSize == sizeof(UINT64_PAIR))
*/
static void StreamCellsInUseToSegments(const char* path, SegmentedStoreWriter* pWriter)
{
    RSFReader* r = RSFOpen(path);
    if (!r)
        Fatal(FATAL_MERGE_LOGIC_ERROR, "LoadLookupSource: cannot open CellsInUse file '%s'", path);

    UINT64_PAIR rec;
    while (RSFRead(r, &rec, 1) == 1)
        pWriter->Write(&rec);
    RSFClose(&r);
}

/*
** Function: StreamRingLevelToSegments
** @brief    Streams a compressed Ring_1/Ring_2 file straight into a
**           segmented store, one record at a time.
** @param    path    - path to the Ring_1 or Ring_2 file
** @param    pWriter - already-Init()'d segmented store writer (recordSize == sizeof(RingLevelRec))
*/
static void StreamRingLevelToSegments(const char* path, SegmentedStoreWriter* pWriter)
{
    RSFReader* r = RSFOpenShaped(path, RSF_SHAPE_RING_LEVEL);
    if (!r)
        Fatal(FATAL_MERGE_LOGIC_ERROR, "LoadLookupSource: cannot open Ring_1/Ring_2 file '%s'", path);

    RingLevelRec rec;
    while (RSFReadShaped(r, &rec, 1) == 1)
        pWriter->Write(&rec);
    RSFClose(&r);
}

/*
** Function: StreamRing34ToSegments
** @brief    Streams a compressed Ring_3_4 file straight into a segmented
**           store, one record at a time.
** @param    path    - path to the Ring_3_4 file
** @param    pWriter - already-Init()'d segmented store writer (recordSize == sizeof(Ring34Rec))
** @return   Number of records streamed (one per board -- see RingNestedIndex.h Notes).
*/
static uint64_t StreamRing34ToSegments(const char* path, SegmentedStoreWriter* pWriter)
{
    RSFReader* r = RSFOpenShaped(path, RSF_SHAPE_LEAF16);
    if (!r)
        Fatal(FATAL_MERGE_LOGIC_ERROR, "LoadLookupSource: cannot open Ring_3_4 file '%s'", path);

    uint64_t     count = 0;
    Ring34Rec    rec;
    while (RSFReadShaped(r, &rec, 1) == 1)
    {
        pWriter->Write(&rec);
        count++;
    }
    RSFClose(&r);
    return count;
}

/*
** Function: LoadLookupSourceForColor
** @brief    Stages one color's ring nested-index and counts data at
**           nextLevel as segmented scratch (see file Notes for how none of
**           it ever holds a whole level resident).
** @param    pConfig      - run configuration
** @param    pState       - calculator state
** @param    pWidthConfig - width table (nextLevel's width is read)
** @param    nextLevel    - the level being staged
** @param    player       - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @param    pOut         - out: filled for this color
*/
static void LoadLookupSourceForColor(POthelloRingMasterCalculatorConfig pConfig, POthelloRingMasterCalculatorState pState,
                                     CounterWidthConfig* pWidthConfig, int nextLevel, int player,
                                     LookupSourceForColor* pOut)
{
    int  boardSize = (int)pConfig->boardSize;
    bool hasRing1  = RingNestedIndexHasRing1(boardSize);
    bool hasRing2  = RingNestedIndexHasRing2(boardSize);
    pOut->hasRing1 = hasRing1;
    pOut->hasRing2 = hasRing2;

    char cellsInUsePath[MAX_FULL_PATH_NAME];
    char ring1PathBuf[MAX_FULL_PATH_NAME];
    char ring2PathBuf[MAX_FULL_PATH_NAME];
    char ring34Path[MAX_FULL_PATH_NAME];
    RSFNameCellsInUseFile(cellsInUsePath, sizeof(cellsInUsePath), pState->storeDirectory, boardSize, nextLevel, player, 0);
    if (hasRing1)
        RSFNameRing1File(ring1PathBuf, sizeof(ring1PathBuf), pState->storeDirectory, boardSize, nextLevel, player, 0);
    if (hasRing2)
        RSFNameRing2File(ring2PathBuf, sizeof(ring2PathBuf), pState->storeDirectory, boardSize, nextLevel, player, 0);
    RSFNameRing34File(ring34Path, sizeof(ring34Path), pState->storeDirectory, boardSize, nextLevel, player, 0);

    const char* ring1Path     = hasRing1 ? ring1PathBuf : nullptr;
    const char* ring2Path     = hasRing2 ? ring2PathBuf : nullptr;
    int         expectedCount = 2 + (hasRing1 ? 1 : 0) + (hasRing2 ? 1 : 0);

    int foundCount = RingNestedIndexFileCount(cellsInUsePath, ring1Path, ring2Path, ring34Path);
    if (foundCount == 0)
        return;   /* genuinely no boards of this color at nextLevel -- legitimate */

    if (foundCount != expectedCount)
        Fatal(FATAL_MERGE_LOGIC_ERROR,
              "LoadLookupSource: level %d %s-to-move nested-index files are corrupt/partial (found %d of %d expected files)",
              nextLevel, RSFPlayerStr(player), foundCount, expectedCount);

    /* ---- Ring scratch: one straight decompress-and-rewrite streaming
    ** pass per file, straight into its own drive-segmented store -- see
    ** file Notes for why this stays ring-shaped rather than rehydrating to
    ** flat BOARD_KEY records.
    */
    char baseName[MAX_FULL_PATH_NAME];

    snprintf(baseName, sizeof(baseName), "L%04d_%s_cellsinuse", nextLevel, RSFPlayerStr(player));
    SegmentedStoreWriter cellsInUseWriter;
    cellsInUseWriter.Init(pState, pConfig->storeDrive, pConfig->countsDrive, sizeof(UINT64_PAIR),
                          pConfig->scratchDirNameNoDrive, baseName);
    StreamCellsInUseToSegments(cellsInUsePath, &cellsInUseWriter);
    cellsInUseWriter.Finish();
    pOut->cellsInUsePlan = cellsInUseWriter.plan;
    pOut->cellsInUse.Load(cellsInUseWriter.segments, sizeof(UINT64_PAIR));

    if (hasRing1)
    {
        snprintf(baseName, sizeof(baseName), "L%04d_%s_ring1", nextLevel, RSFPlayerStr(player));
        SegmentedStoreWriter ring1Writer;
        ring1Writer.Init(pState, pConfig->storeDrive, pConfig->countsDrive, sizeof(RingLevelRec),
                         pConfig->scratchDirNameNoDrive, baseName);
        StreamRingLevelToSegments(ring1Path, &ring1Writer);
        ring1Writer.Finish();
        pOut->ring1Plan = ring1Writer.plan;
        pOut->ring1.Load(ring1Writer.segments, sizeof(RingLevelRec));
    }

    if (hasRing2)
    {
        snprintf(baseName, sizeof(baseName), "L%04d_%s_ring2", nextLevel, RSFPlayerStr(player));
        SegmentedStoreWriter ring2Writer;
        ring2Writer.Init(pState, pConfig->storeDrive, pConfig->countsDrive, sizeof(RingLevelRec),
                         pConfig->scratchDirNameNoDrive, baseName);
        StreamRingLevelToSegments(ring2Path, &ring2Writer);
        ring2Writer.Finish();
        pOut->ring2Plan = ring2Writer.plan;
        pOut->ring2.Load(ring2Writer.segments, sizeof(RingLevelRec));
    }

    snprintf(baseName, sizeof(baseName), "L%04d_%s_ring34", nextLevel, RSFPlayerStr(player));
    SegmentedStoreWriter ring34Writer;
    ring34Writer.Init(pState, pConfig->storeDrive, pConfig->countsDrive, sizeof(Ring34Rec),
                      pConfig->scratchDirNameNoDrive, baseName);
    uint64_t boardCount = StreamRing34ToSegments(ring34Path, &ring34Writer);
    ring34Writer.Finish();
    pOut->ring34Plan = ring34Writer.plan;
    pOut->ring34.Load(ring34Writer.segments, sizeof(Ring34Rec));

    /* ---- Counts scratch: the permanent counts file is already read
    ** sequentially, one record at a time -- no wholesale load at all.
    ** Reuses ScratchCountsWriter's own WriteTriple/WriteNibbleTriple
    ** packing rather than re-deriving it here.
    */
    int officialByteWidth  = CounterWidthConfigGet(pWidthConfig, nextLevel);
    pOut->scratchByteWidth = (officialByteWidth == COUNTER_WIDTH_NIBBLE) ? 1 : officialByteWidth;

    char countsPath[MAX_FULL_PATH_NAME];
    CalcNameCountsFile(countsPath, sizeof(countsPath), pState->countsDirectory, boardSize, nextLevel, player);

    char countsBaseName[MAX_FULL_PATH_NAME];
    snprintf(countsBaseName, sizeof(countsBaseName), "L%04d_%s_counts", nextLevel, RSFPlayerStr(player));

    ScratchCountsWriter countsWriter;
    countsWriter.Init(pState, pConfig->storeDrive, pConfig->countsDrive, officialByteWidth,
                      pConfig->scratchDirNameNoDrive, countsBaseName);

    if (officialByteWidth == COUNTER_WIDTH_NIBBLE)
    {
        NibbleCountsReader* pReader = NibbleCountsReaderOpen(countsPath, boardCount);
        if (!pReader)
            Fatal(FATAL_MERGE_LOGIC_ERROR, "LoadLookupSource: level %d %s-to-move counts file missing at '%s'",
                  nextLevel, RSFPlayerStr(player), countsPath);

        NibbleOutcomeTriple t;
        uint64_t gotCount = 0;
        while (NibbleCountsReaderRead(pReader, &t))
        {
            countsWriter.WriteNibbleTriple(t);
            gotCount++;
        }
        NibbleCountsReaderClose(&pReader);

        if (gotCount != boardCount)
            Fatal(FATAL_MERGE_LOGIC_ERROR, "LoadLookupSource: level %d %s-to-move counts file has %llu records, expected %llu",
                  nextLevel, RSFPlayerStr(player), (unsigned long long)gotCount, (unsigned long long)boardCount);
    }
    else
    {
        CalculatorCountsReader* pReader = CalculatorCountsReaderOpen(countsPath, officialByteWidth);
        if (!pReader)
            Fatal(FATAL_MERGE_LOGIC_ERROR, "LoadLookupSource: level %d %s-to-move counts file missing at '%s'",
                  nextLevel, RSFPlayerStr(player), countsPath);

        OutcomeTriple t;
        uint64_t gotCount = 0;
        while (CalculatorCountsReaderRead(pReader, &t))
        {
            countsWriter.WriteTriple(t);
            gotCount++;
        }
        CalculatorCountsReaderClose(&pReader);

        if (gotCount != boardCount)
            Fatal(FATAL_MERGE_LOGIC_ERROR, "LoadLookupSource: level %d %s-to-move counts file has %llu records, expected %llu",
                  nextLevel, RSFPlayerStr(player), (unsigned long long)gotCount, (unsigned long long)boardCount);
    }

    countsWriter.Finish();
    pOut->countsPlan = countsWriter.store.plan;
    pOut->counts.Load(countsWriter.store.segments, 3 * pOut->scratchByteWidth);

    pOut->hasData = true;
}

/* Functions */

/*
** Function: LoadLookupSource
** @brief    See CalculatorLookupSource.h.
*/
void LoadLookupSource(POthelloRingMasterCalculatorConfig pConfig, POthelloRingMasterCalculatorState pState,
                       CounterWidthConfig* pWidthConfig, int nextLevel, LookupSource* pOut)
{
    LoadLookupSourceForColor(pConfig, pState, pWidthConfig, nextLevel, RSF_PLAYER_BLACK, &pOut->black);
    LoadLookupSourceForColor(pConfig, pState, pWidthConfig, nextLevel, RSF_PLAYER_WHITE, &pOut->white);
}

/*
** Function: ReleaseLookupSource
** @brief    See CalculatorLookupSource.h.
*/
void ReleaseLookupSource(POthelloRingMasterCalculatorState pState, LookupSource* pSource)
{
    for (LookupSourceForColor* pColor : { &pSource->black, &pSource->white })
    {
        if (!pColor->hasData) continue;
        DeleteSegments(pState, pColor->cellsInUse.segments, pColor->cellsInUsePlan);
        if (pColor->hasRing1) DeleteSegments(pState, pColor->ring1.segments, pColor->ring1Plan);
        if (pColor->hasRing2) DeleteSegments(pState, pColor->ring2.segments, pColor->ring2Plan);
        DeleteSegments(pState, pColor->ring34.segments, pColor->ring34Plan);
        DeleteSegments(pState, pColor->counts.segments, pColor->countsPlan);
        pColor->hasData = false;
    }
}

/*
** ============================================================
** LookupChildTriple's hierarchical pattern comparators -- each ring level's
** record shape has its match pattern as its first field (see
** RingNestedIndex.h: CellsInUseRec/RingLevelRec/Ring34Rec), so these mirror
** RingNestedIndex.cpp's BinarySearchPattern, just via a function pointer
** per width instead of a template (SegmentedStoreReader::FindPatternInRange
** takes a plain C comparator, matching Utility's BinarySearch convention).
** ============================================================
*/

static int CompareCellsInUsePattern(void* /*pContext*/, const void* pEntry, const void* pKey)
{
    uint64_t e = ((const CellsInUseRec*)pEntry)->pattern;
    uint64_t k = *(const uint64_t*)pKey;
    if (e != k) return (e < k) ? -1 : 1;
    return 0;
}

static int CompareRingLevelPattern(void* /*pContext*/, const void* pEntry, const void* pKey)
{
    uint32_t e = ((const RingLevelRec*)pEntry)->pattern;
    uint32_t k = *(const uint32_t*)pKey;
    if (e != k) return (e < k) ? -1 : 1;
    return 0;
}

static int CompareRing34Pattern(void* /*pContext*/, const void* pEntry, const void* pKey)
{
    uint16_t e = ((const Ring34Rec*)pEntry)->pattern;
    uint16_t k = *(const uint16_t*)pKey;
    if (e != k) return (e < k) ? -1 : 1;
    return 0;
}

/*
** Function: LookupChildTriple
** @brief    See CalculatorLookupSource.h.
*/
bool LookupChildTriple(const LookupSource& source, uint8_t childPlayer, const BOARD_KEY& childKey, OutcomeTriple* pOut)
{
    const LookupSourceForColor& color = (childPlayer == RSF_PLAYER_BLACK) ? source.black : source.white;
    if (!color.hasData) return false;

    uint32_t ring1Pattern  = (uint32_t)((childKey.ullCellColors >> RING1_SHIFT)  & ((1u << RING1_BITS) - 1));
    uint32_t ring2Pattern  = (uint32_t)((childKey.ullCellColors >> RING2_SHIFT)  & ((1u << RING2_BITS) - 1));
    uint16_t ring34Pattern = (uint16_t)((childKey.ullCellColors >> RING34_SHIFT) & ((1u << RING34_BITS) - 1));

    /* Walk CellsInUse -> Ring_1 -> Ring_2 -> Ring_3_4, exactly like
    ** RingNestedIndexReader::FindBoardPosition, but against segmented,
    ** drive-spanning stores via FindPatternInRange/ReadAt instead of
    ** in-memory vectors.
    */
    uint64_t numCellsInUse = color.cellsInUse.GetRecordCount();
    uint64_t i;
    if (!color.cellsInUse.FindPatternInRange(0, numCellsInUse, &childKey.ullCellsInUse,
                                              CompareCellsInUsePattern, nullptr, &i))
        return false;

    CellsInUseRec curCells;
    if (!color.cellsInUse.ReadAt(i, &curCells)) return false;

    uint64_t begin = curCells.offset, end;
    if (i + 1 < numCellsInUse)
    {
        CellsInUseRec nextCells;
        if (!color.cellsInUse.ReadAt(i + 1, &nextCells)) return false;
        end = nextCells.offset;
    }
    else
    {
        end = color.hasRing1 ? color.ring1.GetRecordCount()
            : color.hasRing2 ? color.ring2.GetRecordCount()
                              : color.ring34.GetRecordCount();
    }

    if (color.hasRing1)
    {
        uint64_t j;
        if (!color.ring1.FindPatternInRange(begin, end, &ring1Pattern, CompareRingLevelPattern, nullptr, &j))
            return false;

        RingLevelRec curRing1;
        if (!color.ring1.ReadAt(j, &curRing1)) return false;
        begin = curRing1.offset;

        uint64_t ring1Count = color.ring1.GetRecordCount();
        if (j + 1 < ring1Count)
        {
            RingLevelRec nextRing1;
            if (!color.ring1.ReadAt(j + 1, &nextRing1)) return false;
            end = nextRing1.offset;
        }
        else
        {
            end = color.hasRing2 ? color.ring2.GetRecordCount() : color.ring34.GetRecordCount();
        }
    }

    if (color.hasRing2)
    {
        uint64_t k;
        if (!color.ring2.FindPatternInRange(begin, end, &ring2Pattern, CompareRingLevelPattern, nullptr, &k))
            return false;

        RingLevelRec curRing2;
        if (!color.ring2.ReadAt(k, &curRing2)) return false;
        begin = curRing2.offset;

        uint64_t ring2Count = color.ring2.GetRecordCount();
        if (k + 1 < ring2Count)
        {
            RingLevelRec nextRing2;
            if (!color.ring2.ReadAt(k + 1, &nextRing2)) return false;
            end = nextRing2.offset;
        }
        else
        {
            end = color.ring34.GetRecordCount();
        }
    }

    uint64_t pos;
    if (!color.ring34.FindPatternInRange(begin, end, &ring34Pattern, CompareRing34Pattern, nullptr, &pos))
        return false;

    uint8_t buf[3 * WIDE_COUNTER_MAX_BYTES];
    if (!color.counts.ReadAt(pos, buf))
        return false;

    OutcomeTripleSetZero(pOut, color.scratchByteWidth);
    memcpy(pOut->black, buf,                              color.scratchByteWidth);
    memcpy(pOut->white, buf + color.scratchByteWidth,      color.scratchByteWidth);
    memcpy(pOut->tie,   buf + 2 * color.scratchByteWidth,  color.scratchByteWidth);
    return true;
}
