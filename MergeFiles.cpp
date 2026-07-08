/*
** Filename:  MergeFiles.cpp
**
** Purpose:
**   Implements the k-way merge / cross-drive consolidation machinery
**   declared in MergeFiles.h: FlushMergeWriterBuffer (in-memory merge of one
**   merge-writer thread's accumulated GPU flush segments to an RSF file),
**   DoCrossDriveIntermediateMerge (consolidates NVMe writer files onto a
**   medium drive, or performs a total flush to the store drive if the
**   medium drive is full), and DoEndOfLevelMerge (the end-of-level
**   consolidation of every remaining writer/intermediate file into a single
**   sorted, deduped store file per player, then converted into the ring
**   nested-index format via ConvertLevelOutputToNestedIndex).
**
** Notes:
**   Adapted from an earlier solver implementation, renamed onto this
**   solution's own types (BOARD_KEY_DISK -> UINT64_PAIR,
**   .ullCellsInUse/.ullCellColors -> .hi/.lo, the old record-file prefix
**   -> RSF-prefixed names and RSFTrailer, RSFFileName.h). Logic and
**   choreography unchanged -- every merge comparator here does a plain
**   numeric (hi, lo) comparison; it never interprets board bits, so the
**   merge is correct whether the underlying encoding is row-major or
**   ring-ordered, as long as every input stream uses the same encoding
**   (see project_gpu_reorder_integration_design memory for the one real
**   risk this implies).
**
**   InMemDiskHead/InMemDiskHeadGreater from the earlier implementation
**   were dropped -- confirmed dead code there (declared, never used
**   anywhere).
**
**   ConvertLevelOutputToNestedIndex is new -- it's the piece that actually
**   realizes the ring-ordered storage scheme's validated space savings; no
**   earlier implementation had an equivalent, since none of them wrote
**   anything but flat files. See that function's own header comment for
**   the design.
*/

/* Includes */
#include "MergeFiles.h"
#include "RSFFileName.h"
#include "DriveLedger.h"
#include "OthelloBasics.h"
#include "RingNestedIndex.h"
#include "Logger.h"
#include "Mem.h"
#include <windows.h>
#include <stdio.h>
#include <algorithm>
#include <queue>
#include <thread>
#include <vector>

/* Functions */

/*
** Function: PeekRecordCount
** @brief    Returns the record count stored in an RSF file's trailer, or 0 on failure.
** @details  Used to convert compressed file sizes to uncompressed-equivalent
**           bytes for imerge progress tracking (KWayMergeFiles counts
**           progress in sizeof(UINT64_PAIR) units).
** @param    path - the RSF file to peek at
** @return   Record count, or 0 if the file can't be read or has a bad magic.
*/
static int64_t PeekRecordCount(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    RSFTrailer trailer = {};
    _fseeki64(f, -(int64_t)sizeof(trailer), SEEK_END);
    fread(&trailer, sizeof(trailer), 1, f);
    fclose(f);
    if (trailer.magic != RSF_MAGIC && trailer.magic != RSFZ_MAGIC && trailer.magic != RSFZL_MAGIC) return 0;
    return (int64_t)trailer.recordCount;
}

/*
** ============================================================
** Min-heap entry for file-based k-way merge (16-byte records)
** ============================================================
*/

/*
** Type:    MergeHead
** @brief   One open reader's current front-of-stream record, for the
**          file-based k-way merge heap.
*/
struct MergeHead
{
    UINT64_PAIR  key;
    RSFReader*   pReader;
};

/*
** Type:    MergeHeadGreater
** @brief   Min-heap comparator (std::priority_queue is a max-heap by
**          default, so "greater" here yields ascending pop order) ordering
**          by (hi, lo) -- a plain numeric comparison, agnostic to whatever
**          bit-ordering convention hi/lo actually encode.
*/
struct MergeHeadGreater
{
    bool operator()(const MergeHead& a, const MergeHead& b) const
    {
        if (a.key.hi != b.key.hi)
            return a.key.hi > b.key.hi;
        return a.key.lo > b.key.lo;
    }
};

/*
** ============================================================
** Helpers
** ============================================================
*/

/*
** Function: EnumerateByPattern
** @brief    Enumerates all RSF files matching fullPattern (e.g.
**           "D:\dir\writer_black_*.rsf"), extracting the directory
**           component from fullPattern automatically.
** @param    fullPattern - glob pattern to search for
** @param    outPaths    - out: array of newly-allocated path strings (caller frees each)
** @param    maxPaths    - capacity of outPaths
** @param    pTotalBytes - out: sum of matched files' sizes
** @param    outSizes    - out (optional): per-file size, parallel to outPaths
** @return   Number of files found.
*/
static int EnumerateByPattern(const char* fullPattern, char** outPaths, int maxPaths,
                               uint64_t* pTotalBytes, uint64_t* outSizes = nullptr)
{
    char dir[MAX_FULL_PATH_NAME];
    strncpy_s(dir, sizeof(dir), fullPattern, _TRUNCATE);
    char* lastSlash = strrchr(dir, '\\');
    if (!lastSlash) { *pTotalBytes = 0; return 0; }
    *lastSlash = '\0';

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(fullPattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { *pTotalBytes = 0; return 0; }

    int count    = 0;
    *pTotalBytes = 0;
    do
    {
        if (count >= maxPaths) { FindClose(h); return count; }
        char full[MAX_FULL_PATH_NAME];
        snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
        outPaths[count] = (char*)MemMalloc("enumPath", strlen(full) + 1);
        if (!outPaths[count])
            Fatal(FATAL_ALLOCATION_FAILED, "EnumerateByPattern: cannot allocate path");
        strcpy(outPaths[count], full);
        ULARGE_INTEGER sz;
        sz.LowPart  = fd.nFileSizeLow;
        sz.HighPart = (DWORD)fd.nFileSizeHigh;
        *pTotalBytes += sz.QuadPart;
        if (outSizes) outSizes[count] = sz.QuadPart;
        count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return count;
}

/*
** Function: KWayMergeFiles
** @brief    File-based k-way merge of player-homogeneous files (all same
**           player). Deduplicates on (hi, lo). Takes ownership of every
**           reader (file or extra) and closes it.
** @details  extraReaders: already-open RSFReaders (e.g. RSFReaderOpenZMem
**           over an in-memory pool segment) merged into the same heap as
**           the file readers -- a memory-backed reader and a file-backed
**           one are the same opaque type and read identically, so there's
**           no need to materialize in-memory data as a file just to merge it in.
** @param    inputPaths     - files to merge
** @param    numInputs      - number of files in inputPaths
** @param    outputPath     - destination file
** @param    pProgressBytes - out (optional): incremented by sizeof(UINT64_PAIR) per record popped
** @param    compressed     - true to write a delta+varint-compressed (.rsfz) output
** @param    pTerminate     - out-of-band cancellation flag, checked between pops
** @param    extraReaders   - already-open readers to merge in alongside inputPaths
** @return   Unique record count written.
*/
static uint64_t KWayMergeFiles(char** inputPaths, int numInputs, const char* outputPath,
                                volatile int64_t* pProgressBytes, bool compressed = false,
                                const volatile bool* pTerminate = nullptr,
                                const std::vector<RSFReader*>& extraReaders = {})
{
    std::priority_queue<MergeHead, std::vector<MergeHead>, MergeHeadGreater> heap;

    for (int i = 0; i < numInputs; i++)
    {
        RSFReader* r = RSFOpen(inputPaths[i]);
        if (!r)
        {
            LoggerLog("KWayMerge: WARNING skipping unreadable file '%s'\n", inputPaths[i]);
            continue;
        }
        UINT64_PAIR first;
        if (RSFRead(r, &first, 1) == 1)
            heap.push({ first, r });
        else
            RSFClose(&r);
    }

    for (RSFReader* r : extraReaders)
    {
        UINT64_PAIR first;
        if (RSFRead(r, &first, 1) == 1)
            heap.push({ first, r });
        else
            RSFClose(&r);
    }

    RSFWriter*   pw      = compressed ? RSFWriterOpenZ(outputPath) : RSFWriterOpen(outputPath);
    UINT64_PAIR  lastKey = {};
    bool         hasLast = false;

    while (!heap.empty())
    {
        if (pTerminate && *pTerminate) break;

        MergeHead top = heap.top();
        heap.pop();

        if (pProgressBytes)
            InterlockedAdd64((volatile LONG64*)pProgressBytes, (LONG64)sizeof(UINT64_PAIR));

        bool isDup = hasLast && top.key.hi == lastKey.hi && top.key.lo == lastKey.lo;
        if (!isDup)
        {
            RSFWriterRecord(pw, &top.key);
            lastKey = top.key;
            hasLast = true;
        }

        UINT64_PAIR next;
        if (RSFRead(top.pReader, &next, 1) == 1)
        {
            top.key = next;
            heap.push(top);
        }
        else
        {
            RSFClose(&top.pReader);
        }
    }

    /* Close any readers still in the heap (handles early termination cleanly). */
    while (!heap.empty())
    {
        MergeHead top = heap.top(); heap.pop();
        RSFClose(&top.pReader);
    }

    return RSFWriterClose(pw);
}

/*
** Function: CascadingMerge
** @brief    Merges numInputs files into finalOutPath, recursing through
**           intermediate grouped passes when numInputs exceeds
**           MAX_MERGE_FANIN (bounded simultaneously-open file handles).
** @details  pCtx is non-null only on the outer call; nullptr is passed for
**           the recursive final-pass call so cascade tracking is not
**           re-entered. tempDirs is an ordered list of candidate directories
**           for cascade temp files (fastest/most-available first), with
**           storeMergeDirectory as the last-resort entry.
**
**           Group sizing is dynamic: for each group we ask each drive "how
**           many files can you hold right now?" and write as many as fit to
**           the best available drive (the fastest medium drive first). This
**           fills that drive as fully as possible before spilling any files
**           to the store drive. After each group's dedup savings are
**           reclaimed, the fast drive may have room again for the next group.
**
**           extraReaders: leftover in-memory pool data (see KWayMergeFiles)
**           to merge in alongside the on-disk inputs. Only handled in the
**           single-pass fast path (numInputs <= MAX_MERGE_FANIN) -- grouped/
**           cascading mode bounds the number of simultaneously-open OS file
**           handles, a constraint that doesn't apply to memory readers, so
**           callers are expected to materialize any leftover pool to disk
**           first if numInputs alone already requires grouping. The Fatal()
**           below is a hard backstop against silently dropping that data if
**           that expectation is ever violated, rather than a normal code path.
** @param    inputPaths             - files to merge
** @param    numInputs              - number of files in inputPaths
** @param    tempDirs               - candidate directories for cascade temp files, ordered fastest-first
** @param    numTempDirs            - number of entries in tempDirs
** @param    finalOutPath           - destination for the final merged output
** @param    pTempCount             - running counter for cascade temp file indices
** @param    level                  - level being merged (for temp file naming)
** @param    player                 - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE (for temp file naming/logging)
** @param    pProgressBytes         - out (optional): merge progress, see KWayMergeFiles
** @param    pCtx                   - solve context (non-null only on the outer call)
** @param    compressFinal          - true to compress the final output
** @param    compressIntermediate   - true to compress cascade temp files
** @param    pTerminate             - out-of-band cancellation flag (used when pCtx is null)
** @param    extraReaders           - leftover in-memory pool data to merge in
** @return   Unique record count written to finalOutPath.
*/
static uint64_t CascadingMerge(char** inputPaths, int numInputs,
                                 const char** tempDirs, int numTempDirs,
                                 const char* finalOutPath,
                                 int* pTempCount, int level, int player,
                                 volatile int64_t* pProgressBytes,
                                 PSolveContext pCtx, bool compressFinal = false,
                                 bool compressIntermediate = false,
                                 const volatile bool* pTerminate = nullptr,
                                 const std::vector<RSFReader*>& extraReaders = {})
{
    const volatile bool* pTerm = pCtx ? &pCtx->pState->terminateThreads : pTerminate;

    if (numInputs <= MAX_MERGE_FANIN)
        return KWayMergeFiles(inputPaths, numInputs, finalOutPath, pProgressBytes,
                              compressFinal, pTerm, extraReaders);

    if (!extraReaders.empty())
        Fatal(FATAL_MERGE_LOGIC_ERROR,
              "CascadingMerge: %s has %zu in-memory pool readers but numInputs=%d "
              "> MAX_MERGE_FANIN requires grouped mode, which only accepts files -- "
              "caller must flush the pool to disk first",
              RSFPlayerStr(player), extraReaders.size(), numInputs);

    POthelloRingMasterState pSt = pCtx ? pCtx->pState : nullptr;

    /* Upper bound on groups is numInputs (1 file per group in the extreme
    ** case). In practice groups are large, but we allocate conservatively.
    */
    char**   tempPaths       = (char**)MemMalloc("cascadeTempPaths",
                                                   (size_t)numInputs * sizeof(char*));
    int64_t* tempActualSizes = pSt
        ? (int64_t*)MemMalloc("cascadeTempSizes", (size_t)numInputs * sizeof(int64_t))
        : nullptr;
    if (!tempPaths || (pSt && !tempActualSizes))
        Fatal(FATAL_ALLOCATION_FAILED, "CascadingMerge: cannot allocate temp arrays");

    /* Seed the status display with a minimum-group estimate; updated if we create more. */
    if (pSt)
    {
        pSt->cascadeNumGroups[player]          = (numInputs + MAX_MERGE_FANIN - 1) / MAX_MERGE_FANIN;
        pSt->cascadeGroupsDone[player]         = 0;
        pSt->cascadeGroupProgressBytes[player] = 0;
        pSt->cascadeStartTickMs[player]        = GetTickCount64();
        pSt->cascadeActive[player]             = true;
    }

    int numTemps = 0;
    int start    = 0;
    /* Heap-allocated once; reused each iteration. Stack would be 28 KB (MAX_MERGE_FANIN*8). */
    std::vector<int64_t> fileSzCache(MAX_MERGE_FANIN, 0);
    while (start < numInputs)
    {
        if (pTerm && *pTerm) break;

        int windowSize = (std::min)(MAX_MERGE_FANIN, numInputs - start);

        /* Precompute file sizes for the next window of up to MAX_MERGE_FANIN
        ** files. Used by each drive check so GetFileAttributesExA is called
        ** once per file.
        */
        std::fill(fileSzCache.begin(), fileSzCache.begin() + windowSize, 0);
        for (int k = 0; k < windowSize; k++)
        {
            WIN32_FILE_ATTRIBUTE_DATA fad = {};
            if (GetFileAttributesExA(inputPaths[start + k], GetFileExInfoStandard, &fad))
                fileSzCache[k] = ((int64_t)fad.nFileSizeHigh << 32)
                               | (int64_t)fad.nFileSizeLow;
        }

        /* For each candidate drive (fastest first, store drive last resort):
        ** count how many consecutive files from 'start' fit within the
        ** drive's available ledger space. Use the first drive that can
        ** accept at least one file.
        */
        const char* chosenDir  = (numTempDirs > 0) ? tempDirs[0] : nullptr;
        int         groupSize  = windowSize;
        int64_t     groupBytes = 0;
        for (int k = 0; k < windowSize; k++) groupBytes += fileSzCache[k];

        if (pSt && numTempDirs > 0)
        {
            chosenDir  = nullptr;
            groupSize  = 0;
            groupBytes = 0;
            for (int d = 0; d < numTempDirs; d++)
            {
                int64_t avail = DriveAvailable(pSt, tempDirs[d][0]);
                int64_t accum = 0;
                int     count = 0;
                for (int k = 0; k < windowSize; k++)
                {
                    if (accum + fileSzCache[k] > avail) break;
                    accum += fileSzCache[k];
                    count++;
                }
                if (count == 0) continue;
                if (DriveReserve(pSt, tempDirs[d][0], accum))
                {
                    chosenDir  = tempDirs[d];
                    groupSize  = count;
                    groupBytes = accum;
                    break;
                }
                /* DriveReserve failed (concurrent allocation narrowed the window) -- try next. */
            }
            if (!chosenDir)
                Fatal(FATAL_DRIVE_SPACE,
                      "CascadingMerge: %s group %d -- no temp drive has room for even one file",
                      RSFPlayerStr(player), numTemps + 1);

            /* Keep the status group-count estimate current if we're creating more groups. */
            if (numTemps + 1 > pSt->cascadeNumGroups[player])
                pSt->cascadeNumGroups[player] = numTemps + 1;
        }

        if (pSt) pSt->cascadeGroupProgressBytes[player] = 0;
        if (pSt) pSt->cascadeGroupStartTickMs[player]  = GetTickCount64();

        LoggerLog("CascadingMerge: %s group %d -> %c: (%d files, %.2f GB input)\n",
                  RSFPlayerStr(player), numTemps + 1, chosenDir[0],
                  groupSize, groupBytes / (1024.0 * 1024.0 * 1024.0));

        char tempPath[MAX_FULL_PATH_NAME];
        {
            bool tempLZ4 = pCtx && compressIntermediate
                        && pCtx->pConfig->lz4Drives[0]
                        && (strchr(pCtx->pConfig->lz4Drives, chosenDir[0]) != nullptr);
            if (tempLZ4)
                RSFZLNameCascadeTemp(tempPath, sizeof(tempPath), chosenDir, level, player, (*pTempCount)++);
            else if (compressIntermediate)
                RSFZNameCascadeTemp(tempPath, sizeof(tempPath), chosenDir, level, player, (*pTempCount)++);
            else
                RSFNameCascadeTemp(tempPath, sizeof(tempPath), chosenDir, level, player, (*pTempCount)++);
        }

        uint64_t tempUnique = KWayMergeFiles(inputPaths + start, groupSize, tempPath,
                                              pSt ? &pSt->cascadeGroupProgressBytes[player]
                                                  : nullptr,
                                              compressIntermediate, pTerm);

        if (pSt)
        {
            int64_t tempActual;
            if (compressIntermediate)
            {
                WIN32_FILE_ATTRIBUTE_DATA fad = {};
                tempActual = (int64_t)sizeof(RSFTrailer);
                if (GetFileAttributesExA(tempPath, GetFileExInfoStandard, &fad))
                    tempActual = ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;
            }
            else
            {
                tempActual = (int64_t)(tempUnique * sizeof(UINT64_PAIR)
                             + sizeof(RSFTrailer));
            }
            tempActualSizes[numTemps] = tempActual;
            /* Reclaim the dedup savings from the per-group reservation immediately. */
            DriveReclaim(pSt, chosenDir[0], groupBytes - tempActual);
        }

        tempPaths[numTemps] = (char*)MemMalloc("cascadeTempPath", strlen(tempPath) + 1);
        if (!tempPaths[numTemps])
            Fatal(FATAL_ALLOCATION_FAILED, "CascadingMerge: cannot allocate temp path");
        strcpy(tempPaths[numTemps], tempPath);
        numTemps++;

        if (pSt) pSt->cascadeGroupsDone[player]++;
        start += groupSize;
    }

    if (pSt) pSt->cascadeActive[player] = false;

    uint64_t unique = CascadingMerge(tempPaths, numTemps, tempDirs, numTempDirs,
                                      finalOutPath, pTempCount, level, player,
                                      pProgressBytes, nullptr, compressFinal,
                                      compressIntermediate, pTerm);

    for (int i = 0; i < numTemps; i++)
    {
        /* Use tempPaths[i][0] (drive letter from path) -- temps may be on different drives. */
        if (pSt) DriveReclaim(pSt, tempPaths[i][0], tempActualSizes[i]);
        DeleteFileA(tempPaths[i]);
        MemFree(tempPaths[i]);
    }
    MemFree(tempPaths);
    if (tempActualSizes) MemFree(tempActualSizes);

    return unique;
}

/* Forward declaration (defined after FlushMergeWriterBuffer) */
static void DoCrossDriveIntermediateMerge(PSolveContext pCtx);

/*
** ============================================================
** FlushMergeWriterBuffer
**
** Merges all in-memory data for thread ti (compressed pool segments via
** RSFReaderOpenZMem + uncompressed staging via raw pointer) into per-player
** NVMe files. Resets all pool and staging counters on return.
** ============================================================
*/

/*
** Type:    PoolMergeHead
** @brief   Heap entry for the unified merge: holds either an RSFReader
**          (compressed pool segment) or a raw board pointer range
**          (uncompressed staging).
*/
struct PoolMergeHead
{
    UINT64_PAIR         key;
    RSFReader*          reader;   /* non-null -> compressed segment */
    const UINT64_PAIR*  rawCur;   /* non-null -> raw staging        */
    const UINT64_PAIR*  rawEnd;
};

/*
** Type:    PoolMergeHeadGreater
** @brief   Min-heap comparator for PoolMergeHead, same (hi, lo) ordering as MergeHeadGreater.
*/
struct PoolMergeHeadGreater
{
    bool operator()(const PoolMergeHead& a, const PoolMergeHead& b) const
    {
        if (a.key.hi != b.key.hi)
            return a.key.hi > b.key.hi;
        return a.key.lo > b.key.lo;
    }
};

/*
** Function: MergePoolToWriter
** @brief    Merges all pool segments + optional uncompressed staging into
**           an open RSFWriter. Does NOT close pw; caller is responsible for
**           RSFWriterClose.
** @param    pw             - the open writer to merge into
** @param    mwBuf          - this thread's MW buffer (base for segOffsets)
** @param    segCount       - number of compressed pool segments
** @param    segOffsets     - byte offset of each segment within mwBuf
** @param    segSizes       - compressed byte size of each segment
** @param    segBoardCounts - record count of each segment
** @param    stagingBegin   - start of any live uncompressed staging (may be unused if stagingCount is 0)
** @param    stagingCount   - record count of live staging (0 = none)
** @param    pTerminate     - out-of-band cancellation flag, checked between pops
** @param    pProgressBytes - out (optional): atomically incremented (in 16 MB batches) as
**                            records are popped, so the stats thread can show live flush progress
*/
static void MergePoolToWriter(
    RSFWriter* pw,
    uint8_t* mwBuf,
    int segCount, const size_t* segOffsets, const size_t* segSizes, const int* segBoardCounts,
    const UINT64_PAIR* stagingBegin, int stagingCount,
    const volatile bool* pTerminate,
    volatile int64_t* pProgressBytes = nullptr)
{
    std::priority_queue<PoolMergeHead,
                        std::vector<PoolMergeHead>,
                        PoolMergeHeadGreater> heap;
    std::vector<RSFReader*> readers;

    /* Add compressed pool segments */
    for (int s = 0; s < segCount; s++)
    {
        RSFReader* r = RSFReaderOpenZMem(mwBuf + segOffsets[s], segSizes[s],
                                         (uint64_t)segBoardCounts[s]);
        readers.push_back(r);
        UINT64_PAIR first;
        if (RSFRead(r, &first, 1) > 0)
            heap.push({ first, r, nullptr, nullptr });
    }

    /* Add uncompressed staging (if live) */
    if (stagingCount > 0)
    {
        const UINT64_PAIR* end = stagingBegin + stagingCount;
        heap.push({ *stagingBegin, nullptr, stagingBegin, end });
    }

    UINT64_PAIR lastKey = {}; bool hasLast = false;
    int64_t progressAccum = 0;
    while (!heap.empty() && !*pTerminate)
    {
        PoolMergeHead top = heap.top(); heap.pop();
        bool dup = hasLast && top.key.hi == lastKey.hi && top.key.lo == lastKey.lo;
        if (!dup) { RSFWriterRecord(pw, &top.key); lastKey = top.key; hasLast = true; }
        if (pProgressBytes)
        {
            progressAccum += (int64_t)sizeof(UINT64_PAIR);
            if (progressAccum >= (int64_t)(1 << 24))   /* flush every 16 MB */
            {
                InterlockedAdd64(pProgressBytes, progressAccum);
                progressAccum = 0;
            }
        }

        if (top.reader)
        {
            UINT64_PAIR next;
            if (RSFRead(top.reader, &next, 1) > 0)
                { top.key = next; heap.push(top); }
        }
        else
        {
            if (++top.rawCur < top.rawEnd)
                { top.key = *top.rawCur; heap.push(top); }
        }
    }
    if (pProgressBytes && progressAccum > 0)
        InterlockedAdd64(pProgressBytes, progressAccum);

    for (RSFReader* r : readers) RSFClose(&r);
}

/*
** Function: FlushMergeWriterBuffer
** @brief    In-memory k-way merge of accumulated GPU flush segments for
**           merge-writer thread ti. Streams the sorted+deduped result
**           directly to an RSF file on that thread's NVMe directory, then
**           resets the segment tracking.
** @param    ti   - the merge-writer thread whose buffer to flush
** @param    pCtx - solve context
*/
void FlushMergeWriterBuffer(int ti, PSolveContext pCtx)
{
    POthelloRingMasterState pSt   = pCtx->pState;
    int                     level = (int)pSt->playLevel;

    bool hasBlack = pSt->mwBlackSegCount[ti] > 0 || pSt->mwBlackStagingCount[ti] > 0;
    bool hasWhite = pSt->mwWhiteSegCount[ti] > 0 || pSt->mwWhiteStagingCount[ti] > 0;
    if (!hasBlack && !hasWhite) return;

    bool  compressMW = (pCtx->pConfig->compressMode == COMPRESS_ALL);
    char  mwDL       = pSt->mwDirectory[ti][0];
    bool  lz4MW      = compressMW && pCtx->pConfig->lz4Drives[0]
                    && (strchr(pCtx->pConfig->lz4Drives, mwDL) != nullptr);

    uint8_t* mwBuf = (uint8_t*)pSt->pMWBuffer[ti];
    const UINT64_PAIR* blackStaging = (const UINT64_PAIR*)mwBuf;
    const UINT64_PAIR* whiteStaging =
        (const UINT64_PAIR*)(mwBuf + pSt->mwBufferSize - pSt->mwStagingSize);

    uint64_t blackCount = 0, whiteCount = 0;
    int      blackFilesCreated = 0, whiteFilesCreated = 0;
    uint64_t blackFileBytes    = 0, whiteFileBytes    = 0;

    /* Publish total input boards (black + white) before starting so the
    ** stats thread can show live progress for this writer's buffer-full
    ** NVMe spill.
    */
    {
        uint64_t totalInputBoards = (uint64_t)pSt->mwBlackStagingCount[ti]
                                   + (uint64_t)pSt->mwWhiteStagingCount[ti];
        for (int s = 0; s < pSt->mwBlackSegCount[ti]; s++)
            totalInputBoards += (uint64_t)pSt->mwBlackSegBoardCount[ti][s];
        for (int s = 0; s < pSt->mwWhiteSegCount[ti]; s++)
            totalInputBoards += (uint64_t)pSt->mwWhiteSegBoardCount[ti][s];

        pSt->mwFlushTotalBytes[ti]   = (int64_t)(totalInputBoards * sizeof(UINT64_PAIR));
        pSt->mwFlushDoneBytes[ti]    = 0;
        pSt->mwFlushStartTickMs[ti]  = GetTickCount64();
        pSt->mwFlushActive[ti]       = 1;
    }

    /* Black and white streams are fully independent -- separate segments,
    ** separate staging regions, separate output files -- so merge them
    ** concurrently instead of one after another. mwFlushDoneBytes[ti] is
    ** updated via InterlockedAdd64 inside MergePoolToWriter, so it's safe
    ** to share between both threads.
    */
    auto flushBlack = [&]()
    {
        if (!hasBlack) return;
        int blackFileIdx = pSt->mwBlackFileCount[ti];
        char blackPath[MAX_FULL_PATH_NAME];
        if (lz4MW)
            RSFZLNameWriterFile(blackPath, sizeof(blackPath), pSt->mwDirectory[ti],
                                RSF_PLAYER_BLACK, blackFileIdx);
        else if (compressMW)
            RSFZNameWriterFile(blackPath, sizeof(blackPath), pSt->mwDirectory[ti],
                               RSF_PLAYER_BLACK, blackFileIdx);
        else
            RSFNameWriterFile(blackPath, sizeof(blackPath), pSt->mwDirectory[ti],
                              RSF_PLAYER_BLACK, blackFileIdx);
        RSFWriter* pw = compressMW ? RSFWriterOpenZ(blackPath) : RSFWriterOpen(blackPath);

        MergePoolToWriter(pw, mwBuf,
                          pSt->mwBlackSegCount[ti],
                          pSt->mwBlackSegOffset[ti],
                          pSt->mwBlackSegSize[ti],
                          pSt->mwBlackSegBoardCount[ti],
                          blackStaging, pSt->mwBlackStagingCount[ti],
                          &pSt->terminateThreads, &pSt->mwFlushDoneBytes[ti]);
        blackCount = RSFWriterClose(pw, &blackFileBytes);
        if (blackCount == 0) { DeleteFileA(blackPath); blackFileBytes = 0; }
        else { pSt->mwBlackFileCount[ti]++; blackFilesCreated = 1; }
    };

    auto flushWhite = [&]()
    {
        if (!hasWhite) return;
        int whiteFileIdx = pSt->mwWhiteFileCount[ti];
        char whitePath[MAX_FULL_PATH_NAME];
        if (lz4MW)
            RSFZLNameWriterFile(whitePath, sizeof(whitePath), pSt->mwDirectory[ti],
                                RSF_PLAYER_WHITE, whiteFileIdx);
        else if (compressMW)
            RSFZNameWriterFile(whitePath, sizeof(whitePath), pSt->mwDirectory[ti],
                               RSF_PLAYER_WHITE, whiteFileIdx);
        else
            RSFNameWriterFile(whitePath, sizeof(whitePath), pSt->mwDirectory[ti],
                              RSF_PLAYER_WHITE, whiteFileIdx);
        RSFWriter* pw = compressMW ? RSFWriterOpenZ(whitePath) : RSFWriterOpen(whitePath);

        MergePoolToWriter(pw, mwBuf,
                          pSt->mwWhiteSegCount[ti],
                          pSt->mwWhiteSegOffset[ti],
                          pSt->mwWhiteSegSize[ti],
                          pSt->mwWhiteSegBoardCount[ti],
                          whiteStaging, pSt->mwWhiteStagingCount[ti],
                          &pSt->terminateThreads, &pSt->mwFlushDoneBytes[ti]);
        whiteCount = RSFWriterClose(pw, &whiteFileBytes);
        if (whiteCount == 0) { DeleteFileA(whitePath); whiteFileBytes = 0; }
        else { pSt->mwWhiteFileCount[ti]++; whiteFilesCreated = 1; }
    };

    std::thread blackThread(flushBlack);
    std::thread whiteThread(flushWhite);
    blackThread.join();
    whiteThread.join();

    int      filesCreated = blackFilesCreated + whiteFilesCreated;
    uint64_t fileBytes    = blackFileBytes + whiteFileBytes;

    pSt->mwFlushActive[ti] = 0;

    /* Reset all pool and staging state for this thread */
    pSt->mwBlackSegCount[ti]      = 0;
    pSt->mwBlackCompBytesUsed[ti] = 0;
    pSt->mwBlackStagingCount[ti]  = 0;
    pSt->mwWhiteSegCount[ti]      = 0;
    pSt->mwWhiteCompBytesUsed[ti] = 0;
    pSt->mwWhiteStagingCount[ti]  = 0;

    uint64_t unique = blackCount + whiteCount;
    uint64_t uncompressedBytes = unique * sizeof(UINT64_PAIR)
                               + (uint64_t)filesCreated * sizeof(RSFTrailer);

    InterlockedAdd64((volatile LONG64*)&pSt->levelStats[level].boardsWrittenToDisk, (LONG64)unique);
    InterlockedAdd64((volatile LONG64*)&pSt->levelStats[level].mwFilesCreated,      (LONG64)filesCreated);
    InterlockedAdd64((volatile LONG64*)&pSt->levelStats[level].mwBytes,             (LONG64)fileBytes);

    /* Debit the NVMe ledger for bytes just written, then check merge triggers.
    ** writerDriveStats[ti] is this thread's own drive -- writerDriveStats[i]
    ** is built 1:1 with mwDirectory[i] (see InitSolver.cpp), so no
    ** search-by-drive-letter is needed here.
    */
    char driveLetter = pSt->mwDirectory[ti][0];
    DriveDebit(pSt, driveLetter, (int64_t)fileBytes);

    pSt->writerDriveStats[ti].levelFilesWritten      += filesCreated;
    pSt->writerDriveStats[ti].levelBytesWritten      += fileBytes;
    pSt->writerDriveStats[ti].levelBytesUncompressed += uncompressedBytes;
    bool needsMerge = DriveAvailable(pSt, driveLetter) < (int64_t)pSt->writerDriveStats[ti].threshold;
    if (!needsMerge)
    {
        /* File-count trigger: merge when total unconsumed files per color >= MAX_MERGE_FANIN. */
        int totalBlack = 0, totalWhite = 0;
        for (int i = 0; i < pSt->numMergeWriters; i++)
        {
            totalBlack += pSt->mwBlackFileCount[i] - pSt->mwBlackFilesConsumed[i];
            totalWhite += pSt->mwWhiteFileCount[i] - pSt->mwWhiteFilesConsumed[i];
        }
        if (totalBlack >= MAX_MERGE_FANIN || totalWhite >= MAX_MERGE_FANIN)
            needsMerge = true;
    }
    if (needsMerge)
        DoCrossDriveIntermediateMerge(pCtx);
}

/*
** ============================================================
** DoCrossDriveIntermediateMerge
**
** Triggered when total unconsumed writer files across ALL NVMe drives (per
** color) reaches MAX_MERGE_FANIN, or when a single drive's free space drops
** below its threshold.
**
** Merges all unconsumed writer files from every MW directory for each
** player (black then white) into a single imerge file on the fastest medium
** drive. If that drive cannot hold the output, performs a TOTAL FLUSH: also
** pulls in all existing imerge files on medium drives for this level and
** player, merging the combined set to the store drive -- clearing both the
** NVMe drives and the medium drives in one shot so all fast drives are free again.
**
** Blocks until any in-progress merge by the other MW thread completes, then
** re-checks space/file-count under the lock. EnterCriticalSection (not Try)
** is used so that a thread with a nearly-full drive cannot skip the wait and
** keep writing until the drive exhausts itself. File counts are snapshotted
** under the lock; because counts are incremented AFTER close, all files with
** index < snapshot[i] are guaranteed complete and safe to read/delete.
** ============================================================
*/

/*
** Function: DoCrossDriveIntermediateMerge
** @brief    See file section comment above.
** @param    pCtx - solve context
*/
static void DoCrossDriveIntermediateMerge(PSolveContext pCtx)
{
    POthelloRingMasterState  pSt      = pCtx->pState;
    POthelloRingMasterConfig pCfg     = pCtx->pConfig;
    int                      level    = (int)pSt->playLevel;
    bool                     compress = (pCfg->compressMode == COMPRESS_ALL);
    const char*              lz4Drv   = pCfg->lz4Drives;

    EnterCriticalSection(&pSt->imergeCS);

    /* Re-check under the lock: counts may have dropped since the caller checked. */
    {
        int bk = 0, wh = 0;
        for (int i = 0; i < pSt->numMergeWriters; i++)
        {
            bk += pSt->mwBlackFileCount[i] - pSt->mwBlackFilesConsumed[i];
            wh += pSt->mwWhiteFileCount[i] - pSt->mwWhiteFilesConsumed[i];
        }
        /* writerDriveStats[i] is writer i's own drive (built 1:1 with
        ** mwDirectory[i] -- see InitSolver.cpp), so no search needed.
        */
        bool spaceOk = true;
        for (int i = 0; i < pSt->numMergeWriters && spaceOk; i++)
        {
            char dl = pSt->mwDirectory[i][0];
            if (DriveAvailable(pSt, dl) < (int64_t)pSt->writerDriveStats[i].threshold)
                spaceOk = false;
        }
        if (bk < MAX_MERGE_FANIN && wh < MAX_MERGE_FANIN && spaceOk)
        {
            LeaveCriticalSection(&pSt->imergeCS);
            return;
        }
    }

    /* Snapshot completed-file counts. Because counts are incremented AFTER
    ** RSFWriterClose, files [consumed..snap) are all fully written and
    ** closed -- safe to enumerate and delete.
    */
    int snapBlack[MAX_WRITERS] = {}, snapWhite[MAX_WRITERS] = {};
    for (int i = 0; i < pSt->numMergeWriters; i++)
    {
        snapBlack[i] = pSt->mwBlackFileCount[i];
        snapWhite[i] = pSt->mwWhiteFileCount[i];
    }

    /* Upper bound: MAX_MERGE_FANIN * numWriters writer files + imerge files on the medium drive. */
    const int kMaxFiles = MAX_MERGE_FANIN * MAX_WRITERS + 1024;

    for (int player = RSF_PLAYER_WHITE; player <= RSF_PLAYER_BLACK; player++)
    {
        pSt->imergeActive[player]          = 1;
        pSt->imergeTotalInputBytes[player] = 0;
        pSt->imergeDoneInputBytes[player]  = 0;
        pSt->imergeStartTickMs[player]     = GetTickCount64();
        if (pSt->terminateThreads) break;

        int* snapArr     = (player == RSF_PLAYER_BLACK) ? snapBlack               : snapWhite;
        int* consumedArr = (player == RSF_PLAYER_BLACK) ? pSt->mwBlackFilesConsumed
                                                        : pSt->mwWhiteFilesConsumed;

        /* Gather unconsumed writer files [consumed..snap) from each MW
        ** directory. Explicit index enumeration keeps us away from any file
        ** the other thread may currently be writing (its index >= snap by
        ** the after-close guarantee).
        */
        char**   paths = (char**)MemMalloc("xdimPaths", (size_t)kMaxFiles * sizeof(char*));
        int64_t* sizes = (int64_t*)MemMalloc("xdimSizes", (size_t)kMaxFiles * sizeof(int64_t));
        if (!paths || !sizes)
            Fatal(FATAL_ALLOCATION_FAILED, "DoCrossDriveIntermediateMerge: alloc");

        int     numFiles         = 0;
        int64_t totalBytes       = 0;
        int64_t totalUncompBytes = 0;

        for (int ti = 0; ti < pSt->numMergeWriters && numFiles < kMaxFiles; ti++)
        {
            const char* writerDir = pSt->mwDirectory[ti];
            char        writerDL  = writerDir[0];
            bool        writerLZ4 = compress && lz4Drv[0]
                                 && (strchr(lz4Drv, writerDL) != nullptr);

            for (int idx = consumedArr[ti]; idx < snapArr[ti] && numFiles < kMaxFiles; idx++)
            {
                char path[MAX_FULL_PATH_NAME];
                WIN32_FILE_ATTRIBUTE_DATA fad = {};
                bool found = false;

                /* Try expected format first, then fall back for mixed-mode / transition runs. */
                if (writerLZ4 && !found) {
                    RSFZLNameWriterFile(path, sizeof(path), writerDir, player, idx);
                    found = GetFileAttributesExA(path, GetFileExInfoStandard, &fad) != 0;
                }
                if (compress && !found) {
                    RSFZNameWriterFile(path, sizeof(path), writerDir, player, idx);
                    found = GetFileAttributesExA(path, GetFileExInfoStandard, &fad) != 0;
                }
                if (!found) {
                    RSFNameWriterFile(path, sizeof(path), writerDir, player, idx);
                    found = GetFileAttributesExA(path, GetFileExInfoStandard, &fad) != 0;
                }
                if (!found)
                    continue;   /* empty flush was deleted -- skip this index */

                int64_t sz = ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;
                paths[numFiles] = (char*)MemMalloc("xdimPath", strlen(path) + 1);
                if (!paths[numFiles])
                    Fatal(FATAL_ALLOCATION_FAILED, "DoCrossDriveIntermediateMerge: path alloc");
                strcpy(paths[numFiles], path);
                sizes[numFiles] = sz;
                totalBytes     += sz;
                totalUncompBytes += PeekRecordCount(path) * (int64_t)sizeof(UINT64_PAIR);
                numFiles++;
            }
        }

        if (numFiles == 0)
        {
            MemFree(paths);
            MemFree(sizes);
            for (int ti = 0; ti < pSt->numMergeWriters; ti++)
                consumedArr[ti] = snapArr[ti];
            continue;
        }

        pSt->imergeTotalInputBytes[player] += totalUncompBytes;

        /* Try to reserve space on the first merge drive (fastest medium drive). */
        int  destDirIdx    = -1;
        bool useTotalFlush = false;

        for (int d = 0; d < pSt->numMergeDirs; d++)
        {
            if (DriveReserve(pSt, pSt->mergeDirectory[d][0], totalBytes))
            {
                destDirIdx = d;
                break;
            }
        }
        if (destDirIdx < 0)
            useTotalFlush = true;

        if (useTotalFlush)
        {
            /* The medium drive is full. Pull in all existing medium-drive
            ** imerge files for this level+player so the combined merge
            ** catches every possible cross-drive duplicate, then flush
            ** everything to the store drive to clear all fast drives at once.
            */
            LoggerLog("DoCrossDriveIntermediateMerge: %s medium drive full -- total flush to %c:\n",
                      RSFPlayerStr(player), pCtx->pConfig->storeDrive);

            for (int d = 0; d < pSt->numMergeDirs && numFiles < kMaxFiles; d++)
            {
                char        pat[MAX_FULL_PATH_NAME];
                uint64_t    iBytes = 0;
                char**      tmp    = (char**)MemMalloc("xdimTmp",
                                        (size_t)(kMaxFiles - numFiles) * sizeof(char*));
                uint64_t*   tmpSz  = (uint64_t*)MemMalloc("xdimTmpSz",
                                        (size_t)(kMaxFiles - numFiles) * sizeof(uint64_t));
                if (!tmp || !tmpSz)
                    Fatal(FATAL_ALLOCATION_FAILED, "DoCrossDriveIntermediateMerge: imerge enum");

                char mDL   = pSt->mergeDirectory[d][0];
                bool mLZ4  = compress && lz4Drv[0] && (strchr(lz4Drv, mDL) != nullptr);
                int  extra = 0;
                int  room  = kMaxFiles - numFiles;

                if (mLZ4 && extra < room) {
                    uint64_t ib = 0;
                    RSFZLPatternImergeFiles(pat, sizeof(pat), pSt->mergeDirectory[d], level, player);
                    extra += EnumerateByPattern(pat, tmp + extra, room - extra, &ib, tmpSz + extra);
                    iBytes += ib;
                }
                if (compress && extra < room) {
                    uint64_t ib = 0;
                    RSFZPatternImergeFiles(pat, sizeof(pat), pSt->mergeDirectory[d], level, player);
                    extra += EnumerateByPattern(pat, tmp + extra, room - extra, &ib, tmpSz + extra);
                    iBytes += ib;
                }
                if (extra < room) {
                    uint64_t ib = 0;
                    RSFPatternImergeFiles(pat, sizeof(pat), pSt->mergeDirectory[d], level, player);
                    extra += EnumerateByPattern(pat, tmp + extra, room - extra, &ib, tmpSz + extra);
                    iBytes += ib;
                }

                for (int k = 0; k < extra && numFiles < kMaxFiles; k++)
                {
                    paths[numFiles] = tmp[k];               /* transfer ownership */
                    sizes[numFiles] = (int64_t)tmpSz[k];
                    totalBytes     += (int64_t)tmpSz[k];
                    pSt->imergeTotalInputBytes[0] += PeekRecordCount(tmp[k]) * (int64_t)sizeof(UINT64_PAIR);
                    numFiles++;
                }
                MemFree(tmp);    /* free array; elements now owned by paths[] */
                MemFree(tmpSz);
            }

            /* Reserve store-drive worst-case (pre-dedup) */
            if (!DriveReserve(pSt, pCtx->pConfig->storeDrive, totalBytes))
                Fatal(FATAL_DRIVE_SPACE,
                      "DoCrossDriveIntermediateMerge: total flush %s needs %.2f GB on %c:",
                      RSFPlayerStr(player),
                      totalBytes / (1024.0 * 1024.0 * 1024.0),
                      pCtx->pConfig->storeDrive);

            volatile LONG* pCount = (player == RSF_PLAYER_BLACK)
                ? (volatile LONG*)&pSt->storeMergeBlackFileCount
                : (volatile LONG*)&pSt->storeMergeWhiteFileCount;
            int fileIdx = (int)InterlockedExchangeAdd(pCount, 1);

            char outPath[MAX_FULL_PATH_NAME];
            {
                char   sDL  = pSt->storeMergeDirectory[0];
                bool   sLZ4 = compress && lz4Drv[0] && (strchr(lz4Drv, sDL) != nullptr);
                if (sLZ4)
                    RSFZLNameImergeFile(outPath, sizeof(outPath), pSt->storeMergeDirectory,
                                        level, player, fileIdx);
                else if (compress)
                    RSFZNameImergeFile(outPath, sizeof(outPath), pSt->storeMergeDirectory,
                                       level, player, fileIdx);
                else
                    RSFNameImergeFile(outPath, sizeof(outPath), pSt->storeMergeDirectory,
                                      level, player, fileIdx);
            }

            LoggerLog("DoCrossDriveIntermediateMerge: total flush %s -> '%s' (%d files, %.2f GB)\n",
                      RSFPlayerStr(player), outPath, numFiles,
                      totalBytes / (1024.0 * 1024.0 * 1024.0));

            uint64_t unique = KWayMergeFiles(paths, numFiles, outPath,
                                              &pSt->imergeDoneInputBytes[player], compress, &pSt->terminateThreads);

            /* Reclaim store-drive overestimate */
            int64_t actual = 0;
            if (compress)
            {
                WIN32_FILE_ATTRIBUTE_DATA fad = {};
                if (GetFileAttributesExA(outPath, GetFileExInfoStandard, &fad))
                    actual = ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;
            }
            else
            {
                actual = (int64_t)(unique * sizeof(UINT64_PAIR) + sizeof(RSFTrailer));
            }
            DriveReclaim(pSt, pCtx->pConfig->storeDrive, totalBytes - actual);

            {
                int64_t uncompStore = (int64_t)(unique * sizeof(UINT64_PAIR)
                                  + (unique > 0 ? sizeof(RSFTrailer) : 0));
                InterlockedAdd64((volatile LONG64*)&pSt->storeMergeBytesWritten, actual);
                InterlockedAdd64((volatile LONG64*)&pSt->storeMergeBytesUncompressed, uncompStore);
            }

            /* Delete all inputs and reclaim their drive space */
            for (int fi = 0; fi < numFiles; fi++)
            {
                DriveReclaim(pSt, paths[fi][0], sizes[fi]);
                DeleteFileA(paths[fi]);
                MemFree(paths[fi]);
            }

            /* Clear medium-drive imerge counters for this player -- all those files were consumed */
            for (int d = 0; d < pSt->numMergeDirs; d++)
            {
                if (player == RSF_PLAYER_BLACK) {
                    pSt->mergeFileBlackCount[d]  = 0;
                    pSt->mergeFileBytesBlack[d]  = 0;
                    pSt->mergeFileUncompBlack[d] = 0;
                } else {
                    pSt->mergeFileWhiteCount[d]  = 0;
                    pSt->mergeFileBytesWhite[d]  = 0;
                    pSt->mergeFileUncompWhite[d] = 0;
                }
            }

            LoggerLog("DoCrossDriveIntermediateMerge: total flush %s done (%llu unique)\n",
                      RSFPlayerStr(player), unique);
        }
        else
        {
            /* Normal path: merge writer files from the fast NVMe drives -> single imerge on the medium drive. */
            volatile LONG* pCount = (player == RSF_PLAYER_BLACK)
                ? (volatile LONG*)&pSt->mergeFileBlackCount[destDirIdx]
                : (volatile LONG*)&pSt->mergeFileWhiteCount[destDirIdx];
            int fileIdx = (int)InterlockedExchangeAdd(pCount, 1);

            char outPath[MAX_FULL_PATH_NAME];
            {
                char   iDL  = pSt->mergeDirectory[destDirIdx][0];
                bool   iLZ4 = compress && lz4Drv[0] && (strchr(lz4Drv, iDL) != nullptr);
                if (iLZ4)
                    RSFZLNameImergeFile(outPath, sizeof(outPath), pSt->mergeDirectory[destDirIdx],
                                        level, player, fileIdx);
                else if (compress)
                    RSFZNameImergeFile(outPath, sizeof(outPath), pSt->mergeDirectory[destDirIdx],
                                       level, player, fileIdx);
                else
                    RSFNameImergeFile(outPath, sizeof(outPath), pSt->mergeDirectory[destDirIdx],
                                      level, player, fileIdx);
            }

            LoggerLog("DoCrossDriveIntermediateMerge: %s -> '%s' (%d files, %.2f GB)\n",
                      RSFPlayerStr(player), outPath, numFiles,
                      totalBytes / (1024.0 * 1024.0 * 1024.0));

            uint64_t unique = KWayMergeFiles(paths, numFiles, outPath,
                                              &pSt->imergeDoneInputBytes[player], compress, &pSt->terminateThreads);

            int64_t actual = 0;
            if (compress)
            {
                WIN32_FILE_ATTRIBUTE_DATA fad = {};
                if (GetFileAttributesExA(outPath, GetFileExInfoStandard, &fad))
                    actual = ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;
            }
            else
            {
                actual = (int64_t)(unique * sizeof(UINT64_PAIR) + sizeof(RSFTrailer));
            }
            DriveReclaim(pSt, pSt->mergeDirectory[destDirIdx][0], totalBytes - actual);

            {
                int64_t uncompMedium = (int64_t)(unique * sizeof(UINT64_PAIR)
                                  + (unique > 0 ? sizeof(RSFTrailer) : 0));
                if (player == RSF_PLAYER_BLACK) {
                    InterlockedAdd64((volatile LONG64*)&pSt->mergeFileBytesBlack[destDirIdx], actual);
                    InterlockedAdd64((volatile LONG64*)&pSt->mergeFileUncompBlack[destDirIdx], uncompMedium);
                } else {
                    InterlockedAdd64((volatile LONG64*)&pSt->mergeFileBytesWhite[destDirIdx], actual);
                    InterlockedAdd64((volatile LONG64*)&pSt->mergeFileUncompWhite[destDirIdx], uncompMedium);
                }
            }

            for (int fi = 0; fi < numFiles; fi++)
            {
                DriveReclaim(pSt, paths[fi][0], sizes[fi]);
                DeleteFileA(paths[fi]);
                MemFree(paths[fi]);
            }

            LoggerLog("DoCrossDriveIntermediateMerge: %s done (%llu unique)\n",
                      RSFPlayerStr(player), unique);
        }

        /* Advance consumed pointers past the files we just merged */
        for (int ti = 0; ti < pSt->numMergeWriters; ti++)
            consumedArr[ti] = snapArr[ti];

        MemFree(paths);
        MemFree(sizes);
    }

    for (int p = RSF_PLAYER_WHITE; p <= RSF_PLAYER_BLACK; p++)
    {
        pSt->imergeActive[p]          = 0;
        pSt->imergeTotalInputBytes[p] = 0;
    }

    LeaveCriticalSection(&pSt->imergeCS);
}

/*
** Function: CollectPoolReadersForPlayer
** @brief    Gathers a player's leftover in-memory pool data across all
**           merge-writer threads as already-open RSFReaders, ready to merge
**           directly alongside on-disk files -- no NVMe round-trip needed.
** @details  Compressed segments are opened in place (no copy); any live
**           uncompressed staging is compressed into a small heap buffer
**           first (in-memory only, no disk I/O) so it can join the same
**           reader-based merge. Callers must RSFClose each returned reader
**           after use (KWayMergeFiles/CascadingMerge already do this for
**           readers passed as extraReaders) and MemFree each buffer in
**           outTempBufs once the merge has consumed it -- RSFClose never
**           frees memory-mode buffers since compressed *segment* data
**           points directly into the writer's persistent pMWBuffer, which
**           must survive it.
** @param    pSt             - solver state
** @param    player          - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @param    outReaders      - out: opened readers, appended to
** @param    outTempBufs     - out: staging-compression buffers to free after the merge, appended to
** @param    outInputBoards  - out: running total of boards represented by outReaders
** @param    outCompBytes    - out: running total of compressed bytes across outReaders
** @param    outMaxSeg       - out: running max segment count seen on any one thread (diagnostic)
*/
static void CollectPoolReadersForPlayer(POthelloRingMasterState pSt, int player,
                                         std::vector<RSFReader*>& outReaders,
                                         std::vector<uint8_t*>& outTempBufs,
                                         uint64_t& outInputBoards,
                                         uint64_t& outCompBytes,
                                         int& outMaxSeg)
{
    for (int i = 0; i < pSt->numMergeWriters; i++)
    {
        uint8_t* mwBuf = (uint8_t*)pSt->pMWBuffer[i];

        int           segCount;
        const size_t* segOffsets;
        const size_t* segSizes;
        const int*    segBoardCounts;
        const UINT64_PAIR* stagingBegin;
        int           stagingCount;

        if (player == RSF_PLAYER_BLACK)
        {
            segCount       = pSt->mwBlackSegCount[i];
            segOffsets     = pSt->mwBlackSegOffset[i];
            segSizes       = pSt->mwBlackSegSize[i];
            segBoardCounts = pSt->mwBlackSegBoardCount[i];
            stagingBegin   = (const UINT64_PAIR*)mwBuf;
            stagingCount   = pSt->mwBlackStagingCount[i];
        }
        else
        {
            segCount       = pSt->mwWhiteSegCount[i];
            segOffsets     = pSt->mwWhiteSegOffset[i];
            segSizes       = pSt->mwWhiteSegSize[i];
            segBoardCounts = pSt->mwWhiteSegBoardCount[i];
            stagingBegin   = (const UINT64_PAIR*)(mwBuf + pSt->mwBufferSize - pSt->mwStagingSize);
            stagingCount   = pSt->mwWhiteStagingCount[i];
        }

        if (segCount > outMaxSeg) outMaxSeg = segCount;

        for (int s = 0; s < segCount; s++)
        {
            outReaders.push_back(RSFReaderOpenZMem(mwBuf + segOffsets[s], segSizes[s],
                                                    (uint64_t)segBoardCounts[s]));
            outInputBoards += (uint64_t)segBoardCounts[s];
            outCompBytes   += (uint64_t)segSizes[s];
        }

        if (stagingCount > 0)
        {
            size_t worst = (size_t)stagingCount * 20 + sizeof(RSFTrailer) + 100;
            uint8_t* tmp = (uint8_t*)MemMalloc("eolStagingComp", worst);
            if (!tmp)
                Fatal(FATAL_ALLOCATION_FAILED,
                      "CollectPoolReadersForPlayer: cannot allocate %zu-byte staging buffer", worst);
            uint64_t compBytes = 0;
            RSFWriter* pw = RSFWriterOpenZMem(tmp, worst);
            for (int r = 0; r < stagingCount; r++)
                RSFWriterRecord(pw, &stagingBegin[r]);
            RSFWriterClose(pw, &compBytes);

            outReaders.push_back(RSFReaderOpenZMem(tmp, compBytes, (uint64_t)stagingCount));
            outTempBufs.push_back(tmp);
            outInputBoards += (uint64_t)stagingCount;
            outCompBytes   += compBytes;
        }
    }
}

/*
** Function: ConvertLevelOutputToNestedIndex
** @brief    Converts one player's flat, sorted+deduped RSF store output into
**           the 4-file ring nested-index format (CellsInUse/Ring_1/Ring_2/
**           Ring_3_4 -- see OthelloBasics/RingNestedIndex.h), deleting the
**           flat intermediate once the nested files are fully written.
** @details  This is the actual point of the ring-ordered storage scheme --
**           the flat RSF file alone only carries the same delta+varint+LZ4
**           compression an earlier implementation already had; the nested
**           index is what realizes the additional validated savings (see
**           project_ring_split_validated_findings memory). Streams directly
**           from the flat reader into the nested-index writers (CellsInUse
**           via RSFWriterOpenZ, Ring_1/Ring_2/Ring_3_4 via
**           Lz4StreamWriterOpen) -- no raw intermediate ever touches disk,
**           and nothing beyond one flat-store-sized read pass is ever held
**           at once, matching the flat store's own streaming discipline
**           rather than requiring a whole level to fit in memory. If
**           interrupted before the flat file is deleted (the last step),
**           the flat file is still intact and the existing resume-scan
**           recovery (re-solve the level from scratch) covers it exactly
**           like any other interrupted merge -- no new failure mode.
** @param    pCtx            - solve context
** @param    level           - the level this output belongs to (the level being produced)
** @param    player          - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @param    flatPath        - path to the flat merged output to convert
** @param    flatActualBytes - bytes on disk for flatPath (already known by the caller)
** @return   Total bytes on disk across the 4 new nested-index files, or 0 if flatPath didn't exist.
*/
static int64_t ConvertLevelOutputToNestedIndex(PSolveContext pCtx, int level, int player,
                                                const char* flatPath, int64_t flatActualBytes)
{
    RSFReader* flatReader = RSFOpen(flatPath);
    if (!flatReader) return 0;

    POthelloRingMasterState  pSt       = pCtx->pState;
    POthelloRingMasterConfig pCfg      = pCtx->pConfig;
    int                      boardSize = (int)pCfg->boardSize;

    char cellsInUsePath[MAX_FULL_PATH_NAME];
    char ring1Path[MAX_FULL_PATH_NAME];
    char ring2Path[MAX_FULL_PATH_NAME];
    char ring34Path[MAX_FULL_PATH_NAME];
    RSFNameCellsInUseFile(cellsInUsePath, sizeof(cellsInUsePath), pSt->storeDirectory, boardSize, level, player, 0);
    RSFNameRing1File(ring1Path,           sizeof(ring1Path),      pSt->storeDirectory, boardSize, level, player, 0);
    RSFNameRing2File(ring2Path,           sizeof(ring2Path),      pSt->storeDirectory, boardSize, level, player, 0);
    RSFNameRing34File(ring34Path,         sizeof(ring34Path),     pSt->storeDirectory, boardSize, level, player, 0);

    RSFWriter*       pCellsInUseWriter = RSFWriterOpenZL(cellsInUsePath);
    Lz4StreamWriter* pRing1Writer      = Lz4StreamWriterOpen(ring1Path);
    Lz4StreamWriter* pRing2Writer      = Lz4StreamWriterOpen(ring2Path);
    Lz4StreamWriter* pRing34Writer     = Lz4StreamWriterOpen(ring34Path);

    RingNestedIndexBuilder builder;
    builder.Init(pCellsInUseWriter, pRing1Writer, pRing2Writer, pRing34Writer);

    UINT64_PAIR rec;
    while (RSFRead(flatReader, &rec, 1) == 1)
    {
        BOARD_KEY key;
        key.ullCellsInUse = rec.hi;
        key.ullCellColors = rec.lo;
        builder.Process(key);
    }
    builder.Finish();
    RSFClose(&flatReader);

    RSFWriterClose(pCellsInUseWriter);
    Lz4StreamWriterClose(pRing1Writer);
    Lz4StreamWriterClose(pRing2Writer);
    Lz4StreamWriterClose(pRing34Writer);

    int64_t nestedBytes = 0;
    {
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        const char* parts[4] = { cellsInUsePath, ring1Path, ring2Path, ring34Path };
        for (int i = 0; i < 4; i++)
            if (GetFileAttributesExA(parts[i], GetFileExInfoStandard, &fad))
                nestedBytes += ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;
    }

    /* Only delete the flat intermediate once every nested-index file is
    ** fully written and closed -- see @details above for why this ordering matters.
    */
    DeleteFileA(flatPath);

    /* flatActualBytes were already charged against the store-drive
    ** reservation by the caller; give those back (the flat file is gone)
    ** and charge for the nested files that replaced it.
    */
    DriveReclaim(pSt, pCfg->storeDrive, flatActualBytes - nestedBytes);

    LoggerLog("ConvertLevelOutputToNestedIndex: level %d %s -> nested index (%.2f GB, was %.2f GB flat)\n",
              level, RSFPlayerStr(player),
              nestedBytes     / (1024.0 * 1024.0 * 1024.0),
              flatActualBytes / (1024.0 * 1024.0 * 1024.0));

    return nestedBytes;
}

/*
** ============================================================
** DoEndOfLevelMerge
**
** Runs two independent cascading merges: one for black-turn files, one for
** white-turn files. Each produces a single sorted store file for level+1,
** then converts that store file into the ring nested-index format.
** ============================================================
*/

/*
** Type:    PlayerData
** @brief   Per-player working state for one DoEndOfLevelMerge call: the
**          enumerated on-disk input files, any leftover in-memory pool
**          data, and the reservation/result bookkeeping needed to finalize
**          drive-ledger accounting once the merge completes.
*/
struct PlayerData
{
    char**    inputPaths;
    uint64_t* inputSizes;         /* per-file sizes for ledger reclaim */
    int       numFiles;
    uint64_t  inputBytes;
    uint64_t  unique;
    int64_t   storeReservation;   /* bytes pre-reserved on the store drive for final output */
    int64_t   actualBytes;        /* actual bytes written to the output file */
    int64_t   yInputBytes;        /* store-drive storeMerge input bytes pre-reclaimed before Phase 1b */
    bool      yInputsPreReclaimed; /* true when yInputBytes were reclaimed early */

    /* Leftover in-memory pool data merged directly alongside inputPaths above. */
    std::vector<RSFReader*> poolReaders;
    std::vector<uint8_t*>   poolTempBufs;   /* staging-compression buffers to free after merge */
    uint64_t                poolInputBoards = 0;
    uint64_t                poolCompBytes   = 0;
};

/*
** Function: DoEndOfLevelMerge
** @brief    Consolidates every remaining writer file (NVMe) and
**           intermediate merge file (medium drives) into a single sorted,
**           deduped store file per player on the store drive.
** @param    pCtx - solve context
*/
void DoEndOfLevelMerge(PSolveContext pCtx)
{
    POthelloRingMasterState  pSt            = pCtx->pState;
    POthelloRingMasterConfig pCfg           = pCtx->pConfig;
    int                      level          = (int)pSt->playLevel;
    int                      boardSize      = (int)pCfg->boardSize;
    bool                     compressOutput = (pCfg->compressMode != COMPRESS_NONE);

    /* Clear stale merge-progress fields from the previous level right away.
    ** The main loop flips currentPhase to "Merging to store" before calling
    ** here, so the status display starts reading these fields immediately --
    ** leaving the previous level's completed (100%) values in place made an
    ** in-progress leftover NVMe flush (below) look like an already-finished
    ** store-drive merge.
    */
    pSt->mergeTotalInputBytes[0]        = pSt->mergeTotalInputBytes[1]        = 0;
    pSt->mergeProgressBytes[0]          = pSt->mergeProgressBytes[1]          = 0;
    pSt->mergeEndTickMs[0]              = pSt->mergeEndTickMs[1]              = 0;
    pSt->mergeInputFileCount[0]         = pSt->mergeInputFileCount[1]         = 0;
    pSt->mergeInputPoolReaderCount[0]   = pSt->mergeInputPoolReaderCount[1]   = 0;

    /* Any leftover in-memory pool data merges directly alongside on-disk
    ** files below (see CollectPoolReadersForPlayer) -- no NVMe round-trip
    ** needed. The one exception: CascadingMerge's grouped/temp-file mode
    ** (needed only when a color's on-disk file count exceeds
    ** MAX_MERGE_FANIN) bounds the number of simultaneously-open OS file
    ** handles, a constraint that doesn't apply to in-memory readers, so it
    ** can't accept pool data directly. That's a scale this project hasn't
    ** reached yet (imerge/Option C keep file counts low), but as a
    ** conservative guard: if either color's on-disk *writer* file count
    ** already exceeds the fan-in limit (a lower bound on the eventual
    ** enumerated count, since intermediate merges only ever consolidate
    ** writer files down), flush all leftover pool data to NVMe up front
    ** exactly as before, so it becomes ordinary files for the grouped path
    ** to consume.
    */
    {
        int totalBlackFiles = 0, totalWhiteFiles = 0;
        for (int i = 0; i < pSt->numMergeWriters; i++)
        {
            totalBlackFiles += pSt->mwBlackFileCount[i];
            totalWhiteFiles += pSt->mwWhiteFileCount[i];
        }
        if (totalBlackFiles > MAX_MERGE_FANIN || totalWhiteFiles > MAX_MERGE_FANIN)
        {
            std::vector<std::thread> flushThreads;
            for (int i = 0; i < pSt->numMergeWriters; i++)
            {
                if (pSt->mwBlackSegCount[i] > 0 || pSt->mwBlackStagingCount[i] > 0 ||
                    pSt->mwWhiteSegCount[i] > 0 || pSt->mwWhiteStagingCount[i] > 0)
                    flushThreads.emplace_back([i, pCtx] { FlushMergeWriterBuffer(i, pCtx); });
            }
            for (std::thread& t : flushThreads) t.join();
        }
    }

    const int kMaxInputFiles = MAX_MERGE_FANIN * MAX_MERGE_FANIN;

    /* -- Phase 1: enumerate files for both players (sequential, fast) --
    ** We scan first so mergeTotalInputBytes is known before the merge
    ** starts, giving the stats listener an accurate denominator from the
    ** beginning.
    */
    PlayerData data[2] = {};   /* indexed by RSF_PLAYER_WHITE(0) / RSF_PLAYER_BLACK(1) */

    for (int player = RSF_PLAYER_WHITE; player <= RSF_PLAYER_BLACK; player++)
    {
        data[player].inputPaths = (char**)MemMalloc("eolInputPaths",
                                                     (size_t)kMaxInputFiles * sizeof(char*));
        data[player].inputSizes = (uint64_t*)MemMalloc("eolInputSizes",
                                                         (size_t)kMaxInputFiles * sizeof(uint64_t));
        if (!data[player].inputPaths || !data[player].inputSizes)
            Fatal(FATAL_ALLOCATION_FAILED, "DoEndOfLevelMerge: cannot allocate path/size arrays");

        int      numFiles    = 0;
        uint64_t playerBytes = 0;
        char     pat[MAX_FULL_PATH_NAME];

        for (int i = 0; i < pSt->numMergeWriters && numFiles < kMaxInputFiles; i++)
        {
            uint64_t d = 0;
            RSFPatternWriterFiles(pat, sizeof(pat), pSt->mwDirectory[i], player);
            numFiles += EnumerateByPattern(pat, data[player].inputPaths + numFiles,
                                           kMaxInputFiles - numFiles, &d,
                                           data[player].inputSizes + numFiles);
            playerBytes += d;
            if (pCfg->compressMode == COMPRESS_ALL && numFiles < kMaxInputFiles)
            {
                d = 0;
                RSFZPatternWriterFiles(pat, sizeof(pat), pSt->mwDirectory[i], player);
                numFiles += EnumerateByPattern(pat, data[player].inputPaths + numFiles,
                                               kMaxInputFiles - numFiles, &d,
                                               data[player].inputSizes + numFiles);
                playerBytes += d;
                if (numFiles < kMaxInputFiles && pCfg->lz4Drives[0])
                {
                    d = 0;
                    RSFZLPatternWriterFiles(pat, sizeof(pat), pSt->mwDirectory[i], player);
                    numFiles += EnumerateByPattern(pat, data[player].inputPaths + numFiles,
                                                   kMaxInputFiles - numFiles, &d,
                                                   data[player].inputSizes + numFiles);
                    playerBytes += d;
                }
            }
        }
        for (int i = 0; i < pSt->numMergeDirs && numFiles < kMaxInputFiles; i++)
        {
            uint64_t d = 0;
            RSFPatternImergeFiles(pat, sizeof(pat), pSt->mergeDirectory[i], level, player);
            numFiles += EnumerateByPattern(pat, data[player].inputPaths + numFiles,
                                           kMaxInputFiles - numFiles, &d,
                                           data[player].inputSizes + numFiles);
            playerBytes += d;
            if (pCfg->compressMode == COMPRESS_ALL && numFiles < kMaxInputFiles)
            {
                d = 0;
                RSFZPatternImergeFiles(pat, sizeof(pat), pSt->mergeDirectory[i], level, player);
                numFiles += EnumerateByPattern(pat, data[player].inputPaths + numFiles,
                                               kMaxInputFiles - numFiles, &d,
                                               data[player].inputSizes + numFiles);
                playerBytes += d;
                if (numFiles < kMaxInputFiles && pCfg->lz4Drives[0])
                {
                    d = 0;
                    RSFZLPatternImergeFiles(pat, sizeof(pat), pSt->mergeDirectory[i], level, player);
                    numFiles += EnumerateByPattern(pat, data[player].inputPaths + numFiles,
                                                   kMaxInputFiles - numFiles, &d,
                                                   data[player].inputSizes + numFiles);
                    playerBytes += d;
                }
            }
        }
        if (numFiles < kMaxInputFiles)
        {
            uint64_t d = 0;
            RSFPatternImergeFiles(pat, sizeof(pat), pSt->storeMergeDirectory, level, player);
            numFiles += EnumerateByPattern(pat, data[player].inputPaths + numFiles,
                                           kMaxInputFiles - numFiles, &d,
                                           data[player].inputSizes + numFiles);
            playerBytes += d;
            if (pCfg->compressMode == COMPRESS_ALL && numFiles < kMaxInputFiles)
            {
                d = 0;
                RSFZPatternImergeFiles(pat, sizeof(pat), pSt->storeMergeDirectory, level, player);
                numFiles += EnumerateByPattern(pat, data[player].inputPaths + numFiles,
                                               kMaxInputFiles - numFiles, &d,
                                               data[player].inputSizes + numFiles);
                playerBytes += d;
                if (numFiles < kMaxInputFiles && pCfg->lz4Drives[0])
                {
                    d = 0;
                    RSFZLPatternImergeFiles(pat, sizeof(pat), pSt->storeMergeDirectory, level, player);
                    numFiles += EnumerateByPattern(pat, data[player].inputPaths + numFiles,
                                                   kMaxInputFiles - numFiles, &d,
                                                   data[player].inputSizes + numFiles);
                    playerBytes += d;
                }
            }
        }

        data[player].numFiles   = numFiles;
        data[player].inputBytes = playerBytes;
    }

    /* Gather each player's leftover in-memory pool data (if any) so it
    ** merges directly alongside the on-disk files enumerated above -- see
    ** CollectPoolReadersForPlayer. Skipped when the pre-check above already
    ** flushed everything to NVMe (pool is empty in that case anyway).
    */
    int maxSegSeen = 0;
    for (int player = RSF_PLAYER_WHITE; player <= RSF_PLAYER_BLACK; player++)
    {
        CollectPoolReadersForPlayer(pSt, player, data[player].poolReaders,
                                     data[player].poolTempBufs,
                                     data[player].poolInputBoards,
                                     data[player].poolCompBytes,
                                     maxSegSeen);
        if (data[player].poolInputBoards > 0)
            InterlockedAdd64((volatile LONG64*)&pSt->levelStats[level].boardsWrittenToDisk,
                             (LONG64)data[player].poolInputBoards);

        pSt->mergeInputFileCount[player]       = data[player].numFiles;
        pSt->mergeInputPoolReaderCount[player] = (int)data[player].poolReaders.size();
    }
    if (maxSegSeen > MAX_MW_SEGS * 3 / 4)
        LoggerLog("WARNING level %d: peak MW seg count %d/%d -- consider increasing MAX_MW_SEGS\n",
                  level, maxSegSeen, MAX_MW_SEGS);

    /* Lifetime high-water (tracked continuously in RunMergeWriterJob, so
    ** this catches the true peak reached mid-level, not just whatever's
    ** left in the pool when the level happens to end).
    */
    int lifetimeMaxSeg = 0;
    for (int ti = 0; ti < pSt->numMergeWriters; ti++)
    {
        if (pSt->mwBlackSegCountHighWater[ti] > lifetimeMaxSeg)
            lifetimeMaxSeg = pSt->mwBlackSegCountHighWater[ti];
        if (pSt->mwWhiteSegCountHighWater[ti] > lifetimeMaxSeg)
            lifetimeMaxSeg = pSt->mwWhiteSegCountHighWater[ti];
    }
    LoggerLog("Level %d: lifetime peak MW seg count so far %d/%d\n",
              level, lifetimeMaxSeg, MAX_MW_SEGS);

    /* Diagnostic for the "white merge always slower" investigation: log the
    ** exact fan-in (files + pool readers) and byte volume feeding each
    ** color's merge heap, since more sources means more per-record heap
    ** overhead in KWayMergeFiles independent of total byte volume, and this
    ** isn't visible anywhere else today.
    */
    LoggerLog("DoEndOfLevelMerge: level %d merge-input breakdown -- "
              "white: %d files (%.2f GB) + %d pool readers (%.2f GB, %llu boards)  |  "
              "black: %d files (%.2f GB) + %d pool readers (%.2f GB, %llu boards)\n",
              level,
              data[RSF_PLAYER_WHITE].numFiles,
              data[RSF_PLAYER_WHITE].inputBytes / (1024.0 * 1024.0 * 1024.0),
              (int)data[RSF_PLAYER_WHITE].poolReaders.size(),
              data[RSF_PLAYER_WHITE].poolCompBytes / (1024.0 * 1024.0 * 1024.0),
              (unsigned long long)data[RSF_PLAYER_WHITE].poolInputBoards,
              data[RSF_PLAYER_BLACK].numFiles,
              data[RSF_PLAYER_BLACK].inputBytes / (1024.0 * 1024.0 * 1024.0),
              (int)data[RSF_PLAYER_BLACK].poolReaders.size(),
              data[RSF_PLAYER_BLACK].poolCompBytes / (1024.0 * 1024.0 * 1024.0),
              (unsigned long long)data[RSF_PLAYER_BLACK].poolInputBoards);

    /* Set mergeTotalInputBytes per player as uncompressed record bytes.
    ** mergeProgressBytes is incremented by sizeof(UINT64_PAIR) per record,
    ** so the denominator must be in the same units to give a valid percentage.
    */
    {
        for (int player = RSF_PLAYER_WHITE; player <= RSF_PLAYER_BLACK; player++)
        {
            uint64_t playerRecordBytes = 0;
            for (int i = 0; i < data[player].numFiles; i++)
            {
                RSFReader* r = RSFOpen(data[player].inputPaths[i]);
                if (r) { playerRecordBytes += RSFReaderTrailer(r)->recordCount * sizeof(UINT64_PAIR); RSFClose(&r); }
            }
            playerRecordBytes += data[player].poolInputBoards * sizeof(UINT64_PAIR);
            pSt->mergeTotalInputBytes[player] = playerRecordBytes;
        }
    }

    /* -- Phase 1b: pre-reserve store-drive space for both players --
    ** Cascade temp space is NOT reserved upfront -- it is claimed per-group
    ** inside CascadingMerge (filling the fastest medium drive first,
    ** spilling to the store drive only as needed). All reservations are
    ** atomic so both threads can run safely in parallel.
    **
    ** Pre-reclaim store-drive storeMerge inputs before reserving outputs.
    ** Both players' storeMerge files live on the store drive and are
    ** already counted in the ledger. Reserving output space for both
    ** players simultaneously would double-count that space (input bytes +
    ** output bytes) and exhaust the ledger. Since the inputs will be
    ** deleted when the merge completes, we reclaim them now and skip the
    ** per-file DriveReclaim in the merge thread for these files.
    */
    for (int player = RSF_PLAYER_WHITE; player <= RSF_PLAYER_BLACK; player++)
    {
        PlayerData& pd = data[player];
        pd.yInputBytes         = 0;
        pd.yInputsPreReclaimed = false;
        for (int i = 0; i < pd.numFiles; i++)
        {
            if (pd.inputPaths[i][0] == pCfg->storeDrive)
                pd.yInputBytes += (int64_t)pd.inputSizes[i];
        }
        if (pd.yInputBytes > 0)
        {
            DriveReclaim(pSt, pCfg->storeDrive, pd.yInputBytes);
            pd.yInputsPreReclaimed = true;
        }
    }

    for (int player = RSF_PLAYER_WHITE; player <= RSF_PLAYER_BLACK; player++)
    {
        PlayerData& pd      = data[player];
        pd.storeReservation = 0;
        if (pd.numFiles == 0 && pd.poolReaders.empty()) continue;

        /* Reserve store-drive space for the final store output (worst case = full input size). */
        int64_t storeNeeded = (pd.numFiles == 1 && pd.poolReaders.empty())
                             ? (int64_t)pd.inputSizes[0]
                             : (int64_t)(pd.inputBytes + pd.poolCompBytes
                                         + sizeof(RSFTrailer) + 256);
        if (!DriveReserve(pSt, pCfg->storeDrive, storeNeeded))
            Fatal(FATAL_DRIVE_SPACE,
                  "EndOfLevelMerge: %s store output needs %.2f GB on %c: (%.2f GB available)",
                  RSFPlayerStr(player),
                  storeNeeded / (1024.0 * 1024.0 * 1024.0),
                  pCfg->storeDrive,
                  DriveAvailable(pSt, pCfg->storeDrive) / (1024.0 * 1024.0 * 1024.0));
        pd.storeReservation = storeNeeded;
    }

    /* Ordered list of candidate temp dirs: merge drives (medium speed) first
    ** -- they are faster for writes and reads, and don't consume store-drive
    ** bandwidth -- then storeMergeDirectory as last resort. CascadingMerge
    ** picks the first dir with ledger space per group, spreading temps
    ** across drives when one fills.
    */
    const char* tempDirs[MAX_WRITER_DRIVES + 1];
    int         numTempDirs = 0;
    for (int d = 0; d < pSt->numMergeDirs; d++)
        tempDirs[numTempDirs++] = pSt->mergeDirectory[d];
    tempDirs[numTempDirs++] = pSt->storeMergeDirectory;

    /* -- Phase 2: merge both players concurrently --
    ** Each thread reads its own input files and writes a separate store
    ** file. Progress is updated atomically so the stats display stays accurate.
    */
    auto mergePlayer = [&](int player)
    {
        pSt->mergeStartTickMs[player] = GetTickCount64();
        volatile int64_t* pProg = &pSt->mergeProgressBytes[player];
        PlayerData& pd = data[player];

        if (pd.numFiles == 0 && pd.poolReaders.empty())
        {
            LoggerLog("EndOfLevelMerge: level %d %s -- no files\n",
                      level, RSFPlayerStr(player));
            MemFree(pd.inputPaths);
            MemFree(pd.inputSizes);
            return;
        }

        char outPath[MAX_FULL_PATH_NAME];
        {
            bool storeLZ4 = compressOutput && pCfg->lz4Drives[0]
                         && (strchr(pCfg->lz4Drives, pCfg->storeDrive) != nullptr);
            if (storeLZ4)
                RSFZLNameStoreFile(outPath, sizeof(outPath), pSt->storeDirectory,
                                   boardSize, level + 1, player, 0);
            else if (compressOutput)
                RSFZNameStoreFile(outPath, sizeof(outPath), pSt->storeDirectory,
                                  boardSize, level + 1, player, 0);
            else
                RSFNameStoreFile(outPath, sizeof(outPath), pSt->storeDirectory,
                                 boardSize, level + 1, player, 0);
        }

        if (pd.numFiles == 1 && pd.poolReaders.empty() && !compressOutput)
        {
            RSFReader* r = RSFOpen(pd.inputPaths[0]);
            if (r) { pd.unique = RSFReaderTrailer(r)->recordCount; RSFClose(&r); }
            if (!MoveFileExA(pd.inputPaths[0], outPath, MOVEFILE_COPY_ALLOWED))
                Fatal(FATAL_FILE_OPEN,
                      "EndOfLevelMerge: cannot move '%s' -> '%s' (err %lu)",
                      pd.inputPaths[0], outPath, GetLastError());
            if (!pd.yInputsPreReclaimed)
                DriveReclaim(pSt, pd.inputPaths[0][0], (int64_t)pd.inputSizes[0]);
            int64_t actual = (int64_t)(pd.unique * sizeof(UINT64_PAIR)
                             + sizeof(RSFTrailer));
            DriveReclaim(pSt, pCfg->storeDrive, pd.storeReservation - actual);
            InterlockedAdd64((volatile LONG64*)pProg, (LONG64)(pd.unique * sizeof(UINT64_PAIR)));
        }
        else
        {
            /* Unified merge: on-disk files (0, 1, or many) plus any leftover
            ** in-memory pool data, all merged in a single pass -- see
            ** CascadingMerge/KWayMergeFiles. Cascade temps (grouped mode,
            ** only when pd.numFiles alone exceeds MAX_MERGE_FANIN) are
            ** placed greedily per-group (fastest drive first, store drive
            ** if needed). When compressOutput is on, also compress
            ** intermediate cascade temps.
            */
            int tempCount = 0;
            pd.unique = CascadingMerge(pd.inputPaths, pd.numFiles, tempDirs, numTempDirs,
                                        outPath, &tempCount, level, player, pProg, pCtx,
                                        compressOutput, compressOutput, nullptr, pd.poolReaders);
            for (uint8_t* buf : pd.poolTempBufs) MemFree(buf);
            pd.poolTempBufs.clear();

            /* Return store-drive overestimate; use actual file size for compressed output. */
            int64_t actual;
            if (compressOutput)
            {
                WIN32_FILE_ATTRIBUTE_DATA fad = {};
                actual = 0;
                if (GetFileAttributesExA(outPath, GetFileExInfoStandard, &fad))
                    actual = ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;
            }
            else
            {
                actual = (int64_t)(pd.unique * sizeof(UINT64_PAIR)
                         + (pd.unique > 0 ? sizeof(RSFTrailer) : 0));
            }
            DriveReclaim(pSt, pCfg->storeDrive, pd.storeReservation - actual);

            /* Reclaim source-file drive space as each input is deleted.
            ** Skip store-drive inputs that were pre-reclaimed in Phase 1b.
            */
            for (int i = 0; i < pd.numFiles; i++)
            {
                if (!pd.yInputsPreReclaimed || pd.inputPaths[i][0] != pCfg->storeDrive)
                    DriveReclaim(pSt, pd.inputPaths[i][0], (int64_t)pd.inputSizes[i]);
                DeleteFileA(pd.inputPaths[i]);
            }
        }

        {
            WIN32_FILE_ATTRIBUTE_DATA fad = {};
            if (GetFileAttributesExA(outPath, GetFileExInfoStandard, &fad))
                pd.actualBytes = ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;
        }
        if (pd.unique > 0)
            InterlockedIncrement((volatile LONG*)&pSt->levelStats[level].mergeFilesWritten);

        LoggerLog("EndOfLevelMerge: level %d %s -> '%s'  (%llu unique boards)\n",
                  level, RSFPlayerStr(player), outPath, pd.unique);

        for (int i = 0; i < pd.numFiles; i++)
            MemFree(pd.inputPaths[i]);
        MemFree(pd.inputPaths);
        MemFree(pd.inputSizes);

        /* Reset this color's pool/staging state for the next level. Safe to
        ** do now -- the merge above (and any RSFReaderOpenZMem segments it
        ** read from pMWBuffer) has already fully consumed and closed
        ** everything. No-op when this color's pool was already empty (e.g.
        ** it took the raw-move shortcut, or was pre-flushed to NVMe by the
        ** fan-in guard).
        */
        for (int i = 0; i < pSt->numMergeWriters; i++)
        {
            if (player == RSF_PLAYER_BLACK)
            {
                pSt->mwBlackSegCount[i]      = 0;
                pSt->mwBlackCompBytesUsed[i] = 0;
                pSt->mwBlackStagingCount[i]  = 0;
            }
            else
            {
                pSt->mwWhiteSegCount[i]      = 0;
                pSt->mwWhiteCompBytesUsed[i] = 0;
                pSt->mwWhiteStagingCount[i]  = 0;
            }
        }
    };

    /* Write "merging" sentinel before launching threads. If the process is
    ** interrupted while either player merge is running, this file will
    ** remain on disk and the resume scan will know the level's output is
    ** incomplete.
    */
    char sentMerging[MAX_FULL_PATH_NAME];
    SentinelNameMerging(sentMerging, sizeof(sentMerging), pSt->storeDirectory, boardSize, level + 1);
    {
        HANDLE hs = CreateFileA(sentMerging, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, NULL);
        if (hs != INVALID_HANDLE_VALUE) CloseHandle(hs);
    }

    std::thread blackThread([&] { mergePlayer(RSF_PLAYER_BLACK); pSt->mergeEndTickMs[RSF_PLAYER_BLACK] = GetTickCount64(); });
    std::thread whiteThread([&] { mergePlayer(RSF_PLAYER_WHITE); pSt->mergeEndTickMs[RSF_PLAYER_WHITE] = GetTickCount64(); });
    blackThread.join();
    whiteThread.join();

    /* Convert each player's flat merged output into the ring nested-index
    ** format -- see ConvertLevelOutputToNestedIndex's own comment for why
    ** this is a separate pass rather than fused into the merge itself.
    */
    for (int player = RSF_PLAYER_WHITE; player <= RSF_PLAYER_BLACK; player++)
    {
        PlayerData& pd = data[player];
        if (pd.actualBytes <= 0) continue;   /* nothing was written for this player */

        char flatPath[MAX_FULL_PATH_NAME];
        bool storeLZ4 = compressOutput && pCfg->lz4Drives[0]
                     && (strchr(pCfg->lz4Drives, pCfg->storeDrive) != nullptr);
        if (storeLZ4)
            RSFZLNameStoreFile(flatPath, sizeof(flatPath), pSt->storeDirectory, boardSize, level + 1, player, 0);
        else if (compressOutput)
            RSFZNameStoreFile(flatPath, sizeof(flatPath), pSt->storeDirectory, boardSize, level + 1, player, 0);
        else
            RSFNameStoreFile(flatPath, sizeof(flatPath), pSt->storeDirectory, boardSize, level + 1, player, 0);

        int64_t nestedBytes = ConvertLevelOutputToNestedIndex(pCtx, level + 1, player, flatPath, pd.actualBytes);
        if (nestedBytes > 0)
            pd.actualBytes = nestedBytes;
    }

    /* Delete "merging" sentinel on clean finish. The "complete" sentinel
    ** (with full LevelStats payload) is written by the main loop after all
    ** stats fields are populated; that way the persisted stats include
    ** totalNanos, driveSnapshot, storeFreeBytes, and completedAt which are
    ** only known after this function returns. If terminateThreads is set,
    ** leave "merging" in place so resume scan re-runs.
    */
    if (!pSt->terminateThreads)
        DeleteFileA(sentMerging);

    /* -- Finalize stats -- */
    uint64_t blackUnique = data[RSF_PLAYER_BLACK].unique;
    uint64_t whiteUnique = data[RSF_PLAYER_WHITE].unique;
    uint64_t totalUnique = blackUnique + whiteUnique;
    uint64_t outBytes    = totalUnique * sizeof(UINT64_PAIR)
                         + (blackUnique > 0 ? sizeof(RSFTrailer) : 0)
                         + (whiteUnique > 0 ? sizeof(RSFTrailer) : 0);

    uint64_t bwd = pSt->levelStats[level].boardsWrittenToDisk;
    pSt->levelStats[level].mrgDupsRemoved =
        (bwd >= totalUnique) ? (bwd - totalUnique) : 0;
    pSt->levelStats[level].mergeBytes       = outBytes;
    pSt->levelStats[level].mergeActualBytes = (uint64_t)(data[RSF_PLAYER_BLACK].actualBytes
                                                        + data[RSF_PLAYER_WHITE].actualBytes);

    /* Diagnostic for the "white merge always slower" investigation:
    ** per-color dedup ratio. mergeProgressBytes counts every record POPPED
    ** off the merge heap (duplicate or not, see KWayMergeFiles), so a color
    ** with a higher duplicate rate does less actual encode/compress/write
    ** work per record and would show a higher MB/s on the live status line
    ** for that reason alone -- this confirms or rules that out once the
    ** merge is done.
    */
    {
        uint64_t whiteIn = pSt->mergeTotalInputBytes[RSF_PLAYER_WHITE] / sizeof(UINT64_PAIR);
        uint64_t blackIn = pSt->mergeTotalInputBytes[RSF_PLAYER_BLACK] / sizeof(UINT64_PAIR);
        double   whiteDupPct = (whiteIn > 0) ? 100.0 * (1.0 - (double)whiteUnique / (double)whiteIn) : 0.0;
        double   blackDupPct = (blackIn > 0) ? 100.0 * (1.0 - (double)blackUnique / (double)blackIn) : 0.0;
        LoggerLog("DoEndOfLevelMerge: level %d merge-output dedup -- "
                  "white: %llu in -> %llu unique (%.2f%% dup)  |  "
                  "black: %llu in -> %llu unique (%.2f%% dup)\n",
                  level,
                  (unsigned long long)whiteIn, (unsigned long long)whiteUnique, whiteDupPct,
                  (unsigned long long)blackIn, (unsigned long long)blackUnique, blackDupPct);
    }
}
