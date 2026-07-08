/*
** Filename:  SegmentedStore.h
**
** Purpose:
**   Declares a drive-spanning segmented store: one logical sorted/ordered
**   dataset (board keys or per-board counts) split into contiguous
**   pieces -- no hashing, just "the next chunk of the already-sorted
**   stream goes here" -- each piece a plain, uncompressed, fixed-size-
**   record file on one drive. Lets a lookup against level+1's data never
**   need the whole level resident in memory: PlanScratchDrives picks
**   which drives to use (fastest first) and how big a piece each one
**   gets; SegmentedStoreWriter streams records out across that plan;
**   SegmentedStoreReader holds only the tiny per-segment index in memory
**   and does real seek+read against whichever one segment a given
**   lookup actually needs.
**
** Notes:
**   Deliberately uncompressed, unlike every other on-disk format in this
**   solution (RSF/Lz4Stream) -- this is fast scratch, not permanent
**   storage, so trading disk space for direct fseek-able random access
**   is the right call here specifically (see CalculatorFileName.h's
**   CalcNameCountsFile / the join step in BackwardWalkDriver.cpp for
**   where the permanent, compressed representation still lives).
**   FindByKey rides Utility/BinarySearchFile.h's existing
**   BinarySearchFile (seek+read one record at a time, no bulk load)
**   for the within-segment search.
**
**   Thread-safe by construction: ReadAt/FindByKey each open, read, and
**   close their own FILE* handle per call, touching only the (read-only
**   after Load) segment index otherwise -- safe to call from many
**   threads concurrently with no locking, which matters since lookups
**   are dispatched one thread-pool job per parent (see
**   NonTerminalLevelStep.cpp).
*/

#pragma once

/* Includes */
#include "CalculatorTypes.h"
#include "RingStoreFile.h"   /* UINT64_PAIR */
#include <vector>
#include <utility>

/* Structures and Types */

/*
** Type:    SegmentInfo
** @brief   One physical segment: a plain fixed-size-record file on one
**          drive, holding a contiguous slice of the original ordered
**          stream. minKey/maxKey are only meaningful for key-sorted
**          (board-key) stores -- left zero for positional (counts) stores.
*/
struct SegmentInfo
{
    char     path[MAX_FULL_PATH_NAME];
    char     driveLetter;
    uint64_t recordCount;
    uint64_t minKeyHi, minKeyLo;
    uint64_t maxKeyHi, maxKeyLo;
};

typedef std::vector<SegmentInfo> SegmentList;

/* Functions */

/*
** Function: PlanScratchDrives
** @brief    Builds a priority-ordered list of (drive letter, byte budget)
**           to cover totalBytes of scratch data: every DRIVE_CAT_FAST
**           drive first (excluding excludeDrive1/2), then DRIVE_CAT_MEDIUM
**           drives only once every fast drive is fully claimed, then
**           DRIVE_CAT_SLOW only once medium is too. Reserves each
**           contributed chunk from pState->driveLedger as it plans, so a
**           second concurrent call correctly sees less room left.
** @param    pState        - calculator state (driveInfo, driveLedger)
** @param    excludeDrive1 - a drive letter to never use as scratch (e.g. RingMaster's store drive), or 0 for none
** @param    excludeDrive2 - a second drive letter to exclude (e.g. the counts drive), or 0 for none
** @param    totalBytes    - total size to plan for
** @param    purpose       - short description for the Fatal message if space runs out
** @param    pOutPlan      - out: ordered (driveLetter, budgetBytes) pairs, already ledger-reserved
*/
void PlanScratchDrives(POthelloRingMasterCalculatorState pState, char excludeDrive1, char excludeDrive2,
                       int64_t totalBytes, const char* purpose,
                       std::vector<std::pair<char, int64_t>>* pOutPlan);

/*
** Function: ReleaseScratchPlan
** @brief    Reclaims every drive budget in plan back to pState->driveLedger
**           (call after a segmented store built from this plan is deleted).
** @param    pState - calculator state (driveLedger)
** @param    plan   - the plan previously returned by PlanScratchDrives
*/
void ReleaseScratchPlan(POthelloRingMasterCalculatorState pState, const std::vector<std::pair<char, int64_t>>& plan);

/*
** Type:    SegmentedStoreWriter
** @brief   Streams fixed-size records out across a drive plan, rolling to
**          the next drive once the current one's budget is used up.
**          Never holds more than one segment's worth of data resident --
**          each Write() call goes straight to disk.
*/
struct SegmentedStoreWriter
{
    std::vector<std::pair<char, int64_t>> plan;
    size_t   planIdx = 0;
    int      recordSize = 0;
    bool     isKeySorted = false;   /* true only for board-key (16-byte UINT64_PAIR) stores */
    char     scratchDirNoDrive[MAX_FULL_PATH_NAME] = {};
    char     baseName[MAX_FULL_PATH_NAME] = {};

    FILE*    pCurrentFile        = nullptr;
    int64_t  currentBytesUsed    = 0;
    uint64_t currentRecordCount  = 0;
    char     currentPath[MAX_FULL_PATH_NAME] = {};
    char     currentDriveLetter  = 0;
    uint64_t currentMinKeyHi = 0, currentMinKeyLo = 0;
    uint64_t currentMaxKeyHi = 0, currentMaxKeyLo = 0;

    SegmentList segments;

    /*
    ** Method: Init
    ** @brief  Prepares the writer to stream records across planIn.
    ** @param  planIn            - drive plan from PlanScratchDrives (already ledger-reserved)
    ** @param  recordSizeIn      - size in bytes of one record
    ** @param  isKeySortedIn     - true if records are 16-byte UINT64_PAIR board keys in sorted order
    ** @param  scratchDirNoDriveIn - sub-path (on whichever drive) segments are written under
    ** @param  baseNameIn        - filename prefix identifying this dataset (level/color/purpose)
    */
    void Init(std::vector<std::pair<char, int64_t>> planIn, int recordSizeIn, bool isKeySortedIn,
              const char* scratchDirNoDriveIn, const char* baseNameIn);

    /*
    ** Method: Write
    ** @brief  Appends one record, rolling to the next drive in the plan if
    **         the current segment's budget is exhausted. Fatals if the
    **         plan runs out before all records are written (a planning
    **         bug, since PlanScratchDrives already Fatals if the plan
    **         couldn't cover the requested total).
    ** @param  pRecord - recordSize bytes to append
    */
    void Write(const void* pRecord);

    /*
    ** Method: Finish
    ** @brief  Closes the final segment. segments is then the complete,
    **         ordered segment list for this dataset.
    */
    void Finish();
};

/*
** Type:    SegmentedStoreReader
** @brief   Holds only the small per-segment index in memory; ReadAt/
**          FindByKey each do their own seek+read against exactly one
**          segment file, never loading a whole level's worth of data.
*/
struct SegmentedStoreReader
{
    SegmentList           segments;
    std::vector<uint64_t> cumulativeCounts;   /* running total of records through segment i, inclusive */
    int                   recordSize = 0;

    /*
    ** Method: Load
    ** @brief  Takes ownership of segmentsIn's index (not its file contents).
    ** @param  segmentsIn   - the segment list (from a writer's Finish(), or reloaded from disk)
    ** @param  recordSizeIn - size in bytes of one record
    */
    void Load(const SegmentList& segmentsIn, int recordSizeIn);

    /*
    ** Method: ReadAt
    ** @brief  Reads the record at globalPosition (0-based, across all
    **         segments in order) -- computes which segment holds it, then
    **         seeks directly to the right offset within that one segment file.
    ** @param  globalPosition - 0-based record index across the whole dataset
    ** @param  pOutRecord     - out: recordSize bytes
    ** @return true if globalPosition was in range and the read succeeded.
    */
    bool ReadAt(uint64_t globalPosition, void* pOutRecord) const;

    /*
    ** Method: FindByKey
    ** @brief  Finds a 16-byte (keyHi, keyLo) key's global position. Coarse
    **         step: segments are disjoint contiguous ranges of one sorted
    **         stream, so at most one segment's [minKey, maxKey] can
    **         contain the target -- a linear scan across the (small)
    **         segment list finds it. Fine step: Utility's BinarySearchFile
    **         seeks within just that one segment.
    ** @param  keyHi               - high 64 bits of the target key (BOARD_KEY::ullCellsInUse)
    ** @param  keyLo               - low 64 bits of the target key (BOARD_KEY::ullCellColors)
    ** @param  pOutGlobalPosition  - out: the key's 0-based global position, if found
    ** @return true if found.
    */
    bool FindByKey(uint64_t keyHi, uint64_t keyLo, uint64_t* pOutGlobalPosition) const;

    /*
    ** Method: GetRecordCount
    ** @brief  Returns the total record count across every segment.
    */
    uint64_t GetRecordCount() const;
};

/*
** Function: DeleteSegments
** @brief    Deletes every segment file in segments and reclaims the space
**           back to pState->driveLedger via plan (the same plan the
**           segments were written under).
** @param    pState   - calculator state (driveLedger)
** @param    segments - the segment files to delete
** @param    plan     - the drive plan those segments were written under
*/
void DeleteSegments(POthelloRingMasterCalculatorState pState, const SegmentList& segments,
                    const std::vector<std::pair<char, int64_t>>& plan);
