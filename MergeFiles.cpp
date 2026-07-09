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
**   consolidation of every remaining writer/intermediate file, merged
**   directly into the level's ring nested-index files -- see
**   KWayMergeFilesToRingIndex and CascadingMerge's pRingBuilder parameter).
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
**   KWayMergeFilesToRingIndex/CascadingMerge's pRingBuilder parameter feed
**   the final merge pass's deduped, sorted records directly into a
**   RingNestedIndexBuilder -- no flat intermediate file is ever written for
**   a level's store output (an earlier version wrote one via
**   ConvertLevelOutputToNestedIndex, then immediately reread and rewrote it
**   as the nested-index format; that doubled the actual store I/O for no
**   benefit, since the flat file was never kept). Cascade's own grouped/
**   intermediate temp files (used only when a color's input file count
**   exceeds MAX_MERGE_FANIN) are unaffected -- still flat.
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
** Function: CountByPattern
** @brief    Counts files matching fullPattern without allocating/copying
**           anything -- same FindFirstFileA/FindNextFileA walk as
**           EnumerateByPattern, used to size an array exactly before a
**           real enumeration pass, instead of guessing a fixed capacity.
** @param    fullPattern - glob pattern to search for
** @return   Number of files found.
*/
static int CountByPattern(const char* fullPattern)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(fullPattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    int count = 0;
    do { count++; } while (FindNextFileA(h, &fd));
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
        /* This file was just enumerated as existing by the caller, and
        ** nothing can be deleting/rewriting files at this point in the
        ** pipeline (see the callers' own static-file-set guarantees) -- an
        ** open failure here means real corruption, not a race. Silently
        ** skipping it would merge an incomplete file set without any sign
        ** something was wrong, so fail loudly instead.
        */
        RSFReader* r = RSFOpen(inputPaths[i]);
        if (!r)
            Fatal(FATAL_MERGE_LOGIC_ERROR,
                  "KWayMergeFiles: cannot open '%s' (missing, incomplete, or corrupt trailer) -- "
                  "this file was enumerated as present moments earlier",
                  inputPaths[i]);

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
** Function: KWayMergeFilesToRingIndex
** @brief    Same k-way merge/dedup heap as KWayMergeFiles, but feeds each
**           deduped, sorted record directly into a RingNestedIndexBuilder
**           instead of writing a flat file -- no flat intermediate is ever
**           created for this pass. Used for the FINAL merge pass (the one
**           that produces a level's actual store output); see
**           DoEndOfLevelMerge's mergePlayer and CascadingMerge's
**           pRingBuilder parameter.
** @param    inputPaths     - files to merge
** @param    numInputs      - number of files in inputPaths
** @param    pBuilder       - already-Init()'d builder to feed deduped records into
** @param    pProgressBytes - out (optional): incremented by sizeof(UINT64_PAIR) per record popped
** @param    pTerminate     - out-of-band cancellation flag, checked between pops
** @param    extraReaders   - already-open readers to merge in alongside inputPaths
** @return   Unique record count fed into pBuilder.
*/
static uint64_t KWayMergeFilesToRingIndex(char** inputPaths, int numInputs, RingNestedIndexBuilder* pBuilder,
                                           volatile int64_t* pProgressBytes,
                                           const volatile bool* pTerminate = nullptr,
                                           const std::vector<RSFReader*>& extraReaders = {})
{
    std::priority_queue<MergeHead, std::vector<MergeHead>, MergeHeadGreater> heap;

    for (int i = 0; i < numInputs; i++)
    {
        RSFReader* r = RSFOpen(inputPaths[i]);
        if (!r)
            Fatal(FATAL_MERGE_LOGIC_ERROR,
                  "KWayMergeFilesToRingIndex: cannot open '%s' (missing, incomplete, or corrupt trailer) -- "
                  "this file was enumerated as present moments earlier",
                  inputPaths[i]);

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

    UINT64_PAIR lastKey = {};
    bool        hasLast = false;
    uint64_t    unique  = 0;

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
            BOARD_KEY key;
            key.ullCellsInUse = top.key.hi;
            key.ullCellColors = top.key.lo;
            pBuilder->Process(key);
            lastKey = top.key;
            hasLast = true;
            unique++;
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

    return unique;
}

/*
** ============================================================
** Min-heap entry for ring-format-group k-way merge (whole BOARD_KEYs)
** ============================================================
*/

/*
** Type:    RingGroupMergeHead
** @brief   One open ring-format cascade group's current front-of-stream
**          board, for MergeRingGroupsIntoBuilder's heap.
*/
struct RingGroupMergeHead
{
    BOARD_KEY                  key;
    RingNestedIndexPullReader* pReader;
};

/*
** Type:    RingGroupMergeHeadGreater
** @brief   Min-heap comparator (see MergeHeadGreater) ordering by
**          (ullCellsInUse, ullCellColors) -- the same plain numeric
**          comparison every merge comparator in this file uses.
*/
struct RingGroupMergeHeadGreater
{
    bool operator()(const RingGroupMergeHead& a, const RingGroupMergeHead& b) const
    {
        if (a.key.ullCellsInUse != b.key.ullCellsInUse)
            return a.key.ullCellsInUse > b.key.ullCellsInUse;
        return a.key.ullCellColors > b.key.ullCellColors;
    }
};

/*
** Function: MergeRingGroupsIntoBuilder
** @brief    K-way merges numGroups already-open ring-format cascade group
**           readers directly into pBuilder, deduping on (ullCellsInUse,
**           ullCellColors) -- the ring-format-group counterpart to
**           KWayMergeFilesToRingIndex, used when the merge inputs are
**           themselves ring-format (cascade groups) rather than flat files.
**           Does not close pReaders -- the caller opened them and is
**           expected to close them (and check IsCorrupted()) afterward.
** @param    pReaders       - numGroups already-Open()'d readers
** @param    numGroups      - number of entries in pReaders
** @param    pBuilder       - already-Init()'d builder to feed deduped records into
** @param    pProgressBytes - out (optional): incremented by sizeof(UINT64_PAIR) per record popped
** @param    pTerminate     - out-of-band cancellation flag, checked between pops
** @return   Unique record count fed into pBuilder.
*/
static uint64_t MergeRingGroupsIntoBuilder(RingNestedIndexPullReader* pReaders, int numGroups,
                                            RingNestedIndexBuilder* pBuilder,
                                            volatile int64_t* pProgressBytes,
                                            const volatile bool* pTerminate)
{
    std::priority_queue<RingGroupMergeHead, std::vector<RingGroupMergeHead>, RingGroupMergeHeadGreater> heap;

    for (int i = 0; i < numGroups; i++)
    {
        BOARD_KEY key;
        if (pReaders[i].Peek(&key))
            heap.push({ key, &pReaders[i] });
    }

    BOARD_KEY lastKey = {};
    bool      hasLast = false;
    uint64_t  unique  = 0;

    while (!heap.empty())
    {
        if (pTerminate && *pTerminate) break;

        RingGroupMergeHead top = heap.top();
        heap.pop();

        if (pProgressBytes)
            InterlockedAdd64((volatile LONG64*)pProgressBytes, (LONG64)sizeof(UINT64_PAIR));

        bool isDup = hasLast && top.key.ullCellsInUse == lastKey.ullCellsInUse
                             && top.key.ullCellColors == lastKey.ullCellColors;
        if (!isDup)
        {
            pBuilder->Process(top.key);
            lastKey = top.key;
            hasLast = true;
            unique++;
        }

        if (top.pReader->Advance())
        {
            BOARD_KEY nextKey;
            top.pReader->Peek(&nextKey);
            top.key = nextKey;
            heap.push(top);
        }
        /* If Advance() returned false, top.pReader is either cleanly
        ** exhausted or corrupted -- the caller checks IsCorrupted() on
        ** every reader once this function returns, so no check is needed
        ** per-pop here.
        */
    }

    return unique;
}

/*
** Function: ChooseNextCascadeGroup
** @brief    Picks how many of the next files (inputPaths[0..windowSize)) form
**           the next cascade group, and which temp drive will hold that
**           group's merged output -- shared by CascadingMerge's flat and
**           ring-format group-forming loops (see file Notes on the latter).
** @details  For each candidate drive (fastest first, store drive last
**           resort): counts how many consecutive files fit within the
**           drive's available ledger space, using the first drive that can
**           accept at least one file. Reserves that drive's ledger space
**           for the chosen group's exact input bytes.
** @param    inputPaths        - remaining files to group (only [0, windowSize) is examined)
** @param    numRemaining      - how many files remain to be grouped in total
** @param    tempDirs          - candidate directories, ordered fastest-first
** @param    numTempDirs       - number of entries in tempDirs
** @param    pSt               - solve state (driveLedger), or nullptr (recursive/no-pCtx call -- no drive accounting)
** @param    player             - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE (logging only)
** @param    groupNumber1Based - this group's 1-based index (logging only)
** @param    fileSzCache       - scratch buffer, at least MAX_MERGE_FANIN entries
** @param    pOutChosenDir     - out: the chosen directory
** @param    pOutGroupSize     - out: how many files this group contains
** @param    pOutGroupBytes    - out: total input bytes reserved for this group
*/
static void ChooseNextCascadeGroup(char** inputPaths, int numRemaining,
                                    const char** tempDirs, int numTempDirs,
                                    POthelloRingMasterState pSt, int player, int groupNumber1Based,
                                    std::vector<int64_t>& fileSzCache,
                                    const char** pOutChosenDir, int* pOutGroupSize, int64_t* pOutGroupBytes)
{
    int windowSize = (std::min)(MAX_MERGE_FANIN, numRemaining);

    std::fill(fileSzCache.begin(), fileSzCache.begin() + windowSize, 0);
    for (int k = 0; k < windowSize; k++)
    {
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (GetFileAttributesExA(inputPaths[k], GetFileExInfoStandard, &fad))
            fileSzCache[k] = ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;
    }

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
                  RSFPlayerStr(player), groupNumber1Based);
    }

    *pOutChosenDir  = chosenDir;
    *pOutGroupSize  = groupSize;
    *pOutGroupBytes = groupBytes;
}

/*
** Type:    RingCascadeGroupPaths
** @brief   One ring-format cascade group's 4 file paths (Ring_1/Ring_2
**          only meaningful when the board size uses them).
*/
struct RingCascadeGroupPaths
{
    char cellsInUse[MAX_FULL_PATH_NAME];
    char ring1[MAX_FULL_PATH_NAME];
    char ring2[MAX_FULL_PATH_NAME];
    char ring34[MAX_FULL_PATH_NAME];
};

/*
** Function: CascadeGroupsToRingIndex
** @brief    Ring-format counterpart to CascadingMerge's grouped/cascading
**           path, used when numInputs alone exceeds MAX_MERGE_FANIN and the
**           final output is ring-format (CascadingMerge's pRingBuilder is
**           set). Groups inputPaths exactly like CascadingMerge's own loop
**           (via the same ChooseNextCascadeGroup helper), but each group's
**           merged output becomes 4 ring files (via
**           KWayMergeFilesToRingIndex) instead of 1 flat file -- to conserve
**           space on these transient temp files too, the same reasoning as
**           the level's own final store.
** @details  Deliberately single-round: if the groups this round produces
**           (at most MAX_MERGE_FANIN of them) still exceed MAX_MERGE_FANIN,
**           Fatals rather than recursing into a second cascade round --
**           that would need well over 12 million real input files for one
**           level/color (the largest real run on record needed ~42,000),
**           a scale never exercised or tested in this project. The
**           realistic case (one round of ring-format groups, merged
**           directly into pRingBuilder) is what this implements.
** @param    inputPaths     - files to merge (caller already knows numInputs > MAX_MERGE_FANIN)
** @param    numInputs      - number of files in inputPaths
** @param    tempDirs       - candidate directories for group temp files, ordered fastest-first
** @param    numTempDirs    - number of entries in tempDirs
** @param    pTempCount     - running counter for cascade temp file indices
** @param    level          - level being merged (for temp file naming)
** @param    player         - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @param    pProgressBytes - out (optional): merge progress, see KWayMergeFiles
** @param    pCtx           - solve context (always real here -- ring mode only ever starts from the outer call)
** @param    pTerm          - out-of-band cancellation flag
** @param    pRingBuilder   - the final, already-Init()'d builder to feed deduped records into
** @return   Unique record count fed into pRingBuilder.
*/
static uint64_t CascadeGroupsToRingIndex(char** inputPaths, int numInputs,
                                          const char** tempDirs, int numTempDirs,
                                          int* pTempCount, int level, int player,
                                          volatile int64_t* pProgressBytes,
                                          PSolveContext pCtx, const volatile bool* pTerm,
                                          RingNestedIndexBuilder* pRingBuilder)
{
    if (!pCtx)
        Fatal(FATAL_MERGE_LOGIC_ERROR, "CascadeGroupsToRingIndex: requires a real solve context (board size)");

    POthelloRingMasterState pSt       = pCtx->pState;
    int                     boardSize = (int)pCtx->pConfig->boardSize;
    bool                    hasRing1  = RingNestedIndexHasRing1(boardSize);
    bool                    hasRing2  = RingNestedIndexHasRing2(boardSize);

    /* Upper bound on groups is numInputs (1 file per group in the extreme case). */
    std::vector<RingCascadeGroupPaths> groupPaths(numInputs);
    std::vector<int64_t>               groupActualBytes(numInputs, 0);
    std::vector<char>                  groupDriveLetter(numInputs, 0);

    if (pSt)
    {
        pSt->cascadeNumGroups[player]          = (numInputs + MAX_MERGE_FANIN - 1) / MAX_MERGE_FANIN;
        pSt->cascadeGroupsDone[player]         = 0;
        pSt->cascadeGroupProgressBytes[player] = 0;
        pSt->cascadeStartTickMs[player]        = GetTickCount64();
        pSt->cascadeActive[player]             = true;
    }

    int numGroups = 0;
    int start     = 0;
    std::vector<int64_t> fileSzCache(MAX_MERGE_FANIN, 0);
    while (start < numInputs)
    {
        if (pTerm && *pTerm) break;

        const char* chosenDir;
        int         groupSize;
        int64_t     groupBytes;
        ChooseNextCascadeGroup(inputPaths + start, numInputs - start, tempDirs, numTempDirs,
                                pSt, player, numGroups + 1, fileSzCache,
                                &chosenDir, &groupSize, &groupBytes);

        if (numGroups + 1 > MAX_MERGE_FANIN)
            Fatal(FATAL_MERGE_LOGIC_ERROR,
                  "CascadeGroupsToRingIndex: %s needs a second cascade round (>%d ring-format groups) -- "
                  "not supported (would need well over 12 million real input files for one level/color, "
                  "never seen at this project's scale)",
                  RSFPlayerStr(player), MAX_MERGE_FANIN);

        if (pSt && numTempDirs > 0 && numGroups + 1 > pSt->cascadeNumGroups[player])
            pSt->cascadeNumGroups[player] = numGroups + 1;

        if (pSt) pSt->cascadeGroupProgressBytes[player] = 0;
        if (pSt) pSt->cascadeGroupStartTickMs[player]  = GetTickCount64();

        LoggerLog("CascadeGroupsToRingIndex: %s group %d -> %c: (%d files, %.2f GB input, ring format)\n",
                  RSFPlayerStr(player), numGroups + 1, chosenDir[0],
                  groupSize, groupBytes / (1024.0 * 1024.0 * 1024.0));

        RingCascadeGroupPaths& gp = groupPaths[numGroups];
        int groupIdx = (*pTempCount)++;
        RSFNameCascadeRingCellsInUseFile(gp.cellsInUse, sizeof(gp.cellsInUse), chosenDir, level, player, groupIdx);
        if (hasRing1) RSFNameCascadeRingRing1File(gp.ring1, sizeof(gp.ring1), chosenDir, level, player, groupIdx);
        if (hasRing2) RSFNameCascadeRingRing2File(gp.ring2, sizeof(gp.ring2), chosenDir, level, player, groupIdx);
        RSFNameCascadeRingRing34File(gp.ring34, sizeof(gp.ring34), chosenDir, level, player, groupIdx);

        RSFWriter* pCellsInUseWriter = RSFWriterOpenZL(gp.cellsInUse);
        RSFWriter* pRing1Writer      = hasRing1 ? RSFWriterOpenZLShaped(gp.ring1, RSF_SHAPE_RING_LEVEL) : nullptr;
        RSFWriter* pRing2Writer      = hasRing2 ? RSFWriterOpenZLShaped(gp.ring2, RSF_SHAPE_RING_LEVEL) : nullptr;
        RSFWriter* pRing34Writer     = RSFWriterOpenZLShaped(gp.ring34, RSF_SHAPE_LEAF16);

        RingNestedIndexBuilder groupBuilder;
        groupBuilder.Init(pCellsInUseWriter, pRing1Writer, pRing2Writer, pRing34Writer);

        KWayMergeFilesToRingIndex(inputPaths + start, groupSize, &groupBuilder,
                                   pSt ? &pSt->cascadeGroupProgressBytes[player] : nullptr, pTerm);
        groupBuilder.Finish();

        RSFWriterClose(pCellsInUseWriter);
        if (pRing1Writer) RSFWriterClose(pRing1Writer);
        if (pRing2Writer) RSFWriterClose(pRing2Writer);
        RSFWriterClose(pRing34Writer);

        int64_t groupActual = 0;
        {
            WIN32_FILE_ATTRIBUTE_DATA fad = {};
            const char* parts[4] = { gp.cellsInUse, hasRing1 ? gp.ring1 : nullptr,
                                      hasRing2 ? gp.ring2 : nullptr, gp.ring34 };
            for (int i = 0; i < 4; i++)
                if (parts[i] && GetFileAttributesExA(parts[i], GetFileExInfoStandard, &fad))
                    groupActual += ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;
        }
        groupActualBytes[numGroups] = groupActual;
        groupDriveLetter[numGroups] = chosenDir[0];
        if (pSt) DriveReclaim(pSt, chosenDir[0], groupBytes - groupActual);

        numGroups++;
        if (pSt) pSt->cascadeGroupsDone[player]++;
        start += groupSize;
    }

    if (pSt) pSt->cascadeActive[player] = false;

    /* Single round only (see @details) -- merge the (at most MAX_MERGE_FANIN)
    ** ring-format groups directly into the final builder.
    */
    std::vector<RingNestedIndexPullReader> readers(numGroups);
    for (int i = 0; i < numGroups; i++)
    {
        const RingCascadeGroupPaths& gp = groupPaths[i];
        if (!readers[i].Open(gp.cellsInUse, hasRing1 ? gp.ring1 : nullptr, hasRing2 ? gp.ring2 : nullptr, gp.ring34))
            Fatal(FATAL_MERGE_LOGIC_ERROR, "CascadeGroupsToRingIndex: cannot reopen ring-format group %d for '%s'",
                  i, gp.cellsInUse);
    }

    uint64_t unique = MergeRingGroupsIntoBuilder(readers.data(), numGroups, pRingBuilder, pProgressBytes, pTerm);

    for (int i = 0; i < numGroups; i++)
    {
        if (readers[i].IsCorrupted())
            Fatal(FATAL_MERGE_LOGIC_ERROR, "CascadeGroupsToRingIndex: ring-format group %d is truncated/corrupt", i);
        readers[i].Close();
    }

    for (int i = 0; i < numGroups; i++)
    {
        const RingCascadeGroupPaths& gp = groupPaths[i];
        if (pSt) DriveReclaim(pSt, groupDriveLetter[i], groupActualBytes[i]);
        DeleteFileA(gp.cellsInUse);
        if (hasRing1) DeleteFileA(gp.ring1);
        if (hasRing2) DeleteFileA(gp.ring2);
        DeleteFileA(gp.ring34);
    }

    return unique;
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
** @param    pRingBuilder           - if non-null, feeds deduped records directly into this
**                                    already-Init()'d builder instead of writing finalOutPath
**                                    (finalOutPath/compressFinal are ignored in that case).
**                                    numInputs <= MAX_MERGE_FANIN goes straight through
**                                    KWayMergeFilesToRingIndex; numInputs > MAX_MERGE_FANIN
**                                    dispatches to CascadeGroupsToRingIndex instead of this
**                                    function's own flat grouped/recursive path below -- in ring
**                                    mode, cascade's own intermediate group temp files become
**                                    ring-format too (see that function's own comment).
** @return   Unique record count written to finalOutPath (or fed into pRingBuilder).
*/
static uint64_t CascadingMerge(char** inputPaths, int numInputs,
                                 const char** tempDirs, int numTempDirs,
                                 const char* finalOutPath,
                                 int* pTempCount, int level, int player,
                                 volatile int64_t* pProgressBytes,
                                 PSolveContext pCtx, bool compressFinal = false,
                                 bool compressIntermediate = false,
                                 const volatile bool* pTerminate = nullptr,
                                 const std::vector<RSFReader*>& extraReaders = {},
                                 RingNestedIndexBuilder* pRingBuilder = nullptr)
{
    const volatile bool* pTerm = pCtx ? &pCtx->pState->terminateThreads : pTerminate;

    if (numInputs <= MAX_MERGE_FANIN)
    {
        if (pRingBuilder)
            return KWayMergeFilesToRingIndex(inputPaths, numInputs, pRingBuilder, pProgressBytes, pTerm, extraReaders);
        return KWayMergeFiles(inputPaths, numInputs, finalOutPath, pProgressBytes,
                              compressFinal, pTerm, extraReaders);
    }

    if (!extraReaders.empty())
        Fatal(FATAL_MERGE_LOGIC_ERROR,
              "CascadingMerge: %s has %zu in-memory pool readers but numInputs=%d "
              "> MAX_MERGE_FANIN requires grouped mode, which only accepts files -- "
              "caller must flush the pool to disk first",
              RSFPlayerStr(player), extraReaders.size(), numInputs);

    /* Ring mode's grouped path is a separate, single-round implementation
    ** (see CascadeGroupsToRingIndex's own comment) -- never recurses back
    ** into CascadingMerge itself.
    */
    if (pRingBuilder)
        return CascadeGroupsToRingIndex(inputPaths, numInputs, tempDirs, numTempDirs,
                                         pTempCount, level, player, pProgressBytes,
                                         pCtx, pTerm, pRingBuilder);

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

        const char* chosenDir;
        int         groupSize;
        int64_t     groupBytes;
        ChooseNextCascadeGroup(inputPaths + start, numInputs - start, tempDirs, numTempDirs,
                                pSt, player, numTemps + 1, fileSzCache,
                                &chosenDir, &groupSize, &groupBytes);

        /* Keep the status group-count estimate current if we're creating more groups. */
        if (pSt && numTempDirs > 0 && numTemps + 1 > pSt->cascadeNumGroups[player])
            pSt->cascadeNumGroups[player] = numTemps + 1;

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
                                      compressIntermediate, pTerm, {}, pRingBuilder);

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

        /* This loop's true count is exactly known ahead of time (the sum of
        ** snapArr[ti]-consumedArr[ti] across every writer) -- hitting the
        ** capacity here means real unconsumed writer files were silently
        ** left off this merge. Fail loudly rather than continue with a
        ** partial file set.
        */
        if (numFiles >= kMaxFiles)
            Fatal(FATAL_MERGE_LOGIC_ERROR,
                  "DoCrossDriveIntermediateMerge: %s writer-file count hit the capacity (%d) -- "
                  "real unconsumed writer files would be silently dropped from this merge",
                  RSFPlayerStr(player), kMaxFiles);

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

            /* Hitting the capacity here means real existing medium-drive
            ** imerge files were silently left out of this total flush --
            ** exactly the kind of cross-drive duplicate this path exists to
            ** catch. Fail loudly rather than flush an incomplete file set.
            */
            if (numFiles >= kMaxFiles)
                Fatal(FATAL_MERGE_LOGIC_ERROR,
                      "DoCrossDriveIntermediateMerge: %s total-flush file count hit the capacity (%d) -- "
                      "real imerge files would be silently dropped from this flush",
                      RSFPlayerStr(player), kMaxFiles);

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
** Function: CountEndOfLevelInputFiles
** @brief    Counts exactly how many on-disk files DoEndOfLevelMerge's Phase 1
**           will enumerate for one player, across every writer directory,
**           merge directory, and the store-merge directory, in every
**           compression tier the current config could have produced.
** @details  Safe to size an allocation from: DoEndOfLevelMerge only ever runs
**           after both WaitForPoolIdle calls and FlushAllMergeWriterBuffers
**           (see OthelloRingMaster.cpp's main loop), so no thread can still
**           be creating new writer/imerge files by the time this counts them --
**           the file set is static, and this count will still be accurate
**           when Phase 1's real enumeration pass runs moments later.
** @param    pCtx   - solve context
** @param    level  - level being merged
** @param    player - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @return   Exact number of files Phase 1 will find for this player.
*/
static int CountEndOfLevelInputFiles(PSolveContext pCtx, int level, int player)
{
    POthelloRingMasterState  pSt  = pCtx->pState;
    POthelloRingMasterConfig pCfg = pCtx->pConfig;
    char                     pat[MAX_FULL_PATH_NAME];
    int                      count = 0;

    for (int i = 0; i < pSt->numMergeWriters; i++)
    {
        RSFPatternWriterFiles(pat, sizeof(pat), pSt->mwDirectory[i], player);
        count += CountByPattern(pat);
        if (pCfg->compressMode == COMPRESS_ALL)
        {
            RSFZPatternWriterFiles(pat, sizeof(pat), pSt->mwDirectory[i], player);
            count += CountByPattern(pat);
            if (pCfg->lz4Drives[0])
            {
                RSFZLPatternWriterFiles(pat, sizeof(pat), pSt->mwDirectory[i], player);
                count += CountByPattern(pat);
            }
        }
    }
    for (int i = 0; i < pSt->numMergeDirs; i++)
    {
        RSFPatternImergeFiles(pat, sizeof(pat), pSt->mergeDirectory[i], level, player);
        count += CountByPattern(pat);
        if (pCfg->compressMode == COMPRESS_ALL)
        {
            RSFZPatternImergeFiles(pat, sizeof(pat), pSt->mergeDirectory[i], level, player);
            count += CountByPattern(pat);
            if (pCfg->lz4Drives[0])
            {
                RSFZLPatternImergeFiles(pat, sizeof(pat), pSt->mergeDirectory[i], level, player);
                count += CountByPattern(pat);
            }
        }
    }
    RSFPatternImergeFiles(pat, sizeof(pat), pSt->storeMergeDirectory, level, player);
    count += CountByPattern(pat);
    if (pCfg->compressMode == COMPRESS_ALL)
    {
        RSFZPatternImergeFiles(pat, sizeof(pat), pSt->storeMergeDirectory, level, player);
        count += CountByPattern(pat);
        if (pCfg->lz4Drives[0])
        {
            RSFZLPatternImergeFiles(pat, sizeof(pat), pSt->storeMergeDirectory, level, player);
            count += CountByPattern(pat);
        }
    }
    return count;
}

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

    /* -- Phase 1: enumerate files for both players (sequential, fast) --
    ** We scan first so mergeTotalInputBytes is known before the merge
    ** starts, giving the stats listener an accurate denominator from the
    ** beginning.
    */
    PlayerData data[2] = {};   /* indexed by RSF_PLAYER_WHITE(0) / RSF_PLAYER_BLACK(1) */

    for (int player = RSF_PLAYER_WHITE; player <= RSF_PLAYER_BLACK; player++)
    {
        /* Sized exactly, not guessed: count real files first (safe here --
        ** no thread can still be creating writer/imerge files by this point,
        ** see CountEndOfLevelInputFiles's own header comment), then allocate
        ** just enough for that count plus a small fixed pad for defense in
        ** depth. Replaces a fixed MAX_MERGE_FANIN*MAX_MERGE_FANIN guess
        ** (12.25M entries, ~392MB) that was never actually checked against
        ** real production numbers -- the largest real run on record needed
        ** only about 42,000 files for one color, ~291x less than the guess.
        */
        const int kInputFilePad = 256;
        const int kMaxInputFiles = CountEndOfLevelInputFiles(pCtx, level, player) + kInputFilePad;

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

        /* Should never happen -- CountEndOfLevelInputFiles just counted the
        ** same static file set plus a pad. Hitting the cap means that
        ** assumption was somehow violated (e.g. a writer thread still
        ** active), and continuing would silently merge an incomplete file
        ** set -- fail loudly instead.
        */
        if (numFiles >= kMaxInputFiles)
            Fatal(FATAL_MERGE_LOGIC_ERROR,
                  "DoEndOfLevelMerge: %s file count hit the counted capacity (%d) -- "
                  "file set changed after counting, which should be impossible here",
                  RSFPlayerStr(player), kMaxInputFiles);

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

        /* Merge directly into the level's ring nested-index files -- no flat
        ** intermediate file is ever written for the store output (see
        ** CascadingMerge/KWayMergeFilesToRingIndex's pRingBuilder path).
        ** Ring_1/Ring_2 paths are only built (and opened) when this board
        ** size actually uses that level -- see RingNestedIndexHasRing1/HasRing2.
        */
        bool hasRing1 = RingNestedIndexHasRing1(boardSize);
        bool hasRing2 = RingNestedIndexHasRing2(boardSize);

        char cellsInUsePath[MAX_FULL_PATH_NAME];
        char ring1PathBuf[MAX_FULL_PATH_NAME];
        char ring2PathBuf[MAX_FULL_PATH_NAME];
        char ring34Path[MAX_FULL_PATH_NAME];
        RSFNameCellsInUseFile(cellsInUsePath, sizeof(cellsInUsePath), pSt->storeDirectory, boardSize, level + 1, player, 0);
        if (hasRing1)
            RSFNameRing1File(ring1PathBuf, sizeof(ring1PathBuf), pSt->storeDirectory, boardSize, level + 1, player, 0);
        if (hasRing2)
            RSFNameRing2File(ring2PathBuf, sizeof(ring2PathBuf), pSt->storeDirectory, boardSize, level + 1, player, 0);
        RSFNameRing34File(ring34Path, sizeof(ring34Path), pSt->storeDirectory, boardSize, level + 1, player, 0);

        const char* ring1Path = hasRing1 ? ring1PathBuf : nullptr;
        const char* ring2Path = hasRing2 ? ring2PathBuf : nullptr;

        RSFWriter* pCellsInUseWriter = RSFWriterOpenZL(cellsInUsePath);
        RSFWriter* pRing1Writer      = hasRing1 ? RSFWriterOpenZLShaped(ring1Path, RSF_SHAPE_RING_LEVEL) : nullptr;
        RSFWriter* pRing2Writer      = hasRing2 ? RSFWriterOpenZLShaped(ring2Path, RSF_SHAPE_RING_LEVEL) : nullptr;
        RSFWriter* pRing34Writer     = RSFWriterOpenZLShaped(ring34Path, RSF_SHAPE_LEAF16);

        RingNestedIndexBuilder builder;
        builder.Init(pCellsInUseWriter, pRing1Writer, pRing2Writer, pRing34Writer);

        /* Unified merge: on-disk files (0, 1, or many) plus any leftover
        ** in-memory pool data, all merged in a single pass -- see
        ** CascadingMerge/KWayMergeFilesToRingIndex. Cascade temps (grouped
        ** mode, only when pd.numFiles alone exceeds MAX_MERGE_FANIN) are
        ** still flat, placed greedily per-group (fastest drive first, store
        ** drive if needed); compressed when compressOutput is on.
        ** compressFinal is passed false -- irrelevant here, since the ring
        ** builder's own shaped writers are always compressed regardless.
        */
        int tempCount = 0;
        pd.unique = CascadingMerge(pd.inputPaths, pd.numFiles, tempDirs, numTempDirs,
                                    nullptr, &tempCount, level, player, pProg, pCtx,
                                    /*compressFinal=*/false, compressOutput, nullptr,
                                    pd.poolReaders, &builder);
        builder.Finish();

        RSFWriterClose(pCellsInUseWriter);
        if (pRing1Writer) RSFWriterClose(pRing1Writer);
        if (pRing2Writer) RSFWriterClose(pRing2Writer);
        RSFWriterClose(pRing34Writer);

        for (uint8_t* buf : pd.poolTempBufs) MemFree(buf);
        pd.poolTempBufs.clear();

        /* Total on-disk bytes across the (up to) 4 ring files just written. */
        int64_t actual = 0;
        {
            WIN32_FILE_ATTRIBUTE_DATA fad = {};
            const char* parts[4] = { cellsInUsePath, ring1Path, ring2Path, ring34Path };
            for (int i = 0; i < 4; i++)
                if (parts[i] && GetFileAttributesExA(parts[i], GetFileExInfoStandard, &fad))
                    actual += ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;
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

        pd.actualBytes = actual;

        if (pd.unique > 0)
            InterlockedIncrement((volatile LONG*)&pSt->levelStats[level].mergeFilesWritten);

        LoggerLog("EndOfLevelMerge: level %d %s -> nested index under '%s'  (%llu unique boards, %.2f GB)\n",
                  level, RSFPlayerStr(player), pSt->storeDirectory, pd.unique,
                  actual / (1024.0 * 1024.0 * 1024.0));

        for (int i = 0; i < pd.numFiles; i++)
            MemFree(pd.inputPaths[i]);
        MemFree(pd.inputPaths);
        MemFree(pd.inputSizes);

        /* Reset this color's pool/staging state for the next level. Safe to
        ** do now -- the merge above (and any RSFReaderOpenZMem segments it
        ** read from pMWBuffer) has already fully consumed and closed
        ** everything. No-op when this color's pool was already empty (e.g.
        ** it was pre-flushed to NVMe by the fan-in guard).
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
