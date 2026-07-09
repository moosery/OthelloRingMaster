/*
** Filename:  CalculatorLookupSource.h
**
** Purpose:
**   Declares LookupSource: level+1's ring nested-index data (CellsInUse/
**   Ring_1/Ring_2/Ring_3_4) and counts data staged as drive-spanning
**   segmented scratch (see SegmentedStore.h) instead of held wholesale in
**   memory, plus LookupChildTriple, which resolves a generated child to
**   its already-computed OutcomeTriple against it.
**
** Notes:
**   Ring scratch is staged as four SEPARATE segmented stores (one per
**   nested-index file, skipping Ring_1/Ring_2 exactly as
**   RingNestedIndexHasRing1/HasRing2 already governs elsewhere) holding
**   the DECOMPRESSED but still ring-SHAPED records -- never rehydrated to
**   flat 16-byte BOARD_KEY records. This conserves real disk space: the
**   ring format's hierarchical grouping (many boards sharing one
**   CellsInUse/Ring_1/Ring_2 group) is inherently smaller even fully
**   decompressed than one flat record per board would be. Each file is
**   staged via a single straight decompress-and-rewrite streaming pass
**   (RSFOpen/RSFOpenShaped -> SegmentedStoreWriter), never holding a whole
**   level resident regardless of board count. LookupChildTriple then walks
**   the same CellsInUse -> Ring_1 -> Ring_2 -> Ring_3_4 hierarchy
**   RingNestedIndexReader::FindBoardPosition does in-memory, but via
**   SegmentedStoreReader::FindPatternInRange/ReadAt against the drive-
**   segmented stores instead of in-memory vectors. The counts side is
**   unchanged: the permanent counts file is already read sequentially,
**   one record at a time, straight into scratch, no count needed up front.
*/

#pragma once

/* Includes */
#include "SegmentedStore.h"
#include "CalculatorScratchCounts.h"
#include "OthelloBasics.h"

/* Structures and Types */

/*
** Type:    LookupSourceForColor
** @brief   One color's staged lookup data at level+1: the four ring
**          nested-index segmented stores (Ring_1/Ring_2 only populated
**          when hasRing1/hasRing2 are true), a positional counts segmented
**          store, plus the drive plans each was written under (needed to
**          release the ledger claim when this color is no longer needed).
*/
struct LookupSourceForColor
{
    bool hasData  = false;
    bool hasRing1 = false;
    bool hasRing2 = false;

    SegmentedStoreReader cellsInUse;
    SegmentedStoreReader ring1;
    SegmentedStoreReader ring2;
    SegmentedStoreReader ring34;
    SegmentedStoreReader counts;
    int scratchByteWidth = 1;

    std::vector<std::pair<char, int64_t>> cellsInUsePlan;
    std::vector<std::pair<char, int64_t>> ring1Plan;
    std::vector<std::pair<char, int64_t>> ring2Plan;
    std::vector<std::pair<char, int64_t>> ring34Plan;
    std::vector<std::pair<char, int64_t>> countsPlan;
};

/*
** Type:    LookupSource
** @brief   Both colors' staged lookup data for one level.
*/
struct LookupSource
{
    LookupSourceForColor black, white;
};

/* Functions */

/*
** Function: LoadLookupSource
** @brief    Stages level+1's board-key and counts data (both colors) as
**           segmented scratch across the fastest available drives,
**           excluding pConfig's store and counts drives.
** @param    pConfig      - run configuration (boardSize, storeDrive, countsDrive)
** @param    pState       - calculator state (storeDirectory, countsDirectory, driveInfo, driveLedger)
** @param    pWidthConfig - this board size's width table (nextLevel's width is read)
** @param    nextLevel    - the already-fully-processed level to stage as a lookup source
** @param    pOut         - out: filled lookup source
*/
void LoadLookupSource(POthelloRingMasterCalculatorConfig pConfig, POthelloRingMasterCalculatorState pState,
                       CounterWidthConfig* pWidthConfig, int nextLevel, LookupSource* pOut);

/*
** Function: ReleaseLookupSource
** @brief    Deletes every scratch segment in pSource and reclaims the
**           drive-ledger space. Call once level (the level that used this
**           as its lookup source) has finished processing.
** @param    pState  - calculator state (driveLedger)
** @param    pSource - the lookup source to release
*/
void ReleaseLookupSource(POthelloRingMasterCalculatorState pState, LookupSource* pSource);

/*
** Function: LookupChildTriple
** @brief    Finds child's position by walking source's ring nested-index
**           segmented stores for childPlayer (CellsInUse -> Ring_1 ->
**           Ring_2 -> Ring_3_4), then returns its already-computed
**           OutcomeTriple from the matching counts store, uniformly
**           zero-extended regardless of the source level's tier width.
** @param    source      - level+1's staged lookup source
** @param    childPlayer - which color's store to search
** @param    childKey    - the child board to find
** @param    pOut        - out: child's OutcomeTriple, valid up to at
**                         least that color's scratchByteWidth bytes (zero beyond that)
** @return   true if found. false is always a real data-integrity problem
**           at the caller (level+1's store must be complete), not a
**           legitimate case to handle quietly.
*/
bool LookupChildTriple(const LookupSource& source, uint8_t childPlayer, const BOARD_KEY& childKey, OutcomeTriple* pOut);
