/*
** Filename:  OthelloTypes.h
**
** Purpose:
**   Declares the core config/state/stats structures for the live solver:
**   OthelloRingMasterConfig (fixed run configuration), OthelloRingMasterState
**   (all live/mutable solver state -- merge-writer buffers, drive ledgers,
**   per-level stats, thread pools), WriterDriveStats and LevelStats (the
**   per-drive and per-level bookkeeping records each hold).
**
** Notes:
**   Adapted from an earlier solver implementation, renamed onto this
**   solution's own types (-> OthelloRingMasterConfig/State) and updated to
**   reference the RSF record-file format (see Utility/RingStoreFile.h).
**   Field shapes are otherwise kept as-is -- the multi-drive/multi-writer
**   machinery is real functionality this project intends to keep, not
**   architectural cruft to trim.
*/

#pragma once

/* Includes */
#include "Utility.h"
#include "RSFFileName.h"
#include <unordered_set>

/* Macros and Defines */
#define VERSION "0.33.9"

/* Compression mode for RSF output files. */
#define COMPRESS_NONE       0   /* all files uncompressed (.rsf)                              */
#define COMPRESS_STORE_ONLY 1   /* only store output compressed (.rsfz); MW/imerge stay .rsf  */
#define COMPRESS_ALL        2   /* all files compressed (.rsfz)                               */

#define MAX_WRITERS       30
#define MAX_WRITER_DRIVES 26    /* at most one entry per drive letter               */
#define MAX_LEVELS        256   /* covers up to 16x16 board (252 levels)            */

/*
** Max compressed pool segments per merge-writer buffer per color. Just a
** bookkeeping-array bound (mwBlackSegOffset/Size/BoardCount below), not a
** deliberate throughput cap -- the real "is the buffer full" signal is the
** byte-space check in RunMergeWriterJob. Sized with headroom for many
** segments per level; cost is trivial (roughly 40 bytes/segment/color x
** MAX_WRITERS).
**
** Doubled 512->1024 alongside the 2026-07-21 RAM upgrade (26.9GB -> 55.4GB
** per MW thread): segment count scales with buffer byte capacity for a
** given average segment size, and live level-22 data showed segment count
** already at ~244/512 after only 6 minutes with zero flushes yet -- the
** old bound was on track to become the real flush trigger instead of the
** byte-space check, capping the benefit the bigger buffers were added for.
*/
#define MAX_MW_SEGS 1024

/*
** Maximum number of files opened simultaneously for a single-color k-way
** merge. The cross-drive intermediate merge fires when total unconsumed
** writer files across all NVMe drives (per color) reaches this limit; the
** drive-space threshold (DRIVE_SPACE_LOW_BYTES) acts as a safety net when
** individual files are larger than expected. _setmaxstdio must be greater
** than MAX_MERGE_FANIN plus overhead; see InitSolver.cpp.
*/
#define MAX_MERGE_FANIN 3500

/*
** Drive space threshold -- when free bytes on a drive drops below
** DRIVE_SPACE_LOW_BYTES, trigger a merge-to-store flush.
*/
#define DRIVE_SPACE_LOW_GB    20ULL
#define DRIVE_SPACE_LOW_BYTES (DRIVE_SPACE_LOW_GB * 1024ULL * 1024ULL * 1024ULL)

/*
** Background small-file consolidator (TryConsolidatePair, MergeFiles.cpp):
** a file at or above this size is left alone -- not worth the merge cost for
** the marginal dedup reward once it's already this big; the final end-of-level
** merge will handle it. Adjustable constant, not derived from anything else.
*/
#define CONSOLIDATION_SIZE_CAP_GB    100ULL
#define CONSOLIDATION_SIZE_CAP_BYTES (CONSOLIDATION_SIZE_CAP_GB * 1024ULL * 1024ULL * 1024ULL)

/*
** Total worker threads in the single, shared background-consolidation pool
** (pConsolidationPool). Deliberately a FLAT constant, independent of
** numMergeWriters/drive count -- adding more NVMe drives should not require
** more OS threads than this. Each thread runs ConsolidationWorkerLoop
** (MergeFiles.cpp) for the whole level: sleep, sweep every (writer drive,
** color) pair merging whatever's eligible, sleep again. No per-pair
** concurrency cap exists -- ClaimRegistry alone mediates multiple workers
** reaching the same pair in the same sweep, one wins the claim and merges,
** the rest move on to the next pair.
*/
#define CONSOLIDATION_POOL_THREADS 12

/*
** Cap on how many input files one TryConsolidatePair batch can gather in a
** single merge. Shared with ConsolidationSlotStats' batchIndices sizing
** (OthelloTypes.h) so the --consol diagnostic display can address the exact
** same array TryConsolidatePair (MergeFiles.cpp) fills in.
*/
#define MAX_CONSOLIDATION_BATCH 64

/*
** How long each consolidation worker sleeps between sweeps. A worker only
** notices terminateConsolidation (or picks up newly-eligible files) at this
** granularity, so this directly bounds how much extra latency the
** solve->merge transition's WaitForPoolIdle(pConsolidationPool) can incur
** beyond whatever in-flight KWayMergeFiles call is already unwinding.
*/
#define CONSOLIDATION_POLL_INTERVAL_MS 2000

/* Structures and Types */

/*
** Type:    WriterDriveStats
** @brief   Per-drive write bookkeeping for the current level, reset at the
**          start of each level.
*/
typedef struct __WriterDriveStats
{
    char      driveLetter;
    uint64_t  threshold;
    uint64_t  lastFreeBytes;
    uint64_t  levelFilesWritten;
    uint64_t  levelBytesWritten;       /* actual bytes on disk (compressed when COMPRESS_ALL) */
    uint64_t  levelBytesUncompressed;  /* uncompressed equivalent (count * 16 + trailers)     */
} WriterDriveStats, * PWriterDriveStats;

/*
** Type:    LevelStats
** @brief   Everything tracked about one level's solve: input/expansion/dedup
**          counters, merge-phase counters, game-logic counters, timing, and
**          a per-drive snapshot captured at completion.
*/
typedef struct __LevelStats
{
    /* Input */
    uint64_t boardsReadFromStore;

    /* GPU expansion + dedup */
    uint64_t boardsGenerated;
    uint64_t gpuDupsRemoved;
    uint64_t gpuFlushes;

    /* Merge-writer output */
    uint64_t boardsReceivedFromGpu;
    uint64_t boardsWrittenToDisk;
    uint64_t mwFilesCreated;
    uint64_t mwBytes;

    /* Background small-file consolidator (TryConsolidatePair, MergeFiles.cpp) */
    uint64_t consolidationFilesCreated;   /* merged-output files the consolidator produced this level */
    uint64_t consolidationFilesRemoved;   /* original files it absorbed/deleted (folded into those outputs) */
    uint64_t consolidationBytesWritten;   /* real on-disk bytes of consolidator-created files */

    /* Merge phase (populated after merge; 0 until then) */
    uint64_t mrgDupsRemoved;
    uint32_t mergeFilesWritten;   /* store files written this level (0-2; 2 = black + white) */
    uint64_t mergeBytes;          /* real ring-format uncompressed equivalent -- sum of recordCount*width across every ring/cellsinuse file written this level, both colors (see DoEndOfLevelMerge) */
    uint64_t mergeActualBytes;    /* actual bytes written to store drive (compressed if .rsfz) */

    /* Game logic */
    uint64_t passBoards;
    uint64_t terminalBoards;
    uint32_t maxMovesInLevel;

    /* Timing */
    ClockTick startTick;
    int64_t   solverNanos;
    int64_t   totalNanos;
    char      completedAt[24];   /* "YYYY-MM-DD HH:MM:SS" stamped when level finishes */

    /*
    ** Per-drive snapshot captured at level completion. Drives reset each
    ** level, so the history table reads this instead of the live
    ** writerDriveStats.
    */
    WriterDriveStats  driveSnapshot[MAX_WRITER_DRIVES];
    int               numDriveSnapshot;
    uint64_t          storeFreeBytes;   /* free space on store drive at level completion */
} LevelStats, * PLevelStats;

/* Magic value embedded at the start of a mid-level checkpoint file, distinct
** from RSF_SENTINEL_STATS_MAGIC (RSFFileName.h) so the two file kinds can
** never be confused if one is accidentally opened as the other.
*/
#define CHECKPOINT_STATS_MAGIC 0x504B48435053544FULL   /* "OTSPCHKP" in ASCII byte order */

/*
** Type:    CheckpointStats
** @brief   Mid-level checkpoint payload -- exactly enough to resume the GPU
**          feeder from a specific point in a level's input stream instead of
**          from record 0. Written only once the feeder is fully paused, its
**          accumulator drained, and every merge-writer buffer force-flushed
**          to real NVMe files, so every field here is guaranteed consistent
**          with the writer-dir's actual on-disk state at write time.
** @details activeSubPass + recordsConsumedInSubPass together describe
**          exactly where RunGpuFeederJob's two sequential sub-passes
**          (black-turn boards, then white-turn boards -- see
**          LevelSolverThread.cpp) had gotten to. recordsConsumedInSubPass
**          is always relative to activeSubPass's OWN stream, never a
**          combined black+white count, since the two sub-passes run
**          strictly one after the other, not interleaved. A checkpoint
**          taken exactly between the two sub-passes (black already fully
**          done, white not yet started) is naturally represented as
**          activeSubPass=RSF_PLAYER_WHITE, recordsConsumedInSubPass=0 --
**          no separate "between passes" state is needed.
**          mwNextFileIdx is a snapshot of every (writer, color) ticket
**          high-water mark at the moment of writing, used on restart to
**          cross-check the writer-dir's independently-scanned state
**          actually matches before trusting this checkpoint (see the
**          restart-validation design) -- never trusted as the resume
**          source for that state by itself, only as a consistency check
**          against a fresh directory scan.
*/
typedef struct __CheckpointStats
{
    uint8_t  boardSize;                      /* sanity check against the running config */
    uint8_t  level;                          /* which level this checkpoint belongs to */
    uint8_t  activeSubPass;                  /* RSF_PLAYER_BLACK or RSF_PLAYER_WHITE */
    uint8_t  numMergeWriters;                /* must match the running config; else mwNextFileIdx below is meaningless */
    uint64_t recordsConsumedInSubPass;       /* position within activeSubPass's own input stream */
    int      mwNextFileIdx[MAX_WRITERS][2];  /* snapshot at checkpoint time, indexed [writerIdx][player] */
    char     timestamp[24];                  /* "YYYY-MM-DD HH:MM:SS", informational only */
} CheckpointStats, * PCheckpointStats;

/*
** Type:    LevelStatsPreConsolidation
** @brief   Frozen, byte-for-byte copy of LevelStats as it existed for every
**          sentinel written before 2026-07-23 (v0.32.0), when background
**          consolidation inserted 3 uint64_t fields
**          (consolidationFilesCreated/Removed/BytesWritten) in the middle
**          of the live struct. Every real sentinel for the current
**          production run's levels 1-22 was written against this exact
**          layout. Never add fields here or otherwise evolve it -- it
**          exists solely so ReadSentinelLevelStats can still translate
**          those pre-existing files; if LevelStats changes shape again
**          later, add another frozen snapshot rather than touching this one.
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
static inline void LevelStatsFromPreConsolidation(const LevelStatsPreConsolidation* src, LevelStats* dst)
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
** Function: ReadSentinelLevelStats
** @brief    Reads LevelStats from a _complete sentinel file (magic + raw
**           struct payload, written by OthelloRingMaster.cpp's
**           WriteSentinelStats), backward-compatibly translating older,
**           smaller payload shapes (see LevelStatsPreConsolidation) rather
**           than rejecting them -- so a struct growth never silently blanks
**           out an already-completed level's stats for any reader.
** @details  Shared by InitSolver.cpp (live solver's own resume-history
**           restoration) and OthelloRingMasterStoreStats/StoreStatsScan.cpp
**           (read-only reporting tool) specifically so both stay in sync
**           automatically the next time LevelStats' shape changes -- add
**           another frozen snapshot here, never touch
**           LevelStatsPreConsolidation itself.
** @param    path - sentinel file path
** @param    out  - out: filled with the sentinel's LevelStats payload
** @return   false if the file is zero-byte (legacy/manually created), can't
**           be opened, or its payload size doesn't match any recognized
**           LevelStats shape.
*/
static inline bool ReadSentinelLevelStats(const char* path, LevelStats* out)
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
** Type:    ClaimRegistry
** @brief   Per-(writer drive, color) set of writer-file indices currently
**          "spoken for" -- either a brand-new index still being written (by
**          a flush or a background-consolidation output), or an existing
**          file claimed as input to a live consolidation/cross-drive merge.
** @details See ClaimRegistry.h for the operations (ClaimSingle/ClaimTryRange/
**          ClaimReleaseOne/ClaimReleaseRange/ClaimIsHeld). `cs` guards
**          `claimed` only -- it must never be held across any file I/O or
**          across another lock (imergeCS, another ClaimRegistry instance).
**          Flush and consolidation both write directly to a file's final
**          path from the moment it's opened (no temp-name-then-rename), so a
**          directory scan must check ClaimIsHeld before trusting anything it
**          finds there -- a partial file is otherwise indistinguishable from
**          a complete one by size/attributes alone.
*/
typedef struct __ClaimRegistry
{
    CRITICAL_SECTION        cs;
    std::unordered_set<int> claimed;
} ClaimRegistry, * PClaimRegistry;

/*
** Type:    ConsolidationSlotStats
** @brief   Live progress for one consolidation worker thread. One instance
**          per worker (indexed by its stable ThreadPool worker index, 0..
**          CONSOLIDATION_POOL_THREADS-1), MemMalloc'd as pConsolSlotStats
**          (InitSolver.cpp) so the stats thread and every consolidation
**          worker reach the same shared memory. writerIdx/player identify
**          which pair this worker is currently merging (only meaningful
**          while active is set) -- needed now that a slot's array position
**          no longer implies a pair, unlike the old per-pair scheme. Read/
**          written without a lock, same convention as the rest of this
**          project's live-display fields (torn/stale reads are acceptable
**          for a status display; see e.g. cascadeActive below).
*/
typedef struct __ConsolidationSlotStats
{
    volatile int      active;
    int               writerIdx;   /* valid only while active */
    int               player;      /* valid only while active */
    int               fileCount;   /* valid only while active -- number of input files this merge is combining */
    int               batchIndices[MAX_CONSOLIDATION_BATCH];  /* valid only while active -- ticket indices of this merge's input files (first fileCount entries) */
    int               outIdx;      /* valid only while active -- ticket index of this merge's new output file */
    volatile int64_t  totalBytes;
    volatile int64_t  doneBytes;
    uint64_t          startTickMs;
} ConsolidationSlotStats, * PConsolidationSlotStats;

/*
** Type:    OthelloRingMasterConfig
** @brief   Fixed run configuration, set once from command-line args at
**          startup and never mutated afterward.
*/
typedef struct __OthelloRingMasterConfig
{
    uint8_t   boardSize;
    char      useDrives[64];
    char      cacheDirName[MAX_FULL_PATH_NAME];
    char      storeDirNameNoDrive[MAX_FULL_PATH_NAME];
    char      storeDrive;
    uint16_t  statsPort;
    uint8_t   compressMode;   /* COMPRESS_NONE / COMPRESS_STORE_ONLY / COMPRESS_ALL */
    char      lz4Drives[64];  /* drive letters that get LZ4 on top of varint (e.g. "DEF") */
    uint64_t  memoryLimitBytes; /* --memory-limit override (MM_SPECIFIED); 0 = use MM_RECOMMENDED against real free RAM */
    double    checkpointIntervalHours; /* --checkpoint-interval-hours; <= 0 disables periodic checkpointing entirely */
} OthelloRingMasterConfig, * POthelloRingMasterConfig;

/*
** Type:    OthelloRingMasterState
** @brief   All live, mutable solver state: current level/phase, per-color
**          merge/cascade progress, merge-writer buffer bookkeeping, drive
**          ledgers, per-level stats history, and thread pools.
*/
typedef struct __OthelloRingMasterState
{
    uint8_t              playLevel;
    int                  resumeLevel;             /* first level not found in storeDir at startup (0 = fresh run) */
    volatile bool        terminateThreads;
    volatile bool        terminateStatsListener;

    /* Set (in addition to terminateThreads) on Ctrl+C/shutdown, AND set alone
    ** (terminateThreads left untouched) at each level's normal solve->merge
    ** transition, so any in-flight background consolidation
    ** (TryConsolidatePair, MergeFiles.cpp) wraps up promptly rather
    ** than running arbitrarily long into the transition window. Reset to
    ** false at the start of each new level.
    */
    volatile bool        terminateConsolidation;
    const char* volatile currentPhase;             /* points to a string literal; set by main thread at each phase transition */
    volatile int64_t     mergeProgressBytes[2];    /* bytes written per player to final merge output; [0]=white [1]=black */
    uint64_t             mergeTotalInputBytes[2];  /* total uncompressed record bytes per player; set before merge threads start */
    uint64_t             mergeStartTickMs[2];      /* GetTickCount64() when each player's merge thread starts */
    uint64_t             mergeEndTickMs[2];        /* GetTickCount64() when each player's merge finishes; 0 = still running */

    /*
    ** Fan-in going into each color's end-of-level merge heap: on-disk files
    ** (enumerated in Phase 1) plus in-memory pool readers (leftover
    ** compressed segments/staging collected via CollectPoolReadersForPlayer).
    ** Set once before the merge threads start.
    */
    int  mergeInputFileCount[2];
    int  mergeInputPoolReaderCount[2];

    /*
    ** Per-player cascade progress -- populated when CascadingMerge triggers
    ** during DoEndOfLevelMerge. Indexed by RSF_PLAYER_WHITE(0)/RSF_PLAYER_BLACK(1).
    ** Written by the merge thread, read by the stats thread (no lock needed; display-only).
    */
    bool              cascadeActive[2];             /* true while intermediate groups are running */
    int               cascadeNumGroups[2];          /* total intermediate groups in this cascade   */
    int               cascadeGroupsDone[2];         /* groups fully written to temp so far         */
    volatile int64_t  cascadeGroupProgressBytes[2]; /* bytes written to current group's temp file  */
    uint64_t          cascadeGroupStartTickMs[2];   /* GetTickCount64() at the start of each cascade group */
    uint64_t          cascadeStartTickMs[2];        /* GetTickCount64() at the start of the whole cascade (group 1); used for the stats thread's ETA */
    uint64_t          currentLevelTotalBoards;      /* total boards in current level's input file(s); set by GPU feeder before reading starts */

    /* Mid-level checkpointing (see Checkpoint.h). checkpointPauseFlag is
    ** passed as RingNestedIndexStreamAll's pTerminate argument in place of
    ** terminateThreads during normal solving -- a distinct flag so the
    ** existing "drop the partial ping-pong slot" behavior tied to
    ** terminateThreads (correct for a real shutdown, wrong for a checkpoint
    ** pause) never fires here. Set by the GPU feeder itself (FeedBoardIntoBatch)
    ** once checkpointRequestedNow is seen, or once checkpointIntervalStartTickMs
    ** shows the configured interval has elapsed; cleared once the checkpoint
    ** sequence completes and streaming is about to resume.
    */
    volatile bool  checkpointPauseFlag;
    volatile bool  checkpointRequestedNow;      /* set by the CHECKPT stats-port command; cleared once handled */
    uint64_t       checkpointIntervalStartTickMs; /* GetTickCount64() when the current interval window started; reset at level start and after each checkpoint */

    /* Set once by InitSolver (via ReadValidCheckpoint) when resumeLevel has a
    ** valid checkpoint file -- consumed exactly once by RunGpuFeederJob for
    ** that specific level (level == resumeLevel), then cleared, so it never
    ** applies to any later level in the same run.
    */
    bool     resumeFromCheckpoint;
    int      resumeCheckpointSubPass;      /* RSF_PLAYER_BLACK or RSF_PLAYER_WHITE, valid only if resumeFromCheckpoint */
    uint64_t resumeCheckpointRecords;      /* records already consumed in that sub-pass, valid only if resumeFromCheckpoint */

    /* Merge-writer threads: one per NVMe drive, stable thdIdx maps to buffer/dir */
    uint8_t  numMergeWriters;
    char     mwDirectory[MAX_WRITERS][MAX_FULL_PATH_NAME];
    size_t   mwBufferSize;                  /* bytes per merge-writer buffer */
    void*    pMWBuffer[MAX_WRITERS];        /* one large buffer per thread */
    /* Pure "files created this level" display counters (StatsListener.cpp's
    ** liveBlack/liveWhite) -- InterlockedIncrement on every real create event
    ** (a flush close, or a background-consolidation merge success). No longer
    ** read-before-write to pick a new file's index -- that's FileTicketNext's
    ** job (FileTicket.h) via mwNextFileIdx below, which is what lets flush and
    ** consolidation allocate a name without ever contending for the same lock.
    */
    volatile int  mwBlackFileCount[MAX_WRITERS];
    volatile int  mwWhiteFileCount[MAX_WRITERS];

    /* Live-only (deliberately NOT part of LevelStats -- see that struct's own
    ** backward-compat history, OthelloTypes.h's LevelStatsPreConsolidation
    ** comment, and the sentinel-stats-regression memory; this is a pure
    ** current-level diagnostic, not something that needs persisting across a
    ** restart) running total of every successful TryConsolidatePair merge's
    ** real on-disk INPUT bytes this level, summed across all writers/colors.
    ** Compare against the already-persisted levelStats[playLevel].consolidationBytesWritten
    ** (the OUTPUT total) to see how much real space consolidation is
    ** reclaiming -- KWayMergeFiles genuinely dedupes on the board key, not
    ** just merge-sorts, so a real gap here reflects real removed duplicates
    ** (plus any compression-ratio change from re-encoding a bigger file).
    */
    volatile int64_t consolidationBytesInput;

    /* Low-water-mark HINT for DoCrossDriveIntermediateMerge's gather-loop scan
    ** start -- NOT a correctness invariant (that's now ClaimRegistry's job:
    ** the gather loop checks ClaimIsHeld per candidate directly, so it can
    ** never race a file a live consolidation merge is holding, regardless of
    ** where this hint says to start). Only ever advanced under imergeCS.
    */
    int      mwBlackFilesConsumed[MAX_WRITERS];
    int      mwWhiteFilesConsumed[MAX_WRITERS];

    /* Lock-free per-(writer, color) ticket counters (FileTicket.h) -- the
    ** sole source of new writer-file indices for both flush
    ** (FlushMergeWriterBuffer) and background consolidation
    ** (TryConsolidatePair). InterlockedIncrement only; never reused
    ** within a level, so an index that's ever been handed out never comes
    ** back around, and a gathering loop can always treat "not found on disk"
    ** as permanent, not "not written yet."
    */
    volatile LONG  mwNextFileIdx[MAX_WRITERS][2];

    /* Per-(writer, color) claim registry (ClaimRegistry.h) -- which writer-
    ** file indices are currently spoken for (mid-write, or claimed as input
    ** to a live consolidation/cross-drive merge). See ClaimRegistry's own
    ** type comment above for the full rationale.
    */
    ClaimRegistry  claimRegistry[MAX_WRITERS][2];

    /* Background small-file consolidator (TryConsolidatePair, MergeFiles.cpp):
    ** an independent low-water-mark HINT, separate from mwBlackFilesConsumed/
    ** mwWhiteFilesConsumed above -- "everything below this index has already
    ** been looked at by the consolidator, either merged away and replaced by
    ** a file appended at a higher index, or left alone because it was already
    ** >= CONSOLIDATION_SIZE_CAP." Purely a scan-start optimization (real
    ** correctness comes from ClaimRegistry + real file-existence checks per
    ** candidate) -- touched by every consolidation worker thread that visits
    ** this pair, without a lock; a lost update just causes some harmless
    ** re-scanning next time, never incorrectness, hence the plain racy
    ** "advance if greater" writes used against it.
    */
    volatile int  mwBlackConsolidatedUpTo[MAX_WRITERS];
    volatile int  mwWhiteConsolidatedUpTo[MAX_WRITERS];

    /* Lock-free per-(writer, color) "someone's already examining this pair"
    ** flag -- 0 = free, 1 = a worker is currently between building a
    ** candidate batch and resolving its ClaimTryRange attempt for this
    ** pair. A worker that finds this already 1 skips the pair for this
    ** sweep entirely (moves on to the next pair) rather than blocking, so
    ** multiple workers never redundantly re-scan the exact same files at
    ** once. Deliberately narrow-scoped: released the instant the claim
    ** attempt resolves (success or failure), well before the actual merge
    ** I/O starts, so it never limits how many merges can run concurrently
    ** on one pair -- only how many workers can be *deciding* what to merge
    ** on it at the same instant.
    */
    volatile LONG  consolScanning[MAX_WRITERS][2];

    size_t   gpuAccumCapacity;   /* GPU accumulator board capacity (shared black+white) */
    size_t   mwStagingSize;      /* bytes per staging area = gpuAccumCapacity * sizeof(UINT64_PAIR) */

    /*
    ** Per-thread compressed pool segment tracking. Layout of each mwBuf[i]:
    **   [0 .. mwStagingSize)                              = black staging (fixed DMA target)
    **   [mwStagingSize .. mwBufferSize-mwStagingSize)      = shared compressed pool -- black and
    **                                                        white segments are both packed in
    **                                                        here back-to-back (one running
    **                                                        offset, mwBlackCompBytesUsed +
    **                                                        mwWhiteCompBytesUsed), not two
    **                                                        separate opposing-growth regions
    **   [mwBufferSize-mwStagingSize .. end)                = white staging (fixed DMA target)
    ** No sync needed: each thread only touches its own slots.
    */
    int     mwBlackSegCount[MAX_WRITERS];
    size_t  mwBlackSegOffset[MAX_WRITERS][MAX_MW_SEGS];     /* byte offset from mwBuf start */
    size_t  mwBlackSegSize[MAX_WRITERS][MAX_MW_SEGS];       /* compressed byte size of segment */
    int     mwBlackSegBoardCount[MAX_WRITERS][MAX_MW_SEGS]; /* board count of segment */
    size_t  mwBlackCompBytesUsed[MAX_WRITERS];              /* bytes consumed in black pool */
    int     mwBlackStagingCount[MAX_WRITERS];               /* boards in black staging (0 = stale) */
    int     mwBlackSegCountHighWater[MAX_WRITERS];          /* lifetime peak of mwBlackSegCount, never reset */

    int     mwWhiteSegCount[MAX_WRITERS];
    size_t  mwWhiteSegOffset[MAX_WRITERS][MAX_MW_SEGS];
    size_t  mwWhiteSegSize[MAX_WRITERS][MAX_MW_SEGS];
    int     mwWhiteSegBoardCount[MAX_WRITERS][MAX_MW_SEGS];
    size_t  mwWhiteCompBytesUsed[MAX_WRITERS];
    int     mwWhiteStagingCount[MAX_WRITERS];
    int     mwWhiteSegCountHighWater[MAX_WRITERS];

    /* Intermediate merge destinations (medium drives) */
    char      mergeDirectory[MAX_WRITER_DRIVES][MAX_FULL_PATH_NAME];
    uint8_t   numMergeDirs;
    int       mergeFileBlackCount[MAX_WRITER_DRIVES];   /* access via InterlockedExchangeAdd */
    int       mergeFileWhiteCount[MAX_WRITER_DRIVES];   /* access via InterlockedExchangeAdd */
    uint64_t  mergeFileBytesBlack[MAX_WRITER_DRIVES];   /* actual bytes: black imerge on this drive */
    uint64_t  mergeFileBytesWhite[MAX_WRITER_DRIVES];   /* actual bytes: white imerge on this drive */
    uint64_t  mergeFileUncompBlack[MAX_WRITER_DRIVES];  /* uncompressed equivalent (black) */
    uint64_t  mergeFileUncompWhite[MAX_WRITER_DRIVES];  /* uncompressed equivalent (white) */

    /*
    ** Per-drive space ledger (indexed by driveLetter - 'A'). Initialized
    ** from the OS after cleanup; updated atomically on every write and
    ** delete. A safety buffer (DRIVE_SPACE_LOW_BYTES) is subtracted at init
    ** so reservations never reach the last bytes on a drive. Replaces all
    ** ad-hoc GetDiskFreeSpaceExA calls at decision points -- see DriveLedger.h.
    */
    volatile int64_t driveLedger[26];

    /* Serializes DoCrossDriveIntermediateMerge so only one thread runs it at a time. */
    CRITICAL_SECTION imergeCS;

    /*
    ** Per-writer intermediate merge progress (written by MW threads, read by
    ** stats thread). imergeActive[i] is set to 1 before the merge and 0
    ** after; the other fields are populated before imergeActive is set so
    ** the stats reader always sees consistent data.
    */
    volatile int      imergeActive[MAX_WRITERS];
    volatile int64_t  imergeTotalInputBytes[MAX_WRITERS];
    volatile int64_t  imergeDoneInputBytes[MAX_WRITERS];
    uint64_t          imergeStartTickMs[MAX_WRITERS];   /* GetTickCount64() when the imerge starts */
    int               imergeFileCount[MAX_WRITERS];     /* valid only while imergeActive -- number of input files this cross-drive merge is combining */

    /*
    ** Per-writer buffer-full flush progress (RunMergeWriterJob ->
    ** FlushMergeWriterBuffer, the in-memory-pool-to-NVMe spill). Same
    ** active-flag convention as imerge above.
    */
    volatile int      mwFlushActive[MAX_WRITERS];
    volatile int64_t  mwFlushTotalBytes[MAX_WRITERS];
    volatile int64_t  mwFlushDoneBytes[MAX_WRITERS];
    uint64_t          mwFlushStartTickMs[MAX_WRITERS];  /* GetTickCount64() when the flush starts */

    /*
    ** Live progress for every consolidation worker thread, MemMalloc'd
    ** (InitSolver.cpp) as CONSOLIDATION_POOL_THREADS ConsolidationSlotStats
    ** entries -- one per worker, indexed by its own stable ThreadPool
    ** worker index. See that type's comment above. Addressed via the
    ** ConsolSlot() helper (below, after this struct, since it dereferences
    ** POthelloRingMasterState).
    */
    ConsolidationSlotStats* pConsolSlotStats;

    /*
    ** Real-time physical file count per writer/color -- unlike
    ** mwBlack/WhiteFileCount (monotonically increasing, never decremented),
    ** this tracks what's actually sitting on disk right now: incremented on
    ** any file creation (a genuine GPU flush or a consolidation merge
    ** output), decremented on any deletion (consolidation absorbing
    ** originals, or DoCrossDriveIntermediateMerge consuming files).
    ** Needed because consolidation creates gaps in the file-index range
    ** (deletes some originals, replaces them with one file at a higher
    ** index), so file count alone can no longer answer "how many files
    ** really exist right now."
    */
    volatile int      mwBlackPhysicalFileCount[MAX_WRITERS];
    volatile int      mwWhitePhysicalFileCount[MAX_WRITERS];

    /*
    ** Fallback intermediate merge destination on the store drive (used when
    ** no medium drive has enough space for even one MAX_MERGE_FANIN batch).
    */
    char      storeMergeDirectory[MAX_FULL_PATH_NAME];
    int       storeMergeBlackFileCount;      /* access via InterlockedExchangeAdd */
    int       storeMergeWhiteFileCount;      /* access via InterlockedExchangeAdd */
    uint64_t  storeMergeBytesWritten;        /* actual bytes on the store drive this level */
    uint64_t  storeMergeBytesUncompressed;   /* uncompressed equivalent */

    /* Store (slow/NAS drive) */
    char  storeDirectory[MAX_FULL_PATH_NAME];
    char  logFileName[MAX_FULL_PATH_NAME];

    /* Ping-pong buffer (GPU feeder) */
    size_t  pingPongBufferSize;
    void*   pPingPongBuffer;

    /* Per-drive stats */
    WriterDriveStats  writerDriveStats[MAX_WRITER_DRIVES];
    int               numWriterDrives;

    /* Per-level stats history */
    LevelStats levelStats[MAX_LEVELS];

    ThreadPool* pMergeWriterPool;
    ThreadPool* pGPUFeederThreadPool;
    ThreadPool* pStatsThreadPool;
    /* Background small-file consolidator (ConsolidationWorkerLoop,
    ** TryConsolidatePair, MergeFiles.cpp) -- CONSOLIDATION_POOL_THREADS
    ** persistent worker threads, each looping sleep/sweep-every-pair/sleep
    ** for the whole level, queued fresh at each level's start (see
    ** OthelloRingMaster.cpp). Deliberately flat-sized (not scaled by
    ** numMergeWriters) so adding more NVMe drives never grows thread count.
    ** Separate from pMergeWriterPool so it draws from otherwise-idle cores.
    */
    ThreadPool* pConsolidationPool;
} OthelloRingMasterState, * POthelloRingMasterState;

/*
** Function: ConsolSlot
** @brief    Computes the address of one consolidation worker's stats entry
**           inside the MemMalloc'd pConsolSlotStats block (InitSolver.cpp).
** @param    pSt       - solver state
** @param    workerIdx - the worker's stable ThreadPool index (0..CONSOLIDATION_POOL_THREADS-1)
** @return   Pointer to that worker's live-progress stats.
*/
static inline PConsolidationSlotStats ConsolSlot(POthelloRingMasterState pSt, int workerIdx)
{
    return &pSt->pConsolSlotStats[workerIdx];
}
