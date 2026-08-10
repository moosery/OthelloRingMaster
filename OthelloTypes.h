/*
** Filename:  OthelloTypes.h
**
** Purpose:
**   Declares the core config/state/stats structures for the live solver:
**   OthelloRingMasterConfig (fixed run configuration), OthelloRingMasterState
**   (all live/mutable solver state -- merge-writer buffers, the per-drive
**   file registry, per-level stats, thread pools), WriterDriveStats and
**   LevelStats (the per-drive and per-level bookkeeping records each hold).
**
** Notes:
**   Adapted from an earlier solver implementation, renamed onto this
**   solution's own types (-> OthelloRingMasterConfig/State) and updated to
**   reference the RSF record-file format (see Utility/RingStoreFile.h).
**   Field shapes are otherwise kept as-is -- the multi-drive/multi-writer
**   machinery is real functionality this project intends to keep, not
**   architectural cruft to trim.
**
**   v1.0.0 (2026-08-xx): replaced every ticket-number-based mechanism
**   (mwNextFileIdx-as-logic, ClaimRegistry, pConsolUp/mwXxxConsolidatedUpTo,
**   mwXxxPhysicalFileCount, the 12-thread polling consolidation pool,
**   MAX_MERGE_FANIN as a trigger) with a single per-drive file registry
**   (RegistryFileNode / driveRegistry) that tracks real file identity and
**   reservation state directly -- see Registry.h for the operations, and
**   project memory project_writer_drive_registry_redesign.md for the full
**   design rationale this header only summarizes.
*/

#pragma once

/* Includes */
#include "Utility.h"
#include "RSFFileName.h"
#include <list>
#include <mutex>
#include <condition_variable>
#include <thread>

/* Macros and Defines */
#define VERSION "1.0.9"

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
*/
#define MAX_MW_SEGS 1024

/*
** Drive space threshold -- when free bytes on a drive drops below this,
** flush/iMerge treat that drive as needing relief. Default; overridable per
** run via --drive-space-low-gb (OthelloRingMasterConfig.driveSpaceLowGBOverride,
** 0 = use this default) specifically so a short validation run can force real
** iMerge activity on tiny real data volume -- see Registry.h/the iMerge pool
** for how this is consumed.
*/
#define DRIVE_SPACE_LOW_GB    20ULL
#define DRIVE_SPACE_LOW_BYTES (DRIVE_SPACE_LOW_GB * 1024ULL * 1024ULL * 1024ULL)

/*
** Background consolidation eligibility cap ("MAX_FILE_SIZE"): a file at or
** above this size is left alone by the consolidation master -- not worth the
** merge cost for the marginal dedup reward once it's already this big; the
** cross-drive iMerge or the final end-of-level merge will handle it
** eventually. Default; overridable via --max-file-size-gb
** (OthelloRingMasterConfig.maxFileSizeGBOverride, 0 = use this default) --
** for a short validation run this must be picked as a comfortable multiple
** (5-10x) of whatever an individual flushed file actually comes out to under
** the chosen --memory-limit, never an arbitrary small number, or every
** freshly-flushed file is "too large to consolidate" from birth and no real
** consolidation ever happens.
*/
#define CONSOLIDATION_SIZE_CAP_GB    100ULL
#define CONSOLIDATION_SIZE_CAP_BYTES (CONSOLIDATION_SIZE_CAP_GB * 1024ULL * 1024ULL * 1024ULL)

/*
** Maximum number of files opened simultaneously for a single grouped k-way
** merge pass (DoEndOfLevelMerge/CascadingMerge, MergeFiles.cpp) -- a real OS
** file-handle bound (_setmaxstdio must exceed this plus overhead; see
** InitSolver.cpp), NOT a trigger threshold. This is the direct replacement
** for the old MAX_MERGE_FANIN constant's file-handle-bounding role; its
** OTHER role (a fanin-based trigger for background consolidation/iMerge) is
** retired entirely, not renamed -- see the top-of-file v1.0.0 note.
*/
#define MAX_MERGE_INPUT_FILES 3500

/*
** How many worker threads each dedicated pool gets. Fixed-but-tunable
** constants (not yet exposed as CLI overrides), deliberately bounded by real
** logical core count rather than scaled with drive count -- same "flat,
** independent of drive count" stance this project already took once for the
** old consolidation pool. Flusher/iMerge are sized "2 per color" (one black,
** one white) so both colors always have dedicated capacity and never fight
** each other for a thread; consolidator is a larger shared worker pool
** dispatched by the single consolidation master thread (ConsolidationMaster.h).
*/
#define FLUSHER_POOL_THREADS      4
#define IMERGE_POOL_THREADS       4
#define CONSOLIDATOR_POOL_THREADS 8

/*
** Cap on how many unreserved candidate files the consolidation master
** collects from a single registry scan of one (drive, color) pair before
** deciding whether to dispatch a merge. Real per-pair unreserved counts
** stay far below this in practice (confirmed repeatedly against live
** production data) -- generous headroom, not a tuned throughput limit.
*/
#define MAX_CONSOLIDATION_BATCH 64

/*
** Default interval for both new background auditors (RegistryAuditor.h,
** DriveSpaceAuditor.h). Chosen short (not the old FANIN-style "rare safety
** net" cadence) because both audits are cheap -- real file counts per drive
** stay small regardless of level size, confirmed repeatedly against the live
** production run this redesign was built from. Overridable via
** --audit-interval-seconds for short validation runs, same reasoning as
** --checkpoint-interval-hours already gets for its own validation runs.
*/
#define AUDIT_INTERVAL_SECONDS_DEFAULT 120

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

    /* Background small-file consolidator (ConsolidationMaster.h/.cpp) */
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
** Bound on how many real files one drive's checkpoint integrity manifest can
** record. Real per-drive file counts never approach this in practice (2-20
** confirmed repeatedly against live production data) -- generous headroom,
** same bounded-array style already used elsewhere in this project (e.g.
** the old ConsolidationSlotStats.batchIndices).
*/
#define CHECKPOINT_MANIFEST_MAX_FILES 256

/*
** Type:    CheckpointManifestEntry
** @brief   One real file's identity+size, as recorded at checkpoint time,
**          for the restart-time integrity cross-check (see
**          Checkpoint.h/.cpp's three hard-Fatal rules: missing on disk,
**          untracked on disk, size mismatch).
*/
typedef struct __CheckpointManifestEntry
{
    char     filename[MAX_FULL_PATH_NAME];
    uint8_t  color;       /* RSF_PLAYER_BLACK / RSF_PLAYER_WHITE */
    int64_t  size;         /* real on-disk size at checkpoint time -- every file is
                            ** finished by the time the manifest is captured (full
                            ** drain happens first), so this is never a partial size */
} CheckpointManifestEntry, * PCheckpointManifestEntry;

/*
** Type:    CheckpointStats
** @brief   Mid-level checkpoint payload -- exactly enough to resume the GPU
**          feeder from a specific point in a level's input stream instead of
**          from record 0, plus a full integrity manifest of every real
**          writer-drive file at checkpoint time. Written only once the
**          feeder is fully paused, its accumulator drained, and every flush/
**          iMerge in flight has completed and consolidation has stopped, so
**          every field here is guaranteed consistent with the writer-dirs'
**          actual on-disk state at write time -- no file is mid-write when
**          the manifest is captured.
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
**          nextFileIdx is a snapshot of every drive's naming counter at
**          checkpoint time -- restored on resume purely so new files never
**          collide with real files already on disk; it is NEVER consulted
**          for any backlog/trigger decision (the registry, rebuilt from a
**          fresh directory scan on restart, is the sole source of truth for
**          that). manifest/manifestCount is the real cross-check: restart
**          re-scans every writer drive and Fatals on any disagreement with
**          what's recorded here (see Checkpoint.h).
*/
typedef struct __CheckpointStats
{
    uint8_t  boardSize;                      /* sanity check against the running config */
    uint8_t  level;                          /* which level this checkpoint belongs to */
    uint8_t  activeSubPass;                  /* RSF_PLAYER_BLACK or RSF_PLAYER_WHITE */
    uint8_t  numMergeWriters;                /* must match the running config; else nextFileIdx/manifest below are meaningless */
    uint64_t recordsConsumedInSubPass;       /* position within activeSubPass's own input stream */
    int      nextFileIdx[MAX_WRITERS];       /* per-drive naming-counter snapshot -- naming only, never logic */
    int      manifestCount[MAX_WRITERS];     /* how many of manifest[wi][*] are valid */
    CheckpointManifestEntry manifest[MAX_WRITERS][CHECKPOINT_MANIFEST_MAX_FILES];
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
** reservedBy values for RegistryFileNode.reservedBy below -- which of the
** four roles currently holds a file reserved. RSF_PLAYER_BLACK/WHITE-style
** small enum, not a bitmask (a file is reserved by exactly one role at a
** time; see project_writer_drive_registry_redesign memory for why "no
** consolidation pause during iMerge" is still safe -- disjoint files, not
** disjoint roles).
*/
#define REGISTRY_RESERVED_NONE         0
#define REGISTRY_RESERVED_FLUSH        1
#define REGISTRY_RESERVED_CONSOL       2
#define REGISTRY_RESERVED_IMERGE       3
#define REGISTRY_RESERVED_FINAL_MERGE  4

/*
** Type:    RegistryFileNode
** @brief   One real writer-drive file's identity and reservation state --
**          the sole source of truth this project uses for "does this file
**          exist, and is it currently in use," replacing every ticket-
**          number-derived proxy for the same question (mwNextFileIdx-as-
**          logic, ClaimRegistry, mwXxxFilesConsumed, pConsolUp/
**          mwXxxConsolidatedUpTo, mwXxxPhysicalFileCount all retired -- see
**          this file's top-of-file note and Registry.h).
** @details Created when a file is first reserved for writing (flush/
**          consolidation/iMerge claims a new output slot), removed when the
**          file is deleted (consolidation/iMerge absorbing an input, or the
**          final merge consuming everything). physfilesize is 0 while
**          isReserved is true and reservedBy names a write-in-progress role
**          -- that is expected, not drift (see RegistryAuditor.h's
**          size-mismatch check, which explicitly skips reserved nodes for
**          this reason). reservedBytes is the worst-case byte reservation
**          claimed via DriveReserve (DriveLedger.h) at reservation time --
**          needed by DriveSpaceAuditor.h to recompute real drive usage from
**          scratch (finished-file real sizes + in-progress reservedBytes),
**          since DriveReserve itself only takes this as a transient call
**          argument and otherwise forgets it.
*/
typedef struct __RegistryFileNode
{
    uint8_t   color;             /* RSF_PLAYER_BLACK / RSF_PLAYER_WHITE */
    char      filename[MAX_FULL_PATH_NAME];   /* full path */
    int64_t   physfilesize;      /* 0 until the writer finishes and reports the real size */
    bool      isReserved;
    uint8_t   reservedBy;        /* REGISTRY_RESERVED_* -- valid only while isReserved */
    int64_t   reservedBytes;     /* worst-case bytes claimed at reservation time -- valid only while isReserved */
    ClockTick reservedSinceTick; /* ClockStart'd when reserved -- fallback for RegistryAuditor.h's
                                  ** stuck-reservation check on nodes with no pProgressBytes linked */
    volatile int64_t* pProgressBytes; /* nullptr, or points at whichever live progress counter
                                       ** (mwFlushDoneBytes/consolSlot.doneBytes/imergeDoneInputBytes)
                                       ** the job currently holding this node is updating -- lets
                                       ** RegistryAuditor.h tell "reserved a long time but genuinely
                                       ** still moving" apart from "reserved and stalled," regardless
                                       ** of file size or how long a real operation legitimately
                                       ** takes at whatever level scale is running. Set by the
                                       ** owning job (RegistryLinkProgress, Registry.h) right before
                                       ** it starts real I/O; left null for the rare operations that
                                       ** don't report incremental progress (e.g. iMerge's single-
                                       ** file MoveFileExA path), which fall back to reservedSinceTick. */
} RegistryFileNode, * PRegistryFileNode;

/*
** Type:    SpaceReliefCoordinator
** @brief   The single, global "a space-relief event is in progress" gate that
**          replaced the old per-color imergeSession pair. When any writer-drive
**          flush can't reserve, exactly one flusher thread becomes the relief
**          coordinator (sets active=true), and EVERY other flush -- whether it
**          also failed to reserve (a coalesced retry) or is just starting fresh
**          (the new-flush gate) -- waits here until the event completes. Making
**          it one global event, not two independent per-color ones, is what
**          lets the coordinator quiesce all flushes, pause consolidation, and
**          sweep BOTH colors off every NVMe drive in one shot -- see
**          project_writer_drive_registry_redesign memory for the full rationale
**          (the per-color design could livelock: a flush stuck behind the OTHER
**          color's space hog could never free enough of its own to proceed).
**          std::mutex/std::condition_variable deliberately, matching
**          ThreadPool's own wait/notify style.
*/
typedef struct __SpaceReliefCoordinator
{
    bool                     active;
    std::mutex                m;
    std::condition_variable   cv;
} SpaceReliefCoordinator;

/*
** Type:    ConsolidatorSlotStats
** @brief   Live progress for one in-flight background consolidation merge,
**          indexed by the pConsolidatorPool worker thread's own index (the
**          uint32_t the pool passes each job). Populated by
**          ConsolidatorWorkerBody (ConsolidationMaster.cpp) and read by the
**          STATUS display to show per-worker "Consol" progress lines --
**          restores the visibility the retired ConsolidationSlotStats gave,
**          minus its ticket-era batchIndices/outIdx (the CONSOL command reads
**          the registry directly now). totalBytes/doneBytes are the
**          uncompressed-equivalent (recordCount*16) convention every other
**          progress line uses, so % and rate read the same way.
*/
typedef struct __ConsolidatorSlotStats
{
    volatile int      active;       /* 1 while this worker is merging, 0 otherwise */
    int               writerIdx;
    int               player;       /* RSF_PLAYER_WHITE / RSF_PLAYER_BLACK */
    int               fileCount;    /* number of input files this merge is combining */
    volatile int64_t  totalBytes;   /* total uncompressed-equivalent input bytes */
    volatile int64_t  doneBytes;    /* progress, same unit; updated live by KWayMergeFiles */
    uint64_t          startTickMs;  /* GetTickCount64() when this merge started */
} ConsolidatorSlotStats;

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

    /* Test-only overrides (v1.0.0 registry redesign) -- let a short,
    ** disposable validation run genuinely exercise the iMerge/consolidation/
    ** auditor machinery without waiting through a real week-plus level. 0 =
    ** use the real production default in every case.
    */
    uint64_t  driveSpaceLowGBOverride;    /* --drive-space-low-gb; overrides DRIVE_SPACE_LOW_GB */
    uint64_t  maxFileSizeGBOverride;      /* --max-file-size-gb; overrides CONSOLIDATION_SIZE_CAP_GB */
    uint32_t  auditIntervalSecondsOverride; /* --audit-interval-seconds; overrides AUDIT_INTERVAL_SECONDS_DEFAULT */
} OthelloRingMasterConfig, * POthelloRingMasterConfig;

/*
** Type:    OthelloRingMasterState
** @brief   All live, mutable solver state: current level/phase, per-color
**          merge/cascade progress, merge-writer buffer bookkeeping, the
**          per-drive file registry, per-level stats history, and thread
**          pools.
*/
typedef struct __OthelloRingMasterState
{
    uint8_t              playLevel;
    int                  resumeLevel;             /* first level not found in storeDir at startup (0 = fresh run) */
    volatile bool        terminateThreads;
    volatile bool        terminateStatsListener;

    /* Set (in addition to terminateThreads) on Ctrl+C/shutdown, AND set alone
    ** (terminateThreads left untouched) at each level's normal solve->merge
    ** transition, so any in-flight background consolidation wraps up
    ** promptly rather than running arbitrarily long into the transition
    ** window. Also the signal the consolidation master thread's own wait
    ** loop checks to know when to stop looping for good. Reset to false at
    ** the start of each new level.
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

    /* Mid-level checkpointing (see Checkpoint.h). v1.0.8: a checkpoint pause
    ** is now handled synchronously, in place, from inside FeedBoardIntoBatch
    ** itself -- the GPU feeder's read stream (RingNestedIndexStreamAll) is
    ** never interrupted for a checkpoint, only for a real terminateThreads
    ** shutdown, so pTerminate is wired directly to terminateThreads and no
    ** separate pause-signaling flag is needed any more (retired
    ** checkpointPauseFlag, which used to exist solely to distinguish the two
    ** without also triggering terminateThreads' "drop the partial ping-pong
    ** slot" behavior -- moot now that a checkpoint pause never unwinds the
    ** stream at all).
    */
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

    /*
    ** The per-drive file registry -- sole source of truth for "which real
    ** writer files exist right now, and are they reserved." driveRegistryCS
    ** covers both colors on that drive (a file's own `color` field is what
    ** distinguishes them, not separate lists); held only for list mutation/
    ** scan, never across real file I/O -- same discipline the old
    ** ClaimRegistry used. See Registry.h for the create/remove/reserve/
    ** unreserve/scan operations; RegistryFileNode above for the node shape.
    */
    std::list<RegistryFileNode>  driveRegistry[MAX_WRITERS];
    CRITICAL_SECTION             driveRegistryCS[MAX_WRITERS];

    /*
    ** Per-drive file-naming counter -- used EXCLUSIVELY to generate a
    ** unique filename for a newly-reserved file (Registry.h's
    ** RegistryReserveNew). Never consulted for any backlog/trigger/logic
    ** decision -- that is the one property that keeps this from
    ** reintroducing the ticket-position-as-logic bug class this whole
    ** redesign exists to eliminate. One counter per drive (not per
    ** drive-per-color, since a filename already encodes color as a prefix
    ** and both colors on a drive share one naming sequence).
    */
    volatile LONG  nextFileIdx[MAX_WRITERS];

    /*
    ** The single global space-relief coordinator (see SpaceReliefCoordinator
    ** above and the flusher/iMerge pools). One relief event at a time: when a
    ** flush can't reserve, it either becomes the coordinator or waits here.
    ** activeFlushWriters is the count of flushes currently mid-WRITE (bracketed
    ** around MergePoolToWriter only, NOT the reserve/relief wait) -- the
    ** coordinator drains this to zero (all in-flight writes finished, drives
    ** quiescent) before sweeping. Distinct from mwFlushActive, which stays 1
    ** across the whole flush including a relief wait and so must NOT be used
    ** for the drain (a flush blocked in relief would wait on itself).
    */
    SpaceReliefCoordinator  spaceRelief;
    volatile LONG           activeFlushWriters;

    /*
    ** Per-color live intermediate-merge progress (written by the iMerge pool,
    ** read by the stats thread). Indexed by RSF_PLAYER_WHITE(0)/
    ** RSF_PLAYER_BLACK(1) -- a single iMerge for a color spans every NVMe drive
    ** at once, and both colors now run together under one relief event.
    ** imergeActive[p] is set to 1 before the merge and 0 after; the other
    ** fields are populated before imergeActive is set so the stats reader
    ** always sees consistent data.
    */
    volatile int      imergeActive[2];
    volatile int64_t  imergeTotalInputBytes[2];
    volatile int64_t  imergeDoneInputBytes[2];
    uint64_t          imergeStartTickMs[2];   /* GetTickCount64() when the imerge starts */
    int               imergeFileCount[2];     /* valid only while imergeActive -- number of input files this cross-drive merge is combining */

    /* Per-consolidator-worker live progress (ConsolidatorSlotStats above),
    ** indexed by pConsolidatorPool worker thread index. Written by
    ** ConsolidatorWorkerBody, read by the STATUS display's "Consol" lines.
    */
    ConsolidatorSlotStats  consolSlot[CONSOLIDATOR_POOL_THREADS];

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
    ** ad-hoc GetDiskFreeSpaceExA calls at decision points -- see
    ** DriveLedger.h. Cross-checked (not replaced) from scratch every
    ** AUDIT_INTERVAL_SECONDS_DEFAULT by DriveSpaceAuditor.h, using the
    ** registry's own reservedBytes bookkeeping -- see that file.
    */
    volatile int64_t driveLedger[26];

    /*
    ** Per-writer buffer-full flush progress (the flusher pool -> real NVMe
    ** file write). Indexed [writerIdx][player] now, not just [writerIdx] --
    ** black and white write concurrently as two separate dispatched jobs
    ** (FlusherPool.h), so each half needs its own live-progress slot.
    ** mwFlushActive[i][p] set to 1 before the flush and 0 after; other
    ** fields populated before mwFlushActive is set so the stats reader
    ** always sees consistent data.
    */
    volatile int      mwFlushActive[MAX_WRITERS][2];
    volatile int64_t  mwFlushTotalBytes[MAX_WRITERS][2];
    volatile int64_t  mwFlushDoneBytes[MAX_WRITERS][2];
    uint64_t          mwFlushStartTickMs[MAX_WRITERS][2];  /* GetTickCount64() when the flush starts */

    /*
    ** Fallback intermediate merge destination on the store drive (used when
    ** no medium drive has enough space for even one gathered iMerge batch).
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

    /* Dedicated pools. pMergeWriterPool keeps its original, narrower role
    ** unchanged from before this redesign: one thread per writer drive,
    ** handling the routine D2H-copy-then-try-compress-into-pool step for
    ** EVERY GPU flush handoff (RunMergeWriterJob) -- frequent, in-memory
    ** only, nothing to do with tickets/files/registry. When that in-memory
    ** pool fills, RunMergeWriterJob dispatches the real disk-write work as
    ** two jobs (one per color) onto the NEW pFlusherPool and *waits* for
    ** both to finish before returning to accept its next GPU batch -- the
    ** wait is required (the per-writer segment buffers are shared memory,
    ** unsafe to let new GPU data land there while a dispatched flush job is
    ** still reading out of them), and is no different in effect from
    ** today's already-blocking-until-flush-completes behavior; what
    ** changes is that the actual write happens on a pool iMerge/
    ** consolidation can never contend with, not the waiting itself. See
    ** the top-of-file note and Registry.h/FlusherPool.h/IMergePool.h/
    ** ConsolidationMaster.h/RegistryAuditor.h/DriveSpaceAuditor.h. Each of
    ** the other four is an independent role with its own fixed thread
    ** count (FLUSHER_POOL_THREADS/IMERGE_POOL_THREADS/
    ** CONSOLIDATOR_POOL_THREADS above) so housekeeping can never starve the
    ** GPU feeder's own flush dependency the way the old single shared
    ** pMergeWriterPool (which used to ALSO do consolidation/iMerge inline)
    ** could.
    */
    ThreadPool* pMergeWriterPool;
    ThreadPool* pGPUFeederThreadPool;
    ThreadPool* pStatsThreadPool;
    ThreadPool* pFlusherPool;
    ThreadPool* pIMergePool;
    ThreadPool* pConsolidatorPool;

    /*
    ** Single event-driven consolidation master thread (ConsolidationMaster.h)
    ** -- not part of any ThreadPool, since it is exactly one thread for the
    ** whole run, woken by a condition variable rather than pulling from a
    ** job queue. Dispatches work onto pConsolidatorPool.
    */
    std::thread              consolidationMasterThread;
    std::mutex                consolidationMasterMutex;
    std::condition_variable   consolidationMasterCV;
    volatile bool             consolidationMasterWake;   /* predicate for the CV wait -- set by any of the three trigger events, cleared once the master wakes and processes it */
    volatile int              consolidatorFreeCount;     /* free worker count in pConsolidatorPool -- checked by the master before reserving, so it never locks files behind a job nothing can start yet */
    volatile int64_t          consolidationBytesInput;   /* real on-disk input bytes across every successful consolidation merge this level -- live-only (STATUS display), deliberately NOT in LevelStats (that struct has a real backward-compat history from growing mid-struct, see project_sentinel_stats_backward_compat_regression memory) */

    /*
    ** The two new background auditors (RegistryAuditor.h,
    ** DriveSpaceAuditor.h) -- each its own dedicated thread, stoppable at
    ** final merge, read-only with respect to everything else (holds no
    ** resource anyone else needs, so no "wait for it" is required the way
    ** the other pools need on the final-merge stop sequence).
    */
    std::thread  registryAuditorThread;
    std::thread  driveSpaceAuditorThread;
} OthelloRingMasterState, * POthelloRingMasterState;
