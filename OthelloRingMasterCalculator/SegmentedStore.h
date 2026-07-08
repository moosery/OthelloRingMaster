/*
** Filename:  SegmentedStore.h
**
** Purpose:
**   Declares a drive-spanning segmented store: one logical sorted/ordered
**   dataset (board keys or per-board counts) split into contiguous
**   pieces -- no hashing, just "the next chunk of the already-sorted
**   stream goes here" -- each piece a plain, uncompressed, fixed-size-
**   record file on one drive. Lets a lookup against level+1's data never
**   need the whole level resident in memory, AND never requires knowing
**   the total record count up front: ReserveNextScratchDrive picks one
**   drive at a time (fastest available first) as SegmentedStoreWriter
**   actually needs a new segment, rather than planning the whole dataset
**   before the first record is even written. SegmentedStoreReader holds
**   only the tiny per-segment index in memory and does real seek+read
**   against whichever one segment a given lookup actually needs.
**
** Notes:
**   Deliberately uncompressed, unlike every other on-disk format in this
**   solution (RSF/Lz4Stream) -- this is fast scratch, not permanent
**   storage, so trading disk space for direct fseek-able random access
**   is the right call here specifically (see CalculatorFileName.h's
**   CalcNameCountsFile / CalculatorScratchCounts.h's join step for where
**   the permanent, compressed representation still lives).
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

/* A drive plan grows incrementally as segments are created (one entry
** per segment actually written), rather than being computed up front --
** kept around purely so DeleteSegments/ReleaseScratchPlan know how much
** to reclaim from pState->driveLedger later.
*/
typedef std::vector<std::pair<char, int64_t>> ScratchPlan;

/* Constants */

/* SegmentedStoreWriter buffers this many bytes of records in memory before
** issuing one real fwrite -- a FIXED cap, never scaled to level/dataset
** size (same category of exception as GpuKernels.h's GPU batch size), so
** it stays consistent with never holding a whole level resident. Larger
** sequential writes measurably lower I/O time on both NVMe and HDD tiers.
** Sized generously (not just "big enough"): at most a small handful of
** SegmentedStoreWriters are ever alive at once (one level's own output,
** plus level+1's board-key/counts scratch while it's being built), so
** even several of these buffers simultaneously is a trivial fraction of
** available RAM.
*/
#define SEGMENTED_STORE_WRITE_BUFFER_BYTES (32 * 1024 * 1024)

/* Functions */

/*
** Function: ReserveNextScratchDrive
** @brief    Picks ONE drive to host the next scratch segment and reserves
**           its entire remaining ledger budget for it: every
**           DRIVE_CAT_FAST drive is tried first (excluding excludeDrive1/2),
**           then DRIVE_CAT_MEDIUM only once no FAST drive has any room
**           left, then DRIVE_CAT_SLOW only once MEDIUM doesn't either.
**           Reserving a whole drive's remaining budget per segment (not
**           just "enough for this one record") is deliberate -- a
**           segment simply rolls to the next drive once this one fills,
**           so there's no reason to under-reserve.
** @param    pState        - calculator state (driveInfo, driveLedger)
** @param    excludeDrive1 - a drive letter to never use as scratch (e.g. RingMaster's store drive), or 0 for none
** @param    excludeDrive2 - a second drive letter to exclude (e.g. the counts drive), or 0 for none
** @param    pOutDriveLetter - out: the reserved drive's letter
** @param    pOutBudgetBytes - out: bytes reserved on that drive
** @return   true if a drive with any room was found and reserved; false
**           if every available drive (across all three tiers) is
**           already fully claimed.
*/
bool ReserveNextScratchDrive(POthelloRingMasterCalculatorState pState, char excludeDrive1, char excludeDrive2,
                             char* pOutDriveLetter, int64_t* pOutBudgetBytes);

/*
** Function: ReleaseScratchPlan
** @brief    Reclaims every drive budget in plan back to pState->driveLedger
**           (call after a segmented store built from this plan is deleted).
** @param    pState - calculator state (driveLedger)
** @param    plan   - the plan a writer accumulated (its own .plan field)
*/
void ReleaseScratchPlan(POthelloRingMasterCalculatorState pState, const ScratchPlan& plan);

/*
** Type:    SegmentedStoreWriter
** @brief   Streams fixed-size records out, reserving one drive at a time
**          on demand (via ReserveNextScratchDrive) as each segment fills
**          up -- never needs to know the total record count in advance.
**          Buffers up to SEGMENTED_STORE_WRITE_BUFFER_BYTES of records in
**          memory between real fwrites (a fixed cap, not proportional to
**          the dataset) -- never holds anything close to a whole level
**          resident, just a small, constant-size chunk of pending output.
*/
struct SegmentedStoreWriter
{
    POthelloRingMasterCalculatorState pState = nullptr;
    char     excludeDrive1 = 0, excludeDrive2 = 0;
    int      recordSize = 0;
    bool     isKeySorted = false;   /* true only for board-key (16-byte UINT64_PAIR) stores */
    char     scratchDirNoDrive[MAX_FULL_PATH_NAME] = {};
    char     baseName[MAX_FULL_PATH_NAME] = {};

    ScratchPlan plan;   /* grows by one entry each time a new segment is opened */

    FILE*    pCurrentFile        = nullptr;
    int64_t  currentBudgetBytes  = 0;
    int64_t  currentBytesUsed    = 0;
    uint64_t currentRecordCount  = 0;
    char     currentPath[MAX_FULL_PATH_NAME] = {};
    char     currentDriveLetter  = 0;
    uint64_t currentMinKeyHi = 0, currentMinKeyLo = 0;
    uint64_t currentMaxKeyHi = 0, currentMaxKeyLo = 0;

    /* Accumulates whole records until it reaches SEGMENTED_STORE_WRITE_
    ** BUFFER_BYTES, then one real fwrite flushes it all at once -- fixed
    ** capacity (reserved once in Init), never proportional to how many
    ** records this writer will ultimately see.
    */
    std::vector<uint8_t> writeBuffer;

    SegmentList segments;

    /*
    ** Method: Init
    ** @brief  Prepares the writer. No segment is opened yet -- the first
    **         Write() call reserves the first drive on demand.
    ** @param  pStateIn            - calculator state (driveInfo, driveLedger)
    ** @param  excludeDrive1In     - drive to never use as scratch (RingMaster's store drive), or 0
    ** @param  excludeDrive2In     - a second drive to exclude (the counts drive), or 0
    ** @param  recordSizeIn        - size in bytes of one record
    ** @param  isKeySortedIn       - true if records are 16-byte UINT64_PAIR board keys in sorted order
    ** @param  scratchDirNoDriveIn - sub-path (on whichever drive) segments are written under
    ** @param  baseNameIn          - filename prefix identifying this dataset (level/color/purpose)
    */
    void Init(POthelloRingMasterCalculatorState pStateIn, char excludeDrive1In, char excludeDrive2In,
              int recordSizeIn, bool isKeySortedIn,
              const char* scratchDirNoDriveIn, const char* baseNameIn);

    /*
    ** Method: Write
    ** @brief  Appends one record to the in-memory writeBuffer (real
    **         fwrite happens only once writeBuffer reaches
    **         SEGMENTED_STORE_WRITE_BUFFER_BYTES, or a segment closes),
    **         reserving a new drive on demand (via ReserveNextScratchDrive)
    **         whenever the current segment's budget is exhausted or no
    **         segment is open yet. Fatals if every available drive is
    **         already fully claimed.
    ** @param  pRecord - recordSize bytes to append
    */
    void Write(const void* pRecord);

    /*
    ** Method: Finish
    ** @brief  Flushes any buffered records and closes the final segment.
    **         segments is then the complete, ordered segment list for
    **         this dataset.
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
                    const ScratchPlan& plan);
