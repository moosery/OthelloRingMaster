/*
** Filename:  SegmentedStore.cpp
**
** Purpose:
**   Implements ReserveNextScratchDrive/ReleaseScratchPlan/
**   SegmentedStoreWriter/SegmentedStoreReader/DeleteSegments, declared in
**   SegmentedStore.h.
*/

/* Includes */
#include "SegmentedStore.h"
#include "CalcDriveLedger.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

/* Internal Helpers */

/*
** Function: FlushWriteBuffer
** @brief    Issues one real fwrite of everything currently buffered in
**           pWriter->writeBuffer to its current segment file, then clears
**           the buffer. A no-op if nothing is buffered.
** @param    pWriter - the writer whose buffer to flush
*/
static void FlushWriteBuffer(SegmentedStoreWriter* pWriter)
{
    if (pWriter->writeBuffer.empty()) return;

    size_t written = fwrite(pWriter->writeBuffer.data(), 1, pWriter->writeBuffer.size(), pWriter->pCurrentFile);
    if (written != pWriter->writeBuffer.size())
        Fatal(FATAL_FI_FLUSH_FAILED, "SegmentedStoreWriter: buffered write failed to '%s'", pWriter->currentPath);

    pWriter->writeBuffer.clear();
}

/*
** Function: OpenNextSegment
** @brief    Reserves the next available drive on demand and opens its
**           segment file for writing, resetting the writer's per-segment
**           tracking fields. Fatals if every available drive is
**           already fully claimed.
** @param    pWriter - the writer to advance
*/
static void OpenNextSegment(SegmentedStoreWriter* pWriter)
{
    char    driveLetter;
    int64_t budgetBytes;
    if (!ReserveNextScratchDrive(pWriter->pState, pWriter->excludeDrive1, pWriter->excludeDrive2,
                                 &driveLetter, &budgetBytes))
        Fatal(FATAL_DRIVE_SPACE, "SegmentedStoreWriter: every available scratch drive is full for '%s'", pWriter->baseName);

    pWriter->plan.push_back({ driveLetter, budgetBytes });
    pWriter->currentBudgetBytes = budgetBytes;

    char dir[MAX_FULL_PATH_NAME];
    snprintf(dir, sizeof(dir), "%c:%s", driveLetter, pWriter->scratchDirNoDrive);
    if (!CreateFullPath(dir))
        Fatal(FATAL_CREATE_DIR_FAILED, "SegmentedStoreWriter: cannot create scratch directory '%s'", dir);

    snprintf(pWriter->currentPath, sizeof(pWriter->currentPath), "%s\\%s_seg%04zu.dat",
             dir, pWriter->baseName, pWriter->plan.size() - 1);
    pWriter->currentDriveLetter = driveLetter;

    pWriter->pCurrentFile = fopen(pWriter->currentPath, "wb");
    if (!pWriter->pCurrentFile)
        Fatal(FATAL_FILE_OPEN, "SegmentedStoreWriter: cannot open '%s' for writing", pWriter->currentPath);

    pWriter->currentBytesUsed   = 0;
    pWriter->currentRecordCount = 0;
}

/*
** Function: CloseCurrentSegment
** @brief    Closes the writer's current segment file and records it in
**           segments (unless nothing was ever written to it).
** @param    pWriter - the writer whose current segment to close
*/
static void CloseCurrentSegment(SegmentedStoreWriter* pWriter)
{
    if (!pWriter->pCurrentFile) return;

    FlushWriteBuffer(pWriter);
    fclose(pWriter->pCurrentFile);
    pWriter->pCurrentFile = nullptr;

    /* ReserveNextScratchDrive deliberately over-reserves (a whole drive's
    ** remaining budget) since a segment's real size isn't known in
    ** advance. Now that it is, give back everything this segment didn't
    ** actually use immediately -- otherwise a tiny writer would monopolize
    ** an entire drive's ledger for as long as its segment file exists
    ** (for lookup-source writers, that's the WHOLE level's processing,
    ** not just the moment of writing), starving other concurrent
    ** datasets that need a drive of their own. plan's entry is corrected
    ** to match, so a later DeleteSegments/ReleaseScratchPlan reclaims
    ** only the (now much smaller) amount actually left reserved.
    */
    int64_t unused = pWriter->currentBudgetBytes - pWriter->currentBytesUsed;
    if (unused > 0)
        CalcDriveReclaim(pWriter->pState, pWriter->currentDriveLetter, unused);
    if (!pWriter->plan.empty())
        pWriter->plan.back().second = pWriter->currentBytesUsed;

    if (pWriter->currentRecordCount == 0)
    {
        /* Nothing was ever written (Finish() called with no Write() at
        ** all) -- discard the empty file rather than recording a
        ** zero-record segment.
        */
        DeleteFileA(pWriter->currentPath);
        return;
    }

    SegmentInfo seg = {};
    strncpy(seg.path, pWriter->currentPath, sizeof(seg.path) - 1);
    seg.driveLetter = pWriter->currentDriveLetter;
    seg.recordCount = pWriter->currentRecordCount;
    pWriter->segments.push_back(seg);
}

/* Functions */

/*
** Function: ReserveNextScratchDrive
** @brief    See SegmentedStore.h.
*/
bool ReserveNextScratchDrive(POthelloRingMasterCalculatorState pState, char excludeDrive1, char excludeDrive2,
                             char* pOutDriveLetter, int64_t* pOutBudgetBytes)
{
    /* FAST first; MEDIUM only once every FAST drive is fully claimed;
    ** SLOW only once MEDIUM is too -- strict tier fallback, never
    ** proportional spread across tiers.
    */
    static const DriveCategory kTiers[3] = { DRIVE_CAT_FAST, DRIVE_CAT_MEDIUM, DRIVE_CAT_SLOW };

    for (int pass = 0; pass < 3; pass++)
    {
        for (int i = 0; i < pState->driveInfo.numDrives; i++)
        {
            const DriveInformation* d = &pState->driveInfo.drives[i];
            if (!d->available) continue;
            if (d->driveCategory != kTiers[pass]) continue;
            if (excludeDrive1 && d->driveLetter == excludeDrive1) continue;
            if (excludeDrive2 && d->driveLetter == excludeDrive2) continue;

            int64_t avail = CalcDriveAvailable(pState, d->driveLetter);
            if (avail <= 0) continue;

            if (!CalcDriveReserve(pState, d->driveLetter, avail))
                continue;   /* lost a race to another concurrent reservation -- try the next drive */

            *pOutDriveLetter = d->driveLetter;
            *pOutBudgetBytes = avail;
            return true;
        }
    }

    return false;
}

/*
** Function: ReleaseScratchPlan
** @brief    See SegmentedStore.h.
*/
void ReleaseScratchPlan(POthelloRingMasterCalculatorState pState, const ScratchPlan& plan)
{
    for (const auto& entry : plan)
        CalcDriveReclaim(pState, entry.first, entry.second);
}

/*
** Method: SegmentedStoreWriter::Init
** @brief  See SegmentedStore.h.
*/
void SegmentedStoreWriter::Init(POthelloRingMasterCalculatorState pStateIn, char excludeDrive1In, char excludeDrive2In,
                                 int recordSizeIn,
                                 const char* scratchDirNoDriveIn, const char* baseNameIn)
{
    pState        = pStateIn;
    excludeDrive1 = excludeDrive1In;
    excludeDrive2 = excludeDrive2In;
    recordSize    = recordSizeIn;
    strncpy(scratchDirNoDrive, scratchDirNoDriveIn, sizeof(scratchDirNoDrive) - 1);
    strncpy(baseName, baseNameIn, sizeof(baseName) - 1);

    writeBuffer.reserve(SEGMENTED_STORE_WRITE_BUFFER_BYTES);

    /* No segment opened yet -- the first Write() reserves a drive on demand. */
}

/*
** Method: SegmentedStoreWriter::Write
** @brief  See SegmentedStore.h.
*/
void SegmentedStoreWriter::Write(const void* pRecord)
{
    if (!pCurrentFile || (currentRecordCount > 0 && currentBytesUsed + recordSize > currentBudgetBytes))
    {
        CloseCurrentSegment(this);
        OpenNextSegment(this);
    }

    const uint8_t* pBytes = (const uint8_t*)pRecord;
    writeBuffer.insert(writeBuffer.end(), pBytes, pBytes + recordSize);

    currentBytesUsed += recordSize;
    currentRecordCount++;

    /* Flush in SEGMENTED_STORE_WRITE_BUFFER_BYTES-sized chunks -- one real
    ** fwrite per chunk instead of one per record. currentBytesUsed already
    ** tracks logical bytes committed to this segment regardless of whether
    ** they've actually hit disk yet, so segment-rollover budget accounting
    ** above is unaffected by how often this flushes.
    */
    if (writeBuffer.size() >= SEGMENTED_STORE_WRITE_BUFFER_BYTES)
        FlushWriteBuffer(this);
}

/*
** Method: SegmentedStoreWriter::Finish
** @brief  See SegmentedStore.h.
*/
void SegmentedStoreWriter::Finish()
{
    CloseCurrentSegment(this);
}

/*
** Method: SegmentedStoreReader::Load
** @brief  See SegmentedStore.h.
*/
void SegmentedStoreReader::Load(const SegmentList& segmentsIn, int recordSizeIn)
{
    segments   = segmentsIn;
    recordSize = recordSizeIn;

    cumulativeCounts.resize(segments.size());
    uint64_t running = 0;
    for (size_t i = 0; i < segments.size(); i++)
    {
        running += segments[i].recordCount;
        cumulativeCounts[i] = running;
    }
}

/*
** Method: SegmentedStoreReader::GetRecordCount
** @brief  See SegmentedStore.h.
*/
uint64_t SegmentedStoreReader::GetRecordCount() const
{
    return cumulativeCounts.empty() ? 0 : cumulativeCounts.back();
}

/*
** Method: SegmentedStoreReader::ReadAt
** @brief  See SegmentedStore.h.
*/
bool SegmentedStoreReader::ReadAt(uint64_t globalPosition, void* pOutRecord) const
{
    uint64_t base = 0;
    for (size_t i = 0; i < segments.size(); i++)
    {
        if (globalPosition < cumulativeCounts[i])
        {
            uint64_t localPos = globalPosition - base;
            FILE* f = fopen(segments[i].path, "rb");
            if (!f) return false;

            bool ok = (_fseeki64(f, (int64_t)(localPos * (uint64_t)recordSize), SEEK_SET) == 0)
                      && (fread(pOutRecord, (size_t)recordSize, 1, f) == 1);
            fclose(f);
            return ok;
        }
        base = cumulativeCounts[i];
    }
    return false;
}

/*
** Function: FindSegmentIndex
** @brief    Finds which segment holds globalPosition, and that segment's
**           own base (the global position of its first record). Shared by
**           FindPatternInRange's single-segment fast-path check.
** @param    cumulativeCounts - running total of records through segment i, inclusive
** @param    globalPosition   - 0-based record index across the whole dataset
** @param    pOutBase         - out: globalPosition of segment i's first record
** @return   Index into segments/cumulativeCounts, or -1 if out of range.
*/
static long long FindSegmentIndex(const std::vector<uint64_t>& cumulativeCounts, uint64_t globalPosition, uint64_t* pOutBase)
{
    uint64_t base = 0;
    for (size_t i = 0; i < cumulativeCounts.size(); i++)
    {
        if (globalPosition < cumulativeCounts[i])
        {
            *pOutBase = base;
            return (long long)i;
        }
        base = cumulativeCounts[i];
    }
    return -1;
}

/*
** Method: SegmentedStoreReader::FindPatternInRange
** @brief  See SegmentedStore.h.
*/
bool SegmentedStoreReader::FindPatternInRange(uint64_t lo, uint64_t hi, const void* pKey,
                                               int (*pComp)(void* pContext, const void* pEntry, const void* pKey),
                                               void* pContext, uint64_t* pOutGlobalPosition) const
{
    if (hi <= lo) return false;

    uint64_t loBase = 0, hiBase = 0;
    long long loSeg = FindSegmentIndex(cumulativeCounts, lo,     &loBase);
    long long hiSeg = FindSegmentIndex(cumulativeCounts, hi - 1, &hiBase);
    if (loSeg < 0 || hiSeg < 0) return false;

    if (loSeg == hiSeg)
    {
        /* Fast path: the whole range lives in one segment file. */
        FILE* f = fopen(segments[(size_t)loSeg].path, "rb");
        if (!f) return false;

        std::vector<uint8_t> scratch((size_t)recordSize);
        long long startIdx = (long long)(lo - loBase);
        long long count    = (long long)(hi - lo);
        long long idx = BinarySearchFile(f, (void*)pKey, scratch.data(), count, recordSize, pComp, pContext, startIdx);
        fclose(f);

        if (idx < 0) return false;
        *pOutGlobalPosition = loBase + (uint64_t)idx;
        return true;
    }

    /* Rare: the range straddles a segment boundary -- fall back to a
    ** manual binary search over global position via ReadAt, correct
    ** regardless of how many segments [lo, hi) actually spans.
    */
    std::vector<uint8_t> rec((size_t)recordSize);
    long long leftIdx = 0, rightIdx = (long long)(hi - lo) - 1;
    while (leftIdx <= rightIdx)
    {
        long long mid = leftIdx + ((rightIdx - leftIdx) >> 1);
        if (!ReadAt(lo + (uint64_t)mid, rec.data())) return false;

        int cmpVal = pComp(pContext, rec.data(), pKey);
        if (cmpVal < 0)      leftIdx  = mid + 1;
        else if (cmpVal > 0) rightIdx = mid - 1;
        else { *pOutGlobalPosition = lo + (uint64_t)mid; return true; }
    }
    return false;
}

/*
** Function: DeleteSegments
** @brief    See SegmentedStore.h.
*/
void DeleteSegments(POthelloRingMasterCalculatorState pState, const SegmentList& segments,
                    const ScratchPlan& plan)
{
    for (const auto& seg : segments)
        DeleteFileA(seg.path);
    ReleaseScratchPlan(pState, plan);
}
