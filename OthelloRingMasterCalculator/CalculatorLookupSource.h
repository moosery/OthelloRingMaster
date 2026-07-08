/*
** Filename:  CalculatorLookupSource.h
**
** Purpose:
**   Declares LookupSource: level+1's board-key and counts data staged as
**   drive-spanning segmented scratch (see SegmentedStore.h) instead of
**   held wholesale in memory, plus LookupChildTriple, which resolves a
**   generated child to its already-computed OutcomeTriple against it.
**
** Notes:
**   Real, disclosed limitation: building a color's board-key scratch
**   still requires one transient pass through
**   RingNestedIndexReader::Load()/ExpandAll() (the existing nested-index
**   reader, which is NOT itself streaming -- it loads the CellsInUse/
**   Ring_1/Ring_2/Ring_3_4 vectors wholesale) to decompress and reshard
**   the ring files into flat, segmented board-key scratch. That transient
**   peak is freed the moment resharding finishes (the RingNestedIndexReader
**   is a local variable, gone once LoadLookupSource returns), so it does
**   not persist for the rest of the level's processing -- but it is not
**   eliminated for that one pass. Fully closing that gap would need
**   OthelloBasics/RingNestedIndex.h's reader itself rewritten to stream
**   rather than load wholesale -- a separate, larger change, not done here.
**   The counts side has no such gap: the permanent counts file is already
**   read sequentially, one record at a time, straight into scratch.
*/

#pragma once

/* Includes */
#include "SegmentedStore.h"
#include "CalculatorScratchCounts.h"
#include "OthelloBasics.h"

/* Structures and Types */

/*
** Type:    LookupSourceForColor
** @brief   One color's staged lookup data at level+1: a key-searchable
**          board-key segmented store and a positional counts segmented
**          store, plus the drive plans both were written under (needed
**          to release the ledger claim when this color is no longer needed).
*/
struct LookupSourceForColor
{
    bool hasData = false;

    SegmentedStoreReader boardKeys;
    SegmentedStoreReader counts;
    int scratchByteWidth = 1;

    std::vector<std::pair<char, int64_t>> boardKeyPlan;
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
** @brief    Finds child's position in source's board-key store for
**           childPlayer and returns its already-computed OutcomeTriple
**           from the matching counts store, uniformly zero-extended
**           regardless of the source level's tier width.
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
