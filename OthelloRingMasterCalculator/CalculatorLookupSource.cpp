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
** Function: LoadLookupSourceForColor
** @brief    Stages one color's board-key and counts data at nextLevel as
**           segmented scratch (see file Notes for how neither side ever
**           holds a whole level resident).
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

    /* ---- Board-key scratch: a single streaming pass -- see
    ** RingNestedIndex.h's RingNestedIndexStreamAll. No count-first pass
    ** needed any more: SegmentedStoreWriter reserves drives on demand as
    ** it goes, so the ring files are read exactly once, never held resident.
    */
    char baseName[MAX_FULL_PATH_NAME];
    snprintf(baseName, sizeof(baseName), "L%04d_%s_keys", nextLevel, RSFPlayerStr(player));

    SegmentedStoreWriter keyWriter;
    keyWriter.Init(pState, pConfig->storeDrive, pConfig->countsDrive, sizeof(UINT64_PAIR), /*isKeySorted=*/true,
                   pConfig->scratchDirNameNoDrive, baseName);

    uint64_t boardCount = 0;
    bool writeOk = RingNestedIndexStreamAll(cellsInUsePath, ring1Path, ring2Path, ring34Path,
                                            [&](const BOARD_KEY& key)
    {
        UINT64_PAIR rec{ key.ullCellsInUse, key.ullCellColors };
        keyWriter.Write(&rec);
        boardCount++;
    });
    if (!writeOk)
        Fatal(FATAL_MERGE_LOGIC_ERROR,
              "LoadLookupSource: level %d %s-to-move nested-index files failed to stream",
              nextLevel, RSFPlayerStr(player));

    keyWriter.Finish();
    pOut->boardKeyPlan = keyWriter.plan;
    pOut->boardKeys.Load(keyWriter.segments, sizeof(UINT64_PAIR));

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
        DeleteSegments(pState, pColor->boardKeys.segments, pColor->boardKeyPlan);
        DeleteSegments(pState, pColor->counts.segments, pColor->countsPlan);
        pColor->hasData = false;
    }
}

/*
** Function: LookupChildTriple
** @brief    See CalculatorLookupSource.h.
*/
bool LookupChildTriple(const LookupSource& source, uint8_t childPlayer, const BOARD_KEY& childKey, OutcomeTriple* pOut)
{
    const LookupSourceForColor& color = (childPlayer == RSF_PLAYER_BLACK) ? source.black : source.white;
    if (!color.hasData) return false;

    uint64_t pos;
    if (!color.boardKeys.FindByKey(childKey.ullCellsInUse, childKey.ullCellColors, &pos))
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
