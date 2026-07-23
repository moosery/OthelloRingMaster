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
#include <unordered_set>

/* Macros and Defines */
#define VERSION "0.32.5"

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
** Background small-file consolidator (DoBackgroundConsolidation, MergeFiles.cpp):
** a file at or above this size is left alone -- not worth the merge cost for
** the marginal dedup reward once it's already this big; the final end-of-level
** merge will handle it. Adjustable constant, not derived from anything else.
*/
#define CONSOLIDATION_SIZE_CAP_GB    100ULL
#define CONSOLIDATION_SIZE_CAP_BYTES (CONSOLIDATION_SIZE_CAP_GB * 1024ULL * 1024ULL * 1024ULL)

/*
** Per-(writer drive, color) concurrency cap for background consolidation --
** at most this many DoBackgroundConsolidation calls may hold a slot (see
** ConsolidationSlotStats/consolSlotOwner below) for the same pair at once.
** Enforced by consolSlotOwner, NOT by dedicated pool sizing -- see
** CONSOLIDATION_POOL_THREADS below for why (a fixed per-pair pool count would
** scale with drive count; this shouldn't).
*/
#define CONSOLIDATION_THREADS_PER_PAIR 3

/*
** Total worker threads in the single, shared background-consolidation pool
** (pConsolidationPool). Deliberately a FLAT constant, independent of
** numMergeWriters/drive count -- adding more NVMe drives should not require
** more OS threads than this. Every (writer, color) pair's examination jobs
** are queued to this one pool; per-pair concurrency is capped separately
** (CONSOLIDATION_THREADS_PER_PAIR, via consolSlotOwner) so no single pair can
** monopolize the shared pool.
*/
#define CONSOLIDATION_POOL_THREADS 12

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

    /* Background small-file consolidator (DoBackgroundConsolidation, MergeFiles.cpp) */
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
** @brief   Live progress for one background-consolidation examination slot
**          (see consolSlotOwner below for how a slot is claimed). One
**          instance per (writer drive, color, slot) triple, MemMalloc'd as
**          pConsolSlotStats (InitSolver.cpp) so the stats thread and every
**          consolidation thread reach the same shared memory. Read/written
**          without a lock, same convention as the rest of this project's
**          live-display fields (torn/stale reads are acceptable for a
**          status display; see e.g. cascadeActive below).
*/
typedef struct __ConsolidationSlotStats
{
    volatile int      active;
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
    ** (DoBackgroundConsolidation, MergeFiles.cpp) wraps up promptly rather
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
    ** (DoBackgroundConsolidation). InterlockedIncrement only; never reused
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

    /* Background small-file consolidator (DoBackgroundConsolidation, MergeFiles.cpp):
    ** an independent low-water-mark HINT, separate from mwBlackFilesConsumed/
    ** mwWhiteFilesConsumed above -- "everything below this index has already
    ** been looked at by the consolidator, either merged away and replaced by
    ** a file appended at a higher index, or left alone because it was already
    ** >= CONSOLIDATION_SIZE_CAP." Purely a scan-start optimization now (real
    ** correctness comes from ClaimRegistry + real file-existence checks per
    ** candidate) -- touched by up to CONSOLIDATION_THREADS_PER_PAIR concurrent
    ** examination threads per pair without a lock; a lost update just causes
    ** some harmless re-scanning next time, never incorrectness, hence the
    ** plain racy "advance if greater" writes used against it.
    */
    volatile int  mwBlackConsolidatedUpTo[MAX_WRITERS];
    volatile int  mwWhiteConsolidatedUpTo[MAX_WRITERS];

    /*
    ** Per-(writer, color) slot ownership for background consolidation's
    ** CONSOLIDATION_THREADS_PER_PAIR concurrency cap -- 0 = free, 1 = claimed.
    ** DoBackgroundConsolidation claims the first free slot (s) via
    ** InterlockedCompareExchange(&consolSlotOwner[wi][p][s], 1, 0) as the very
    ** first thing it does (declining immediately, no side effects, if all
    ** CONSOLIDATION_THREADS_PER_PAIR are already 1), and releases it
    ** (InterlockedExchange back to 0) on every exit path. Does double duty as
    ** both the concurrency cap (at most CONSOLIDATION_THREADS_PER_PAIR slots
    ** exist per pair) and the stable slot number ConsolSlot() addresses --
    ** needed because pConsolidationPool is one pool shared across every
    ** (writer, color) pair, so a job's ThreadPool-assigned thdIdx is a
    ** *global* worker index, not usable as a per-pair slot number.
    */
    volatile LONG  consolSlotOwner[MAX_WRITERS][2][CONSOLIDATION_THREADS_PER_PAIR];
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
    ** Live progress for every background-consolidation examination slot,
    ** MemMalloc'd (InitSolver.cpp) as numMergeWriters*2*
    ** CONSOLIDATION_THREADS_PER_PAIR ConsolidationSlotStats entries -- see
    ** that type's comment above. Addressed via the ConsolSlot() helper
    ** (below, after this struct, since it dereferences POthelloRingMasterState).
    ** Replaces the old one-scalar-per-writer consolidationActive/Player/
    ** TotalBytes/DoneBytes/StartTickMs fields, which could only ever
    ** represent ONE in-progress merge per writer -- insufficient once
    ** multiple concurrent merges per (writer, color) pair became possible.
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
    /* Background small-file consolidator (DoBackgroundConsolidation,
    ** MergeFiles.cpp) -- ONE shared pool, CONSOLIDATION_POOL_THREADS threads,
    ** servicing every (writer, color) pair's examination jobs. Deliberately
    ** flat-sized (not scaled by numMergeWriters) so adding more NVMe drives
    ** never grows total thread count -- per-pair concurrency is capped
    ** separately via consolSlotOwner instead of dedicated per-pair pools.
    ** Separate from pMergeWriterPool so it draws from otherwise-idle cores.
    */
    ThreadPool* pConsolidationPool;
} OthelloRingMasterState, * POthelloRingMasterState;

/*
** Function: ConsolSlot
** @brief    Computes the address of one (writerIdx, player, slotIdx) entry
**           inside the MemMalloc'd pConsolSlotStats block (InitSolver.cpp).
**           slotIdx is the value claimed via consolSlotOwner[writerIdx][player]
**           (0..CONSOLIDATION_THREADS_PER_PAIR-1) -- NOT a ThreadPool thdIdx,
**           which is a global worker index shared across every pair now that
**           pConsolidationPool is one pool for all of them.
** @param    pSt       - solver state
** @param    writerIdx - which writer drive (0..numMergeWriters-1)
** @param    player    - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @param    slotIdx   - claimed consolSlotOwner slot (0..CONSOLIDATION_THREADS_PER_PAIR-1)
** @return   Pointer to that slot's live-progress stats.
*/
static inline PConsolidationSlotStats ConsolSlot(POthelloRingMasterState pSt,
                                                  int writerIdx, int player, int slotIdx)
{
    return &pSt->pConsolSlotStats[(writerIdx * 2 + player) * CONSOLIDATION_THREADS_PER_PAIR + slotIdx];
}
