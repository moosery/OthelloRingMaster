/*
** Filename:  CalculatorScratchCounts.h
**
** Purpose:
**   Declares ScratchCountsWriter and JoinScratchCountsToFinal: the shared
**   output-side half of the drive-spanning scratch design, used
**   identically by both Phase 2's terminal bootstrap and Phase 3's
**   non-terminal step, so "write this level's own result" works the
**   same way regardless of which kind of level it is.
**
** Notes:
**   A level's result is always written to SCRATCH first (a
**   SegmentedStoreWriter spread across the fastest available drives,
**   excluding the store and counts drives), never directly to the
**   permanent counts directory. Once a level's both colors succeed with
**   no overflow, JoinScratchCountsToFinal reads every segment back in
**   original order -- "read the first drive, write it out, read the
**   next drive, etc." -- with no sort needed, since segments are already
**   contiguous slices of one sorted stream -- and writes the result as
**   the same compressed CalculatorCountsFile/NibbleCountsFile format
**   Phases 1-3 already produce, to the permanent countsDirectory on Y:.
**
**   The scratch record width is never narrower than what's needed to
**   hold the level's official tier losslessly: nibble-tier levels (max
**   value 14) get promoted to a 1-byte-per-counter scratch record (still
**   losslessly exact, since 14 fits trivially in a byte); byte-and-wider
**   levels use their official width directly. Overflow detection always
**   happens at the OFFICIAL width, before scratch conversion -- scratch
**   width is purely a storage-format detail, never part of the
**   overflow-checked arithmetic itself.
*/

#pragma once

/* Includes */
#include "SegmentedStore.h"
#include "OutcomeTriple.h"
#include "CounterWidthConfig.h"

/* Structures and Types */

/*
** Type:    ScratchCountsWriter
** @brief   Wraps a SegmentedStoreWriter with the official-vs-scratch
**          width bookkeeping described above.
*/
struct ScratchCountsWriter
{
    SegmentedStoreWriter store;
    int officialByteWidth = COUNTER_WIDTH_NIBBLE;
    int scratchByteWidth  = 1;

    /*
    ** Method: Init
    ** @brief  Plans scratch drives for count boards at officialByteWidthIn,
    **         and opens the segmented writer.
    ** @param  pState              - calculator state (driveInfo, driveLedger)
    ** @param  excludeDrive1       - drive to never use as scratch (RingMaster's store drive)
    ** @param  excludeDrive2       - a second drive to exclude (the counts drive)
    ** @param  count               - number of boards (records) this writer will receive
    ** @param  officialByteWidthIn - this level's CounterWidthConfig tier width
    ** @param  scratchDirNoDrive   - sub-path (on whichever drive) segments are written under
    ** @param  baseName            - filename prefix identifying this dataset (level/color)
    */
    void Init(POthelloRingMasterCalculatorState pState, char excludeDrive1, char excludeDrive2,
              uint64_t count, int officialByteWidthIn,
              const char* scratchDirNoDrive, const char* baseName);

    /*
    ** Method: WriteTriple
    ** @brief  Writes one board's result, valid when officialByteWidth != COUNTER_WIDTH_NIBBLE.
    ** @param  triple - the OutcomeTriple to write (only the first
    **                  scratchByteWidth bytes of each counter are used)
    */
    void WriteTriple(const OutcomeTriple& triple);

    /*
    ** Method: WriteNibbleTriple
    ** @brief  Writes one board's result, valid when officialByteWidth == COUNTER_WIDTH_NIBBLE.
    ** @param  triple - the NibbleOutcomeTriple to write (each field 0-14,
    **                  promoted losslessly into a 1-byte scratch slot)
    */
    void WriteNibbleTriple(const NibbleOutcomeTriple& triple);

    /*
    ** Method: Finish
    ** @brief  Closes the final segment. store.segments/store.plan are then
    **         the complete segment list and drive plan for this dataset.
    */
    void Finish();
};

/*
** Function: JoinScratchCountsToFinal
** @brief    Reads every segment back in original order (no sort needed --
**           segments are already contiguous slices of the sorted stream)
**           and writes the result as a permanent, compressed
**           CalculatorCountsFile/NibbleCountsFile at finalPath.
** @param    segments          - the writer's completed segment list, in original order
** @param    scratchByteWidth  - the width each scratch record was written at
** @param    officialByteWidth - this level's real tier width (COUNTER_WIDTH_NIBBLE or a byte width)
** @param    finalPath         - destination path in the permanent counts directory
*/
void JoinScratchCountsToFinal(const SegmentList& segments, int scratchByteWidth, int officialByteWidth,
                              const char* finalPath);
