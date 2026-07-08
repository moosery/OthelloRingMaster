/*
** Filename:  SegmentedStore.cpp
**
** Purpose:
**   Implements PlanScratchDrives/ReleaseScratchPlan/SegmentedStoreWriter/
**   SegmentedStoreReader/DeleteSegments, declared in SegmentedStore.h.
*/

/* Includes */
#include "SegmentedStore.h"
#include "CalcDriveLedger.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

/* Internal Helpers */

/*
** Function: OpenNextSegment
** @brief    Opens the next planned drive's segment file for writing,
**           resetting the writer's per-segment tracking fields.
** @param    pWriter - the writer to advance
*/
static void OpenNextSegment(SegmentedStoreWriter* pWriter)
{
    if (pWriter->planIdx >= pWriter->plan.size())
        Fatal(FATAL_MERGE_LOGIC_ERROR, "SegmentedStoreWriter: ran out of planned drives for '%s' -- PlanScratchDrives should have Fataled first", pWriter->baseName);

    char driveLetter = pWriter->plan[pWriter->planIdx].first;
    char dir[MAX_FULL_PATH_NAME];
    snprintf(dir, sizeof(dir), "%c:%s", driveLetter, pWriter->scratchDirNoDrive);
    if (!CreateFullPath(dir))
        Fatal(FATAL_CREATE_DIR_FAILED, "SegmentedStoreWriter: cannot create scratch directory '%s'", dir);

    snprintf(pWriter->currentPath, sizeof(pWriter->currentPath), "%s\\%s_seg%04zu.dat",
             dir, pWriter->baseName, pWriter->planIdx);
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

    fclose(pWriter->pCurrentFile);
    pWriter->pCurrentFile = nullptr;

    if (pWriter->currentRecordCount == 0)
    {
        /* The plan had more drives allotted than turned out to be needed --
        ** discard the empty file rather than recording a zero-record segment.
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
** Function: PlanScratchDrives
** @brief    See SegmentedStore.h.
*/
void PlanScratchDrives(POthelloRingMasterCalculatorState pState, char excludeDrive1, char excludeDrive2,
                       int64_t totalBytes, const char* purpose,
                       std::vector<std::pair<char, int64_t>>* pOutPlan)
{
    pOutPlan->clear();
    int64_t remaining = totalBytes;

    /* FAST first; MEDIUM only once every FAST drive is fully claimed;
    ** SLOW only once MEDIUM is too -- strict tier fallback, never
    ** proportional spread across tiers.
    */
    static const DriveCategory kTiers[3] = { DRIVE_CAT_FAST, DRIVE_CAT_MEDIUM, DRIVE_CAT_SLOW };

    for (int pass = 0; pass < 3 && remaining > 0; pass++)
    {
        for (int i = 0; i < pState->driveInfo.numDrives && remaining > 0; i++)
        {
            const DriveInformation* d = &pState->driveInfo.drives[i];
            if (!d->available) continue;
            if (d->driveCategory != kTiers[pass]) continue;
            if (excludeDrive1 && d->driveLetter == excludeDrive1) continue;
            if (excludeDrive2 && d->driveLetter == excludeDrive2) continue;

            int64_t avail = CalcDriveAvailable(pState, d->driveLetter);
            if (avail <= 0) continue;

            int64_t take = (avail < remaining) ? avail : remaining;
            if (!CalcDriveReserve(pState, d->driveLetter, take))
                continue;   /* lost a race to another concurrent plan -- try the next drive */

            pOutPlan->push_back({ d->driveLetter, take });
            remaining -= take;
        }
    }

    if (remaining > 0)
    {
        ReleaseScratchPlan(pState, *pOutPlan);
        Fatal(FATAL_DRIVE_SPACE,
              "PlanScratchDrives: not enough scratch space for %s (%lld bytes needed, %lld bytes short across all available drives)",
              purpose, (long long)totalBytes, (long long)remaining);
    }
}

/*
** Function: ReleaseScratchPlan
** @brief    See SegmentedStore.h.
*/
void ReleaseScratchPlan(POthelloRingMasterCalculatorState pState, const std::vector<std::pair<char, int64_t>>& plan)
{
    for (const auto& entry : plan)
        CalcDriveReclaim(pState, entry.first, entry.second);
}

/*
** Method: SegmentedStoreWriter::Init
** @brief  See SegmentedStore.h.
*/
void SegmentedStoreWriter::Init(std::vector<std::pair<char, int64_t>> planIn, int recordSizeIn, bool isKeySortedIn,
                                 const char* scratchDirNoDriveIn, const char* baseNameIn)
{
    plan        = std::move(planIn);
    planIdx     = 0;
    recordSize  = recordSizeIn;
    isKeySorted = isKeySortedIn;
    strncpy(scratchDirNoDrive, scratchDirNoDriveIn, sizeof(scratchDirNoDrive) - 1);
    strncpy(baseName, baseNameIn, sizeof(baseName) - 1);

    if (isKeySorted && recordSize != (int)sizeof(UINT64_PAIR))
        Fatal(FATAL_MERGE_LOGIC_ERROR, "SegmentedStoreWriter::Init: isKeySorted requires recordSize==%d, got %d",
              (int)sizeof(UINT64_PAIR), recordSize);

    if (plan.empty())
        Fatal(FATAL_MERGE_LOGIC_ERROR, "SegmentedStoreWriter::Init: empty drive plan for '%s'", baseName);

    OpenNextSegment(this);
}

/*
** Method: SegmentedStoreWriter::Write
** @brief  See SegmentedStore.h.
*/
void SegmentedStoreWriter::Write(const void* pRecord)
{
    if (currentRecordCount > 0 && currentBytesUsed + recordSize > plan[planIdx].second)
    {
        CloseCurrentSegment(this);
        planIdx++;
        OpenNextSegment(this);
    }

    if (fwrite(pRecord, (size_t)recordSize, 1, pCurrentFile) != 1)
        Fatal(FATAL_FI_FLUSH_FAILED, "SegmentedStoreWriter::Write: write failed to '%s'", currentPath);

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
                    const std::vector<std::pair<char, int64_t>>& plan)
{
    for (const auto& seg : segments)
        DeleteFileA(seg.path);
    ReleaseScratchPlan(pState, plan);
}
