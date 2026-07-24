/*
** Filename:  InitSolver.cpp
**
** Purpose:
**   Implements InitSolver/CleanupSolver declared in InitSolver.h, plus their
**   private helpers: buffer-size computation (computeState), the sentinel-
**   file-based resume scan (ScanForResumeLevel and its helpers), ephemeral
**   working-directory purge/creation (cleanUpDrives/createDirectories), and
**   the machine-wide single-instance mutex (AcquireInstanceLock/
**   ReleaseInstanceLock).
**
** Notes:
**   Adapted from an earlier solver implementation, renamed onto this
**   solution's own types (BOARD_KEY_DISK -> UINT64_PAIR, the old
**   record-file prefix -> RSF*, RSFFileName.h, -> OthelloRingMasterConfig/
**   State, file extensions -> .rsf/.rsfz/.rsfzl). The single-instance mutex
**   uses its own name, "Local\OthelloRingMaster_SingleInstance" -- nothing
**   in this project should name another solution, even in an OS-level name
**   nobody but this process ever reads.
*/

/* Includes */
#include "InitSolver.h"
#include "RSFFileName.h"
#include "DriveLedger.h"
#include "OthelloBasicsForCUDA.h"
#include "RingNestedIndex.h"
#include "Utility.h"
#include <windows.h>
#include <shellapi.h>

/* Structures and Types */

/*
** Type:    LevelFileStatus
** @brief   Result of probing a level's player output file during resume scan.
*/
enum LevelFileStatus { LFS_VALID, LFS_CORRUPT, LFS_ABSENT };

/* Globals */

/*
** One GLOBAL mutex for the whole machine, not per store directory -- two
** runs against different storeDirs (e.g. a 4x4 test alongside a 6x6 solve)
** still share the same NVMe writer drives, ephemeral merge directories, and
** GPU, so only one instance may ever run at a time regardless of which
** board size or storeDir it targets. The OS releases it automatically on
** exit or crash -- no stale state, nothing to clean up manually.
*/
static HANDLE g_instanceMutex = NULL;

/* Functions */

/*
** Function: createMergeWriterDirectoryName
** @brief    Builds the path for the one merge-writer directory on a given drive.
** @param    driveLetter      - drive to place the directory on
** @param    pStoreDirNoDrive - store directory path with the drive letter stripped
** @param    pOutDir          - out: the built path (MAX_FULL_PATH_NAME capacity assumed)
*/
static void createMergeWriterDirectoryName(char driveLetter, const char* pStoreDirNoDrive,
                                           char* pOutDir)
{
    snprintf(pOutDir, MAX_FULL_PATH_NAME, "%c:%s\\writerDir",
             driveLetter, pStoreDirNoDrive);
}

/*
** Function: DeleteDirRecursive
** @brief    Recursively deletes a directory and everything under it.
** @param    dir - directory to delete
*/
static void DeleteDirRecursive(const char* dir)
{
    char pattern[MAX_FULL_PATH_NAME];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char full[MAX_FULL_PATH_NAME];
        snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            DeleteDirRecursive(full);
        else
        {
            SetFileAttributesA(full, FILE_ATTRIBUTE_NORMAL);
            DeleteFileA(full);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    RemoveDirectoryA(dir);
}

/*
** Function: AcquireInstanceLock
** @brief    Acquires the machine-wide single-instance mutex, Fatal()-ing if
**           another instance already holds it.
** @param    storeDir - store directory this instance targets (for the error message only)
*/
static void AcquireInstanceLock(const char* storeDir)
{
    g_instanceMutex = CreateMutexA(NULL, FALSE, "Local\\OthelloRingMaster_SingleInstance");
    if (!g_instanceMutex)
    {
        LoggerLog("WARNING: Could not create instance mutex (err %lu) -- proceeding unlocked.\n",
                  (unsigned long)GetLastError());
        return;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(g_instanceMutex);
        g_instanceMutex = NULL;
        Fatal(FATAL_FILE_OPEN,
              "Another OthelloRingMaster instance is already running (targeting '%s' or a different storeDir).\n"
              "Only one instance may run at a time -- they share NVMe writer drives and the GPU.\n"
              "Stop it before launching a new run.",
              storeDir);
    }
}

/*
** Function: ReleaseInstanceLock
** @brief    Releases the machine-wide single-instance mutex, if held.
*/
static void ReleaseInstanceLock()
{
    if (g_instanceMutex)
    {
        CloseHandle(g_instanceMutex);
        g_instanceMutex = NULL;
    }
}

/*
** Function: computeState
** @brief    Sizes every large buffer from the machine's memory budget: the
**           ping-pong buffer, then divides remaining RAM across fast
**           (NVMe-class) drives to size the per-thread merge-writer
**           buffers, falling back to a GPU-sized minimum and capping writer
**           count when memory is tight. Also picks merge-writer/merge
**           directories and allocates every buffer.
** @param    pConfig      - run configuration
** @param    pState       - out: filled with the computed sizes/directories/buffers
** @param    pMachineInfo - probed machine capability (memory/drives/GPU)
*/
static void computeState(POthelloRingMasterConfig pConfig, POthelloRingMasterState pState,
                         PMachineInfo pMachineInfo)
{
    pState->playLevel        = 0;
    pState->numMergeWriters  = 0;
    pState->terminateThreads       = false;
    pState->terminateConsolidation = false;
    size_t availableMemoryToAllocate = pMachineInfo->g_memInfo.budgetedSize;

    /* Four batch-sized slots: reader keeps 3 filled while GPU reads the other */
    pState->pingPongBufferSize = (size_t)pMachineInfo->g_gpuInfo.optimalBatchSize
                                 * sizeof(UINT64_PAIR) * 4;

    if (pState->pingPongBufferSize > availableMemoryToAllocate)
        Fatal(FATAL_INSUFFICIENT_MEMORY,
              "Ping-pong buffer (%zu bytes) exceeds budgeted RAM (%zu bytes).",
              pState->pingPongBufferSize, availableMemoryToAllocate);

    availableMemoryToAllocate -= pState->pingPongBufferSize;

    /* GPU-based minimum per thread: must hold at least one worst-case GPU flush. */
    const size_t gpuMinBufSize = pMachineInfo->g_gpuInfo.totalGlobalMemBytes * 8 / 10;

    if (availableMemoryToAllocate < gpuMinBufSize)
        Fatal(FATAL_INSUFFICIENT_MEMORY,
              "Not enough RAM for even one merge-writer buffer (%zu GB).",
              gpuMinBufSize / (1024 * 1024 * 1024));

    /* Count fast drives first so we can maximize the per-thread MW buffer. */
    int numFastDrives = 0;
    for (int i = 0; i < pMachineInfo->g_drives.numDrives; i++)
    {
        const DriveInformation* d = &pMachineInfo->g_drives.drives[i];
        if (d->available && d->driveCategory == DRIVE_CAT_FAST
                         && d->driveLetter != pConfig->storeDrive)
            numFastDrives++;
    }

    /* Maximize MW buffer size: a larger buffer accumulates more GPU flushes
    ** before a disk write, widening the in-memory dedup window and reducing
    ** how much data reaches disk -- fewer files, less imerge pressure,
    ** smaller end-of-level merge. Divide all available RAM evenly across
    ** fast drives. Fall back to the GPU-sized minimum and cap writer count
    ** when memory is tight.
    */
    size_t mwBufSize;
    int numWritersToCreate;
    if (numFastDrives > 0 &&
        availableMemoryToAllocate / (size_t)numFastDrives >= gpuMinBufSize)
    {
        numWritersToCreate = numFastDrives;
        mwBufSize = availableMemoryToAllocate / (size_t)numWritersToCreate;
    }
    else
    {
        /* RAM-constrained: keep gpuMinBufSize per thread and cap writer count. */
        mwBufSize = gpuMinBufSize;
        numWritersToCreate = (int)(availableMemoryToAllocate / mwBufSize);
        if (numWritersToCreate > numFastDrives) numWritersToCreate = numFastDrives;
    }

    if (numWritersToCreate < 1)
        Fatal(FATAL_INSUFFICIENT_MEMORY, "No fast drives available for merge-writer threads.");

    for (int i = 0; i < pMachineInfo->g_drives.numDrives
                     && pState->numMergeWriters < numWritersToCreate; i++)
    {
        const DriveInformation* d = &pMachineInfo->g_drives.drives[i];
        if (!d->available) continue;
        if (d->driveCategory != DRIVE_CAT_FAST) continue;
        if (d->driveLetter == pConfig->storeDrive) continue;
        createMergeWriterDirectoryName(d->driveLetter, pConfig->storeDirNameNoDrive,
                                       pState->mwDirectory[pState->numMergeWriters]);
        pState->numMergeWriters++;
    }

    /* One merge dir per medium drive (intermediate merge destination for NVMe overflow) */
    pState->numMergeDirs = 0;
    for (int i = 0; i < pMachineInfo->g_drives.numDrives; i++)
    {
        const DriveInformation* d = &pMachineInfo->g_drives.drives[i];
        if (!d->available) continue;
        if (d->driveCategory != DRIVE_CAT_MEDIUM) continue;
        if (d->driveLetter == pConfig->storeDrive) continue;
        snprintf(pState->mergeDirectory[pState->numMergeDirs], MAX_FULL_PATH_NAME,
                 "%c:%s\\mergeDir", d->driveLetter, pConfig->storeDirNameNoDrive);
        pState->numMergeDirs++;
    }

    snprintf(pState->storeDirectory, MAX_FULL_PATH_NAME, "%c:%s\\storeDir",
             pConfig->storeDrive, pConfig->storeDirNameNoDrive);
    snprintf(pState->storeMergeDirectory, MAX_FULL_PATH_NAME, "%c:%s\\storeMergeDir",
             pConfig->storeDrive, pConfig->storeDirNameNoDrive);
    pState->storeMergeBlackFileCount = 0;
    pState->storeMergeWhiteFileCount = 0;

    /* Build per-drive stats -- one merge-writer directory per fast drive
    ** (see the loop above), so writerDriveStats[i] always corresponds
    ** one-to-one with mwDirectory[i]; no per-drive grouping needed.
    */
    pState->numWriterDrives = pState->numMergeWriters;
    for (int i = 0; i < pState->numWriterDrives; i++)
    {
        pState->writerDriveStats[i] = {};
        pState->writerDriveStats[i].driveLetter = pState->mwDirectory[i][0];
        pState->writerDriveStats[i].threshold   = DRIVE_SPACE_LOW_BYTES;
    }

    /* GPU accumulator worst-case capacity (boards) -- used by merge-writer
    ** HasRoom check. Mirrors the formula in GpuAccumulatorCreate: per-slot
    ** cost = 57 bytes; expand overhead is batchSize*16 + 8 bytes (d_input +
    ** two atomic counters).
    */
    const size_t gpuBudget    = pMachineInfo->g_gpuInfo.totalGlobalMemBytes * 8 / 10;
    const size_t expandBytes  = (size_t)pMachineInfo->g_gpuInfo.optimalBatchSize
                                * sizeof(UINT64_PAIR) + 2 * sizeof(uint32_t);
    pState->gpuAccumCapacity  = (gpuBudget - expandBytes) / 57;
    pState->mwStagingSize     = pState->gpuAccumCapacity * sizeof(UINT64_PAIR);

    /* Merge-writer buffers: one per thread, sized to fill available RAM (see mwBufSize above). */
    pState->mwBufferSize = mwBufSize;

    /* Segment tracking is implicitly zero-initialized via pState = {}; verify explicitly. */
    for (int i = 0; i < (int)pState->numMergeWriters; i++)
    {
        pState->mwBlackSegCount[i]          = 0;
        pState->mwBlackCompBytesUsed[i]     = 0;
        pState->mwBlackStagingCount[i]      = 0;
        pState->mwBlackSegCountHighWater[i] = 0;
        pState->mwWhiteSegCount[i]          = 0;
        pState->mwWhiteCompBytesUsed[i]     = 0;
        pState->mwWhiteStagingCount[i]      = 0;
        pState->mwWhiteSegCountHighWater[i] = 0;
        pState->mwBlackFileCount[i]     = 0;
        pState->mwWhiteFileCount[i]     = 0;
        pState->mwBlackFilesConsumed[i] = 0;
        pState->mwWhiteFilesConsumed[i] = 0;
        pState->mwBlackConsolidatedUpTo[i] = 0;
        pState->mwWhiteConsolidatedUpTo[i] = 0;
        pState->mwNextFileIdx[i][RSF_PLAYER_BLACK] = 0;
        pState->mwNextFileIdx[i][RSF_PLAYER_WHITE] = 0;
        for (int s = 0; s < CONSOLIDATION_THREADS_PER_PAIR; s++)
        {
            pState->consolSlotOwner[i][RSF_PLAYER_BLACK][s] = 0;
            pState->consolSlotOwner[i][RSF_PLAYER_WHITE][s] = 0;
        }
    }

    double totalAllocGB = (pState->pingPongBufferSize
                           + mwBufSize * (size_t)pState->numMergeWriters)
                          / (1024.0 * 1024.0 * 1024.0);
    LoggerLog("Allocating %.1f GB of buffers...\n", totalAllocGB);

    pState->pPingPongBuffer = MemMalloc("pingPongBuffer", pState->pingPongBufferSize);
    if (!pState->pPingPongBuffer)
        Fatal(FATAL_ALLOCATION_FAILED,
              "computeState: cannot allocate ping-pong buffer (%zu bytes)",
              pState->pingPongBufferSize);

    for (int i = 0; i < pState->numMergeWriters; i++)
    {
        pState->pMWBuffer[i] = MemMalloc("mwBuffer", pState->mwBufferSize);
        if (!pState->pMWBuffer[i])
            Fatal(FATAL_ALLOCATION_FAILED,
                  "computeState: cannot allocate merge-writer buffer %d (%zu bytes)",
                  i, pState->mwBufferSize);
    }

    /* Background-consolidation live-progress slots -- MemMalloc'd (not a
    ** MAX_WRITERS-sized fixed array baked into the struct) and sized off the
    ** real runtime numMergeWriters, so both consolidation threads and the
    ** stats thread reach the same shared memory via pConsolSlotStats/
    ** ConsolSlot() (OthelloTypes.h).
    */
    size_t numConsolSlots = (size_t)pState->numMergeWriters * 2 * CONSOLIDATION_THREADS_PER_PAIR;
    pState->pConsolSlotStats = (ConsolidationSlotStats*)MemMalloc("consolSlotStats",
                                   numConsolSlots * sizeof(ConsolidationSlotStats));
    if (!pState->pConsolSlotStats)
        Fatal(FATAL_ALLOCATION_FAILED,
              "computeState: cannot allocate %zu consolidation stats slots", numConsolSlots);

    LoggerLog("Allocation complete.\n");
}

/*
** Function: deletePlayerOutputFile
** @brief    Deletes every on-disk form of one level/player's output -- the
**           ring nested-index file set (.cellsinuse/.ring1/.ring2/.ring34,
**           the current store format -- .ring1/.ring2 only exist for board
**           sizes that use them, see RingNestedIndexHasRing1/HasRing2) and
**           any legacy flat file (.rsf/.rsfz/.rsfzl, from a store produced
**           before the nested-index format existed) -- without validating
**           any of it first.
** @details  Exact board size only (not a wildcard) -- must never touch
**           another board size's files sharing the same storeDir.
** @param    storeDir  - store directory to search
** @param    level     - level to purge
** @param    boardSize - exact board size (never a wildcard)
** @param    player    - "black" or "white"
*/
static void deletePlayerOutputFile(const char* storeDir, int level, int boardSize, const char* player)
{
    int  playerCode = (strcmp(player, "black") == 0) ? RSF_PLAYER_BLACK : RSF_PLAYER_WHITE;
    bool hasRing1   = RingNestedIndexHasRing1(boardSize);
    bool hasRing2   = RingNestedIndexHasRing2(boardSize);

    char cellsInUsePath[MAX_FULL_PATH_NAME];
    char ring1Path[MAX_FULL_PATH_NAME];
    char ring2Path[MAX_FULL_PATH_NAME];
    char ring34Path[MAX_FULL_PATH_NAME];
    RSFNameCellsInUseFile(cellsInUsePath, sizeof(cellsInUsePath), storeDir, boardSize, level, playerCode, 0);
    if (hasRing1)
        RSFNameRing1File(ring1Path, sizeof(ring1Path), storeDir, boardSize, level, playerCode, 0);
    if (hasRing2)
        RSFNameRing2File(ring2Path, sizeof(ring2Path), storeDir, boardSize, level, playerCode, 0);
    RSFNameRing34File(ring34Path, sizeof(ring34Path), storeDir, boardSize, level, playerCode, 0);

    const char* nestedPaths[4] = { cellsInUsePath, hasRing1 ? ring1Path : nullptr,
                                   hasRing2 ? ring2Path : nullptr, ring34Path };
    for (int i = 0; i < 4; i++)
    {
        if (nestedPaths[i] && GetFileAttributesA(nestedPaths[i]) != INVALID_FILE_ATTRIBUTES)
        {
            LoggerLog("  Deleting partial output '%s'\n", nestedPaths[i]);
            DeleteFileA(nestedPaths[i]);
        }
    }

    static const char* exts[] = { "rsf", "rsfz", "rsfzl" };
    for (int e = 0; e < 3; e++)
    {
        char pattern[MAX_FULL_PATH_NAME];
        snprintf(pattern, sizeof(pattern), "%s\\Level_%04d_%dx%d_%s_0000.%s",
                 storeDir, level, boardSize, boardSize, player, exts[e]);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        FindClose(h);
        char fullPath[MAX_FULL_PATH_NAME];
        snprintf(fullPath, sizeof(fullPath), "%s\\%s", storeDir, fd.cFileName);
        LoggerLog("  Deleting partial output '%s'\n", fullPath);
        DeleteFileA(fullPath);
        break;
    }
}

/*
** Function: checkLevelFile
** @brief    Probes for one level/player's output, checking the ring
**           nested-index file set (.cellsinuse/.ring1/.ring2/.ring34, the
**           current store format -- .ring1/.ring2 only apply to board
**           sizes that use them) first, falling back to a legacy flat
**           Level_NNNN_WxH_<player>_0000.rsf[z][l] (from a store produced
**           before the nested-index format existed). Exact board size only
**           (not a wildcard -- a storeDir must never have another board
**           size's files touched). Any corrupt/partial find is deleted in
**           place via deletePlayerOutputFile.
** @param    storeDir  - store directory to probe
** @param    level     - level to probe
** @param    boardSize - exact board size (never a wildcard)
** @param    player    - "black" or "white"
** @return   LFS_VALID / LFS_CORRUPT (file(s) existed but were deleted) / LFS_ABSENT.
*/
static LevelFileStatus checkLevelFile(const char* storeDir, int level, int boardSize, const char* player)
{
    int  playerCode = (strcmp(player, "black") == 0) ? RSF_PLAYER_BLACK : RSF_PLAYER_WHITE;
    bool hasRing1   = RingNestedIndexHasRing1(boardSize);
    bool hasRing2   = RingNestedIndexHasRing2(boardSize);

    char cellsInUsePath[MAX_FULL_PATH_NAME];
    char ring1PathBuf[MAX_FULL_PATH_NAME];
    char ring2PathBuf[MAX_FULL_PATH_NAME];
    char ring34Path[MAX_FULL_PATH_NAME];
    RSFNameCellsInUseFile(cellsInUsePath, sizeof(cellsInUsePath), storeDir, boardSize, level, playerCode, 0);
    if (hasRing1)
        RSFNameRing1File(ring1PathBuf, sizeof(ring1PathBuf), storeDir, boardSize, level, playerCode, 0);
    if (hasRing2)
        RSFNameRing2File(ring2PathBuf, sizeof(ring2PathBuf), storeDir, boardSize, level, playerCode, 0);
    RSFNameRing34File(ring34Path, sizeof(ring34Path), storeDir, boardSize, level, playerCode, 0);

    const char* ring1Path      = hasRing1 ? ring1PathBuf : nullptr;
    const char* ring2Path      = hasRing2 ? ring2PathBuf : nullptr;
    int         expectedCount  = 2 + (hasRing1 ? 1 : 0) + (hasRing2 ? 1 : 0);

    int nestedFoundCount = RingNestedIndexFileCount(cellsInUsePath, ring1Path, ring2Path, ring34Path);

    if (nestedFoundCount > 0)
    {
        /* Streamed validation only -- a resume scan runs at solver startup
        ** and must never hold a whole level resident just to check the
        ** files read cleanly (RingNestedIndexReader::Load() would buffer
        ** every record; RingNestedIndexStreamAll walks the same lockstep
        ** structure with a no-op callback instead).
        */
        if (nestedFoundCount == expectedCount &&
            RingNestedIndexStreamAll(cellsInUsePath, ring1Path, ring2Path, ring34Path, [](const BOARD_KEY&) {}))
            return LFS_VALID;

        LoggerLog("ScanForResumeLevel: corrupt/partial level %d %s nested-index files, deleting\n",
                  level, player);
        deletePlayerOutputFile(storeDir, level, boardSize, player);
        return LFS_CORRUPT;
    }

    static const char* exts[] = { "rsf", "rsfz", "rsfzl" };
    for (int e = 0; e < 3; e++)
    {
        char pattern[MAX_FULL_PATH_NAME];
        snprintf(pattern, sizeof(pattern), "%s\\Level_%04d_%dx%d_%s_0000.%s",
                 storeDir, level, boardSize, boardSize, player, exts[e]);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        FindClose(h);
        char flatPath[MAX_FULL_PATH_NAME];
        snprintf(flatPath, sizeof(flatPath), "%s\\%s", storeDir, fd.cFileName);
        RSFReader* r = RSFOpen(flatPath);
        if (!r)
        {
            LoggerLog("ScanForResumeLevel: corrupt level %d %s file, deleting '%s'\n",
                      level, player, flatPath);
            DeleteFileA(flatPath);
            return LFS_CORRUPT;
        }
        RSFClose(&r);
        return LFS_VALID;
    }
    return LFS_ABSENT;
}

/*
** Type:    LevelStatsPreConsolidation
** @brief   Frozen, byte-for-byte copy of LevelStats as it existed for every
**          sentinel written before 2026-07-23 (v0.32.0), when background
**          consolidation inserted 3 uint64_t fields
**          (consolidationFilesCreated/Removed/BytesWritten) in the middle
**          of the live struct. Every real sentinel for the current
**          production run's levels 1-22 was written against this exact
**          layout. Never add fields here or otherwise evolve it -- it
**          exists solely so ReadSentinelStats can still translate those
**          pre-existing files; if LevelStats changes shape again later,
**          add another frozen snapshot rather than touching this one.
*/
typedef struct __LevelStatsPreConsolidation
{
    uint64_t boardsReadFromStore;
    uint64_t boardsGenerated;
    uint64_t gpuDupsRemoved;
    uint64_t gpuFlushes;
    uint64_t boardsReceivedFromGpu;
    uint64_t boardsWrittenToDisk;
    uint64_t mwFilesCreated;
    uint64_t mwBytes;
    uint64_t mrgDupsRemoved;
    uint32_t mergeFilesWritten;
    uint64_t mergeBytes;
    uint64_t mergeActualBytes;
    uint64_t passBoards;
    uint64_t terminalBoards;
    uint32_t maxMovesInLevel;
    ClockTick startTick;
    int64_t  solverNanos;
    int64_t  totalNanos;
    char     completedAt[24];
    WriterDriveStats driveSnapshot[MAX_WRITER_DRIVES];
    int      numDriveSnapshot;
    uint64_t storeFreeBytes;
} LevelStatsPreConsolidation;

/*
** Function: LevelStatsFromPreConsolidation
** @brief    Field-by-field translation from the frozen pre-2026-07-23
**           layout into the current LevelStats shape. The 3 consolidation
**           counters didn't exist yet in the source, so they come out
**           zeroed -- correct, since no consolidation ever ran on those
**           levels.
*/
static void LevelStatsFromPreConsolidation(const LevelStatsPreConsolidation* src, LevelStats* dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->boardsReadFromStore   = src->boardsReadFromStore;
    dst->boardsGenerated       = src->boardsGenerated;
    dst->gpuDupsRemoved        = src->gpuDupsRemoved;
    dst->gpuFlushes            = src->gpuFlushes;
    dst->boardsReceivedFromGpu = src->boardsReceivedFromGpu;
    dst->boardsWrittenToDisk   = src->boardsWrittenToDisk;
    dst->mwFilesCreated        = src->mwFilesCreated;
    dst->mwBytes               = src->mwBytes;
    dst->mrgDupsRemoved        = src->mrgDupsRemoved;
    dst->mergeFilesWritten     = src->mergeFilesWritten;
    dst->mergeBytes            = src->mergeBytes;
    dst->mergeActualBytes      = src->mergeActualBytes;
    dst->passBoards            = src->passBoards;
    dst->terminalBoards        = src->terminalBoards;
    dst->maxMovesInLevel       = src->maxMovesInLevel;
    dst->startTick             = src->startTick;
    dst->solverNanos           = src->solverNanos;
    dst->totalNanos            = src->totalNanos;
    memcpy(dst->completedAt, src->completedAt, sizeof(dst->completedAt));
    memcpy(dst->driveSnapshot, src->driveSnapshot, sizeof(dst->driveSnapshot));
    dst->numDriveSnapshot      = src->numDriveSnapshot;
    dst->storeFreeBytes        = src->storeFreeBytes;
}

/*
** Function: ReadSentinelStats
** @brief    Reads LevelStats from a _complete sentinel file.
** @param    path - sentinel file path
** @param    out  - out: filled with the sentinel's LevelStats payload
** @return   false if the file is zero-byte (legacy / manually created) or
**           does not contain valid, recognized stats data.
** @details  Compares the file's actual payload size against sizeof(LevelStats)
**           rather than assuming today's shape is the only one on disk --
**           sentinels older than 2026-07-23 (every real level 1-22 sentinel
**           from the current production run) were written against
**           LevelStatsPreConsolidation, 24 bytes smaller. Those are
**           translated explicitly rather than rejected, so a struct growth
**           doesn't silently blank out already-completed levels' history.
*/
static bool ReadSentinelStats(const char* path, LevelStats* out)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER fileSize = {};
    bool ok = GetFileSizeEx(h, &fileSize) != 0;

    uint64_t magic = 0;
    DWORD    nr    = 0;
    ok = ok
         && ReadFile(h, &magic, (DWORD)sizeof(magic), &nr, NULL)
         && nr == sizeof(magic)
         && magic == RSF_SENTINEL_STATS_MAGIC;

    if (ok)
    {
        int64_t payloadBytes = fileSize.QuadPart - (int64_t)sizeof(magic);
        if (payloadBytes == (int64_t)sizeof(*out))
        {
            ok = ReadFile(h, out, (DWORD)sizeof(*out), &nr, NULL) && nr == sizeof(*out);
        }
        else if (payloadBytes == (int64_t)sizeof(LevelStatsPreConsolidation))
        {
            LevelStatsPreConsolidation old = {};
            ok = ReadFile(h, &old, (DWORD)sizeof(old), &nr, NULL) && nr == sizeof(old);
            if (ok)
                LevelStatsFromPreConsolidation(&old, out);
        }
        else
        {
            ok = false;
        }
    }

    CloseHandle(h);
    return ok;
}

/*
** Function: ScanForResumeLevel
** @brief    Sentinel-aware scan for the first level not yet fully written,
**           purging any interrupted level's partial output along the way.
** @details  For each level:
**             _complete present               -> level fully written; continue to next.
**                                                 If sentinel contains stats payload,
**                                                 restore levelStats[level-1] for history display.
**             _merging present (no _complete) -> DoEndOfLevelMerge was interrupted;
**                                                 delete sentinel + any player files;
**                                                 resume from this level.
**             Neither sentinel, no player files -> level is missing; resume from here.
**             Neither sentinel, corrupt file     -> delete all for this level; re-run.
**             Neither sentinel, valid file(s)    -> backwards-compat: treat as complete
**                                                    (old data without sentinels -- add
**                                                    manually: type nul > Level_NNNN_complete).
** @param    pState    - solver state (storeDirectory; levelStats restored into on hit)
** @param    boardSize - exact board size being run
** @return   Index of the first level not found (MAX_LEVELS if every level is present).
*/
static int ScanForResumeLevel(POthelloRingMasterState pState, int boardSize)
{
    for (int level = 0; level < MAX_LEVELS; level++)
    {
        char sentPath[MAX_FULL_PATH_NAME];

        /* Fast path: complete sentinel -> level done; try to restore stats. */
        SentinelNameComplete(sentPath, sizeof(sentPath), pState->storeDirectory, boardSize, level);
        if (GetFileAttributesA(sentPath) != INVALID_FILE_ATTRIBUTES)
        {
            if (level > 0)
            {
                LevelStats restored = {};
                if (ReadSentinelStats(sentPath, &restored))
                    pState->levelStats[level - 1] = restored;
            }
            continue;
        }

        /* Merging sentinel -> interrupted mid-merge; purge partial output and re-run. */
        SentinelNameMerging(sentPath, sizeof(sentPath), pState->storeDirectory, boardSize, level);
        if (GetFileAttributesA(sentPath) != INVALID_FILE_ATTRIBUTES)
        {
            LoggerLog("ScanForResumeLevel: level %d merge was interrupted; purging partial output\n", level);
            DeleteFileA(sentPath);
            deletePlayerOutputFile(pState->storeDirectory, level, boardSize, "black");
            deletePlayerOutputFile(pState->storeDirectory, level, boardSize, "white");
            return level;
        }

        /* No sentinels: check for player files. */
        LevelFileStatus bs = checkLevelFile(pState->storeDirectory, level, boardSize, "black");
        LevelFileStatus ws = checkLevelFile(pState->storeDirectory, level, boardSize, "white");

        if (bs == LFS_ABSENT && ws == LFS_ABSENT)
            return level;

        if (bs == LFS_CORRUPT || ws == LFS_CORRUPT)
        {
            if (bs == LFS_VALID) { LoggerLog("  Deleting valid level %d black alongside corrupt white\n", level); deletePlayerOutputFile(pState->storeDirectory, level, boardSize, "black"); }
            if (ws == LFS_VALID) { LoggerLog("  Deleting valid level %d white alongside corrupt black\n", level); deletePlayerOutputFile(pState->storeDirectory, level, boardSize, "white"); }
            return level;
        }

        /* Valid file(s), no sentinel -- old pre-sentinel data; treat as complete. */
    }
    return MAX_LEVELS;
}

/*
** Function: cleanUpDrives
** @brief    Purges every ephemeral working directory (merge-writer dirs,
**           merge dirs, store-merge dir) from a previous run. storeDir
**           itself is never purged -- it holds the permanent level output archive.
** @param    pState       - solver state (directories to purge, resumeLevel for logging)
** @param    pMachineInfo - refreshed in place after the purge frees space
*/
static void cleanUpDrives(POthelloRingMasterState pState, PMachineInfo pMachineInfo)
{
    LoggerLog("Purging previous run data...\n");

    for (int i = 0; i < pState->numMergeWriters; i++)
    {
        if (GetFileAttributesA(pState->mwDirectory[i]) == INVALID_FILE_ATTRIBUTES) continue;
        LoggerLog("  Deleting merge-writer dir: %s\n", pState->mwDirectory[i]);
        DeleteDirRecursive(pState->mwDirectory[i]);
    }

    for (int i = 0; i < pState->numMergeDirs; i++)
    {
        if (GetFileAttributesA(pState->mergeDirectory[i]) == INVALID_FILE_ATTRIBUTES) continue;
        LoggerLog("  Deleting merge dir: %s\n", pState->mergeDirectory[i]);
        DeleteDirRecursive(pState->mergeDirectory[i]);
    }

    if (GetFileAttributesA(pState->storeMergeDirectory) != INVALID_FILE_ATTRIBUTES)
    {
        LoggerLog("  Deleting store merge dir: %s\n", pState->storeMergeDirectory);
        DeleteDirRecursive(pState->storeMergeDirectory);
    }

    if (pState->resumeLevel > 0)
        LoggerLog("  Resuming from level %d (levels 0..%d already in store).\n",
                  pState->resumeLevel, pState->resumeLevel - 1);
    else
        LoggerLog("  Store dir kept (fresh run or resuming from level 0).\n");

    RefreshDriveFreeSpace(&pMachineInfo->g_drives);
    LoggerLog("Purge complete.\n");
}

/*
** Function: createDirectories
** @brief    Creates every working directory (merge-writer dirs, merge dirs,
**           store-merge dir, store dir) fresh after the purge.
** @param    pState - solver state (directories to create)
*/
static void createDirectories(POthelloRingMasterState pState)
{
    for (int i = 0; i < pState->numMergeWriters; i++)
        if (!CreateFullPath(pState->mwDirectory[i]))
            Fatal(FATAL_CREATE_DIR_FAILED, "Cannot create merge-writer directory '%s'",
                  pState->mwDirectory[i]);

    for (int i = 0; i < pState->numMergeDirs; i++)
        if (!CreateFullPath(pState->mergeDirectory[i]))
            Fatal(FATAL_CREATE_DIR_FAILED, "Cannot create merge directory '%s'",
                  pState->mergeDirectory[i]);

    if (!CreateFullPath(pState->storeMergeDirectory))
        Fatal(FATAL_CREATE_DIR_FAILED, "Cannot create store merge directory '%s'",
              pState->storeMergeDirectory);

    if (!CreateFullPath(pState->storeDirectory))
        Fatal(FATAL_CREATE_DIR_FAILED, "Cannot create store directory '%s'",
              pState->storeDirectory);
}

/*
** Function: InitSolver
** @brief    Runs the full one-time startup sequence -- see InitSolver.h for details.
** @param    pConfig      - run configuration
** @param    pState       - out: fully initialized solver state
** @param    pMachineInfo - out: filled with probed machine information
*/
void InitSolver(POthelloRingMasterConfig pConfig, POthelloRingMasterState pState,
                PMachineInfo pMachineInfo)
{
    _setmaxstdio(4000);   /* k-way merge opens up to MAX_MERGE_FANIN files simultaneously */
    SetBoardSizeForRun(pConfig->boardSize);

    for (const char* p = pConfig->useDrives; *p; p++)
    {
        char root[4] = { *p, ':', '\\', '\0' };
        if (GetDriveTypeA(root) == DRIVE_REMOTE) continue;
        SHEmptyRecycleBinA(nullptr, root, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
    }

    GetMachineInfo(pConfig->cacheDirName, pConfig->useDrives, pConfig->memoryLimitBytes, pMachineInfo);
    computeState(pConfig, pState, pMachineInfo);

    /* ScanForResumeLevel returns the index of the first missing store file.
    ** Iteration N reads Level_N and writes Level_N+1, so if Level_N+1 is the
    ** first missing file we need to re-run iteration N (= firstMissingFile - 1).
    */
    int firstMissingFile = ScanForResumeLevel(pState, (int)pConfig->boardSize);
    pState->resumeLevel  = (firstMissingFile > 0) ? firstMissingFile - 1 : 0;
    cleanUpDrives(pState, pMachineInfo);
    createDirectories(pState);

    /* Initialize drive space ledgers after cleanup so we start from clean
    ** free space. Each ledger is seeded with (OS free bytes - safety buffer).
    */
    for (int i = 0; i < pState->numMergeWriters; i++)
        DriveInitLedger(pState, pState->mwDirectory[i][0]);
    for (int i = 0; i < pState->numMergeDirs; i++)
        DriveInitLedger(pState, pState->mergeDirectory[i][0]);
    DriveInitLedger(pState, pConfig->storeDrive);

    int numMWThreads        = pState->numMergeWriters;
    int numStatsThreads     = 1;

    /* Exactly one feeder thread: GpuAccumulatorCreate makes one accumulator
    ** owning all GPU device buffers for the whole level, so there is only
    ** ever one thread to size regardless of the GPU's own hardware
    ** concurrency (async engine count, etc.) -- if a future design ever
    ** needs more than one, that's a decision this project makes from its
    ** own requirements, not something a generic GPU capability query can
    ** answer on its behalf.
    */
    int numGPUFeederThreads = 1;

    InitializeCriticalSection(&pState->imergeCS);
    for (int wi = 0; wi < MAX_WRITERS; wi++)
        for (int p = 0; p < 2; p++)
            InitializeCriticalSection(&pState->claimRegistry[wi][p].cs);

    pState->pMergeWriterPool = new ThreadPool(numMWThreads, "MergeWriterPool");
    if (!pState->pMergeWriterPool)
        Fatal(FATAL_ALLOCATION_FAILED, "InitSolver: cannot create merge-writer thread pool");

    pState->pGPUFeederThreadPool = new ThreadPool(numGPUFeederThreads, "GPUFeederThreadPool");
    if (!pState->pGPUFeederThreadPool)
        Fatal(FATAL_ALLOCATION_FAILED, "InitSolver: cannot create GPU feeder thread pool");

    pState->pStatsThreadPool = new ThreadPool(numStatsThreads, "StatsThreadPool");
    if (!pState->pStatsThreadPool)
        Fatal(FATAL_ALLOCATION_FAILED, "InitSolver: cannot create stats thread pool");

    /* Background small-file consolidator: ONE shared pool, CONSOLIDATION_POOL_THREADS
    ** threads, servicing every (writer, color) pair's examination jobs --
    ** deliberately flat-sized (not scaled by numMWThreads/drive count) so
    ** adding more NVMe drives never grows total thread count. Per-pair
    ** concurrency is capped separately via consolSlotOwner (OthelloTypes.h).
    ** A SEPARATE pool from pMergeWriterPool so it draws from otherwise-idle
    ** cores and never competes with active flush-writing. See
    ** DoBackgroundConsolidation (MergeFiles.cpp) and
    ** project_background_nvme_consolidation_design memory.
    */
    pState->pConsolidationPool = new ThreadPool(CONSOLIDATION_POOL_THREADS, "ConsolidationPool");
    if (!pState->pConsolidationPool)
        Fatal(FATAL_ALLOCATION_FAILED, "InitSolver: cannot create consolidation thread pool");

    AcquireInstanceLock(pState->storeDirectory);

    pState->pMergeWriterPool->Start();
    pState->pGPUFeederThreadPool->Start();
    pState->pStatsThreadPool->Start();
    pState->pConsolidationPool->Start();

    /* Block until every worker thread in all four pools is genuinely
    ** running (not just constructed) before the solve loop starts
    ** dispatching jobs -- otherwise the very first level's timing would
    ** silently include an unpredictable amount of thread-spin-up noise.
    */
    pState->pMergeWriterPool->WaitUntilReady();
    pState->pGPUFeederThreadPool->WaitUntilReady();
    pState->pStatsThreadPool->WaitUntilReady();
    pState->pConsolidationPool->WaitUntilReady();

    int lastLevel = (int)pConfig->boardSize * (int)pConfig->boardSize - 4;
    LoggerLog("\nSolver configuration:\n");
    LoggerLog("  Board size         : %dx%d  (levels 0..%d)\n",
              pConfig->boardSize, pConfig->boardSize, lastLevel);
    LoggerLog("  MW threads         : %d\n", numMWThreads);
    LoggerLog("  Consolidation thrds: %d shared  (cap %d per drive/color pair, size cap %llu GB)\n",
              CONSOLIDATION_POOL_THREADS, CONSOLIDATION_THREADS_PER_PAIR,
              (unsigned long long)CONSOLIDATION_SIZE_CAP_GB);
    LoggerLog("  GPU threads        : %d\n", numGPUFeederThreads);
    LoggerLog("  Stats port         : %d\n", (int)pConfig->statsPort);
    LoggerLog("  Store format       : %s\n",
              pConfig->compressMode == COMPRESS_ALL        ? "all files .rsfz (delta+varint compressed)" :
              pConfig->compressMode == COMPRESS_STORE_ONLY ? "store .rsfz, MW/imerge .rsf" :
                                                             "all files .rsf (uncompressed)");
    if (pConfig->compressMode == COMPRESS_ALL && pConfig->lz4Drives[0])
        LoggerLog("  LZ4 drives         : %s (varint+LZ4 -> .rsfzl)\n", pConfig->lz4Drives);
    else if (pConfig->compressMode == COMPRESS_ALL)
        LoggerLog("  LZ4 drives         : (none)\n");
    LoggerLog("  Ping-pong buf      : %.1f MB\n",
              pState->pingPongBufferSize / (1024.0 * 1024.0));
    LoggerLog("  MW buf             : %.1f GB x %d threads\n",
              pState->mwBufferSize / (1024.0 * 1024.0 * 1024.0), pState->numMergeWriters);
    LoggerLog("  GPU accum capacity : %zu boards\n", pState->gpuAccumCapacity);
    LoggerLog("  Merge-writer dirs:\n");
    for (int i = 0; i < pState->numMergeWriters; i++)
        LoggerLog("    [%d] %s\n", i, pState->mwDirectory[i]);
    LoggerLog("  Merge dirs:\n");
    for (int i = 0; i < pState->numMergeDirs; i++)
        LoggerLog("    [%d] %s\n", i, pState->mergeDirectory[i]);
    LoggerLog("  Store merge dir    : %s\n", pState->storeMergeDirectory);
    LoggerLog("  Store dir          : %s\n", pState->storeDirectory);
    if (pState->resumeLevel > 0)
        LoggerLog("  ** Resuming from level %d (levels 0..%d already stored)\n",
                  pState->resumeLevel, pState->resumeLevel - 1);
    LoggerLog("\n");
}

/*
** Function: CleanupSolver
** @brief    Releases the instance lock, stops and frees all four thread
**           pools, frees every large buffer, and destroys the imerge critical section.
** @param    pState - the solver state to tear down
*/
void CleanupSolver(POthelloRingMasterState pState)
{
    ReleaseInstanceLock();
    pState->terminateThreads       = true;
    pState->terminateConsolidation = true;
    pState->pConsolidationPool->Stop();
    delete pState->pConsolidationPool;
    pState->pMergeWriterPool->Stop();
    delete pState->pMergeWriterPool;
    pState->pGPUFeederThreadPool->Stop();
    delete pState->pGPUFeederThreadPool;
    pState->terminateStatsListener = true;
    pState->pStatsThreadPool->Stop();
    delete pState->pStatsThreadPool;

    for (int i = 0; i < pState->numMergeWriters; i++)
        MemFree(pState->pMWBuffer[i]);
    MemFree(pState->pPingPongBuffer);
    MemFree(pState->pConsolSlotStats);
    DeleteCriticalSection(&pState->imergeCS);
    for (int wi = 0; wi < MAX_WRITERS; wi++)
        for (int p = 0; p < 2; p++)
            DeleteCriticalSection(&pState->claimRegistry[wi][p].cs);
}
