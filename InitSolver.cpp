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
#include "Registry.h"
#include "ConsolidationMaster.h"
#include "RegistryAuditor.h"
#include "DriveSpaceAuditor.h"
#include "OthelloBasicsForCUDA.h"
#include "RingNestedIndex.h"
#include "Checkpoint.h"
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

    /* Segment tracking is implicitly zero-initialized via pState = {}; verify explicitly.
    ** Registry init (list + lock + naming counter) happens separately in
    ** InitSolver itself, after this function returns -- RegistryInit needs
    ** nothing from computeState beyond numMergeWriters being finalized,
    ** which it already is by this point.
    */
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

/* LevelStatsPreConsolidation, LevelStatsFromPreConsolidation, and the
** backward-compatible sentinel reader itself now live in OthelloTypes.h as
** ReadSentinelLevelStats -- shared with OthelloRingMasterStoreStats'
** StoreStatsScan.cpp, which had its own separate (and, until now, not
** backward-compatible) sentinel read. See OthelloTypes.h's comment on
** ReadSentinelLevelStats for why this moved out of here.
*/

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
                if (ReadSentinelLevelStats(sentPath, &restored))
                    pState->levelStats[level - 1] = restored;
            }
            continue;
        }

        /* Merging sentinel -> the end-of-level merge was interrupted. The solve
        ** is durably complete and every merge input was preserved on disk (see
        ** DoEndOfLevelMerge), so re-run ONLY the merge (merge-resume) instead of
        ** re-solving the whole level: restore this level's solve counters from
        ** the sentinel's payload and flag it. Purge the partial store output so
        ** the re-merge writes fresh; the _merging sentinel is rewritten when the
        ** re-merge starts. (`level` is the STORE level being written; the
        ** iteration that produces it is level-1, which becomes resumeLevel via
        ** the firstMissing-1 mapping in the caller.)
        */
        SentinelNameMerging(sentPath, sizeof(sentPath), pState->storeDirectory, boardSize, level);
        if (GetFileAttributesA(sentPath) != INVALID_FILE_ATTRIBUTES)
        {
            LevelStats restored = {};
            if (level > 0 && ReadSentinelLevelStats(sentPath, &restored))
            {
                pState->resumeLevelStats = restored;
                pState->resumeIntoMerge  = true;
                LoggerLog("ScanForResumeLevel: level %d merge was interrupted; will re-run ONLY the merge (merge-resume), no re-solve\n", level);
            }
            else
            {
                /* No usable stats payload (e.g. a pre-this-version zero-byte
                ** _merging sentinel) -- fall back to the old behavior: full
                ** re-solve of the level. */
                LoggerLog("ScanForResumeLevel: level %d merge was interrupted but _merging carries no stats payload -- falling back to a full re-solve\n", level);
            }
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
** @param    preserveWriterAndMergeDirs - true when a validated mid-level
**           checkpoint exists for resumeLevel (Checkpoint.h) -- skips
**           wiping mwDirectory/mergeDirectory so real, already-flushed
**           writer/imerge files survive to be resumed from. storeMergeDirectory
**           is always purged regardless -- irrelevant this early (real merge
**           output staging only begins once the solve phase actually
**           finishes), so there's nothing there worth preserving.
*/
static void cleanUpDrives(POthelloRingMasterState pState, PMachineInfo pMachineInfo,
                          bool preserveWriterAndMergeDirs)
{
    LoggerLog("Purging previous run data...\n");

    if (preserveWriterAndMergeDirs)
    {
        LoggerLog("  Preserving merge-writer/merge dirs -- valid mid-level checkpoint found for level %d.\n",
                  pState->resumeLevel);
    }
    else
    {
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
    }

    /* The store-drive fallback merge dir must be preserved on a valid-checkpoint
    ** restart too: space-relief iMerges spill here when the medium drives are
    ** full, so it can legitimately hold recorded checkpoint output. Wiping it
    ** unconditionally (as this did before) was a real data-loss path. On a
    ** fresh/non-checkpoint start it is always purged, same as the writer/merge
    ** dirs above. */
    if (!preserveWriterAndMergeDirs &&
        GetFileAttributesA(pState->storeMergeDirectory) != INVALID_FILE_ATTRIBUTES)
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
    _setmaxstdio(4000);   /* k-way merge opens up to MAX_MERGE_INPUT_FILES files simultaneously */
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

    /* Mid-level checkpoint check (Checkpoint.h): if resumeLevel has a valid,
    ** validated checkpoint, preserve the writer/merge dirs instead of the
    ** usual wholesale wipe, and remember exactly where to resume the input
    ** stream from -- consumed once by RunGpuFeederJob for this specific
    ** level, then cleared. Any invalid/stale checkpoint is deleted outright
    ** so a later attempt at this same level can never pick it up by mistake.
    */
    SolveContext tempCtx = { pConfig, pState, pMachineInfo };

    /* Merge-resume (flagged by ScanForResumeLevel when this level's end-of-level
    ** merge was interrupted): the level's SOLVE is already durably complete and
    ** every merge input was preserved on disk, so we re-run only the merge. The
    ** mid-solve checkpoint path is skipped entirely -- any leftover checkpoint
    ** for this level is obsolete (the solve finished) and must not be mistaken
    ** for a solve to resume, so delete it. resumeLevelStats was already restored
    ** from the _merging sentinel's payload in ScanForResumeLevel. Both a valid
    ** checkpoint and a merge-resume preserve the work dirs and rebuild the
    ** registry/counters from disk below (via `resumeFromDisk`). */
    bool mergeResume         = pState->resumeIntoMerge;
    bool haveValidCheckpoint = false;

    if (mergeResume)
    {
        DeleteLevelCheckpoint(&tempCtx, pState->resumeLevel);
    }
    else
    {
        /* Mid-level checkpoint check (Checkpoint.h): if resumeLevel has a valid,
        ** validated checkpoint, preserve the writer/merge dirs and remember
        ** where to resume the input stream; else delete any stale checkpoint.
        ** CheckpointStats is small now (no manifest); MemMalloc/free kept for
        ** consistency. */
        CheckpointStats* cpPtr = (CheckpointStats*)MemMalloc("checkpointStats", sizeof(CheckpointStats));
        CheckpointStats& cp    = *cpPtr;
        haveValidCheckpoint = ReadValidCheckpoint(&tempCtx, pState->resumeLevel, &cp);
        if (haveValidCheckpoint)
        {
            pState->resumeFromCheckpoint    = true;
            pState->resumeCheckpointSubPass = cp.activeSubPass;
            pState->resumeCheckpointRecords = cp.recordsConsumedInSubPass;
            /* Carry the checkpoint's cumulative-counter snapshot into live state so
            ** RunGpuFeederJob can restore it after the per-level reset -- makes the
            ** resumed level report the FULL level's work (UniqueOut/Generated/
            ** Written/solve%) and the _complete sentinel's numbers correct, not just
            ** the post-resume slice. Copied out before MemFree(cpPtr) below. */
            pState->resumeLevelStats        = cp.levelStatsSnapshot;
            /* Last-record cross-check: after skip-decoding resumeCheckpointRecords,
            ** the feeder must land on exactly this record (FeedBoardIntoBatch), else
            ** the input stream changed under us. */
            pState->resumeLastRecordHi      = cp.lastRecordHi;
            pState->resumeLastRecordLo      = cp.lastRecordLo;
            pState->resumeHaveLastRecord    = cp.haveLastRecord;
            /* Deliberately NOT restoring any file-index counter from the checkpoint
            ** -- CheckpointSeedCountersFromDisk (below) seeds every one from a fresh
            ** disk scan (max-on-disk + 1). That is the crux of the design: post-
            ** checkpoint consolidation can move pre-checkpoint boards into higher-
            ** indexed files, so trusting a recorded lower index would let resumed
            ** writes overwrite them. The registry is likewise rebuilt from a scan. */
        }
        else
        {
            pState->resumeFromCheckpoint = false;
            DeleteLevelCheckpoint(&tempCtx, pState->resumeLevel);
        }
        MemFree(cpPtr);
    }

    /* Both a valid mid-level checkpoint and a merge-resume keep the on-disk work
    ** files and rebuild the registry/counters from a fresh scan of them. */
    bool resumeFromDisk = haveValidCheckpoint || mergeResume;

    cleanUpDrives(pState, pMachineInfo, resumeFromDisk);
    createDirectories(pState);

    /* Resuming from disk: trust it (see Checkpoint.h).
    ** (1) Validate every preserved data file has an intact trailer; crash-
    **     partial files are removed with --forcerestart, else this Fatals with
    **     the list. Must run BEFORE the registry rebuild so partials never enter
    **     the registry or skew the index scan. For merge-resume this validates
    **     the preserved merge inputs before they're re-merged.
    */
    if (resumeFromDisk)
        ValidateCheckpointFilesOnDisk(&tempCtx, pState->resumeLevel, pConfig->forceRestart);

    /* Registry init: one per writer drive. On a fresh start each drive's
    ** registry is simply empty (RegistryInit alone). Resuming from disk (valid
    ** checkpoint or merge-resume) is the case where real files already sit on
    ** disk (cleanUpDrives preserved them) -- rebuild each drive's registry from
    ** a real scan rather than trusting anything persisted.
    */
    for (int i = 0; i < pState->numMergeWriters; i++)
    {
        RegistryInit(pState, i);   /* resets nextFileIdx[i] to 0 */
        if (resumeFromDisk)
            RegistryRebuildFromDisk(pState, i, pState->mwDirectory[i]);
    }

    /* (2) Seed every work dir's next-file index from a disk scan (max-on-disk
    **     + 1), so resumed writes always land above every existing file and can
    **     never overwrite a preserved file. This runs AFTER RegistryInit (which
    **     zeroed nextFileIdx) so it is authoritative.
    */
    if (resumeFromDisk)
        CheckpointSeedCountersFromDisk(&tempCtx, pState->resumeLevel);

    /* Initialize drive space ledgers after cleanup so we start from clean
    ** free space. Each ledger is seeded with (OS free bytes - safety buffer).
    */
    for (int i = 0; i < pState->numMergeWriters; i++)
        DriveInitLedger(pState, pState->mwDirectory[i][0], pConfig->driveSpaceLowGBOverride);
    for (int i = 0; i < pState->numMergeDirs; i++)
        DriveInitLedger(pState, pState->mergeDirectory[i][0]);
    DriveInitLedger(pState, pConfig->storeDrive);

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

    /* Six dedicated pools. pMergeWriterPool keeps its original, narrower
    ** role (routine D2H-copy-then-compress-into-pool for every GPU flush
    ** handoff, sized one thread per writer drive, unchanged from before
    ** this redesign) -- see OthelloTypes.h's field comment. The other five
    ** are new/renamed so housekeeping (flush's real disk write, iMerge,
    ** consolidation) can never starve the GPU feeder's own dependency the
    ** way the old single shared pool (which used to ALSO run consolidation/
    ** iMerge inline) could -- see project_writer_drive_registry_redesign
    ** memory for the incident this replaces. Fixed thread counts for flush/
    ** iMerge/consolidator (OthelloTypes.h), not yet exposed as CLI overrides.
    */
    pState->pMergeWriterPool = new ThreadPool(pState->numMergeWriters, "MergeWriterPool");
    if (!pState->pMergeWriterPool)
        Fatal(FATAL_ALLOCATION_FAILED, "InitSolver: cannot create merge-writer thread pool");

    pState->pGPUFeederThreadPool = new ThreadPool(numGPUFeederThreads, "GPUFeederThreadPool");
    if (!pState->pGPUFeederThreadPool)
        Fatal(FATAL_ALLOCATION_FAILED, "InitSolver: cannot create GPU feeder thread pool");

    pState->pStatsThreadPool = new ThreadPool(numStatsThreads, "StatsThreadPool");
    if (!pState->pStatsThreadPool)
        Fatal(FATAL_ALLOCATION_FAILED, "InitSolver: cannot create stats thread pool");

    pState->pFlusherPool = new ThreadPool(FLUSHER_POOL_THREADS, "FlusherPool");
    if (!pState->pFlusherPool)
        Fatal(FATAL_ALLOCATION_FAILED, "InitSolver: cannot create flusher thread pool");

    pState->pIMergePool = new ThreadPool(IMERGE_POOL_THREADS, "IMergePool");
    if (!pState->pIMergePool)
        Fatal(FATAL_ALLOCATION_FAILED, "InitSolver: cannot create iMerge thread pool");

    pState->pConsolidatorPool = new ThreadPool(CONSOLIDATOR_POOL_THREADS, "ConsolidatorPool");
    if (!pState->pConsolidatorPool)
        Fatal(FATAL_ALLOCATION_FAILED, "InitSolver: cannot create consolidator thread pool");
    /* Master's own free-worker count -- zero-initialized via pState = {},
    ** must start at the real pool size or the master would never dispatch
    ** anything. Also reset per-level (OthelloRingMaster.cpp) since a lost
    ** InterlockedIncrement is impossible but this is cheap insurance
    ** against drift across a very long run.
    */
    pState->consolidatorFreeCount = CONSOLIDATOR_POOL_THREADS;

    AcquireInstanceLock(pState->storeDirectory);

    pState->pMergeWriterPool->Start();
    pState->pGPUFeederThreadPool->Start();
    pState->pStatsThreadPool->Start();
    pState->pFlusherPool->Start();
    pState->pIMergePool->Start();
    pState->pConsolidatorPool->Start();

    /* Block until every worker thread in all six pools is genuinely
    ** running (not just constructed) before the solve loop starts
    ** dispatching jobs -- otherwise the very first level's timing would
    ** silently include an unpredictable amount of thread-spin-up noise.
    */
    pState->pMergeWriterPool->WaitUntilReady();
    pState->pGPUFeederThreadPool->WaitUntilReady();
    pState->pStatsThreadPool->WaitUntilReady();
    pState->pFlusherPool->WaitUntilReady();
    pState->pIMergePool->WaitUntilReady();
    pState->pConsolidatorPool->WaitUntilReady();

    /* The single event-driven consolidation master thread and the two
    ** background auditors are NOT started here -- each needs a stable,
    ** long-lived PSolveContext (ConsolidationMasterLoop/RegistryAuditorLoop/
    ** DriveSpaceAuditorLoop all run for the rest of the process), and the
    ** only such SolveContext lives in OthelloRingMaster.cpp's main(),
    ** constructed right after this function returns -- constructing one
    ** here would either dangle (a local) or leak (a lone heap allocation
    ** with no owner). Same reason SubmitStatsListenerJob is called from
    ** main(), not from in here -- see OthelloRingMaster.cpp, right after
    ** `SolveContext ctx = { &g_config, &g_state, &g_machineInfo };`.
    */

    int lastLevel = (int)pConfig->boardSize * (int)pConfig->boardSize - 4;
    LoggerLog("\nSolver configuration:\n");
    LoggerLog("  Board size         : %dx%d  (levels 0..%d)\n",
              pConfig->boardSize, pConfig->boardSize, lastLevel);
    LoggerLog("  MW (writer) drives : %d\n", pState->numMergeWriters);
    LoggerLog("  Flusher threads    : %d\n", FLUSHER_POOL_THREADS);
    LoggerLog("  iMerge threads     : %d  (drive-space-low %llu GB)\n",
              IMERGE_POOL_THREADS,
              pConfig->driveSpaceLowGBOverride ? (unsigned long long)pConfig->driveSpaceLowGBOverride
                                                : (unsigned long long)DRIVE_SPACE_LOW_GB);
    LoggerLog("  Consolidator thrds : %d  (master: event-driven, size cap %llu GB)\n",
              CONSOLIDATOR_POOL_THREADS,
              pConfig->maxFileSizeGBOverride ? (unsigned long long)pConfig->maxFileSizeGBOverride
                                              : (unsigned long long)CONSOLIDATION_SIZE_CAP_GB);
    LoggerLog("  Audit interval     : %u sec\n",
              pConfig->auditIntervalSecondsOverride ? pConfig->auditIntervalSecondsOverride
                                                      : (unsigned)AUDIT_INTERVAL_SECONDS_DEFAULT);
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
** @brief    Releases the instance lock, stops and frees all six thread
**           pools, stops and joins the consolidation master thread and both
**           background auditor threads, frees every large buffer, and tears
**           down every writer drive's registry lock.
** @param    pState - the solver state to tear down
*/
void CleanupSolver(POthelloRingMasterState pState)
{
    ReleaseInstanceLock();
    pState->terminateThreads       = true;
    pState->terminateConsolidation = true;

    /* Wake the consolidation master explicitly -- it's blocked on a
    ** condition variable, not just polling a flag, so setting
    ** terminateThreads/terminateConsolidation alone would never wake it.
    ** RegistryAuditorLoop/DriveSpaceAuditorLoop are plain sleep-loops and
    ** notice terminateThreads within one sleep interval on their own, no
    ** explicit wake needed.
    */
    {
        std::lock_guard<std::mutex> lock(pState->consolidationMasterMutex);
        pState->consolidationMasterWake = true;
    }
    pState->consolidationMasterCV.notify_all();
    pState->consolidationMasterThread.join();
    pState->registryAuditorThread.join();
    pState->driveSpaceAuditorThread.join();

    pState->pConsolidatorPool->Stop();
    delete pState->pConsolidatorPool;
    pState->pIMergePool->Stop();
    delete pState->pIMergePool;
    pState->pFlusherPool->Stop();
    delete pState->pFlusherPool;
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
    for (int i = 0; i < pState->numMergeWriters; i++)
        RegistryTeardown(pState, i);
}
