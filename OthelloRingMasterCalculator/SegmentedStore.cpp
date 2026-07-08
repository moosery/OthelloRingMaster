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
    pWriter->currentMinKeyHi = pWriter->currentMinKeyLo = 0;
    pWriter->currentMaxKeyHi = pWriter->currentMaxKeyLo = 0;
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
    if (pWriter->isKeySorted)
    {
        seg.minKeyHi = pWriter->currentMinKeyHi;
        seg.minKeyLo = pWriter->currentMinKeyLo;
        seg.maxKeyHi = pWriter->currentMaxKeyHi;
        seg.maxKeyLo = pWriter->currentMaxKeyLo;
    }
    pWriter->segments.push_back(seg);
}

/*
** Function: CompareUint64PairRecord
** @brief    BinarySearchFile-compatible 3-way comparator over 16-byte
**           UINT64_PAIR records (board keys): compares hi first, then lo.
*/
static int CompareUint64PairRecord(void* /*pContext*/, const void* pEntry, const void* pKey)
{
    const UINT64_PAIR* e = (const UINT64_PAIR*)pEntry;
    const UINT64_PAIR* k = (const UINT64_PAIR*)pKey;
    if (e->hi != k->hi) return (e->hi < k->hi) ? -1 : 1;
    if (e->lo != k->lo) return (e->lo < k->lo) ? -1 : 1;
    return 0;
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
                                 int recordSizeIn, bool isKeySortedIn,
                                 const char* scratchDirNoDriveIn, const char* baseNameIn)
{
    pState        = pStateIn;
    excludeDrive1 = excludeDrive1In;
    excludeDrive2 = excludeDrive2In;
    recordSize    = recordSizeIn;
    isKeySorted   = isKeySortedIn;
    strncpy(scratchDirNoDrive, scratchDirNoDriveIn, sizeof(scratchDirNoDrive) - 1);
    strncpy(baseName, baseNameIn, sizeof(baseName) - 1);

    if (isKeySorted && recordSize != (int)sizeof(UINT64_PAIR))
        Fatal(FATAL_MERGE_LOGIC_ERROR, "SegmentedStoreWriter::Init: isKeySorted requires recordSize==%d, got %d",
              (int)sizeof(UINT64_PAIR), recordSize);

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

    if (isKeySorted)
    {
        const UINT64_PAIR* pKey = (const UINT64_PAIR*)pRecord;
        if (currentRecordCount == 0)
        {
            currentMinKeyHi = currentMaxKeyHi = pKey->hi;
            currentMinKeyLo = currentMaxKeyLo = pKey->lo;
        }
        else
        {
            /* Records arrive in sorted order, so the most recently written
            ** one is always this segment's max so far. */
            currentMaxKeyHi = pKey->hi;
            currentMaxKeyLo = pKey->lo;
        }
    }

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
** Method: SegmentedStoreReader::FindByKey
** @brief  See SegmentedStore.h.
*/
bool SegmentedStoreReader::FindByKey(uint64_t keyHi, uint64_t keyLo, uint64_t* pOutGlobalPosition) const
{
    uint64_t base = 0;
    for (size_t i = 0; i < segments.size(); i++)
    {
        const SegmentInfo& seg = segments[i];

        bool afterMin  = (keyHi > seg.minKeyHi) || (keyHi == seg.minKeyHi && keyLo >= seg.minKeyLo);
        bool beforeMax = (keyHi < seg.maxKeyHi) || (keyHi == seg.maxKeyHi && keyLo <= seg.maxKeyLo);

        if (afterMin && beforeMax)
        {
            FILE* f = fopen(seg.path, "rb");
            if (!f) return false;

            UINT64_PAIR target{ keyHi, keyLo };
            UINT64_PAIR scratch{};
            long long idx = BinarySearchFile(f, &target, &scratch, (long long)seg.recordCount,
                                              sizeof(UINT64_PAIR), CompareUint64PairRecord, nullptr);
            fclose(f);

            if (idx < 0) return false;
            *pOutGlobalPosition = base + (uint64_t)idx;
            return true;
        }

        base += seg.recordCount;
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
