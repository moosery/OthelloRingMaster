# OthelloRingMaster

GPU-accelerated BFS enumeration of unique Othello (Reversi) board states by level, storing
each board in a **ring-gathered** bit layout instead of row-major -- and, as of real 6x6-scale
measurement, meaningfully smaller and at least as fast as the row-major sibling project
(`OthelloLevelBlaster`) it was built to supersede.

Each *level* corresponds to one piece placed on the board (starting from 4 pre-placed pieces).
The solver expands all legal moves from every unique board at level N, canonicalizes each
result under the 16-element D4 x color-swap symmetry group, and deduplicates to produce
the set of unique boards at level N+1. The output for each level is a **4-file ring
nested-index** per player-to-move (`CellsInUse` -> `Ring_1` -> `Ring_2` -> `Ring_3_4`) on the
store drive, written directly by the merge -- there is no flat intermediate file at any point.

This is a fresh, isolated solution, separate from the sibling `OthelloLevelBlaster` project
(a precious, actively-running production solver) -- `OthelloLevelBlaster` is never modified,
only read from as reusable raw material. A companion project,
`OthelloRingMasterCalculator`, walks a completed store backward from its deepest level to
level 0 to compute per-board win/tie/loss retrograde counts -- the actual best-move oracle
this whole storage design exists to make possible.

## Architecture

```
Store drive (Y:)
  Level_N_black.cellsinuse.rsfzl / .ring1.rsfzl / .ring2.rsfzl / .ring34.rsfzl  ──┐
  Level_N_white.cellsinuse.rsfzl / .ring1.rsfzl / .ring2.rsfzl / .ring34.rsfzl  ──┘
         │
    GPU feeder (ping-pong buffer, one batch at a time)
    ── ring -> row-major boundary conversion on load ──
         │
    ExpandKernel (row-major move-gen/flip/canonicalize) → two-stack GPU accumulator
    ── row-major -> ring boundary conversion before scatter into the accumulator ──
    (black ↑ from 0, white ↓ from cap; accumulator is ring-ordered from the moment
    it's written, so CUB dedup/sort downstream never needs to know about row-major at all)
         │
    GpuFlushPrepare: CUB DeviceRadixSort + dedup (two independent passes)
         │
    D2H to merge-writer thread's staging buffer (one per NVMe directory)
         │
    LZ4-compress staging → in-memory pool segment (no disk I/O)
         │  [pool full: FlushMergeWriterBuffer k-way merges pool (black + white concurrently)
         │              → writer_black/white_NNNN.rsfz on D:, E:, ...]
         │  [NVMe low: DoCrossDriveIntermediateMerge → imerge files on F:]
         │
    DoEndOfLevelMerge (parallel: black thread + white thread)
      ├─ black: merge remaining pool segments + writer/imerge files, straight into
      │         Level_(N+1)_black's 4 ring files on Y: (RingNestedIndexBuilder)
      └─ white: same, for white                          (both direct to Y:, no NVMe
                                                            round-trip, no flat intermediate)
```

### Key components

- **GPU feeder** -- reads a level's boards from the store drive in batches via a ping-pong
  buffer (`RingNestedIndexStreamAll`, O(1) memory regardless of level size) and feeds the
  GPU accumulator. Two sub-passes per level: black-to-move files first, then white-to-move.
- **Ring <-> row-major boundary conversion** -- the CPU never interprets row-major bit
  structure; it only organizes (sorting, merging, grouping into the nested index, file I/O).
  All actual board manipulation is GPU-exclusive. `ExpandKernel` converts a loaded parent's
  ring-ordered bits to row-major, runs the existing row-major move-gen/flip/canonicalize
  device functions unchanged, then converts each child back to ring order before the atomic
  scatter into `d_accum` -- so the accumulator, and everything downstream of it (CUB sort,
  dedup, the merge), is ring-ordered from the very first write and never needs to know a
  row-major representation exists.
- **GPU accumulator (two-stack)** -- a single `BOARD_KEY*` array holds both players: black
  grows from index 0 upward, white grows from `capacity-1` downward. Flushed to
  merge-writer threads when VRAM is ~80% full.
- **Merge-writer threads** -- one per fast NVMe directory, same staging/pool/flush design as
  `OthelloLevelBlaster`'s own (see that project's README for the full mechanics) -- opaque
  16-byte board keys move through this whole stage without any ring-specific handling.
- **Intermediate merge / cascading merge** -- same fan-in-bounded (`MAX_MERGE_FANIN`)
  grouped-merge design as `OthelloLevelBlaster`. When a level's merge target is ring format
  (i.e. always, for the real per-level store), cascade's own intermediate group files become
  ring-format too, via a pull-style incremental ring reader (`RingNestedIndexPullReader`) that
  lets a k-way merge heap interleave several ring-format inputs at once -- single-round only;
  a level needing a second cascade round would require well over 12 million real input files
  for one color, never seen at this project's scale, so that case Fatals with a clear
  diagnostic rather than running an untested path.
- **End-of-level merge, writing ring format directly** -- `DoEndOfLevelMerge` opens the next
  level's 4 ring files up front and merges every remaining writer/intermediate file plus any
  in-memory pool leftovers straight into them (`KWayMergeFilesToRingIndex`,
  `RingNestedIndexBuilder`). No flat intermediate file is ever written for a level's store
  output, and no separate "convert flat to ring format" pass exists -- an earlier version of
  this project did write one, then immediately reread and rewrote it as ring format; that
  doubled the actual store I/O for zero benefit and was removed once found.
- **Drive space ledger** -- same atomic per-drive ledger design as `OthelloLevelBlaster`,
  seeded from the OS after startup cleanup with a safety buffer subtracted.

## Requirements

| Component | Minimum |
|-----------|---------|
| OS        | Windows 10/11 x64 |
| Compiler  | Visual Studio 2022 with CUDA toolkit |
| GPU       | NVIDIA sm_89 (RTX 40-series) -- change `sm_89` in vcxproj for other architectures |
| RAM       | 40 GB recommended (scales with VRAM and number of NVMe drives) |
| Fast drives | 1-4 NVMe drives for merge-writer working space |
| Intermediate drive | HDD/SATA SSD for overflow merge (F: by default) |
| Store drive | Large slow drive for level output files (Y: by default) -- real 6x6 measurements so far show meaningfully less growth per level than the row-major store format needs for the same data (see Performance below), but the run is still in progress; size generously |

## Build

Open `OthelloRingMaster.slnx` in Visual Studio 2022 and build the solution in **Release | x64**.

Outputs:
- `x64/Release/OthelloRingMaster.exe` -- main solver
- `x64/Release/OthelloRingMasterStatus.exe` -- live status client (TCP)
- `x64/Release/OthelloRingMasterCalculator.exe` -- retrograde win/tie/loss calculator
- `x64/Release/OthelloRingMasterCalculatorStatus.exe` -- calculator's live status client (TCP)

## Usage

### Solver

```
OthelloRingMaster.exe [options]

  --board-size N    Board size: 4, 6, or 8                     [default: 6]
  --drives LETTERS  Drive letters to use, e.g. DEFY             [default: DEFY]
  --store-drive L   Drive letter for NAS/store output           [default: Y]
  --store-dir PATH  Sub-path on store drive (no drive letter)   [default: \OthelloRingMaster\Store]
  --cache-dir PATH  Full path for logs and drive-bench cache    [default: C:\OthelloRingMaster\Cache]
  --port N          Stats listener TCP port                     [default: 17532]
  --compress        Compress writer/intermediate merge files as .rsfz [default]
  --compress-store-only  Compress only intermediate merge output; writer files stay .rsf
  --no-compress     Write writer/intermediate merge files as .rsf (uncompressed)
  --lz4-drives DEF  Drive letters that get LZ4 on top of varint (.rsfzl) [default: DEF]
  --help            Show this help
```

Note: `--compress`/`--no-compress`/`--compress-store-only`/`--lz4-drives` only affect writer
and intermediate/cascade merge files -- a level's actual permanent store output (the 4 ring
nested-index files) is **always** written via the same delta+varint+LZ4 shaped compression
regardless of these flags, since that tier is what the ring format's own space savings depend on.

While the solver runs, query live status from another terminal:

```
OthelloRingMasterStatus.exe
```

### Retrograde calculator

```
OthelloRingMasterCalculator.exe [options]

  --board-size N          Board size: 4, 6, or 8                    [default: 6]
  --store-drive L         Drive letter holding RingMaster's finished store [default: Y]
  --store-dir PATH        Sub-path on store drive (no drive letter) -- same value given to RingMaster
  --counts-drive L        Drive letter this calculator writes its own counts files to [default: Y]
  --counts-dir PATH       Sub-path on counts drive (no drive letter) [default: \OthelloRingMasterCalculator\Counts]
  --cache-dir PATH        Full path for logs and the width-config file [default: C:\OthelloRingMasterCalculator\Cache]
  --drive-cache-dir PATH  Cache dir for the shared drive-benchmark file (driveinfo.json) -- defaults to RingMaster's own cache dir
  --use-drives STR        Drive letters available for segmented scratch (e.g. DEFG) [default: DEFY, same as RingMaster's own default]
  --scratch-dir PATH      Sub-path (on whichever scratch drive) segments are written under [default: \OthelloRingMasterCalculator\Scratch]
  --port N                Stats listener TCP port                   [default: 17632]
  --force                 Delete this board size's own sentinel/counts files before starting, ignoring any prior run's completed levels
  --help                  Show this help
```

```
OthelloRingMasterCalculatorStatus.exe
```

Walks the completed store backward from its deepest level to level 0: classifies terminal
boards directly, sums non-terminal ones by regenerating children on the GPU and looking each
one up against level+1's already-computed results -- via four drive-spanning segmented
scratch stores (`CellsInUse`/`Ring_1`/`Ring_2`/`Ring_3_4`, decompressed but still ring-shaped,
never rehydrated to flat board-key records, and never holding a whole level resident in
memory), with adaptive per-level counter width (nibble through arbitrary byte widths,
widening automatically on overflow). Validated end to end on 4x4: reproduces the known-correct
whole-tree result exactly -- 24,632 black wins / 30,116 white wins / 5,312 ties.

Both the solver and the calculator auto-resume: if their respective output directories
already contain completed level data, they pick up from the first incomplete level.
Press **Ctrl+C** on the solver for a graceful shutdown -- merge loops check the terminate
flag and stop promptly; partial output is cleaned up automatically on the next run.

### Drive layout convention

| Drive | Role | Notes |
|-------|------|-------|
| D:, E: | Fast (NVMe) | Merge-writer working space; needs room for one full level's writer output |
| F: | Intermediate (HDD/SATA) | Overflow intermediate merge + cascade temp files (ring-format when the final target is ring format) |
| Y: | Store (NAS/large HDD) | Accumulates the 4 ring nested-index files per player per level; needs total of all level output |

Drives are auto-detected and categorized by benchmark speed. The cache file
`C:\OthelloRingMaster\Cache\driveinfo.json` stores benchmark results across runs (shared with
the calculator via `--drive-cache-dir`, since drive speed is a property of the machine, not
of which program is asking).

## File format

Board states are stored in **RSF** (Ring Store File) format, in three compression tiers
sharing one 64-byte trailer layout (magic, record count, min/max key):
- `.rsf` -- plain, uncompressed 16-byte `{hi, lo}` records.
- `.rsfz` -- delta+zigzag+varint compressed.
- `.rsfzl` -- delta+zigzag+varint, then LZ4-framed on top.

A **shaped** variant of the same machinery (`RSF_SHAPE_PAIR64`/`RSF_SHAPE_RING_LEVEL`/
`RSF_SHAPE_LEAF16`) generalizes this to narrower, differently-laid-out records without
changing the underlying delta+varint+LZ4 core, which is what lets all 4 ring nested-index
files share one compression tier instead of mixing formats.

### The ring nested-index (a level's actual permanent store)

Each level/player is stored as 4 files forming a hierarchy, not one flat sorted stream:

- **`CellsInUse`** (`.cellsinuse.rsfzl`) -- unique ring-gathered occupancy pattern + offset
  into the next stored level.
- **`Ring_1`** (`.ring1.rsfzl`) -- 28-bit outermost-ring color subpattern + offset into
  `Ring_2`. Skipped entirely for 4x4 and 6x6 boards (their active cells, centered within the
  8x8 word the ring geometry always assumes, never reach this ring).
- **`Ring_2`** (`.ring2.rsfzl`) -- 20-bit second-ring color subpattern + offset into
  `Ring_3_4`. Skipped for 4x4.
- **`Ring_3_4`** (`.ring34.rsfzl`) -- 16-bit combined mid+inner-ring color subpattern, one
  record per board (the leaf -- every group here has exactly 1 member, by construction).
  Deliberately kept as one combined field rather than split into a `Ring_3`
  grouping level + `Ring_4` leaf -- tested directly on real 6x6 data (see Performance below)
  and found to make total storage ~5-6% *worse*, not better, both times tried.

None of `CellsInUse`/`Ring_1`/`Ring_2` store an explicit child count -- a group's span in the
next level down is implied by the next record's own offset (or "consume until EOF" for the
very last group), the same trick applied at every level of the hierarchy.

**Key trick**: ring geometry is always the full 8x8 depth regardless of actual board size,
since a smaller board's active cells are already centered within the 8x8 word by the
production encoding -- this lets every board size share one fixed layout, just with the
degenerate outer ring(s) skipped where they're provably always empty.

Player turn (black-to-move / white-to-move) is encoded in the filename, not the record.

### Filename conventions

| Pattern | Description |
|---------|-------------|
| `Level_NNNN_SxS_black_0000.cellsinuse.rsfzl` (+ `.ring1`/`.ring2`/`.ring34.rsfzl`) | Level N black-to-move store files on Y: |
| `Level_NNNN_SxS_white_0000.cellsinuse.rsfzl` (+ ...) | Level N white-to-move store files on Y: |
| `writer_black_NNNN.rsfz` | In-progress flush output from a merge-writer thread |
| `imerge_LNNN_black_NNNN.rsfz` | Intermediate merge output on F: (flat) |
| `cascade_temp_LNNN_black_NNNN.rsf` | Flat cascade temp file on F: (deleted after use) |
| `cascade_ring_LNNN_black_NNNN.cellsinuse.rsfzl` (+ ...) | Ring-format cascade group temp files on F: (deleted after use) |

## Performance (6x6, RTX 4080 SUPER)

Real measured run, same physical machine and drive layout as `OthelloLevelBlaster`'s own
measured run in its README (D:/E: NVMe, F: HDD, Y: NAS). Every correctness-critical field
(`BoardsIn`, `NewBoards`, `Pass`, `UniqueOut`, `Ends`, `MaxMv`, `Fls`) matches `OthelloLevelBlaster`'s
row-major run **exactly**, level by level -- the ring-ordered pipeline produces bit-for-bit
the same dedup result, just stored differently.

| Level | Unique boards | Blaster store (MrgGB) | RingMaster store (MrgGB) | Smaller by | Blaster total time | RingMaster total time |
|-------|---------------|------------------------|---------------------------|------------|---------------------|------------------------|
| 12    | 251 M         | 0.62 GB   | 0.37 GB   | 40.3% | 12.6 s    | 13.2 s    |
| 13    | 1.21 B        | 2.66 GB   | 1.53 GB   | 42.5% | 59.7 s    | 63.2 s    |
| 14    | 5.01 B        | 9.75 GB   | 5.47 GB   | 43.9% | 4.3 min   | 4.9 min   |
| 15    | 19.8 B        | 34.63 GB  | 19.06 GB  | 45.0% | 20.7 min  | 19.9 min  |
| 16    | 65.9 B        | 103.67 GB | 55.52 GB  | 46.4% | 81.8 min  | 79.6 min  |
| 17    | 203.5 B       | 292.92 GB | 153.67 GB | 47.5% | 4.17 h    | 4.43 h    |
| 18    | 526.0 B       | 695.54 GB | 355.62 GB | 48.9% | 13.10 h   | 12.62 h   |

**Storage is the clear, dramatic, and still-growing win** -- 40% smaller at level 12, up to
49% smaller at level 18, with no sign of leveling off yet. **Wall-clock time is competitive,
not dramatically different either way** -- within a few percent of Blaster's own row-major
run at every level shown, sometimes a little faster, sometimes a little slower, consistent
with writing meaningfully less data while spending some of that saved I/O time on a
4-files-instead-of-1 compression pipeline. Levels beyond 18 aren't complete yet for this run.

## Project layout

```
OthelloRingMaster/
  OthelloRingMaster.cpp         Main loop, argument parsing, level driver
  OthelloTypes.h                Shared structs: config, state, stats, driveLedger
  DriveLedger.h                 Per-drive atomic space ledger (Reserve/Reclaim/Debit)
  InitSolver.cpp / .h           Resource allocation, drive setup, cleanup, ledger init, resume scan
  GpuKernels.cu / .h            CUDA move expansion, ring<->row-major boundary conversion,
                                  canonical form, two-stack accumulator
  GpuInfo.cu / .h                GPU device query
  LevelSolverThread.cpp / .h    Merge-writer job + GPU feeder thread (ping-pong reader → GPU)
  MergeFiles.cpp / .h           FlushMergeWriterBuffer, DoCrossDriveIntermediateMerge,
                                  CascadingMerge / CascadeGroupsToRingIndex, DoEndOfLevelMerge
  RSFFileName.h                 RSF filename construction and pattern helpers (flat + ring)
  StatsListener.cpp / .h        TCP stats server (port 17532)
  CreateSeedFile.cpp / .h       Level-0 seed file generator (ring nested-index format directly)
  GetMachineInfo.cpp / .h       Drive detection, GPU detection, benchmarking
  InitLogger.cpp / .h           Log file setup

  OthelloBasics/                Board representation, move generation, canonicalization (CPU);
                                  RingNestedIndex.h/.cpp -- the ring nested-index builder/reader/
                                  pull-reader, shared by the solver and the calculator
  OthelloBasicsForCUDA/          Same, compiled for device code; RingConversion.h/.cu -- the
                                  ring<->row-major boundary conversion tables/kernels
  Utility/                       Threading, memory, clocks, drive info, logging, RingStoreFile.h/.cpp
                                  (the generic RSF record-file format, plain + shaped)
  lz4/                           Vendored LZ4 library

  OthelloRingMasterStatus/       Solver's status client project
  OthelloRingMasterCalculator/   Retrograde win/tie/loss calculator (see its own --help)
  OthelloRingMasterCalculatorStatus/  Calculator's status client project
```

## Related

`OthelloLevelBlaster` (sibling solution, not touched by this project) is the source of the
design work and reusable infrastructure behind this project, and remains an actively-running
production solver in its own right.
